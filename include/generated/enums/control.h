/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_CONTROL_H
#define REDFISH_ENUM_CONTROL_H

#include <stddef.h>

/* Control.v1_0_0.ControlType */
enum rf_control_type {
	CONTROL_TYPE_INVALID = 0,
	CONTROL_TYPE_TEMPERATURE = 1,
	CONTROL_TYPE_POWER = 2,
	CONTROL_TYPE_FREQUENCY = 3,
	CONTROL_TYPE_FREQUENCY_M_HZ = 4,
	CONTROL_TYPE_PRESSURE = 5,
	CONTROL_TYPE_PRESSUREK_PA = 6,
	CONTROL_TYPE_VALVE = 7,
	CONTROL_TYPE_PERCENT = 8,
	CONTROL_TYPE_DUTY_CYCLE = 9,
	CONTROL_TYPE_LINEAR_POSITION = 10,
	CONTROL_TYPE_LINEAR_VELOCITY = 11,
	CONTROL_TYPE_LINEAR_ACCELERATION = 12,
	CONTROL_TYPE_ROTATIONAL_POSITION = 13,
	CONTROL_TYPE_ROTATIONAL_VELOCITY = 14,
	CONTROL_TYPE_ROTATIONAL_ACCELERATION = 15,
	CONTROL_TYPE_LIQUID_FLOW_LPM = 16,
	CONTROL_TYPE_VOLTAGE = 17,
};

static inline const char *rf_control_type_to_str(enum rf_control_type val)
{
	switch (val) {
	case CONTROL_TYPE_INVALID: return "Invalid";
	case CONTROL_TYPE_TEMPERATURE: return "Temperature";
	case CONTROL_TYPE_POWER: return "Power";
	case CONTROL_TYPE_FREQUENCY: return "Frequency";
	case CONTROL_TYPE_FREQUENCY_M_HZ: return "FrequencyMHz";
	case CONTROL_TYPE_PRESSURE: return "Pressure";
	case CONTROL_TYPE_PRESSUREK_PA: return "PressurekPa";
	case CONTROL_TYPE_VALVE: return "Valve";
	case CONTROL_TYPE_PERCENT: return "Percent";
	case CONTROL_TYPE_DUTY_CYCLE: return "DutyCycle";
	case CONTROL_TYPE_LINEAR_POSITION: return "LinearPosition";
	case CONTROL_TYPE_LINEAR_VELOCITY: return "LinearVelocity";
	case CONTROL_TYPE_LINEAR_ACCELERATION: return "LinearAcceleration";
	case CONTROL_TYPE_ROTATIONAL_POSITION: return "RotationalPosition";
	case CONTROL_TYPE_ROTATIONAL_VELOCITY: return "RotationalVelocity";
	case CONTROL_TYPE_ROTATIONAL_ACCELERATION: return "RotationalAcceleration";
	case CONTROL_TYPE_LIQUID_FLOW_LPM: return "LiquidFlowLPM";
	case CONTROL_TYPE_VOLTAGE: return "Voltage";
	default: return NULL;
	}
}

/* Control.v1_0_0.SetPointType */
enum rf_set_point_type {
	SET_POINT_TYPE_INVALID = 0,
	SET_POINT_TYPE_SINGLE = 1,
	SET_POINT_TYPE_RANGE = 2,
	SET_POINT_TYPE_MONITOR = 3,
};

static inline const char *rf_set_point_type_to_str(enum rf_set_point_type val)
{
	switch (val) {
	case SET_POINT_TYPE_INVALID: return "Invalid";
	case SET_POINT_TYPE_SINGLE: return "Single";
	case SET_POINT_TYPE_RANGE: return "Range";
	case SET_POINT_TYPE_MONITOR: return "Monitor";
	default: return NULL;
	}
}

/* Control.v1_0_0.ControlMode */
enum rf_control_mode {
	CONTROL_MODE_INVALID = 0,
	CONTROL_MODE_AUTOMATIC = 1,
	CONTROL_MODE_OVERRIDE = 2,
	CONTROL_MODE_MANUAL = 3,
	CONTROL_MODE_DISABLED = 4,
};

static inline const char *rf_control_mode_to_str(enum rf_control_mode val)
{
	switch (val) {
	case CONTROL_MODE_INVALID: return "Invalid";
	case CONTROL_MODE_AUTOMATIC: return "Automatic";
	case CONTROL_MODE_OVERRIDE: return "Override";
	case CONTROL_MODE_MANUAL: return "Manual";
	case CONTROL_MODE_DISABLED: return "Disabled";
	default: return NULL;
	}
}

/* Control.v1_0_0.ImplementationType */
enum rf_implementation_type {
	IMPLEMENTATION_TYPE_INVALID = 0,
	IMPLEMENTATION_TYPE_PROGRAMMABLE = 1,
	IMPLEMENTATION_TYPE_DIRECT = 2,
	IMPLEMENTATION_TYPE_MONITORED = 3,
};

static inline const char *rf_implementation_type_to_str(enum rf_implementation_type val)
{
	switch (val) {
	case IMPLEMENTATION_TYPE_INVALID: return "Invalid";
	case IMPLEMENTATION_TYPE_PROGRAMMABLE: return "Programmable";
	case IMPLEMENTATION_TYPE_DIRECT: return "Direct";
	case IMPLEMENTATION_TYPE_MONITORED: return "Monitored";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_CONTROL_H */
