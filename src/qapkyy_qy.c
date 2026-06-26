// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Acer WMI Laptop Extras - Qapkyy Fork
 *
 *  Original Acer WMI Driver:
 *    Copyright (C) 2007-2009    Carlos Corbacho <carlos@strangeworlds.co.uk>
 *
 *  Qapkyy Fork:
 *    Adapted for Acer Nitro V16 ANV16-71 by Qapky
 *    Based on acer_wmi.c
 *
 *  Features:
 *    - Battery charge limit (80% health mode)
 *    - Fan RPM reading via hwmon (fan1/fan2)
 *    - Fan speed control (CPU + GPU)
 *    - Thermal profile / platform profile
 *    - WMI hotkey input
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <acpi/video.h>
#include <linux/acpi.h>
#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/i8042.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/rfkill.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/units.h>
#include <linux/workqueue.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

MODULE_AUTHOR("Carlos Corbacho, modified by Qapky");
MODULE_DESCRIPTION("Acer WMI driver compatible with Acer Nitro V16 ANV16-71");
MODULE_LICENSE("GPL");

#define DRIVER_VERSION "1.0.0"

/*
 * Method IDs for WMID interface
 */
#define ACER_WMID_GET_WIRELESS_METHODID                    1
#define ACER_WMID_SET_WIRELESS_METHODID                    4
#define ACER_WMID_GET_BLUETOOTH_METHODID                   2
#define ACER_WMID_SET_BLUETOOTH_METHODID                   5
#define ACER_WMID_GET_BRIGHTNESS_METHODID                  3
#define ACER_WMID_SET_BRIGHTNESS_METHODID                  6
#define ACER_WMID_GET_THREEG_METHODID                      10
#define ACER_WMID_SET_THREEG_METHODID                      11
#define ACER_WMID_SET_FUNCTION                             1
#define ACER_WMID_GET_FUNCTION                             2

/* Gaming-specific method IDs */
#define ACER_WMID_GET_GAMING_PROFILE_METHODID              3
#define ACER_WMID_SET_GAMING_PROFILE_METHODID              1
#define ACER_WMID_GET_GAMING_SYS_INFO_METHODID             5
#define ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID         14
#define ACER_WMID_SET_GAMING_FAN_SPEED_METHODID            16
#define ACER_WMID_SET_GAMING_MISC_SETTING_METHODID         22
#define ACER_WMID_GET_GAMING_MISC_SETTING_METHODID         23
#define ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID 20
#define ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID      21


#define ACER_NITRO_V5_FAN_SPEED_READ_BIT_MASK          GENMASK(20, 8)
#define ACER_GAMING_MISC_SETTING_STATUS_MASK           GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_INDEX_MASK            GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_VALUE_MASK            GENMASK_ULL(15, 8)
#define ACER_NITRO_V5_RETURN_STATUS_BIT_MASK            GENMASK_ULL(7, 0)
#define ACER_NITRO_V5_SENSOR_INDEX_BIT_MASK             GENMASK_ULL(15, 8)
#define ACER_NITRO_V5_SENSOR_READING_BIT_MASK           GENMASK_ULL(23, 8)
#define ACER_NITRO_V5_SUPPORTED_SENSORS_BIT_MASK        GENMASK_ULL(39, 24)

/*
 * Acer ACPI Method GUIDs
 */
#define WMID_GUID1   "6AF4F258-B401-42FD-BE91-3D4AC2D7C0D3"
#define WMID_GUID2   "95764E09-FB56-4E83-B31A-37761F60994A"
#define WMID_GUID3   "61EF69EA-865C-4BC3-A502-A0DEBA0CB531"
#define WMID_GUID4   "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"
#define WMID_GUID5   "79772EC5-04B1-4BFD-843C-61E7F77B6CC9"

/* Acer ACPI event GUID */
#define ACERWMID_EVENT_GUID  "61EF69EA-865C-4BC3-A502-A0DEBA0CB531"

MODULE_ALIAS("wmi:" WMID_GUID3);
MODULE_ALIAS("wmi:" WMID_GUID5);

enum acer_wmi_event_ids {
	WMID_HOTKEY_EVENT           = 0x1,
	WMID_ACCEL_OR_KBD_DOCK_EVENT = 0x5,
	WMID_GAMING_TURBO_KEY_EVENT = 0x7,
	WMID_AC_EVENT               = 0x8,
	WMID_BATTERY_BOOST_EVENT    = 0x9,
	WMID_CALIBRATION_EVENT      = 0x0B,
};

enum battery_mode {
	HEALTH_MODE      = 1,
	CALIBRATION_MODE = 2,
};

enum acer_wmi_nitro_v5_sys_info_command {
	ACER_WMID_CMD_GET_NITRO_V5_SUPPORTED_SENSORS = 0x0000,
	ACER_WMID_CMD_GET_NITRO_V5_BAT_STATUS        = 0x02,
	ACER_WMID_CMD_GET_NITRO_V5_SENSOR_READING    = 0x0001,
	ACER_WMID_CMD_GET_NITRO_V5_CPU_FAN_SPEED     = 0x0201,
	ACER_WMID_CMD_GET_NITRO_V5_GPU_FAN_SPEED     = 0x0601,
};

enum acer_wmi_nitro_v5_sensor_id {
	ACER_WMID_SENSOR_CPU_TEMPERATURE       = 0x01,
	ACER_WMID_SENSOR_CPU_FAN_SPEED         = 0x02,
	ACER_WMID_SENSOR_EXTERNAL_TEMPERATURE_2 = 0x03,
	ACER_WMID_SENSOR_GPU_FAN_SPEED         = 0x06,
	ACER_WMID_SENSOR_GPU_TEMPERATURE       = 0x0A,
};

enum acer_wmi_nitro_oc {
	ACER_WMID_OC_NORMAL = 0x0000,
	ACER_WMID_OC_TURBO  = 0x0002,
};

enum acer_wmi_gaming_misc_setting {
	ACER_WMID_MISC_SETTING_OC_1              = 0x0005,
	ACER_WMID_MISC_SETTING_OC_2              = 0x0007,
	ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES = 0x000A,
	ACER_WMID_MISC_SETTING_PLATFORM_PROFILE  = 0x000B,
};

enum acer_nitro_v5_thermal_profile {
	ACER_NITRO_V5_THERMAL_PROFILE_QUIET       = 0x00,
	ACER_NITRO_V5_THERMAL_PROFILE_BALANCED    = 0x01,
	ACER_NITRO_V5_THERMAL_PROFILE_PERFORMANCE = 0x04,
	ACER_NITRO_V5_THERMAL_PROFILE_TURBO       = 0x05,
	ACER_NITRO_V5_THERMAL_PROFILE_ECO         = 0x06,
};

// Interface Flags
enum interface_flags {
	ACER_WMID,
	ACER_WMID_V2,
};

#define ACER_CAP_MAILLED            BIT(0)
#define ACER_CAP_WIRELESS           BIT(1)
#define ACER_CAP_BLUETOOTH          BIT(2)
#define ACER_CAP_BRIGHTNESS         BIT(3)
#define ACER_CAP_THREEG             BIT(4)
#define ACER_CAP_SET_FUNCTION_MODE  BIT(5)
#define ACER_CAP_KBD_DOCK           BIT(6)
#define ACER_CAP_TURBO_OC           BIT(7)
#define ACER_CAP_TURBO_LED          BIT(8)
#define ACER_CAP_TURBO_FAN          BIT(9)
#define ACER_CAP_PLATFORM_PROFILE   BIT(10)
#define ACER_CAP_FAN_SPEED_READ     BIT(11)
#define ACER_CAP_NITRO_SENSE     BIT(12)
#define ACER_CAP_NITRO_SENSE        BIT(13)
#define ACER_CAP_NITRO_SENSE_V5     BIT(14)

