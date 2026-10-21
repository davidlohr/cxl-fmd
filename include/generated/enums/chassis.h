/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_CHASSIS_H
#define REDFISH_ENUM_CHASSIS_H

#include <stddef.h>

/* Chassis.v1_0_0.ChassisType */
enum rf_chassis_type {
	CHASSIS_TYPE_INVALID = 0,
	CHASSIS_TYPE_RACK = 1,
	CHASSIS_TYPE_BLADE = 2,
	CHASSIS_TYPE_ENCLOSURE = 3,
	CHASSIS_TYPE_STAND_ALONE = 4,
	CHASSIS_TYPE_RACK_MOUNT = 5,
	CHASSIS_TYPE_CARD = 6,
	CHASSIS_TYPE_CARTRIDGE = 7,
	CHASSIS_TYPE_ROW = 8,
	CHASSIS_TYPE_POD = 9,
	CHASSIS_TYPE_EXPANSION = 10,
	CHASSIS_TYPE_SIDECAR = 11,
	CHASSIS_TYPE_ZONE = 12,
	CHASSIS_TYPE_SLED = 13,
	CHASSIS_TYPE_SHELF = 14,
	CHASSIS_TYPE_DRAWER = 15,
	CHASSIS_TYPE_MODULE = 16,
	CHASSIS_TYPE_COMPONENT = 17,
	CHASSIS_TYPE_IP_BASED_DRIVE = 18,
	CHASSIS_TYPE_RACK_GROUP = 19,
	CHASSIS_TYPE_STORAGE_ENCLOSURE = 20,
	CHASSIS_TYPE_IMMERSION_TANK = 21,
	CHASSIS_TYPE_HEAT_EXCHANGER = 22,
	CHASSIS_TYPE_POWER_STRIP = 23,
	CHASSIS_TYPE_OTHER = 24,
};

static inline const char *rf_chassis_type_to_str(enum rf_chassis_type val)
{
	switch (val) {
	case CHASSIS_TYPE_INVALID: return "Invalid";
	case CHASSIS_TYPE_RACK: return "Rack";
	case CHASSIS_TYPE_BLADE: return "Blade";
	case CHASSIS_TYPE_ENCLOSURE: return "Enclosure";
	case CHASSIS_TYPE_STAND_ALONE: return "StandAlone";
	case CHASSIS_TYPE_RACK_MOUNT: return "RackMount";
	case CHASSIS_TYPE_CARD: return "Card";
	case CHASSIS_TYPE_CARTRIDGE: return "Cartridge";
	case CHASSIS_TYPE_ROW: return "Row";
	case CHASSIS_TYPE_POD: return "Pod";
	case CHASSIS_TYPE_EXPANSION: return "Expansion";
	case CHASSIS_TYPE_SIDECAR: return "Sidecar";
	case CHASSIS_TYPE_ZONE: return "Zone";
	case CHASSIS_TYPE_SLED: return "Sled";
	case CHASSIS_TYPE_SHELF: return "Shelf";
	case CHASSIS_TYPE_DRAWER: return "Drawer";
	case CHASSIS_TYPE_MODULE: return "Module";
	case CHASSIS_TYPE_COMPONENT: return "Component";
	case CHASSIS_TYPE_IP_BASED_DRIVE: return "IPBasedDrive";
	case CHASSIS_TYPE_RACK_GROUP: return "RackGroup";
	case CHASSIS_TYPE_STORAGE_ENCLOSURE: return "StorageEnclosure";
	case CHASSIS_TYPE_IMMERSION_TANK: return "ImmersionTank";
	case CHASSIS_TYPE_HEAT_EXCHANGER: return "HeatExchanger";
	case CHASSIS_TYPE_POWER_STRIP: return "PowerStrip";
	case CHASSIS_TYPE_OTHER: return "Other";
	default: return NULL;
	}
}

/* Chassis.v1_0_0.IndicatorLED */
enum rf_indicator_led {
	INDICATOR_LED_INVALID = 0,
	INDICATOR_LED_UNKNOWN = 1,
	INDICATOR_LED_LIT = 2,
	INDICATOR_LED_BLINKING = 3,
	INDICATOR_LED_OFF = 4,
};

static inline const char *rf_indicator_led_to_str(enum rf_indicator_led val)
{
	switch (val) {
	case INDICATOR_LED_INVALID: return "Invalid";
	case INDICATOR_LED_UNKNOWN: return "Unknown";
	case INDICATOR_LED_LIT: return "Lit";
	case INDICATOR_LED_BLINKING: return "Blinking";
	case INDICATOR_LED_OFF: return "Off";
	default: return NULL;
	}
}

/* Chassis.v1_1_0.IntrusionSensor */
enum rf_intrusion_sensor {
	INTRUSION_SENSOR_INVALID = 0,
	INTRUSION_SENSOR_NORMAL = 1,
	INTRUSION_SENSOR_HARDWARE_INTRUSION = 2,
	INTRUSION_SENSOR_TAMPERING_DETECTED = 3,
};

static inline const char *rf_intrusion_sensor_to_str(enum rf_intrusion_sensor val)
{
	switch (val) {
	case INTRUSION_SENSOR_INVALID: return "Invalid";
	case INTRUSION_SENSOR_NORMAL: return "Normal";
	case INTRUSION_SENSOR_HARDWARE_INTRUSION: return "HardwareIntrusion";
	case INTRUSION_SENSOR_TAMPERING_DETECTED: return "TamperingDetected";
	default: return NULL;
	}
}

