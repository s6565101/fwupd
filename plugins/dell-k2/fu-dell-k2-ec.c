/*
 * Copyright 2024 Dell Technologies
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
 */

#include "config.h"

#include <string.h>

#include "fu-dell-k2-common.h"

typedef struct __attribute__((packed)) { /* nocheck:blocked */
	guint32 ec_version;
	guint32 mst_version;
	guint32 hub1_version;
	guint32 hub2_version;
	guint32 tbt_version;
	guint32 pkg_version;
	guint32 pd_version;
	guint32 epr_version;
	guint32 dpmux_version;
	guint32 rmm_version;
	guint32 reserved[6];
} FuDellK2DockFWVersion;

typedef struct __attribute__((packed)) { /* nocheck:blocked */
	struct FuDellK2V2DockInfoHeader {
		guint8 total_devices;
		guint8 first_index;
		guint8 last_index;
	} header;
	struct FuDellK2EcQueryEntry {
		struct FuDellK2V2EcAddrMap {
			guint8 location;
			guint8 device_type;
			guint8 sub_type;
			guint8 arg;
			guint8 instance;
		} ec_addr_map;
		union {
			guint32 version_32;
			guint8 version_8[4];
		} __attribute__((packed)) version; /* nocheck:blocked */
	} devices[20];
} FuDellK2DockInfoStructure;

/* Private structure */
struct _FuDellK2Ec {
	FuDevice parent_instance;
	FuStructDellK2DockData *dock_data;
	FuDellK2DockInfoStructure *dock_info;
	FuDellK2DockFWVersion *raw_versions;
	FuDellK2BaseType base_type;
	guint8 base_sku;
	guint64 blob_version_offset;
	gboolean dock_lock_state;
};

G_DEFINE_TYPE(FuDellK2Ec, fu_dell_k2_ec, FU_TYPE_HID_DEVICE)

static struct FuDellK2EcQueryEntry *
fu_dell_k2_ec_dev_entry(FuDevice *device, guint8 device_type, guint8 sub_type, guint8 instance)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);

	for (guint i = 0; i < self->dock_info->header.total_devices; i++) {
		if (self->dock_info->devices[i].ec_addr_map.device_type != device_type)
			continue;
		if (sub_type != 0 && self->dock_info->devices[i].ec_addr_map.sub_type != sub_type)
			continue;

		/* vary by instance index */
		if (device_type == DELL_K2_EC_DEV_TYPE_PD &&
		    self->dock_info->devices[i].ec_addr_map.instance != instance)
			continue;

		return &self->dock_info->devices[i];
	}
	return NULL;
}

gboolean
fu_dell_k2_ec_is_dev_present(FuDevice *device, guint8 dev_type, guint8 sub_type, guint8 instance)
{
	return fu_dell_k2_ec_dev_entry(device, dev_type, sub_type, instance) != NULL;
}

const gchar *
fu_dell_k2_ec_devicetype_to_str(guint8 device_type, guint8 sub_type, guint8 instance)
{
	switch (device_type) {
	case DELL_K2_EC_DEV_TYPE_MAIN_EC:
		return "EC";
	case DELL_K2_EC_DEV_TYPE_PD:
		if (sub_type == DELL_K2_EC_DEV_PD_SUBTYPE_TI) {
			if (instance == DELL_K2_EC_DEV_PD_SUBTYPE_TI_INSTANCE_UP5)
				return "PD UP5";
			if (instance == DELL_K2_EC_DEV_PD_SUBTYPE_TI_INSTANCE_UP15)
				return "PD UP15";
			if (instance == DELL_K2_EC_DEV_PD_SUBTYPE_TI_INSTANCE_UP17)
				return "PD UP17";
		}
		return NULL;
	case DELL_K2_EC_DEV_TYPE_USBHUB:
		if (sub_type == DELL_K2_EC_DEV_USBHUB_SUBTYPE_RTS5480)
			return "RTS5480 USB Hub";
		if (sub_type == DELL_K2_EC_DEV_USBHUB_SUBTYPE_RTS5485)
			return "RTS5485 USB Hub";
		return NULL;
	case DELL_K2_EC_DEV_TYPE_MST:
		if (sub_type == DELL_K2_EC_DEV_MST_SUBTYPE_VMM8430)
			return "MST VMM8430";
		if (sub_type == DELL_K2_EC_DEV_MST_SUBTYPE_VMM9430)
			return "MST VMM9430";
		return NULL;
	case DELL_K2_EC_DEV_TYPE_TBT:
		if (sub_type == DELL_K2_EC_DEV_TBT_SUBTYPE_TR)
			return "Titan Ridge";
		if (sub_type == DELL_K2_EC_DEV_TBT_SUBTYPE_GR)
			return "Goshen Ridge";
		if (sub_type == DELL_K2_EC_DEV_TBT_SUBTYPE_BR)
			return "Barlow Ridge";
		return NULL;
	case DELL_K2_EC_DEV_TYPE_QI:
		return "Qi";
	case DELL_K2_EC_DEV_TYPE_DP_MUX:
		return "DP Mux";
	case DELL_K2_EC_DEV_TYPE_LAN:
		return "Intel i226-LM";
	case DELL_K2_EC_DEV_TYPE_FAN:
		return "Fan";
	case DELL_K2_EC_DEV_TYPE_RMM:
		return "Remote Management";
	case DELL_K2_EC_DEV_TYPE_WTPD:
		return "Weltrend PD";
	default:
		return NULL;
	}
}

