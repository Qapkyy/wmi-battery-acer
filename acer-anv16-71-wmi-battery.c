// SPDX-License-Identifier: GPL-2.0-only
/*
 * Acer Nitro V16 Battery Health Control Driver
 *
 *
 * Author: Qapky <qapkyy3@gmail.com>
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/version.h>
#include <linux/wmi.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/slab.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
#include <asm/unaligned.h>
#else
#include <linux/unaligned.h>
#endif


/*
 * Acer GUID,
 *
 */

#define AMW0_GUID1 "67C3371D-95A3-4C37-BB61-DD47B491DAAB"
#define AMW0_GUID2 "431F16ED-0C2B-444C-B267-27DEB140CF9C"
#define WMI_GUID1 "6AF4F258-B401-42FD-BE91-3D4AC2D7C0D3"
#define WMI_GUID2 "95764E09-FB56-4E83-B31A-37761F60994A"
#define WMI_GUID3 "61EF69EA-865C-4BC3-A502-A0DEBA0CB531"
#define WMI_GUID4 "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"
#define WMI_GUID5 "79772EC5-04B1-4BFD-843C-61E7F77B6CC9"

MODULE_DESCRIPTION("Acer Nitro V16 battery health control driver");
MODULE_AUTHOR("Qapky <qapkyy3@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("wmi:" WMI_GUID);

/* Battery index used in all WMI calls */
#define ACER_BATTERY_INDEX	0x1

/* WMI method IDs */
#define WMI_METHOD_GET_BATTERY_INFO	19 /* 0x13 -> BEBI */
#define WMI_METHOD_GET_HEALTH_STATUS	20 /* 0x14 -> BEGB */
#define WMI_METHOD_SET_HEALTH_CONTROL	21 /* 0x15 -> BESB */
#define WMI_HEALTH_STATUS_BUF_LEN	8
#define WMI_SET_CONTROL_BUF_LEN		4
#define WMI_BATTERY_INFO_BUF_LEN	sizeof(u32)

/* Sub-index used with WMI_METHOD_GET_BATTERY_INFO to read temperature */
#define BATTERY_INFO_TEMPERATURE	0x8

#define TEMP_OFFSET_DK		2731	/* 0°C in decikelvin */
#define TEMP_DK_TO_MILLI_C(dk)	(((dk) - TEMP_OFFSET_DK) * 100)

struct get_battery_health_control_status_input {
	u8 battery_no;
	u8 function_query;
	u8 reserved[2];
} __packed;

struct get_battery_health_control_status_output {
	u8 function_list;
	u8 ret[2];
	u8 function_status[5];
} __packed;

struct set_battery_health_control_input {
	u8 battery_no;
	u8 function_mask;
	u8 function_status;
	u8 reserved_in[5];
} __packed;

struct set_battery_health_control_output {
	u8 ret;
	u8 reserved_out;
} __packed;

enum battery_mode {
	HEALTH_MODE      = BIT(0),	/* 0x1 */
	CALIBRATION_MODE = BIT(1),	/* 0x2 */
};

struct battery_info {
	s8 health_mode;		/* -1: unsupported, 0: off, 1: on */
	s8 calibration_mode;	/* -1: unsupported, 0: off, 1: on */
};

/* Per-device driver state, attached to the wmi_device via drvdata */
struct acer_battery_data {
	struct wmi_device *wdev;
	struct battery_info status;
	struct mutex lock;
};

static short enable_health_mode = -1;
module_param(enable_health_mode, short, 0444);
MODULE_PARM_DESC(enable_health_mode,
	"Set battery health mode at probe time: >0 enables, 0 disables, <0 (default) keeps current setting");

/* ------------------------------------------------------------------ */
/* WMI helpers                                                          */
/* ------------------------------------------------------------------ */

static acpi_status get_battery_information(struct wmi_device *wdev, u32 index,
					    u32 battery, u32 *result)
{
	u32 args[2] = { index, battery };
	struct acpi_buffer input  = { sizeof(args), args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = wmidev_evaluate_method(wdev, 0, WMI_METHOD_GET_BATTERY_INFO,
					&input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj)
		return AE_ERROR;

	if (obj->type != ACPI_TYPE_BUFFER) {
		dev_err(&wdev->dev, "Unexpected WMI object type %u (expected BUFFER)\n",
			obj->type);
		kfree(obj);
		return AE_ERROR;
	}

	if (obj->buffer.length < WMI_BATTERY_INFO_BUF_LEN) {
		dev_err(&wdev->dev, "WMI battery info buffer too short: %u bytes\n",
			obj->buffer.length);
		kfree(obj);
		return AE_ERROR;
	}

	*result = get_unaligned_le32(obj->buffer.pointer);
	kfree(obj);

	return AE_OK;
}

static acpi_status
get_battery_health_control_status(struct wmi_device *wdev,
				  struct battery_info *bat_status)
{
	struct get_battery_health_control_status_input params = {
		.battery_no     = ACER_BATTERY_INDEX,
		.function_query = 0x1,
		.reserved       = { 0x0, 0x0 },
	};
	struct get_battery_health_control_status_output ret;
	struct acpi_buffer input  = { sizeof(params), &params };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = wmidev_evaluate_method(wdev, 0, WMI_METHOD_GET_HEALTH_STATUS,
					&input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj)
		return AE_ERROR;

	if (obj->type != ACPI_TYPE_BUFFER) {
		dev_err(&wdev->dev, "Unexpected WMI object type %u (expected BUFFER)\n",
			obj->type);
		kfree(obj);
		return AE_ERROR;
	}

	if (obj->buffer.length < WMI_HEALTH_STATUS_BUF_LEN) {
		dev_err(&wdev->dev, "WMI health status buffer too short: %u (need %u)\n",
			obj->buffer.length, WMI_HEALTH_STATUS_BUF_LEN);
		kfree(obj);
		return AE_ERROR;
	}

	memcpy(&ret, obj->buffer.pointer, sizeof(ret));
	kfree(obj);

	bat_status->health_mode =
		(ret.function_list & HEALTH_MODE) ?
		(ret.function_status[0] > 0 ? 1 : 0) : -1;

	bat_status->calibration_mode =
		(ret.function_list & CALIBRATION_MODE) ?
		(ret.function_status[1] > 0 ? 1 : 0) : -1;

	return AE_OK;
}

static acpi_status set_battery_health_control(struct wmi_device *wdev,
					      u8 function, bool function_status)
{
	struct set_battery_health_control_input params = {
		.battery_no      = ACER_BATTERY_INDEX,
		.function_mask   = function,
		.function_status = (u8)function_status,
		.reserved_in     = { 0x0, 0x0, 0x0, 0x0, 0x0 },
	};
	struct set_battery_health_control_output ret;
	struct acpi_buffer input  = { sizeof(params), &params };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;

	status = wmidev_evaluate_method(wdev, 0, WMI_METHOD_SET_HEALTH_CONTROL,
					&input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj)
		return AE_ERROR;

	if (obj->type != ACPI_TYPE_BUFFER) {
		dev_err(&wdev->dev, "Unexpected WMI object type %u (expected BUFFER)\n",
			obj->type);
		kfree(obj);
		return AE_ERROR;
	}

	if (obj->buffer.length < WMI_SET_CONTROL_BUF_LEN) {
		dev_err(&wdev->dev, "WMI set control buffer too short: %u (need %u)\n",
			obj->buffer.length, WMI_SET_CONTROL_BUF_LEN);
		kfree(obj);
		return AE_ERROR;
	}

	memcpy(&ret, obj->buffer.pointer, sizeof(ret));
	kfree(obj);

	if (ret.ret != 0) {
		dev_err(&wdev->dev, "Firmware rejected set_battery_health_control: error 0x%02x\n",
			ret.ret);
		return AE_ERROR;
	}

	return AE_OK;
}

/* ------------------------------------------------------------------ */
/* State management                                                     */
/* ------------------------------------------------------------------ */

static void print_modes(struct device *dev, const char *prefix,
			bool print_if_empty, bool health_mode, bool calib_mode)
{
	if (!health_mode && !calib_mode && !print_if_empty)
		return;

	dev_info(dev, "%s modes:%s%s%s\n",
		prefix,
		health_mode               ? " health"      : "",
		health_mode && calib_mode ? ","            : "",
		calib_mode                ? " calibration" : "");
}

static void update_state(struct acer_battery_data *data)
{
	struct battery_info old_state;
	struct battery_info new_state;
	acpi_status status;

	mutex_lock(&data->lock);
	old_state = data->status;
	mutex_unlock(&data->lock);

	status = get_battery_health_control_status(data->wdev, &new_state);
	if (ACPI_FAILURE(status)) {
		dev_err(&data->wdev->dev, "Failed to refresh battery status\n");
		return;
	}

	mutex_lock(&data->lock);
	data->status = new_state;
	mutex_unlock(&data->lock);

	if (new_state.calibration_mode != old_state.calibration_mode)
		dev_info(&data->wdev->dev, "%s calibration mode\n",
			new_state.calibration_mode > 0 ? "enabled" : "disabled");

	if (new_state.health_mode != old_state.health_mode)
		dev_info(&data->wdev->dev, "%s health mode\n",
			new_state.health_mode > 0 ? "enabled" : "disabled");
}

/* ------------------------------------------------------------------ */
/* sysfs attributes                                                   */
/* ------------------------------------------------------------------ */

static ssize_t temperature_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct acer_battery_data *data = dev_get_drvdata(dev);
	acpi_status status;
	u32 value;

	status = get_battery_information(data->wdev, BATTERY_INFO_TEMPERATURE,
					  ACER_BATTERY_INDEX, &value);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (value > U16_MAX)
		return -ENXIO;

	return sysfs_emit(buf, "%d\n", TEMP_DK_TO_MILLI_C(value));
}

static ssize_t health_mode_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct acer_battery_data *data = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&data->lock);
	ret = sysfs_emit(buf, "%d\n", data->status.health_mode);
	mutex_unlock(&data->lock);

	return ret;
}