static const struct key_entry acer_wmi_keymap[] __initconst = {
	{KE_KEY, 0x01, {KEY_WLAN}},
	{KE_KEY, 0x03, {KEY_WLAN}},
	{KE_KEY, 0x04, {KEY_WLAN}},
	{KE_KEY, 0x12, {KEY_BLUETOOTH}},
	{KE_KEY, 0x21, {KEY_PROG1}},
	{KE_KEY, 0x22, {KEY_PROG2}},
	{KE_KEY, 0x23, {KEY_PROG3}},
	{KE_KEY, 0x24, {KEY_PROG4}},
	{KE_KEY, 0x27, {KEY_HELP}},
	{KE_KEY, 0x29, {KEY_PROG3}},
	{KE_IGNORE, 0x41, {KEY_MUTE}},
	{KE_IGNORE, 0x42, {KEY_PREVIOUSSONG}},
	{KE_IGNORE, 0x4d, {KEY_PREVIOUSSONG}},
	{KE_IGNORE, 0x43, {KEY_NEXTSONG}},
	{KE_IGNORE, 0x4e, {KEY_NEXTSONG}},
	{KE_IGNORE, 0x44, {KEY_PLAYPAUSE}},
	{KE_IGNORE, 0x4f, {KEY_PLAYPAUSE}},
	{KE_IGNORE, 0x45, {KEY_STOP}},
	{KE_IGNORE, 0x50, {KEY_STOP}},
	{KE_IGNORE, 0x48, {KEY_VOLUMEUP}},
	{KE_IGNORE, 0x49, {KEY_VOLUMEDOWN}},
	{KE_IGNORE, 0x4a, {KEY_VOLUMEDOWN}},
	{KE_KEY, 0x61, {KEY_UNKNOWN}},
	{KE_IGNORE, 0x62, {KEY_BRIGHTNESSUP}},
	{KE_IGNORE, 0x63, {KEY_BRIGHTNESSDOWN}},
	{KE_KEY, 0x64, {KEY_SWITCHVIDEOMODE}},
	{KE_IGNORE, 0x81, {KEY_SLEEP}},
	{KE_KEY, 0x82, {KEY_TOUCHPAD_TOGGLE}},
	{KE_IGNORE, 0x84, {KEY_KBDILLUMTOGGLE}},
	{KE_KEY, KEY_TOUCHPAD_ON, {KEY_TOUCHPAD_ON}},
	{KE_KEY, KEY_TOUCHPAD_OFF, {KEY_TOUCHPAD_OFF}},
	{KE_IGNORE, 0x83, {KEY_TOUCHPAD_TOGGLE}},
	{KE_KEY, 0x85, {KEY_TOUCHPAD_TOGGLE}},
	{KE_KEY, 0x86, {KEY_WLAN}},
	{KE_KEY, 0x87, {KEY_POWER}},
	{KE_END, 0}
};

#define ACER_WMID1_GDS_WIRELESS   (1 << 0)
#define ACER_WMID1_GDS_BLUETOOTH  (1 << 11)
#define ACER_WMID1_GDS_TOUCHPAD   (1 << 1)
#define ACER_WMID3_GDS_WIRELESS   (1 << 0)
#define ACER_WMID3_GDS_THREEG     (1 << 6)
#define ACER_WMID3_GDS_WIMAX      (1 << 7)
#define ACER_WMID3_GDS_BLUETOOTH  (1 << 11)
#define ACER_WMID3_GDS_RFBTN      (1 << 14)
#define ACER_WMID3_GDS_TOUCHPAD   (1 << 1)

struct event_return_value {
	u8  function;
	u8  key_num;
	u16 device_state;
	u16 reserved1;
	u8  kbd_dock_state;
	u8  reserved2;
} __packed;

struct func_input_params {
	u8  function_num;
	u16 commun_devices;
	u16 devices;
	u8  app_status;
	u8  app_mask;
	u8  reserved;
} __packed;

struct func_return_value {
	u8  error_code;
	u8  ec_return_value;
	u16 reserved;
} __packed;

struct wmid3_gds_set_input_param {
	u8  function_num;
	u8  hotkey_number;
	u16 devices;
	u8  volume_value;
} __packed;

struct wmid3_gds_get_input_param {
	u8  function_num;
	u8  hotkey_number;
	u16 devices;
} __packed;

struct wmid3_gds_return_value {
	u8  error_code;
	u8  ec_return_value;
	u16 devices;
	u32 reserved;
} __packed;

struct hotkey_function_type_aa {
	u8  type;
	u8  length;
	u16 handle;
	u16 commun_func_bitmap;
	u16 application_func_bitmap;
	u16 media_func_bitmap;
	u16 display_func_bitmap;
	u16 others_func_bitmap;
	u8  commun_fn_key_number;
} __packed;

struct get_battery_health_control_status_input {
	u8 uBatteryNo;
	u8 uFunctionQuery;
	u8 uReserved[2];
} __packed;

struct get_battery_health_control_status_output {
	u8 uFunctionList;
	u8 uReturn[2];
	u8 uFunctionStatus[5];
} __packed;

struct set_battery_health_control_input {
	u8 uBatteryNo;
	u8 uFunctionMask;
	u8 uFunctionStatus;
	u8 uReservedIn[5];
} __packed;

struct set_battery_health_control_output {
	u8 uReturn;
	u8 uReservedOut;
} __packed;

struct quirk_entry {
	u8 wireless;
	u8 mailled;
	s8 brightness;
	u8 bluetooth;
	u8 turbo;
	u8 cpu_fans;
	u8 gpu_fans;
	u8 nitro_v5;
	u8 nitro_sense;
};

struct acer_data {
	int mailled;
	int threeg;
	int brightness;
};

struct acer_debug {
	struct dentry *root;
	u32 wmid_devices;
};

struct wmi_interface {
	u32 type;
	u32 capability;
	struct acer_data data;
	struct acer_debug debug;
};

/*
 * Static globals
 */
static struct wmi_interface *interface;
static struct quirk_entry *quirks;
static struct input_dev *acer_wmi_input_dev;
static struct input_dev *acer_wmi_accel_dev;
static struct platform_device *acer_platform_device;
static struct device *platform_profile_device;
static bool platform_profile_support;
static bool rfkill_inited;
static struct rfkill *wireless_rfkill;
static struct rfkill *bluetooth_rfkill;
static struct rfkill *threeg_rfkill;
static acpi_handle gsensor_handle;
static bool has_type_aa;
static u16 commun_func_bitmap;
static u8  commun_fn_key_number;
static int max_brightness = 0xF;
static int mailled  = -1;
static int brightness = -1;
static int threeg   = -1;
static int force_series;
static int force_caps = -1;
static bool ec_raw_mode;
static bool cycle_gaming_thermal_profile = true;
static bool nitro_v5 = true;
static u64 supported_sensors;
static int acer_nitro_v5_max_perf;
static int last_non_turbo_profile = INT_MIN;

/* Fan speed state (percentage 0-100) */
static int cpu_fan_speed;
static int gpu_fan_speed;

module_param(mailled,    int,  0444);
module_param(brightness, int,  0444);
module_param(threeg,     int,  0444);
module_param(force_series, int, 0444);
module_param(force_caps, int,  0444);
module_param(ec_raw_mode, bool, 0444);
module_param(cycle_gaming_thermal_profile, bool, 0644);
module_param(nitro_v5,  bool, 0444);
MODULE_PARM_DESC(nitro_v5, "Enable features for nitro laptops that use nitro sense v5 (default=true for ANV16-71)");
MODULE_PARM_DESC(cycle_gaming_thermal_profile, "Thermal mode key cycles profiles. Disable to use turbo toggle mode");

static struct quirk_entry quirk_unknown = {};

static struct quirk_entry quirk_acer_nitro_v16_anv16_71 = {
	.nitro_v5  = 1,
	.cpu_fans  = 1,
	.gpu_fans  = 1,
};

static struct quirk_entry quirk_acer_nitro_v5 = {
	.nitro_v5  = 1,
	.cpu_fans  = 1,
	.gpu_fans  = 1,
};

static const struct dmi_system_id wmi_whitelist[] __initconst = {
	{
		.ident = "Acer",
		.matches = { DMI_MATCH(DMI_SYS_VENDOR, "Acer") },
	},
	{
		.ident = "Gateway",
		.matches = { DMI_MATCH(DMI_SYS_VENDOR, "Gateway") },
	},
	{
		.ident = "Packard Bell",
		.matches = { DMI_MATCH(DMI_SYS_VENDOR, "Packard Bell") },
	},
	{}
};

static int __init dmi_matched(const struct dmi_system_id *dmi)
{
	quirks = dmi->driver_data;
	return 1;
}

static const struct dmi_system_id acer_quirks[] __initconst = {
	{
		.callback    = dmi_matched,
		.ident       = "Acer Nitro V16 ANV16-71",
		.matches     = {
			DMI_MATCH(DMI_SYS_VENDOR,   "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro ANV16-71"),
		},
		.driver_data = &quirk_acer_nitro_v16_anv16_71,
	},
};


static bool has_cap(u32 cap)
{
	return interface->capability & cap;
}

/*
 * WMID (v1) interface
 */
static acpi_status WMI_execute_u32(u32 method_id, u32 in, u32 *out)
{
	struct acpi_buffer input  = {(acpi_size)sizeof(u32), (void *)(&in)};
	struct acpi_buffer result = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	u32 tmp = 0;
	acpi_status status;

	status = wmi_evaluate_method(WMID_GUID1, 0, method_id, &input, &result);
	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *)result.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER &&
		    (obj->buffer.length == sizeof(u32) ||
		     obj->buffer.length == sizeof(u64))) {
			tmp = *((u32 *)obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			tmp = (u32)obj->integer.value;
		}
	}
	if (out)
		*out = tmp;
	kfree(result.pointer);
	return status;
}