FuDellK2BaseType
fu_dell_k2_ec_get_dock_type(FuDevice *device)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	return self->base_type;
}

guint8
fu_dell_k2_ec_get_dock_sku(FuDevice *device)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	return self->base_sku;
}

static gboolean
fu_dell_k2_ec_read(FuDevice *device, guint32 cmd, GByteArray *res, GError **error)
{
	if (!fu_dell_k2_ec_hid_i2c_read(device, cmd, res, 800, error)) {
		g_prefix_error(error, "read over HID-I2C failed: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_dell_k2_ec_write(FuDevice *device, GByteArray *buf, GError **error)
{
	g_return_val_if_fail(device != NULL, FALSE);
	g_return_val_if_fail(buf->len > 1, FALSE);

	if (!fu_dell_k2_ec_hid_i2c_write(device, buf->data, buf->len, error)) {
		g_prefix_error(error, "write over HID-I2C failed: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_dell_k2_ec_create_node(FuDevice *ec_device, FuDevice *new_device, GError **error)
{
	g_autoptr(FuDeviceLocker) locker = NULL;

	locker = fu_device_locker_new(new_device, error);
	if (locker == NULL)
		return FALSE;

	/* setup relationship */
	fu_device_add_child(ec_device, FU_DEVICE(new_device));
	return TRUE;
}

static gboolean
fu_dell_k2_ec_probe_package(FuDevice *ec_dev, GError **error)
{
	g_autoptr(FuDellK2Package) pkg_dev = NULL;

	pkg_dev = fu_dell_k2_package_new(ec_dev);
	return fu_dell_k2_ec_create_node(ec_dev, FU_DEVICE(pkg_dev), error);
}

static gboolean
fu_dell_k2_ec_probe_pd(FuDevice *ec_dev,
		       DellK2EcDevType dev_type,
		       DellK2EcDevPdSubtype subtype,
		       guint8 instance,
		       GError **error)
{
	g_autoptr(FuDellK2Pd) pd_dev = NULL;

	if (fu_dell_k2_ec_dev_entry(ec_dev, dev_type, subtype, instance) == NULL)
		return TRUE;

	pd_dev = fu_dell_k2_pd_new(ec_dev, subtype, instance);
	return fu_dell_k2_ec_create_node(ec_dev, FU_DEVICE(pd_dev), error);
}

static gboolean
fu_dell_k2_ec_probe_subcomponents(FuDevice *device, GError **error)
{
	g_return_val_if_fail(device != NULL, FALSE);

	/* Package */
	if (!fu_dell_k2_ec_probe_package(device, error))
		return FALSE;

	/* PD UP5 */
	if (!fu_dell_k2_ec_probe_pd(device,
				    DELL_K2_EC_DEV_TYPE_PD,
				    DELL_K2_EC_DEV_PD_SUBTYPE_TI,
				    DELL_K2_EC_DEV_PD_SUBTYPE_TI_INSTANCE_UP5,
				    error))
		return FALSE;

	/* PD UP15 */
	if (!fu_dell_k2_ec_probe_pd(device,
				    DELL_K2_EC_DEV_TYPE_PD,
				    DELL_K2_EC_DEV_PD_SUBTYPE_TI,
				    DELL_K2_EC_DEV_PD_SUBTYPE_TI_INSTANCE_UP15,
				    error))
		return FALSE;

	/* PD UP17 */
	if (!fu_dell_k2_ec_probe_pd(device,
				    DELL_K2_EC_DEV_TYPE_PD,
				    DELL_K2_EC_DEV_PD_SUBTYPE_TI,
				    DELL_K2_EC_DEV_PD_SUBTYPE_TI_INSTANCE_UP17,
				    error))
		return FALSE;

	/* DP MUX */
	if (fu_dell_k2_ec_dev_entry(device, DELL_K2_EC_DEV_TYPE_DP_MUX, 0, 0) != NULL) {
		g_autoptr(FuDellK2Dpmux) dpmux_device = NULL;

		dpmux_device = fu_dell_k2_dpmux_new(device);
		if (!fu_dell_k2_ec_create_node(device, FU_DEVICE(dpmux_device), error))
			return FALSE;
	}

	/* WELTREND PD */
	if (fu_dell_k2_ec_dev_entry(device, DELL_K2_EC_DEV_TYPE_WTPD, 0, 0) != NULL) {
		g_autoptr(FuDellK2Wtpd) weltrend_device = NULL;

		weltrend_device = fu_dell_k2_wtpd_new(device);
		if (!fu_dell_k2_ec_create_node(device, FU_DEVICE(weltrend_device), error))
			return FALSE;
	}

	/* Intel i266-LM */
	if (fu_dell_k2_ec_dev_entry(device, DELL_K2_EC_DEV_TYPE_LAN, 0, 0) != NULL) {
		g_autoptr(FuDellK2Ilan) ilan_device = NULL;

		ilan_device = fu_dell_k2_ilan_new(device);
		if (!fu_dell_k2_ec_create_node(device, FU_DEVICE(ilan_device), error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
fu_dell_k2_ec_dock_type_extract(FuDevice *device, GError **error)
{
	FuDellK2BaseType dock_type = fu_dell_k2_ec_get_dock_type(device);
	guint8 dev_type = DELL_K2_EC_DEV_TYPE_MAIN_EC;

	/* don't change error type, the plugin ignores it */
	if (dock_type != FU_DELL_K2_BASE_TYPE_K2) {
		g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "No valid dock was found");
		return FALSE;
	}

	/* this will trigger setting up all the quirks */
	fu_device_add_instance_u8(device, "DOCKTYPE", dock_type);
	fu_device_add_instance_u8(device, "DEVTYPE", dev_type);
	fu_device_build_instance_id(device,
				    error,
				    "USB",
				    "VID",
				    "PID",
				    "DOCKTYPE",
				    "DEVTYPE",
				    NULL);
	return TRUE;
}

static gboolean
fu_dell_k2_ec_dock_type_cmd(FuDevice *device, GError **error)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	gsize length = 1;
	g_autoptr(GByteArray) res = g_byte_array_new_take(g_malloc0(length), length);
	guint8 cmd = DELL_K2_EC_HID_CMD_GET_DOCK_TYPE;

	/* expect response 1 byte */
	if (!fu_dell_k2_ec_read(device, cmd, res, error)) {
		g_prefix_error(error, "Failed to query dock type: ");
		return FALSE;
	}

	self->base_type = res->data[0];

	/* check dock type to proceed with this plugin or exit as unsupported */
	return fu_dell_k2_ec_dock_type_extract(device, error);
}

static gboolean
fu_dell_k2_ec_dock_info_extract(FuDevice *device, GError **error)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);

	if (!self->dock_info) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "Failed to parse dock info");
		return FALSE;
	}

	if (self->dock_info->header.total_devices == 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_SIGNATURE_INVALID,
			    "No bridge devices detected, dock may be booting up");
		return FALSE;
	}
	g_info("found %u devices [%u->%u]",
	       self->dock_info->header.total_devices,
	       self->dock_info->header.first_index,
	       self->dock_info->header.last_index);

	for (guint i = 0; i < self->dock_info->header.total_devices; i++) {
		struct FuDellK2EcQueryEntry dev_entry = self->dock_info->devices[i];
		const gchar *type_str;
		const gchar *location_str;
		guint32 version32 = GUINT32_FROM_BE(dev_entry.version.version_32);
		g_autofree gchar *version_str = NULL;

		/* name the component */
		type_str = fu_dell_k2_ec_devicetype_to_str(dev_entry.ec_addr_map.device_type,
							   dev_entry.ec_addr_map.sub_type,
							   dev_entry.ec_addr_map.instance);
		if (type_str == NULL) {
			g_warning("missing device name, DevType: %u, SubType: %u, Inst: %u",
				  dev_entry.ec_addr_map.device_type,
				  dev_entry.ec_addr_map.sub_type,
				  dev_entry.ec_addr_map.instance);
			continue;
		}

		/* name the location of component */
		location_str = (dev_entry.ec_addr_map.location == DELL_K2_EC_LOCATION_BASE)
				   ? "Base"
				   : "Module";

		/* show the component location */
		g_debug("#%u: %s located in %s (A: %u I: %u)",
			i,
			type_str,
			location_str,
			dev_entry.ec_addr_map.arg,
			dev_entry.ec_addr_map.instance);

		/* show the component version */
		version_str = fu_version_from_uint32_hex(version32, FWUPD_VERSION_FORMAT_QUAD);
		g_debug("version32: %08x, version8: %s", dev_entry.version.version_32, version_str);
	}
	return TRUE;
}

static gboolean
fu_dell_k2_ec_dock_info_cmd(FuDevice *device, GError **error)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	gsize length = sizeof(FuDellK2DockInfoStructure);
	g_autoptr(GByteArray) res = g_byte_array_new_take(g_malloc0(length), length);
	guint8 cmd = DELL_K2_EC_HID_CMD_GET_DOCK_INFO;

	/* get dock info over HID */
	if (!fu_dell_k2_ec_read(device, cmd, res, error)) {
		g_prefix_error(error, "Failed to query dock info: ");
		return FALSE;
	}
	if (res->len != length) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "invalid dock info size: expected %" G_GSIZE_FORMAT ",got %u",
			    length,
			    res->len);
		return FALSE;
	}

	if (!fu_memcpy_safe((guint8 *)self->dock_info,
			    length,
			    0,
			    res->data,
			    res->len,
			    0,
			    length,
			    error))
		return FALSE;

	if (!fu_dell_k2_ec_dock_info_extract(device, error))
		return FALSE;
	return TRUE;
}