static ssize_t health_mode_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct acer_battery_data *data = dev_get_drvdata(dev);
	acpi_status status;
	bool param_val;
	int err;

	mutex_lock(&data->lock);
	if (data->status.health_mode < 0) {
		mutex_unlock(&data->lock);
		dev_warn(dev, "health mode not supported on this battery\n");
		return -EOPNOTSUPP;
	}
	mutex_unlock(&data->lock);

	err = kstrtobool(buf, &param_val);
	if (err)
		return err;

	status = set_battery_health_control(data->wdev, HEALTH_MODE, param_val);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to set health mode\n");
		return -EIO;
	}

	update_state(data);

	return count;
}

static ssize_t calibration_mode_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct acer_battery_data *data = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&data->lock);
	ret = sysfs_emit(buf, "%d\n", data->status.calibration_mode);
	mutex_unlock(&data->lock);

	return ret;
}

static ssize_t calibration_mode_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct acer_battery_data *data = dev_get_drvdata(dev);
	acpi_status status;
	bool param_val;
	int err;

	mutex_lock(&data->lock);
	if (data->status.calibration_mode < 0) {
		mutex_unlock(&data->lock);
		dev_warn(dev, "calibration mode not supported on this battery\n");
		return -EOPNOTSUPP;
	}
	mutex_unlock(&data->lock);

	err = kstrtobool(buf, &param_val);
	if (err)
		return err;

	status = set_battery_health_control(data->wdev, CALIBRATION_MODE, param_val);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to set calibration mode\n");
		return -EIO;
	}

	update_state(data);

	return count;
}