/* Chassis.v1_1_0.IntrusionSensorReArm */
enum rf_intrusion_sensor_re_arm {
	INTRUSION_SENSOR_RE_ARM_INVALID = 0,
	INTRUSION_SENSOR_RE_ARM_MANUAL = 1,
	INTRUSION_SENSOR_RE_ARM_AUTOMATIC = 2,
};

static inline const char *rf_intrusion_sensor_re_arm_to_str(enum rf_intrusion_sensor_re_arm val)
{
	switch (val) {
	case INTRUSION_SENSOR_RE_ARM_INVALID: return "Invalid";
	case INTRUSION_SENSOR_RE_ARM_MANUAL: return "Manual";
	case INTRUSION_SENSOR_RE_ARM_AUTOMATIC: return "Automatic";
	default: return NULL;
	}
}

/* Chassis.v1_9_0.EnvironmentalClass */
enum rf_environmental_class {
	ENVIRONMENTAL_CLASS_INVALID = 0,
	ENVIRONMENTAL_CLASS_A1 = 1,
	ENVIRONMENTAL_CLASS_A2 = 2,
	ENVIRONMENTAL_CLASS_A3 = 3,
	ENVIRONMENTAL_CLASS_A4 = 4,
};

static inline const char *rf_environmental_class_to_str(enum rf_environmental_class val)
{
	switch (val) {
	case ENVIRONMENTAL_CLASS_INVALID: return "Invalid";
	case ENVIRONMENTAL_CLASS_A1: return "A1";
	case ENVIRONMENTAL_CLASS_A2: return "A2";
	case ENVIRONMENTAL_CLASS_A3: return "A3";
	case ENVIRONMENTAL_CLASS_A4: return "A4";
	default: return NULL;
	}
}

/* Chassis.v1_20_0.ThermalDirection */
enum rf_thermal_direction {
	THERMAL_DIRECTION_INVALID = 0,
	THERMAL_DIRECTION_FRONT_TO_BACK = 1,
	THERMAL_DIRECTION_BACK_TO_FRONT = 2,
	THERMAL_DIRECTION_TOP_EXHAUST = 3,
	THERMAL_DIRECTION_SEALED = 4,
};

static inline const char *rf_thermal_direction_to_str(enum rf_thermal_direction val)
{
	switch (val) {
	case THERMAL_DIRECTION_INVALID: return "Invalid";
	case THERMAL_DIRECTION_FRONT_TO_BACK: return "FrontToBack";
	case THERMAL_DIRECTION_BACK_TO_FRONT: return "BackToFront";
	case THERMAL_DIRECTION_TOP_EXHAUST: return "TopExhaust";
	case THERMAL_DIRECTION_SEALED: return "Sealed";
	default: return NULL;
	}
}

/* Chassis.v1_24_0.DoorState */
enum rf_door_state {
	DOOR_STATE_INVALID = 0,
	DOOR_STATE_LOCKED = 1,
	DOOR_STATE_CLOSED = 2,
	DOOR_STATE_LOCKED_AND_OPEN = 3,
	DOOR_STATE_OPEN = 4,
};

static inline const char *rf_door_state_to_str(enum rf_door_state val)
{
	switch (val) {
	case DOOR_STATE_INVALID: return "Invalid";
	case DOOR_STATE_LOCKED: return "Locked";
	case DOOR_STATE_CLOSED: return "Closed";
	case DOOR_STATE_LOCKED_AND_OPEN: return "LockedAndOpen";
	case DOOR_STATE_OPEN: return "Open";
	default: return NULL;
	}
}

/* Chassis.v1_28_0.RackUnits */
enum rf_rack_units {
	RACK_UNITS_INVALID = 0,
	RACK_UNITS_OPEN_U = 1,
	RACK_UNITS_EIA_310 = 2,
};

static inline const char *rf_rack_units_to_str(enum rf_rack_units val)
{
	switch (val) {
	case RACK_UNITS_INVALID: return "Invalid";
	case RACK_UNITS_OPEN_U: return "OpenU";
	case RACK_UNITS_EIA_310: return "EIA_310";
	default: return NULL;
	}
}

/* Chassis.v1_28_0.RackMountWidth */
enum rf_rack_mount_width {
	RACK_MOUNT_WIDTH_INVALID = 0,
	RACK_MOUNT_WIDTH_OPEN_RACK = 1,
	RACK_MOUNT_WIDTH_OPEN_RACK_WIDE = 2,
	RACK_MOUNT_WIDTH_OPEN_U = 3,
	RACK_MOUNT_WIDTH_EIA_310 = 4,
	RACK_MOUNT_WIDTH_EIA_310__TELCO = 5,
	RACK_MOUNT_WIDTH_HALF_WIDTH = 6,
};

static inline const char *rf_rack_mount_width_to_str(enum rf_rack_mount_width val)
{
	switch (val) {
	case RACK_MOUNT_WIDTH_INVALID: return "Invalid";
	case RACK_MOUNT_WIDTH_OPEN_RACK: return "OpenRack";
	case RACK_MOUNT_WIDTH_OPEN_RACK_WIDE: return "OpenRackWide";
	case RACK_MOUNT_WIDTH_OPEN_U: return "OpenU";
	case RACK_MOUNT_WIDTH_EIA_310: return "EIA_310";
	case RACK_MOUNT_WIDTH_EIA_310__TELCO: return "EIA_310_Telco";
	case RACK_MOUNT_WIDTH_HALF_WIDTH: return "HalfWidth";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_CHASSIS_H */