static gboolean
fu_dell_k2_ec_dock_data_extract(FuDevice *device, GError **error)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	g_autofree gchar *mkt_name = NULL;
	g_autofree gchar *serial = NULL;

	/* set FuDevice name */
	mkt_name = fu_struct_dell_k2_dock_data_get_marketing_name(self->dock_data);
	fu_device_set_name(device, mkt_name);

	/* set FuDevice serial */
	serial = g_strdup_printf("%.7s/%016" G_GUINT64_FORMAT,
				 fu_struct_dell_k2_dock_data_get_service_tag(self->dock_data),
				 fu_struct_dell_k2_dock_data_get_module_serial(self->dock_data));
	fu_device_set_serial(device, serial);

	return TRUE;
}

static gboolean
fu_dell_k2_ec_dock_data_cmd(FuDevice *device, GError **error)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	g_autoptr(GByteArray) res = fu_struct_dell_k2_dock_data_new();
	guint8 cmd = DELL_K2_EC_HID_CMD_GET_DOCK_DATA;

	/* get dock data over HID */
	if (!fu_dell_k2_ec_read(device, cmd, res, error)) {
		g_prefix_error(error, "Failed to query dock data: ");
		return FALSE;
	}

	self->dock_data = fu_struct_dell_k2_dock_data_parse(res->data, res->len, 0, error);
	if (!fu_dell_k2_ec_dock_data_extract(device, error))
		return FALSE;
	return TRUE;
}