static acpi_status WMID_get_u32(u32 *value, u32 cap)
{
	acpi_status status;
	u32 result, method_id = 0;
	u8  tmp;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		method_id = ACER_WMID_GET_WIRELESS_METHODID;
		break;
	case ACER_CAP_BLUETOOTH:
		method_id = ACER_WMID_GET_BLUETOOTH_METHODID;
		break;
	case ACER_CAP_BRIGHTNESS:
		method_id = ACER_WMID_GET_BRIGHTNESS_METHODID;
		break;
	case ACER_CAP_THREEG:
		method_id = ACER_WMID_GET_THREEG_METHODID;
		break;
	case ACER_CAP_MAILLED:
		if (quirks->mailled == 1) {
			ec_read(0x9f, &tmp);
			*value = tmp & 0x1;
			return 0;
		}
		fallthrough;
	default:
		return AE_ERROR;
	}
	status = WMI_execute_u32(method_id, 0, &result);
	if (ACPI_SUCCESS(status))
		*value = (u8)result;
	return status;
}

static acpi_status WMID_set_u32(u32 value, u32 cap)
{
	u32 method_id = 0;
	char param;

	switch (cap) {
	case ACER_CAP_BRIGHTNESS:
		if (value > max_brightness)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_BRIGHTNESS_METHODID;
		break;
	case ACER_CAP_WIRELESS:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_WIRELESS_METHODID;
		break;
	case ACER_CAP_BLUETOOTH:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_BLUETOOTH_METHODID;
		break;
	case ACER_CAP_THREEG:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_THREEG_METHODID;
		break;
	case ACER_CAP_MAILLED:
		if (value > 1)
			return AE_BAD_PARAMETER;
		if (quirks->mailled == 1) {
			param = value ? 0x92 : 0x93;
			i8042_lock_chip();
			i8042_command(&param, 0x1059);
			i8042_unlock_chip();
			return 0;
		}
		break;
	default:
		return AE_ERROR;
	}
	return WMI_execute_u32(method_id, (u32)value, NULL);
}

static struct wmi_interface wmid_interface    = { .type = ACER_WMID };
static struct wmi_interface wmid_v2_interface = { .type = ACER_WMID_V2 };

/*
 * WMID3 (GDS) device status
 */
static acpi_status wmid3_get_device_status(u32 *value, u16 device)
{
	struct wmid3_gds_return_value return_value;
	acpi_status status;
	union acpi_object *obj;
	struct wmid3_gds_get_input_param params = {
		.function_num  = 0x1,
		.hotkey_number = commun_fn_key_number,
		.devices       = device,
	};
	struct acpi_buffer input  = { sizeof(params), &params };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x2, &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj)
		return AE_ERROR;
	if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return AE_ERROR;
	}
	if (obj->buffer.length != 8) {
		pr_warn("Unknown buffer length %d\n", obj->buffer.length);
		kfree(obj);
		return AE_ERROR;
	}

	return_value = *((struct wmid3_gds_return_value *)obj->buffer.pointer);
	kfree(obj);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Get 0x%x device status failed: 0x%x - 0x%x\n",
			device, return_value.error_code, return_value.ec_return_value);
	else
		*value = !!(return_value.devices & device);

	return status;
}

static acpi_status wmid_v2_get_u32(u32 *value, u32 cap)
{
	u16 device;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		device = ACER_WMID3_GDS_WIRELESS;
		break;
	case ACER_CAP_BLUETOOTH:
		device = ACER_WMID3_GDS_BLUETOOTH;
		break;
	case ACER_CAP_THREEG:
		device = ACER_WMID3_GDS_THREEG;
		break;
	default:
		return AE_ERROR;
	}
	return wmid3_get_device_status(value, device);
}

static acpi_status wmid3_set_device_status(u32 value, u16 device)
{
	struct wmid3_gds_return_value return_value;
	acpi_status status;
	union acpi_object *obj;
	u16 devices;
	struct wmid3_gds_get_input_param get_params = {
		.function_num  = 0x1,
		.hotkey_number = commun_fn_key_number,
		.devices       = commun_func_bitmap,
	};
	struct wmid3_gds_set_input_param set_params = {
		.function_num  = 0x2,
		.hotkey_number = commun_fn_key_number,
		.devices       = commun_func_bitmap,
	};
	struct acpi_buffer get_input  = { sizeof(get_params), &get_params };
	struct acpi_buffer set_input  = { sizeof(set_params), &set_params };
	struct acpi_buffer output     = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer output2    = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x2, &get_input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj)
		return AE_ERROR;
	if (obj->type != ACPI_TYPE_BUFFER || obj->buffer.length != 8) {
		kfree(obj);
		return AE_ERROR;
	}

	return_value = *((struct wmid3_gds_return_value *)obj->buffer.pointer);
	kfree(obj);

	if (return_value.error_code || return_value.ec_return_value) {
		pr_warn("Get current device status failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);
		return status;
	}

	devices = return_value.devices;
	set_params.devices = value ? (devices | device) : (devices & ~device);

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x1, &set_input, &output2);
	if (ACPI_FAILURE(status))
		return status;

	obj = output2.pointer;
	if (!obj)
		return AE_ERROR;
	if (obj->type != ACPI_TYPE_BUFFER || obj->buffer.length != 4) {
		kfree(obj);
		return AE_ERROR;
	}

	return_value = *((struct wmid3_gds_return_value *)obj->buffer.pointer);
	kfree(obj);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Set device status failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);

	return status;
}

static acpi_status wmid_v2_set_u32(u32 value, u32 cap)
{
	u16 device;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		device = ACER_WMID3_GDS_WIRELESS;
		break;
	case ACER_CAP_BLUETOOTH:
		device = ACER_WMID3_GDS_BLUETOOTH;
		break;
	case ACER_CAP_THREEG:
		device = ACER_WMID3_GDS_THREEG;
		break;
	default:
		return AE_ERROR;
	}
	return wmid3_set_device_status(value, device);
}

/*
 * WMID Gaming (GUID4) interface
 */
static acpi_status WMI_gaming_execute_u64(u32 method_id, u64 in, u64 *out)
{
	struct acpi_buffer input  = {(acpi_size)sizeof(u64), (void *)(&in)};
	struct acpi_buffer result = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	u64 tmp = 0;
	acpi_status status;

	status = wmi_evaluate_method(WMID_GUID4, 0, method_id, &input, &result);
	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *)result.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER) {
			if (obj->buffer.length == sizeof(u32))
				tmp = *((u32 *)obj->buffer.pointer);
			else if (obj->buffer.length == sizeof(u64))
				tmp = *((u64 *)obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			tmp = (u64)obj->integer.value;
		}
	}
	if (out)
		*out = tmp;
	kfree(result.pointer);
	return status;
}

static int WMI_gaming_execute_u32_u64(u32 method_id, u32 in, u64 *out)
{
	struct acpi_buffer result = {ACPI_ALLOCATE_BUFFER, NULL};
	struct acpi_buffer input  = { .length = sizeof(in), .pointer = &in };
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;

	status = wmi_evaluate_method(WMID_GUID4, 0, method_id, &input, &result);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = result.pointer;
	if (obj && out) {
		switch (obj->type) {
		case ACPI_TYPE_INTEGER:
			*out = obj->integer.value;
			break;
		case ACPI_TYPE_BUFFER:
			if (obj->buffer.length < sizeof(*out))
				ret = -ENOMSG;
			else
				*out = get_unaligned_le64(obj->buffer.pointer);
			break;
		default:
			ret = -ENOMSG;
			break;
		}
	}
	kfree(obj);
	return ret;
}

/* WMID Gaming - GUID5 sys info */
static int WMID_gaming_get_sys_info(u32 command, u64 *out)
{
	acpi_status status;
	u64 result;

	status = WMI_gaming_execute_u64(ACER_WMID_GET_GAMING_SYS_INFO_METHODID,
					command, &result);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (FIELD_GET(ACER_NITRO_V5_RETURN_STATUS_BIT_MASK, result))
		return -EIO;

	*out = result;
	return 0;
}

/* WMID Gaming misc settings */
static int WMID_gaming_set_misc_setting(enum acer_wmi_gaming_misc_setting setting,
					u8 value)
{
	acpi_status status;
	u64 input = 0, result;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);
	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_VALUE_MASK, value);

	status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_MISC_SETTING_METHODID,
					input, &result);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;
	return 0;
}