static DEVICE_ATTR_RO(temperature);
static DEVICE_ATTR_RW(health_mode);
static DEVICE_ATTR_RW(calibration_mode);

static struct attribute *acer_battery_attrs[] = {
	&dev_attr_temperature.attr,
	&dev_attr_health_mode.attr,
	&dev_attr_calibration_mode.attr,
	NULL,
};

static const struct attribute_group acer_battery_group = {
	.attrs = acer_battery_attrs,
};

/* ------------------------------------------------------------------ */
/* WMI driver registration                                              */
/* ------------------------------------------------------------------ */

static int acer_battery_probe(struct wmi_device *wdev, const void *context)
{
	struct acer_battery_data *data;
	acpi_status status;
	int ret;

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->wdev = wdev;
	mutex_init(&data->lock);
	dev_set_drvdata(&wdev->dev, data);

	status = get_battery_health_control_status(wdev, &data->status);
	if (ACPI_FAILURE(status)) {
		dev_err(&wdev->dev, "Failed to query initial battery status\n");
		return -EIO;
	}

	print_modes(&wdev->dev, "available", true,
		    data->status.health_mode >= 0,
		    data->status.calibration_mode >= 0);
	print_modes(&wdev->dev, "active", false,
		    data->status.health_mode > 0,
		    data->status.calibration_mode > 0);

	if (enable_health_mode >= 0) {
		if (data->status.health_mode < 0) {
			dev_warn(&wdev->dev,
				 "health mode not supported, ignoring enable_health_mode parameter\n");
		} else {
			status = set_battery_health_control(wdev, HEALTH_MODE,
							    enable_health_mode > 0);
			if (ACPI_FAILURE(status))
				dev_warn(&wdev->dev,
					 "Failed to apply enable_health_mode=%d at probe\n",
					 enable_health_mode);
			else
				update_state(data);
		}
	}

	ret = devm_device_add_group(&wdev->dev, &acer_battery_group);
	if (ret) {
		dev_err(&wdev->dev, "Failed to create sysfs attributes\n");
		return ret;
	}

	return 0;
}

static void acer_battery_remove(struct wmi_device *wdev)
{
	struct acer_battery_data *data = dev_get_drvdata(&wdev->dev);

	mutex_destroy(&data->lock);
}

static const struct wmi_device_id acer_battery_id_table[] = {
	{ .guid_string = WMI_GUID },
	{ },
};
MODULE_DEVICE_TABLE(wmi, acer_battery_id_table);

static struct wmi_driver acer_battery_driver = {
	.driver = {
		.name = "wmi-battery-acer",
	},
	.id_table = acer_battery_id_table,
	.probe    = acer_battery_probe,
	.remove   = acer_battery_remove,
};
module_wmi_driver(acer_battery_driver);