gboolean
fu_dell_k2_ec_is_dock_ready4update(FuDevice *device, GError **error)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	guint16 bitmask_fw_update_pending = 1 << 8;
	guint32 dock_status = 0;

	if (!fu_dell_k2_ec_dock_data_cmd(device, error))
		return FALSE;

	dock_status = fu_struct_dell_k2_dock_data_get_dock_status(self->dock_data);
	if ((dock_status & bitmask_fw_update_pending) != 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_BUSY,
			    "dock status (%x) has pending updates, unavailable for now.",
			    dock_status);
		return FALSE;
	}

	return TRUE;
}

gboolean
fu_dell_k2_ec_own_dock(FuDevice *device, gboolean lock, GError **error)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	g_autoptr(GByteArray) req = g_byte_array_new();
	g_autoptr(GError) error_local = NULL;

	fu_byte_array_append_uint8(req, DELL_K2_EC_HID_CMD_SET_MODIFY_LOCK);
	fu_byte_array_append_uint8(req, 2); // length of data
	fu_byte_array_append_uint16(req, lock ? 0xFFFF : 0x0000, G_LITTLE_ENDIAN);

	fu_device_sleep(device, 1000);
	if (!fu_dell_k2_ec_write(device, req, &error_local)) {
		if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND))
			g_debug("ignoring: %s", error_local->message);
		else {
			g_propagate_error(error, g_steal_pointer(&error_local));
			g_prefix_error(error, "failed to %s dock: ", lock ? "own" : "release");
			return FALSE;
		}
	}
	self->dock_lock_state = lock;
	g_debug("dock is %s successfully", lock ? "owned" : "released");

	return TRUE;
}