static int WMID_gaming_get_misc_setting(enum acer_wmi_gaming_misc_setting setting, u8 *value)
{
	u64 input = 0, result;
	int ret;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);
	ret = WMI_gaming_execute_u32_u64(ACER_WMID_GET_GAMING_MISC_SETTING_METHODID,
					 input, &result);
	if (ret < 0)
		return ret;
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;
	*value = FIELD_GET(ACER_GAMING_MISC_SETTING_VALUE_MASK, result);
	return 0;
}

/*
 * Generic get/set (dispatch by interface type)
 */
static acpi_status get_u32(u32 *value, u32 cap)
{
	acpi_status status = AE_ERROR;

	switch (interface->type) {
	case ACER_WMID:
		status = WMID_get_u32(value, cap);
		break;
	case ACER_WMID_V2:
		if (cap & (ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH | ACER_CAP_THREEG))
			status = wmid_v2_get_u32(value, cap);
		else if (wmi_has_guid(WMID_GUID2))
			status = WMID_get_u32(value, cap);
		break;
	}
	return status;
}

static acpi_status set_u32(u32 value, u32 cap)
{
	if (interface->capability & cap) {
		switch (interface->type) {
		case ACER_WMID:
			return WMID_set_u32(value, cap);
		case ACER_WMID_V2:
			if (cap & (ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH | ACER_CAP_THREEG))
				return wmid_v2_set_u32(value, cap);
			else if (wmi_has_guid(WMID_GUID2))
				return WMID_set_u32(value, cap);
			fallthrough;
		default:
			return AE_BAD_PARAMETER;
		}
	}
	return AE_BAD_PARAMETER;
}

/*
 * Battery health control (WMID_GUID5)
 */
static acpi_status battery_health_query(int mode, int *enabled)
{
	acpi_status status;
	union acpi_object *obj;
	struct get_battery_health_control_status_input params = {
		.uBatteryNo    = 0x1,
		.uFunctionQuery = 0x1,
		.uReserved     = {0x0, 0x0},
	};
	struct get_battery_health_control_status_output ret;
	struct acpi_buffer input  = { sizeof(params), &params };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMID_GUID5, 0,
				     ACER_WMID_GET_BATTERY_HEALTH_CONTROL_STATUS_METHODID,
				     &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER || obj->buffer.length != 8) {
		pr_err("Unexpected battery health query output (len=%d)\n",
		       obj ? (int)obj->buffer.length : -1);
		goto failed;
	}

	ret = *((struct get_battery_health_control_status_output *)obj->buffer.pointer);

	if (mode == HEALTH_MODE)
		*enabled = ret.uFunctionStatus[0];
	else if (mode == CALIBRATION_MODE)
		*enabled = ret.uFunctionStatus[1];
	else
		goto failed;

	kfree(obj);
	return AE_OK;
failed:
	kfree(obj);
	return AE_ERROR;
}

static acpi_status battery_health_set(u8 function, u8 function_status)
{
	acpi_status status;
	union acpi_object *obj;
	struct set_battery_health_control_input params = {
		.uBatteryNo      = 0x1,
		.uFunctionMask   = function,
		.uFunctionStatus = function_status,
		.uReservedIn     = {0x0, 0x0, 0x0, 0x0, 0x0},
	};
	struct set_battery_health_control_output ret;
	struct acpi_buffer input  = { sizeof(params), &params };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMID_GUID5, 0,
				     ACER_WMID_SET_BATTERY_HEALTH_CONTROL_METHODID,
				     &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER || obj->buffer.length != 4) {
		pr_err("Unexpected battery health set output (len=%d)\n",
		       obj ? (int)obj->buffer.length : -1);
		goto failed;
	}

	ret = *((struct set_battery_health_control_output *)obj->buffer.pointer);
	if (ret.uReturn != 0) {
		pr_err("Battery health set returned error: 0x%x\n", ret.uReturn);
		goto failed;
	}

	kfree(obj);
	return AE_OK;
failed:
	kfree(obj);
	return AE_ERROR;
}

/* sysfs: battery_limiter */
static ssize_t battery_limiter_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int enabled;
	acpi_status status = battery_health_query(HEALTH_MODE, &enabled);

	if (ACPI_FAILURE(status))
		return -ENODEV;
	return sprintf(buf, "%d\n", enabled);
}

static ssize_t battery_limiter_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	u8 val;

	if (sscanf(buf, "%hhd", &val) != 1)
		return -EINVAL;
	if (val != 0 && val != 1)
		return -EINVAL;
	if (battery_health_set(HEALTH_MODE, val) != AE_OK)
		return -ENODEV;
	return count;
}

/* sysfs: battery_calibration */
static ssize_t battery_calibration_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	int enabled;
	acpi_status status = battery_health_query(CALIBRATION_MODE, &enabled);

	if (ACPI_FAILURE(status))
		return -ENODEV;
	return sprintf(buf, "%d\n", enabled);
}

static ssize_t battery_calibration_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	u8 val;

	if (sscanf(buf, "%hhd", &val) != 1)
		return -EINVAL;
	if (val != 0 && val != 1)
		return -EINVAL;
	if (battery_health_set(CALIBRATION_MODE, val) != AE_OK)
		return -ENODEV;
	return count;
}

/*
 * Fan speed control
 */
static u64 fan_val_calc(int percentage, int fan_index)
{
	return (((percentage * 25600) / 100) & 0xFF00) + fan_index;
}

static acpi_status acer_set_fan_speed(int t_cpu_fan_speed, int t_gpu_fan_speed)
{
	acpi_status status;

	if (t_cpu_fan_speed == 100 && t_gpu_fan_speed == 100) {
		/* MAX fan */
		status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID,
						0x820009, NULL);
		if (ACPI_FAILURE(status))
			return AE_ERROR;
	} else if (t_cpu_fan_speed == 0 && t_gpu_fan_speed == 0) {
		/* AUTO fan */
		status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID,
						0x410009, NULL);
		if (ACPI_FAILURE(status))
			return AE_ERROR;
	} else if (t_cpu_fan_speed <= 100 && t_gpu_fan_speed <= 100) {
		if (t_cpu_fan_speed == 0) {
			/* GPU only */
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID,
							0x10001, NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID,
							0xC00008, NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_SPEED_METHODID,
							fan_val_calc(t_gpu_fan_speed, 4), NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
		} else if (t_gpu_fan_speed == 0) {
			/* CPU only */
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID,
							0x400008, NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID,
							0x30001, NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_SPEED_METHODID,
							fan_val_calc(t_cpu_fan_speed, 1), NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
		} else {
			/* Both fans custom */
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID,
							0xC30009, NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_SPEED_METHODID,
							fan_val_calc(t_cpu_fan_speed, 1), NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
			status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_FAN_SPEED_METHODID,
							fan_val_calc(t_gpu_fan_speed, 4), NULL);
			if (ACPI_FAILURE(status))
				return AE_ERROR;
		}
	} else {
		return AE_ERROR;
	}

	cpu_fan_speed = t_cpu_fan_speed;
	gpu_fan_speed = t_gpu_fan_speed;
	pr_info("Fan speed set: CPU=%d%%, GPU=%d%%\n", cpu_fan_speed, gpu_fan_speed);
	return AE_OK;
}

/* sysfs: fan_speed  (read: "cpu%,gpu%"  write: "cpu%,gpu%") */
static ssize_t fan_speed_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d,%d\n", cpu_fan_speed, gpu_fan_speed);
}

static ssize_t fan_speed_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int t_cpu, t_gpu;
	char input[9];
	char *token, *input_ptr = input;
	size_t len = min(count, sizeof(input) - 1);

	strncpy(input, buf, len);
	input[len - 1] == '\n' ? (input[len - 1] = '\0') : (input[len] = '\0');

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &t_cpu) || t_cpu < 0 || t_cpu > 100)
		return -EINVAL;

	token = strsep(&input_ptr, ",");
	if (!token || kstrtoint(token, 10, &t_gpu) || t_gpu < 0 || t_gpu > 100)
		return -EINVAL;

	if (ACPI_FAILURE(acer_set_fan_speed(t_cpu, t_gpu)))
		return -ENODEV;

	return count;
}

/*
 * Platform profile (thermal modes)
 */