gboolean
fu_dell_k2_ec_run_passive_update(FuDevice *device, GError **error)
{
	g_autoptr(GByteArray) req = g_byte_array_new();
	g_return_val_if_fail(device != NULL, FALSE);

	/* ec included in cmd, set bit2 in data for tbt */
	fu_byte_array_append_uint8(req, DELL_K2_EC_HID_CMD_SET_PASSIVE);
	fu_byte_array_append_uint8(req, 1); // length of data
	fu_byte_array_append_uint8(req, 0x02);

	g_info("Registered passive update for dock");
	return fu_dell_k2_ec_write(device, req, error);
}

static gboolean
fu_dell_k2_ec_set_dock_sku(FuDevice *device, GError **error)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);

	switch (self->base_type) {
	case FU_DELL_K2_BASE_TYPE_K2:
		/* TBT type yet available, do workaround */
		if (fu_dell_k2_ec_dev_entry(device,
					    DELL_K2_EC_DEV_TYPE_TBT,
					    DELL_K2_EC_DEV_TBT_SUBTYPE_BR,
					    0) != NULL) {
			self->base_sku = K2_DOCK_SKU_TBT5;
			return TRUE;
		}
		if (fu_dell_k2_ec_dev_entry(device,
					    DELL_K2_EC_DEV_TYPE_TBT,
					    DELL_K2_EC_DEV_TBT_SUBTYPE_GR,
					    0) != NULL) {
			self->base_sku = K2_DOCK_SKU_TBT4;
			return TRUE;
		}
		self->base_sku = K2_DOCK_SKU_DPALT;
		return TRUE;
	default:
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "unsupported dock type: %x",
			    self->base_type);
		return FALSE;
	}
}

guint32
fu_dell_k2_ec_get_pd_version(FuDevice *device, guint8 sub_type, guint8 instance)
{
	struct FuDellK2EcQueryEntry *dev_entry = NULL;
	DellK2EcDevType dev_type = DELL_K2_EC_DEV_TYPE_PD;

	dev_entry = fu_dell_k2_ec_dev_entry(device, dev_type, sub_type, instance);
	return (dev_entry == NULL) ? 0 : GUINT32_FROM_BE(dev_entry->version.version_32);
}

guint32
fu_dell_k2_ec_get_ilan_version(FuDevice *device)
{
	struct FuDellK2EcQueryEntry *dev_entry = NULL;
	DellK2EcDevType dev_type = DELL_K2_EC_DEV_TYPE_LAN;

	dev_entry = fu_dell_k2_ec_dev_entry(device, dev_type, 0, 0);
	return (dev_entry == NULL) ? 0 : GUINT32_FROM_BE(dev_entry->version.version_32);
}

guint32
fu_dell_k2_ec_get_wtpd_version(FuDevice *device)
{
	struct FuDellK2EcQueryEntry *dev_entry = NULL;
	DellK2EcDevType dev_type = DELL_K2_EC_DEV_TYPE_WTPD;

	dev_entry = fu_dell_k2_ec_dev_entry(device, dev_type, 0, 0);
	return (dev_entry == NULL) ? 0 : GUINT32_FROM_BE(dev_entry->version.version_32);
}

guint32
fu_dell_k2_ec_get_dpmux_version(FuDevice *device)
{
	struct FuDellK2EcQueryEntry *dev_entry = NULL;
	DellK2EcDevType dev_type = DELL_K2_EC_DEV_TYPE_DP_MUX;

	dev_entry = fu_dell_k2_ec_dev_entry(device, dev_type, 0, 0);
	return (dev_entry == NULL) ? 0 : GUINT32_FROM_BE(dev_entry->version.version_32);
}