static int acer_nitro_v5_platform_profile_get(struct device *dev, enum platform_profile_option *profile)
{
	u8 tp;
	int err = WMID_gaming_get_misc_setting(ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, &tp);

	if (err)
		return err;

	switch (tp) {
	case ACER_NITRO_V5_THERMAL_PROFILE_TURBO:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case ACER_NITRO_V5_THERMAL_PROFILE_PERFORMANCE:
		*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
		break;
	case ACER_NITRO_V5_THERMAL_PROFILE_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case ACER_NITRO_V5_THERMAL_PROFILE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	case ACER_NITRO_V5_THERMAL_PROFILE_ECO:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int acer_nitro_v5_platform_profile_set(struct device *dev, enum platform_profile_option profile)
{
	int tp;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		tp = ACER_NITRO_V5_THERMAL_PROFILE_TURBO;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		tp = ACER_NITRO_V5_THERMAL_PROFILE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_BALANCED:
		tp = ACER_NITRO_V5_THERMAL_PROFILE_BALANCED;
		break;
	case PLATFORM_PROFILE_QUIET:
		tp = ACER_NITRO_V5_THERMAL_PROFILE_QUIET;
		break;
	case PLATFORM_PROFILE_LOW_POWER:
		tp = ACER_NITRO_V5_THERMAL_PROFILE_ECO;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (tp != acer_nitro_v5_max_perf)
		last_non_turbo_profile = tp;

	return WMID_gaming_set_misc_setting(ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, tp);
}

static int acer_nitro_v5_platform_profile_probe(struct device *dev, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER,          choices);
	set_bit(PLATFORM_PROFILE_QUIET,              choices);
	set_bit(PLATFORM_PROFILE_BALANCED,           choices);
	set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE,        choices);

	acer_nitro_v5_max_perf = ACER_NITRO_V5_THERMAL_PROFILE_TURBO;
	return 0;
}

static const struct platform_profile_ops acer_nitro_v5_platform_profile_ops = {
	.probe       = acer_nitro_v5_platform_profile_probe,
	.profile_get = acer_nitro_v5_platform_profile_get,
	.profile_set = acer_nitro_v5_platform_profile_set,
};

static int acer_platform_profile_setup(struct platform_device *device)
{
	int retry;

	for (retry = 0; retry < 10; retry++) {
		platform_profile_device = devm_platform_profile_register(
			&device->dev, "acer-wmi", NULL,
			&acer_nitro_v5_platform_profile_ops);

		if (!IS_ERR(platform_profile_device)) {
			platform_profile_support = true;
			pr_info("Platform profile registered (attempt %d)\n", retry + 1);
			return 0;
		}
		pr_warn("Platform profile registration failed (attempt %d): %ld\n",
			retry + 1, PTR_ERR(platform_profile_device));
		if (retry < 9)
			msleep(100 << min(retry, 3));
	}
	pr_warn("Platform profile not available after retries\n");
	platform_profile_support = false;
	return 0;
}

static int acer_thermal_profile_change(void)
{
	u8 current_tp;
	int tp, err;
	u64 on_AC;
	acpi_status status;

	err = WMID_gaming_get_misc_setting(ACER_WMID_MISC_SETTING_PLATFORM_PROFILE,
					   &current_tp);
	if (err)
		return err;

	status = WMI_gaming_execute_u64(ACER_WMID_GET_GAMING_SYS_INFO_METHODID,
					ACER_WMID_CMD_GET_NITRO_V5_BAT_STATUS, &on_AC);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (!on_AC) {
		/* On battery: toggle ECO <-> BALANCED */
		tp = (current_tp == ACER_NITRO_V5_THERMAL_PROFILE_ECO)
			? ACER_NITRO_V5_THERMAL_PROFILE_BALANCED
			: ACER_NITRO_V5_THERMAL_PROFILE_ECO;
	} else {
		switch (current_tp) {
		case ACER_NITRO_V5_THERMAL_PROFILE_TURBO:
			tp = cycle_gaming_thermal_profile
				? ACER_NITRO_V5_THERMAL_PROFILE_QUIET
				: last_non_turbo_profile;
			break;
		case ACER_NITRO_V5_THERMAL_PROFILE_PERFORMANCE:
			tp = ACER_NITRO_V5_THERMAL_PROFILE_TURBO;
			break;
		case ACER_NITRO_V5_THERMAL_PROFILE_BALANCED:
			tp = cycle_gaming_thermal_profile
				? ACER_NITRO_V5_THERMAL_PROFILE_PERFORMANCE
				: ACER_NITRO_V5_THERMAL_PROFILE_TURBO;
			break;
		case ACER_NITRO_V5_THERMAL_PROFILE_QUIET:
			tp = cycle_gaming_thermal_profile
				? ACER_NITRO_V5_THERMAL_PROFILE_BALANCED
				: ACER_NITRO_V5_THERMAL_PROFILE_TURBO;
			break;
		case ACER_NITRO_V5_THERMAL_PROFILE_ECO:
			tp = cycle_gaming_thermal_profile
				? ACER_NITRO_V5_THERMAL_PROFILE_QUIET
				: ACER_NITRO_V5_THERMAL_PROFILE_TURBO;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	err = WMID_gaming_set_misc_setting(ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, tp);
	if (err)
		return err;

	if (tp != acer_nitro_v5_max_perf)
		last_non_turbo_profile = tp;

	if (platform_profile_support)
		platform_profile_notify(platform_profile_device);

	return 0;
}

/*
 * hwmon - fan RPM and temperature via GUID5/gaming sys info
 */
static const enum acer_wmi_nitro_v5_sensor_id acer_wmi_temp_channel_to_sensor_id[] = {
	[0] = ACER_WMID_SENSOR_CPU_TEMPERATURE,
	[1] = ACER_WMID_SENSOR_GPU_TEMPERATURE,
	[2] = ACER_WMID_SENSOR_EXTERNAL_TEMPERATURE_2,
};

static const enum acer_wmi_nitro_v5_sensor_id acer_wmi_fan_channel_to_sensor_id[] = {
	[0] = ACER_WMID_SENSOR_CPU_FAN_SPEED,
	[1] = ACER_WMID_SENSOR_GPU_FAN_SPEED,
};

static umode_t acer_wmi_hwmon_is_visible(const void *data,
					  enum hwmon_sensor_types type,
					  u32 attr, int channel)
{
	enum acer_wmi_nitro_v5_sensor_id sensor_id;
	const u64 *sensors = data;

	switch (type) {
	case hwmon_temp:
		sensor_id = acer_wmi_temp_channel_to_sensor_id[channel];
		break;
	case hwmon_fan:
		sensor_id = acer_wmi_fan_channel_to_sensor_id[channel];
		break;
	default:
		return 0;
	}
	return (*sensors & BIT(sensor_id - 1)) ? 0444 : 0;
}

static int acer_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	u64 command = ACER_WMID_CMD_GET_NITRO_V5_SENSOR_READING;
	u64 result;
	int ret;

	switch (type) {
	case hwmon_temp:
		command |= FIELD_PREP(ACER_NITRO_V5_SENSOR_INDEX_BIT_MASK,
				      acer_wmi_temp_channel_to_sensor_id[channel]);
		ret = WMID_gaming_get_sys_info(command, &result);
		if (ret < 0)
			return ret;
		result = FIELD_GET(ACER_NITRO_V5_SENSOR_READING_BIT_MASK, result);
		*val = result * MILLIDEGREE_PER_DEGREE;
		return 0;

	case hwmon_fan:
		command |= FIELD_PREP(ACER_NITRO_V5_SENSOR_INDEX_BIT_MASK,
				      acer_wmi_fan_channel_to_sensor_id[channel]);
		ret = WMID_gaming_get_sys_info(command, &result);
		if (ret < 0)
			return ret;
		*val = FIELD_GET(ACER_NITRO_V5_SENSOR_READING_BIT_MASK, result);
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info *const acer_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT,
			   HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT),
	NULL
};

static const struct hwmon_ops acer_wmi_hwmon_ops = {
	.read       = acer_wmi_hwmon_read,
	.is_visible = acer_wmi_hwmon_is_visible,
};

static const struct hwmon_chip_info acer_wmi_hwmon_chip_info = {
	.ops  = &acer_wmi_hwmon_ops,
	.info = acer_wmi_hwmon_info,
};

static int acer_wmi_hwmon_init(void)
{
	struct device *dev = &acer_platform_device->dev;
	struct device *hwmon;
	u64 result;
	int ret;

	ret = WMID_gaming_get_sys_info(ACER_WMID_CMD_GET_NITRO_V5_SUPPORTED_SENSORS,
				       &result);
	if (ret < 0)
		return ret;

	supported_sensors = FIELD_GET(ACER_NITRO_V5_SUPPORTED_SENSORS_BIT_MASK, result);
	if (!supported_sensors)
		return 0;

	hwmon = devm_hwmon_device_register_with_info(dev, "acer",
						     &supported_sensors,
						     &acer_wmi_hwmon_chip_info,
						     NULL);
	if (IS_ERR(hwmon)) {
		dev_err(dev, "Could not register acer hwmon device\n");
		return PTR_ERR(hwmon);
	}
	return 0;
}

/*
 * Rfkill
 */
static void acer_rfkill_update(struct work_struct *ignored);
static DECLARE_DELAYED_WORK(acer_rfkill_work, acer_rfkill_update);

static void acer_rfkill_update(struct work_struct *ignored)
{
	u32 state;
	acpi_status status;

	if (has_cap(ACER_CAP_WIRELESS)) {
		status = get_u32(&state, ACER_CAP_WIRELESS);
		if (ACPI_SUCCESS(status))
			rfkill_set_sw_state(wireless_rfkill, !state);
	}
	if (has_cap(ACER_CAP_BLUETOOTH)) {
		status = get_u32(&state, ACER_CAP_BLUETOOTH);
		if (ACPI_SUCCESS(status))
			rfkill_set_sw_state(bluetooth_rfkill, !state);
	}
	schedule_delayed_work(&acer_rfkill_work, round_jiffies_relative(HZ));
}

static int acer_rfkill_set(void *data, bool blocked)
{
	acpi_status status;
	u32 cap = (unsigned long)data;

	if (rfkill_inited) {
		status = set_u32(!blocked, cap);
		if (ACPI_FAILURE(status))
			return -ENODEV;
	}
	return 0;
}

static const struct rfkill_ops acer_rfkill_ops = {
	.set_block = acer_rfkill_set,
};

static struct rfkill *acer_rfkill_register(struct device *dev,
					   enum rfkill_type type,
					   char *name, u32 cap)
{
	struct rfkill *rfkill_dev;
	u32 state;
	acpi_status status;

	rfkill_dev = rfkill_alloc(name, dev, type, &acer_rfkill_ops,
				  (void *)(unsigned long)cap);
	if (!rfkill_dev)
		return ERR_PTR(-ENOMEM);

	status = get_u32(&state, cap);
	if (rfkill_register(rfkill_dev)) {
		rfkill_destroy(rfkill_dev);
		return ERR_PTR(-ENODEV);
	}
	if (ACPI_SUCCESS(status))
		rfkill_set_sw_state(rfkill_dev, !state);
	return rfkill_dev;
}

static int acer_rfkill_init(struct device *dev)
{
	if (has_cap(ACER_CAP_WIRELESS)) {
		wireless_rfkill = acer_rfkill_register(dev, RFKILL_TYPE_WLAN,
						       "acer-wireless", ACER_CAP_WIRELESS);
		if (IS_ERR(wireless_rfkill))
			goto error_wireless;
	}
	if (has_cap(ACER_CAP_BLUETOOTH)) {
		bluetooth_rfkill = acer_rfkill_register(dev, RFKILL_TYPE_BLUETOOTH,
							"acer-bluetooth", ACER_CAP_BLUETOOTH);
		if (IS_ERR(bluetooth_rfkill))
			goto error_bluetooth;
	}
	rfkill_inited = true;
	if ((ec_raw_mode || !wmi_has_guid(ACERWMID_EVENT_GUID)) &&
	    has_cap(ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH))
		schedule_delayed_work(&acer_rfkill_work,
				      round_jiffies_relative(HZ));
	return 0;

error_bluetooth:
	if (has_cap(ACER_CAP_WIRELESS)) {
		rfkill_unregister(wireless_rfkill);
		rfkill_destroy(wireless_rfkill);
	}
error_wireless:
	return -ENODEV;
}

static void acer_rfkill_exit(void)
{
	if ((ec_raw_mode || !wmi_has_guid(ACERWMID_EVENT_GUID)) &&
	    has_cap(ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH))
		cancel_delayed_work_sync(&acer_rfkill_work);

	if (has_cap(ACER_CAP_WIRELESS)) {
		rfkill_unregister(wireless_rfkill);
		rfkill_destroy(wireless_rfkill);
	}
	if (has_cap(ACER_CAP_BLUETOOTH)) {
		rfkill_unregister(bluetooth_rfkill);
		rfkill_destroy(bluetooth_rfkill);
	}
}

/*
 * Accelerometer
 */
static int acer_gsensor_init(void)
{
	acpi_status status;
	struct acpi_buffer output;
	union acpi_object out_obj;

	output.length  = sizeof(out_obj);
	output.pointer = &out_obj;
	status = acpi_evaluate_object(gsensor_handle, "_INI", NULL, &output);
	return ACPI_FAILURE(status) ? -1 : 0;
}

static int acer_gsensor_open(struct input_dev *input)
{
	return acer_gsensor_init();
}

static int acer_gsensor_event(void)
{
	acpi_status status;
	struct acpi_buffer output;
	union acpi_object out_obj[5];

	if (!acer_wmi_accel_dev)
		return -1;

	output.length  = sizeof(out_obj);
	output.pointer = out_obj;
	status = acpi_evaluate_object(gsensor_handle, "RDVL", NULL, &output);
	if (ACPI_FAILURE(status))
		return -1;
	if (out_obj->package.count != 4)
		return -1;

	input_report_abs(acer_wmi_accel_dev, ABS_X,
			 (s16)out_obj->package.elements[0].integer.value);
	input_report_abs(acer_wmi_accel_dev, ABS_Y,
			 (s16)out_obj->package.elements[1].integer.value);
	input_report_abs(acer_wmi_accel_dev, ABS_Z,
			 (s16)out_obj->package.elements[2].integer.value);
	input_sync(acer_wmi_accel_dev);
	return 0;
}

/*
 * WMI notify handler
 */
static void acer_wmi_notify(union acpi_object *obj, void *context)
{
	struct event_return_value return_value;
	u16 device_state;
	const struct key_entry *key;
	u32 scancode;

	if (!obj || obj->type != ACPI_TYPE_BUFFER || obj->buffer.length != 8)
		return;

	return_value = *((struct event_return_value *)obj->buffer.pointer);

	switch (return_value.function) {
	case WMID_HOTKEY_EVENT:
		device_state = return_value.device_state;
		key = sparse_keymap_entry_from_scancode(acer_wmi_input_dev,
							return_value.key_num);
		if (!key) {
			pr_warn("Unknown key number 0x%x\n", return_value.key_num);
		} else {
			scancode = return_value.key_num;
			switch (key->keycode) {
			case KEY_WLAN:
			case KEY_BLUETOOTH:
				if (has_cap(ACER_CAP_WIRELESS))
					rfkill_set_sw_state(wireless_rfkill,
						!(device_state & ACER_WMID3_GDS_WIRELESS));
				if (has_cap(ACER_CAP_BLUETOOTH))
					rfkill_set_sw_state(bluetooth_rfkill,
						!(device_state & ACER_WMID3_GDS_BLUETOOTH));
				break;
			case KEY_TOUCHPAD_TOGGLE:
				scancode = (device_state & ACER_WMID3_GDS_TOUCHPAD)
					? KEY_TOUCHPAD_ON : KEY_TOUCHPAD_OFF;
				break;
			}
			sparse_keymap_report_event(acer_wmi_input_dev, scancode, 1, true);
		}
		break;

	case WMID_ACCEL_OR_KBD_DOCK_EVENT:
		acer_gsensor_event();
		break;

	case WMID_GAMING_TURBO_KEY_EVENT:
		pr_info("Turbo/profile key pressed: key_num=0x%x\n", return_value.key_num);
		if ((return_value.key_num == 0x5 ||
		     (return_value.key_num == 0x4 && has_cap(ACER_CAP_NITRO_SENSE_V5))) &&
		    has_cap(ACER_CAP_PLATFORM_PROFILE))
			acer_thermal_profile_change();
		break;

	case WMID_AC_EVENT:
		break;

	case WMID_BATTERY_BOOST_EVENT:
		break;

	case WMID_CALIBRATION_EVENT:
		if (battery_health_set(CALIBRATION_MODE, return_value.key_num) != AE_OK)
			pr_err("Error changing calibration state\n");
		break;

	default:
		pr_warn("Unknown WMI function 0x%x key 0x%x\n",
			return_value.function, return_value.key_num);
		break;
	}
}

/*
 * WMID3 function mode helpers
 */
static acpi_status __init wmid3_set_function_mode(struct func_input_params *params, struct func_return_value *return_value)
{
	acpi_status status;
	union acpi_object *obj;
	struct acpi_buffer input  = { sizeof(struct func_input_params), params };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x1, &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;
	if (!obj)
		return AE_ERROR;
	if (obj->type != ACPI_TYPE_BUFFER || obj->buffer.length != 4) {
		kfree(obj);
		return AE_ERROR;
	}

	*return_value = *((struct func_return_value *)obj->buffer.pointer);
	kfree(obj);
	return status;
}

static int __init acer_wmi_enable_lm(void)
{
	struct func_return_value return_value;
	struct func_input_params params = {
		.function_num    = 0x1,
		.commun_devices  = 0xFFFF,
		.devices         = 0xFFFF,
		.app_status      = 0x01,
		.app_mask        = 0x01,
	};
	acpi_status status = wmid3_set_function_mode(&params, &return_value);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling Launch Manager failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);
	return status;
}