guint32
fu_dell_k2_ec_get_rmm_version(FuDevice *device)
{
	struct FuDellK2EcQueryEntry *dev_entry = NULL;
	DellK2EcDevType dev_type = DELL_K2_EC_DEV_TYPE_RMM;

	dev_entry = fu_dell_k2_ec_dev_entry(device, dev_type, 0, 0);
	return (dev_entry == NULL) ? 0 : GUINT32_FROM_BE(dev_entry->version.version_32);
}

static guint32
fu_dell_k2_ec_get_ec_version(FuDevice *device)
{
	struct FuDellK2EcQueryEntry *dev_entry = NULL;
	DellK2EcDevType dev_type = DELL_K2_EC_DEV_TYPE_MAIN_EC;

	dev_entry = fu_dell_k2_ec_dev_entry(device, dev_type, 0, 0);
	return (dev_entry == NULL) ? 0 : GUINT32_FROM_BE(dev_entry->version.version_32);
}

guint32
fu_dell_k2_ec_get_package_version(FuDevice *device)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(device);
	return GUINT32_FROM_BE(
	    fu_struct_dell_k2_dock_data_get_dock_firmware_pkg_ver(self->dock_data));
}

gboolean
fu_dell_k2_ec_commit_package(FuDevice *device, GBytes *blob_fw, GError **error)
{
	g_autoptr(GByteArray) req = g_byte_array_new();
	gsize length = g_bytes_get_size(blob_fw);

	g_return_val_if_fail(device != NULL, FALSE);
	g_return_val_if_fail(blob_fw != NULL, FALSE);

	/* verify package length */
	if (length != sizeof(FuDellK2DockFWVersion)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "Invalid package size %" G_GSIZE_FORMAT,
			    length);
		return FALSE;
	}

	fu_byte_array_append_uint8(req, DELL_K2_EC_HID_CMD_SET_DOCK_PKG);
	fu_byte_array_append_uint8(req, length); // length of data
	fu_byte_array_append_bytes(req, blob_fw);
	fu_dump_raw(G_LOG_DOMAIN, "->PACKAGE", req->data, req->len);

	if (!fu_dell_k2_ec_write(device, req, error)) {
		g_prefix_error(error, "Failed to commit package: ");
		return FALSE;
	}
	return TRUE;
}

static guint
fu_dell_k2_ec_get_chunk_delaytime(guint8 dev_type)
{
	switch (dev_type) {
	case DELL_K2_EC_DEV_TYPE_MAIN_EC:
		return 3 * 1000;
	case DELL_K2_EC_DEV_TYPE_RMM:
		return 60 * 1000;
	case DELL_K2_EC_DEV_TYPE_PD:
		return 15 * 1000;
	case DELL_K2_EC_DEV_TYPE_LAN:
		return 70 * 1000;
	default:
		return 30 * 1000;
	}
}

static gsize
fu_dell_k2_ec_get_chunk_size(guint8 dev_type)
{
	/* return the max chunk size in bytes */
	switch (dev_type) {
	case DELL_K2_EC_DEV_TYPE_MAIN_EC:
		return DELL_K2_EC_DEV_EC_CHUNK_SZ;
	case DELL_K2_EC_DEV_TYPE_RMM:
		return DELL_K2_EC_DEV_NO_CHUNK_SZ;
	default:
		return DELL_K2_EC_DEV_ANY_CHUNK_SZ;
	}
}

static guint
fu_dell_k2_ec_get_first_page_delaytime(guint8 dev_type)
{
	return (dev_type == DELL_K2_EC_DEV_TYPE_RMM) ? 75 * 1000 : 0;
}

gboolean
fu_dell_k2_ec_write_firmware_helper(FuDevice *device,
				    FuFirmware *firmware,
				    guint8 dev_type,
				    guint8 dev_identifier,
				    GError **error)
{
	gsize fw_sz = 0;
	gsize chunk_sz = fu_dell_k2_ec_get_chunk_size(dev_type);
	guint first_page_delay = fu_dell_k2_ec_get_first_page_delaytime(dev_type);
	guint chunk_delay = fu_dell_k2_ec_get_chunk_delaytime(dev_type);
	g_autoptr(GBytes) fw = NULL;
	g_autoptr(FuChunkArray) chunks = NULL;

	/* basic test */
	g_return_val_if_fail(device != NULL, FALSE);
	g_return_val_if_fail(FU_IS_FIRMWARE(firmware), FALSE);

	/* get default image */
	fw = fu_firmware_get_bytes(firmware, error);
	if (fw == NULL)
		return FALSE;

	/* payload size */
	fw_sz = g_bytes_get_size(fw);

	/* maximum buffer size */
	chunks = fu_chunk_array_new_from_bytes(fw,
					       FU_CHUNK_ADDR_OFFSET_NONE,
					       FU_CHUNK_PAGESZ_NONE,
					       chunk_sz);

	/* iterate the chunks */
	for (guint i = 0; i < fu_chunk_array_length(chunks); i++) {
		guint8 res[DELL_K2_EC_HID_DATA_PAGE_SZ] = {0xff};
		g_autoptr(FuChunk) chk = NULL;
		g_autoptr(FuChunkArray) pages = NULL;
		g_autoptr(GBytes) buf = NULL;

		chk = fu_chunk_array_index(chunks, i, error);
		if (chk == NULL)
			return FALSE;

		/* prepend header and command to the chunk data */
		buf = fu_dell_k2_ec_hid_fwup_pkg_new(chk, fw_sz, dev_type, dev_identifier);

		/* slice the chunk into pages */
		pages = fu_chunk_array_new_from_bytes(buf,
						      FU_CHUNK_ADDR_OFFSET_NONE,
						      FU_CHUNK_PAGESZ_NONE,
						      DELL_K2_EC_HID_DATA_PAGE_SZ);

		/* iterate the pages */
		for (guint j = 0; j < fu_chunk_array_length(pages); j++) {
			guint8 page_aligned[DELL_K2_EC_HID_DATA_PAGE_SZ] = {0xff};
			g_autoptr(FuChunk) page = NULL;

			page = fu_chunk_array_index(pages, j, error);
			if (page == NULL)
				return FALSE;

			/* strictly align the page size with 0x00 as packet */
			if (!fu_memcpy_safe(page_aligned,
					    sizeof(page_aligned),
					    0,
					    fu_chunk_get_data(page),
					    fu_chunk_get_data_sz(page),
					    0,
					    fu_chunk_get_data_sz(page),
					    error))
				return FALSE;

			/* send to ec */
			g_debug("sending chunk: %u, page: %u.", i, j);
			if (!fu_dell_k2_ec_hid_write(
				device,
				g_bytes_new(page_aligned, sizeof(page_aligned)),
				error))
				return FALSE;

			/* device needs time to process incoming pages */
			if (j == 0) {
				g_debug("wait %u ms before the next page", first_page_delay);

				fu_device_sleep(device, first_page_delay);
			}
		}

		/* delay time */
		g_debug("wait %u ms for dock to finish the chunk", chunk_delay);
		fu_device_sleep(device, chunk_delay);

		/* ensure the chunk has been acknowledged */
		if (!fu_hid_device_get_report(FU_HID_DEVICE(device),
					      0x0,
					      res,
					      sizeof(res),
					      DELL_K2_EC_HID_TIMEOUT,
					      FU_HID_DEVICE_FLAG_NONE,
					      error))
			return FALSE;

		switch (res[1]) {
		case DELL_K2_EC_RESP_TO_CHUNK_UPDATE_COMPLETE:
			g_debug("dock response '%u' to chunk[%u]: firmware updated successfully.",
				res[1],
				i);
			break;
		case DELL_K2_EC_RESP_TO_CHUNK_SEND_NEXT_CHUNK:
			g_debug("dock response '%u' to chunk[%u]: send next chunk.", res[1], i);
			break;
		case DELL_K2_EC_RESP_TO_CHUNK_UPDATE_FAILED:
		default:
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_WRITE,
				    "dock response '%u' to chunk[%u]: failed to write firmware.",
				    res[1],
				    i);
			return FALSE;
		}
	}

	/* success */
	g_debug("firmware written successfully");

	return TRUE;
}

static gboolean
fu_dell_k2_ec_write_firmware(FuDevice *device,
			     FuFirmware *firmware,
			     FuProgress *progress,
			     FwupdInstallFlags flags,
			     GError **error)
{
	return fu_dell_k2_ec_write_firmware_helper(device,
						   firmware,
						   DELL_K2_EC_DEV_TYPE_MAIN_EC,
						   0,
						   error);
}