static int __init acer_wmi_enable_rf_button(void)
{
	struct func_return_value return_value;
	struct func_input_params params = {
		.function_num    = 0x1,
		.commun_devices  = 0xFFFF,
		.devices         = 0xFFFF,
		.app_status      = 0x10,
		.app_mask        = 0x10,
	};
	acpi_status status = wmid3_set_function_mode(&params, &return_value);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling RF Button failed: 0x%x - 0x%x\n",
			return_value.error_code, return_value.ec_return_value);
	return status;
}

/*
 * Input device setup
 */
static int __init acer_wmi_accel_setup(void)
{
	struct acpi_device *adev;
	int err;

	adev = acpi_dev_get_first_match_dev("BST0001", NULL, -1);
	if (!adev)
		return -ENODEV;

	gsensor_handle = acpi_device_handle(adev);
	acpi_dev_put(adev);

	acer_wmi_accel_dev = input_allocate_device();
	if (!acer_wmi_accel_dev)
		return -ENOMEM;

	acer_wmi_accel_dev->open      = acer_gsensor_open;
	acer_wmi_accel_dev->name      = "Acer BMA150 accelerometer";
	acer_wmi_accel_dev->phys      = "wmi/input1";
	acer_wmi_accel_dev->id.bustype = BUS_HOST;
	acer_wmi_accel_dev->evbit[0]   = BIT_MASK(EV_ABS);
	input_set_abs_params(acer_wmi_accel_dev, ABS_X, -16384, 16384, 0, 0);
	input_set_abs_params(acer_wmi_accel_dev, ABS_Y, -16384, 16384, 0, 0);
	input_set_abs_params(acer_wmi_accel_dev, ABS_Z, -16384, 16384, 0, 0);

	err = input_register_device(acer_wmi_accel_dev);
	if (err) {
		input_free_device(acer_wmi_accel_dev);
		return err;
	}
	return 0;
}

static int __init acer_wmi_input_setup(void)
{
	acpi_status status;
	int err;

	acer_wmi_input_dev = input_allocate_device();
	if (!acer_wmi_input_dev)
		return -ENOMEM;

	acer_wmi_input_dev->name      = "Acer WMI hotkeys";
	acer_wmi_input_dev->phys      = "wmi/input0";
	acer_wmi_input_dev->id.bustype = BUS_HOST;

	err = sparse_keymap_setup(acer_wmi_input_dev, acer_wmi_keymap, NULL);
	if (err)
		goto err_free_dev;

	status = wmi_install_notify_handler(ACERWMID_EVENT_GUID, acer_wmi_notify, NULL);
	if (ACPI_FAILURE(status)) {
		err = -EIO;
		goto err_free_dev;
	}

	err = input_register_device(acer_wmi_input_dev);
	if (err)
		goto err_uninstall;
	return 0;

err_uninstall:
	wmi_remove_notify_handler(ACERWMID_EVENT_GUID);
err_free_dev:
	input_free_device(acer_wmi_input_dev);
	return err;
}

static void acer_wmi_input_destroy(void)
{
	wmi_remove_notify_handler(ACERWMID_EVENT_GUID);
	input_unregister_device(acer_wmi_input_dev);
}

/*
 * WMID capability detection
 */
static void __init type_aa_dmi_decode(const struct dmi_header *header, void *d)
{
	struct hotkey_function_type_aa *type_aa;

	if (header->type != 0xAA)
		return;

	has_type_aa = true;
	type_aa = (struct hotkey_function_type_aa *)header;
	commun_func_bitmap = type_aa->commun_func_bitmap;

	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_WIRELESS)
		interface->capability |= ACER_CAP_WIRELESS;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_THREEG)
		interface->capability |= ACER_CAP_THREEG;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_BLUETOOTH)
		interface->capability |= ACER_CAP_BLUETOOTH;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_RFBTN)
		commun_func_bitmap &= ~ACER_WMID3_GDS_RFBTN;

	commun_fn_key_number = type_aa->commun_fn_key_number;
}

static acpi_status __init WMID_set_capabilities(void)
{
	struct acpi_buffer out = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	acpi_status status;
	u32 devices;

	status = wmi_query_block(WMID_GUID2, 0, &out);
	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *)out.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER &&
		    (obj->buffer.length == sizeof(u32) ||
		     obj->buffer.length == sizeof(u64))) {
			devices = *((u32 *)obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			devices = (u32)obj->integer.value;
		} else {
			kfree(out.pointer);
			return AE_ERROR;
		}
	} else {
		kfree(out.pointer);
		return AE_ERROR;
	}

	if (devices & 0x07)
		interface->capability |= ACER_CAP_WIRELESS;
	if (devices & 0x40)
		interface->capability |= ACER_CAP_THREEG;
	if (devices & 0x10)
		interface->capability |= ACER_CAP_BLUETOOTH;
	if (!(devices & 0x20))
		max_brightness = 0x9;

	kfree(out.pointer);
	return status;
}

static void __init set_quirks(void)
{
	if (quirks->mailled)
		interface->capability |= ACER_CAP_MAILLED;
	if (quirks->brightness)
		interface->capability |= ACER_CAP_BRIGHTNESS;
	if (quirks->turbo)
		interface->capability |= ACER_CAP_TURBO_OC | ACER_CAP_TURBO_LED | ACER_CAP_TURBO_FAN;
	if (quirks->nitro_sense)
		interface->capability |= ACER_CAP_PLATFORM_PROFILE |
					  ACER_CAP_FAN_SPEED_READ | ACER_CAP_NITRO_SENSE;
	if (quirks->nitro_v5)
		interface->capability |= ACER_CAP_PLATFORM_PROFILE |
					  ACER_CAP_FAN_SPEED_READ |
					  ACER_CAP_NITRO_SENSE | ACER_CAP_NITRO_SENSE_V5;
}

static void __init find_quirks(void)
{
	if (nitro_v5) {
		quirks = &quirk_acer_nitro_v5;
		return;
	}
	dmi_check_system(acer_quirks);
	if (!quirks)
		quirks = &quirk_unknown;
}

/*
 * Command-line init
 */
static void __init acer_commandline_init(void)
{
	if (mailled >= 0)
		set_u32(mailled,    ACER_CAP_MAILLED);
	if (!has_type_aa && threeg >= 0)
		set_u32(threeg,     ACER_CAP_THREEG);
	if (brightness >= 0)
		set_u32(brightness, ACER_CAP_BRIGHTNESS);
}

/*
 * Backlight
 */
static struct backlight_device *acer_backlight_device;

static int read_brightness(struct backlight_device *bd)
{
	u32 value;

	get_u32(&value, ACER_CAP_BRIGHTNESS);
	return value;
}

static int update_bl_status(struct backlight_device *bd)
{
	set_u32(backlight_get_brightness(bd), ACER_CAP_BRIGHTNESS);
	return 0;
}

static const struct backlight_ops acer_bl_ops = {
	.get_brightness = read_brightness,
	.update_status  = update_bl_status,
};

static int acer_backlight_init(struct device *dev)
{
	struct backlight_properties props;
	struct backlight_device *bd;

	memset(&props, 0, sizeof(props));
	props.type           = BACKLIGHT_PLATFORM;
	props.max_brightness = max_brightness;
	bd = backlight_device_register("acer-wmi", dev, NULL, &acer_bl_ops, &props);
	if (IS_ERR(bd)) {
		pr_err("Could not register backlight device\n");
		acer_backlight_device = NULL;
		return PTR_ERR(bd);
	}
	acer_backlight_device = bd;
	bd->props.power      = BACKLIGHT_POWER_ON;
	bd->props.brightness = read_brightness(bd);
	backlight_update_status(bd);
	return 0;
}

static void acer_backlight_exit(void) { backlight_device_unregister(acer_backlight_device); }

/*
 * debugfs
 */
static u32 get_wmid_devices(void)
{
	struct acpi_buffer out = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	acpi_status status;
	u32 devices = 0;

	status = wmi_query_block(WMID_GUID2, 0, &out);
	if (ACPI_FAILURE(status))
		return 0;

	obj = (union acpi_object *)out.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER &&
		    (obj->buffer.length == sizeof(u32) || obj->buffer.length == sizeof(u64)))
			devices = *((u32 *)obj->buffer.pointer);
		else if (obj->type == ACPI_TYPE_INTEGER)
			devices = (u32)obj->integer.value;
	}
	kfree(out.pointer);
	return devices;
}

static void __init create_debugfs(void)
{
	interface->debug.root = debugfs_create_dir("acer-wmi", NULL);
	debugfs_create_u32("devices", S_IRUGO, interface->debug.root,
			   &interface->debug.wmid_devices);
}