static gboolean
fu_dell_k2_ec_query_cb(FuDevice *device, gpointer user_data, GError **error)
{
	/* dock data */
	if (!fu_dell_k2_ec_dock_data_cmd(device, error))
		return FALSE;

	/* dock info */
	if (!fu_dell_k2_ec_dock_info_cmd(device, error))
		return FALSE;

	/* set internal dock sku, must after dock info */
	if (!fu_dell_k2_ec_set_dock_sku(device, error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_dell_k2_ec_reload(FuDevice *device, GError **error)
{
	/* if query looks bad, wait a few seconds and retry */
	if (!fu_device_retry_full(device, fu_dell_k2_ec_query_cb, 10, 2000, NULL, error)) {
		g_prefix_error(error, "failed to query dock ec: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_dell_k2_ec_setup(FuDevice *device, GError **error)
{
	guint32 ec_version = 0;

	/* FuUsbDevice->setup */
	if (!FU_DEVICE_CLASS(fu_dell_k2_ec_parent_class)->setup(device, error))
		return FALSE;

	/* get dock type */
	if (!fu_dell_k2_ec_dock_type_cmd(device, error))
		return FALSE;

	/* if query looks bad, wait a few seconds and retry */
	if (!fu_device_retry_full(device, fu_dell_k2_ec_query_cb, 10, 2000, NULL, error)) {
		g_prefix_error(error, "failed to query dock ec: ");
		return FALSE;
	}

	/* setup version */
	ec_version = fu_dell_k2_ec_get_ec_version(device);
	fu_device_set_version_raw(device, ec_version);

	/* create the subcomponents */
	if (!fu_dell_k2_ec_probe_subcomponents(device, error))
		return FALSE;

	g_debug("dell-k2-ec->setup done successfully");
	return TRUE;
}

static gchar *
fu_dell_k2_ec_convert_version(FuDevice *device, guint64 version_raw)
{
	return fu_version_from_uint32_hex(version_raw, fu_device_get_version_format(device));
}

static gboolean
fu_dell_k2_ec_open(FuDevice *device, GError **error)
{
	/* FuUdevDevice->open */
	return FU_DEVICE_CLASS(fu_dell_k2_ec_parent_class)->open(device, error);
}

static void
fu_dell_k2_ec_finalize(GObject *object)
{
	FuDellK2Ec *self = FU_DELL_K2_EC(object);
	g_free(self->dock_data);
	g_free(self->dock_info);
	g_free(self->raw_versions);
	G_OBJECT_CLASS(fu_dell_k2_ec_parent_class)->finalize(object);
}

static void
fu_dell_k2_ec_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 100, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 0, "reload");
}

static void
fu_dell_k2_ec_init(FuDellK2Ec *self)
{
	self->dock_data = g_new0(FuStructDellK2DockData, 1);
	self->raw_versions = g_new0(FuDellK2DockFWVersion, 1);
	self->dock_info = g_new0(FuDellK2DockInfoStructure, 1);

	fu_device_add_protocol(FU_DEVICE(self), "com.dell.k2");
	fu_device_add_vendor_id(FU_DEVICE(self), "USB:0x413C");
	fu_device_add_icon(FU_DEVICE(self), "dock-usb");
	fu_device_set_summary(FU_DEVICE(self), "Dell Dock");
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_USABLE_DURING_UPDATE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_DUAL_IMAGE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_SELF_RECOVERY);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INSTALL_SKIP_VERSION_CHECK);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_SKIPS_RESTART);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_NO_AUTO_REMOVE_CHILDREN);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_EXPLICIT_ORDER);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_RETRY_OPEN);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_MD_SET_FLAGS);
	fu_device_register_private_flag(FU_DEVICE(self), FWUPD_DELL_K2_DEVICE_PRIVATE_FLAG_UOD_OFF);
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_QUAD);
}

static void
fu_dell_k2_ec_class_init(FuDellK2EcClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	object_class->finalize = fu_dell_k2_ec_finalize;
	device_class->open = fu_dell_k2_ec_open;
	device_class->setup = fu_dell_k2_ec_setup;
	device_class->write_firmware = fu_dell_k2_ec_write_firmware;
	device_class->reload = fu_dell_k2_ec_reload;
	device_class->set_progress = fu_dell_k2_ec_set_progress;
	device_class->convert_version = fu_dell_k2_ec_convert_version;
}

FuDellK2Ec *
fu_dell_k2_ec_new(FuDevice *device)
{
	FuDellK2Ec *self = NULL;
	FuContext *ctx = fu_device_get_context(device);

	self = g_object_new(FU_TYPE_DELL_K2_EC, "context", ctx, NULL);
	fu_device_incorporate(FU_DEVICE(self), device, FU_DEVICE_INCORPORATE_FLAG_ALL);
	fu_device_set_logical_id(FU_DEVICE(self), "ec");
	return self;
}