static void remove_debugfs(void)
{
	debugfs_remove_recursive(interface->debug.root);
}

/*
 * version sysfs attribute
 */
static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", DRIVER_VERSION);
}
static DEVICE_ATTR_RO(version);

/*
 * sysfs attribute group for Nitro Sense (ANV16-71)
 */
static struct device_attribute nitro_battery_limiter =
	__ATTR(battery_limiter, 0644, battery_limiter_show, battery_limiter_store);
static struct device_attribute nitro_battery_calibration =
	__ATTR(battery_calibration, 0644, battery_calibration_show, battery_calibration_store);
static struct device_attribute nitro_fan_speed =
	__ATTR(fan_speed, 0644, fan_speed_show, fan_speed_store);

static struct attribute *nitro_sense_attrs[] = {
	&dev_attr_version.attr,
	&nitro_battery_limiter.attr,
	&nitro_battery_calibration.attr,
	&nitro_fan_speed.attr,
	NULL
};

static struct attribute_group nitro_sense_attr_group = {
	.name  = "nitro_sense",
	.attrs = nitro_sense_attrs,
};

/*
 * Platform driver probe / remove
*/
static int acer_platform_probe(struct platform_device *device)
{
	int err;

	if (has_cap(ACER_CAP_BRIGHTNESS)) {
		err = acer_backlight_init(&device->dev);
		if (err)
			goto error_brightness;
	}

	err = acer_rfkill_init(&device->dev);
	if (err)
		goto error_rfkill;

	if (has_cap(ACER_CAP_PLATFORM_PROFILE)) {
		err = acer_platform_profile_setup(device);
		if (err)
			goto error_platform_profile;
	}

	/* Nitro V16 ANV16-71: uses NITRO_SENSE_V5 path */
	if (has_cap(ACER_CAP_NITRO_SENSE) && has_cap(ACER_CAP_NITRO_SENSE_V5)) {
		err = sysfs_create_group(&device->dev.kobj, &nitro_sense_attr_group);
		if (err)
			goto error_sysfs;
	}

	if (has_cap(ACER_CAP_FAN_SPEED_READ)) {
		err = acer_wmi_hwmon_init();
		if (err)
			goto error_hwmon;
	}

	return 0;

error_hwmon:
	if (has_cap(ACER_CAP_NITRO_SENSE) && has_cap(ACER_CAP_NITRO_SENSE_V5))
		sysfs_remove_group(&device->dev.kobj, &nitro_sense_attr_group);
error_sysfs:
error_platform_profile:
	acer_rfkill_exit();
error_rfkill:
	if (has_cap(ACER_CAP_BRIGHTNESS))
		acer_backlight_exit();
error_brightness:
	return err;
}

static void acer_platform_remove(struct platform_device *device)
{
	if (has_cap(ACER_CAP_BRIGHTNESS))
		acer_backlight_exit();
	if (has_cap(ACER_CAP_NITRO_SENSE) && has_cap(ACER_CAP_NITRO_SENSE_V5))
		sysfs_remove_group(&device->dev.kobj, &nitro_sense_attr_group);
	acer_rfkill_exit();
}

#ifdef CONFIG_PM_SLEEP
static int acer_suspend(struct device *dev)
{
	u32 value;
	struct acer_data *data = &interface->data;

	if (!data)
		return -ENOMEM;
	if (has_cap(ACER_CAP_BRIGHTNESS)) {
		get_u32(&value, ACER_CAP_BRIGHTNESS);
		data->brightness = value;
	}
	return 0;
}

static int acer_resume(struct device *dev)
{
	struct acer_data *data = &interface->data;

	if (!data)
		return -ENOMEM;
	if (has_cap(ACER_CAP_BRIGHTNESS))
		set_u32(data->brightness, ACER_CAP_BRIGHTNESS);
	if (acer_wmi_accel_dev)
		acer_gsensor_init();
	return 0;
}
#else
#define acer_suspend NULL
#define acer_resume  NULL
#endif

static SIMPLE_DEV_PM_OPS(acer_pm, acer_suspend, acer_resume);

static struct platform_driver acer_platform_driver = {
	.driver = {
		.name = "acer-wmi",
		.pm   = &acer_pm,
	},
	.probe    = acer_platform_probe,
	.remove   = acer_platform_remove,
};

/*
 * Module init / exit
 */
static int __init acer_wmi_init(void)
{
	int err;

	pr_info("Initializing qapkyy_qy v%s for Acer Nitro V16 ANV16-71\n",
		DRIVER_VERSION);

	if (dmi_check_system(acer_blacklist)) {
		pr_info("Blacklisted hardware detected - not loading\n");
		return -ENODEV;
	}

	find_quirks();

	pr_info("Detected model: %s\n", dmi_get_system_info(DMI_PRODUCT_NAME));

	/*
	 * Detect ACPI-WMI interface.
	 */
	if (wmi_has_guid(WMID_GUID1))
		interface = &wmid_interface;

	if (wmi_has_guid(WMID_GUID3))
		interface = &wmid_v2_interface;

	if (interface)
		dmi_walk(type_aa_dmi_decode, NULL);

	if (wmi_has_guid(WMID_GUID2) && interface) {
		if (!has_type_aa && ACPI_FAILURE(WMID_set_capabilities())) {
			pr_err("Unable to detect WMID devices\n");
			return -ENODEV;
		}
		interface->capability |= ACER_CAP_BRIGHTNESS;
	} else if (!wmi_has_guid(WMID_GUID2) && interface && !has_type_aa && force_caps == -1) {
		pr_err("No WMID device detection method found\n");
		return -ENODEV;
	}

	if (!interface) {
		pr_err("No or unsupported WMI interface\n");
		return -ENODEV;
	}

	set_quirks();

	if (acpi_video_get_backlight_type() != acpi_backlight_vendor)
		interface->capability &= ~ACER_CAP_BRIGHTNESS;

	if (wmi_has_guid(WMID_GUID3))
		interface->capability |= ACER_CAP_SET_FUNCTION_MODE;

	if (nitro_v5) {
		interface->capability |= ACER_CAP_PLATFORM_PROFILE |
					  ACER_CAP_FAN_SPEED_READ |
					  ACER_CAP_NITRO_SENSE |
					  ACER_CAP_NITRO_SENSE_V5;
	}

	if (force_caps != -1)
		interface->capability = force_caps;

	if (wmi_has_guid(WMID_GUID3) && (interface->capability & ACER_CAP_SET_FUNCTION_MODE)) {
		if (ec_raw_mode) {
			/* EC raw mode not used on Nitro */
			pr_info("EC raw mode requested but not supported on this model\n");
		} else if (ACPI_FAILURE(acer_wmi_enable_lm())) {
			pr_err("Cannot enable Launch Manager mode\n");
			return -ENODEV;
		}
		acer_wmi_enable_rf_button();
	}

	if (wmi_has_guid(ACERWMID_EVENT_GUID)) {
		err = acer_wmi_input_setup();
		if (err)
			return err;
		err = acer_wmi_accel_setup();
		if (err && err != -ENODEV)
			pr_warn("Cannot enable accelerometer\n");
	}

	err = platform_driver_register(&acer_platform_driver);
	if (err) {
		pr_err("Unable to register platform driver\n");
		goto error_platform_register;
	}

	acer_platform_device = platform_device_alloc("acer-wmi", PLATFORM_DEVID_NONE);
	if (!acer_platform_device) {
		err = -ENOMEM;
		goto error_device_alloc;
	}

	err = platform_device_add(acer_platform_device);
	if (err)
		goto error_device_add;

	if (wmi_has_guid(WMID_GUID2)) {
		interface->debug.wmid_devices = get_wmid_devices();
		create_debugfs();
	}

	acer_commandline_init();

	pr_info("qapkyy_qy loaded. Capabilities: 0x%x\n", interface->capability);
	return 0;

error_device_add:
	platform_device_put(acer_platform_device);
error_device_alloc:
	platform_driver_unregister(&acer_platform_driver);
error_platform_register:
	if (wmi_has_guid(ACERWMID_EVENT_GUID))
		acer_wmi_input_destroy();
	if (acer_wmi_accel_dev)
		input_unregister_device(acer_wmi_accel_dev);
	return err;
}

static void __exit acer_wmi_exit(void)
{
	if (wmi_has_guid(ACERWMID_EVENT_GUID))
		acer_wmi_input_destroy();
	if (acer_wmi_accel_dev)
		input_unregister_device(acer_wmi_accel_dev);

	remove_debugfs();
	platform_device_unregister(acer_platform_device);
	platform_driver_unregister(&acer_platform_driver);

	pr_info("qapkyy_qy unloaded\n");
}

module_init(acer_wmi_init);
module_exit(acer_wmi_exit);
