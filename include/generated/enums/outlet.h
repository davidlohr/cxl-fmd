/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_OUTLET_H
#define REDFISH_ENUM_OUTLET_H

#include <stddef.h>

/* Outlet.PowerState */
enum rf_power_state {
	POWER_STATE_INVALID = 0,
	POWER_STATE_ON = 1,
	POWER_STATE_OFF = 2,
	POWER_STATE_POWER_CYCLE = 3,
};

static inline const char *rf_power_state_to_str(enum rf_power_state val)
{
	switch (val) {
	case POWER_STATE_INVALID: return "Invalid";
	case POWER_STATE_ON: return "On";
	case POWER_STATE_OFF: return "Off";
	case POWER_STATE_POWER_CYCLE: return "PowerCycle";
	default: return NULL;
	}
}

/* Outlet.ReceptacleType */
enum rf_receptacle_type {
	RECEPTACLE_TYPE_INVALID = 0,
	RECEPTACLE_TYPE_NEMA_5_15_R = 1,
	RECEPTACLE_TYPE_NEMA_5_20_R = 2,
	RECEPTACLE_TYPE_NEMA_L5_20_R = 3,
	RECEPTACLE_TYPE_NEMA_L5_30_R = 4,
	RECEPTACLE_TYPE_NEMA_L6_20_R = 5,
	RECEPTACLE_TYPE_NEMA_L6_30_R = 6,
	RECEPTACLE_TYPE_IEC_60320_C13 = 7,
	RECEPTACLE_TYPE_IEC_60320_C15 = 8,
	RECEPTACLE_TYPE_IEC_60320_C19 = 9,
	RECEPTACLE_TYPE_IEC_60320_C21 = 10,
	RECEPTACLE_TYPE_CEE_7__TYPE_E = 11,
	RECEPTACLE_TYPE_CEE_7__TYPE_F = 12,
	RECEPTACLE_TYPE_SEV_1011_TYPE_12 = 13,
	RECEPTACLE_TYPE_SEV_1011_TYPE_23 = 14,
	RECEPTACLE_TYPE_BS_1363__TYPE_G = 15,
	RECEPTACLE_TYPE_BUS_CONNECTION = 16,
	RECEPTACLE_TYPE_COMBINATION = 17,
};

static inline const char *rf_receptacle_type_to_str(enum rf_receptacle_type val)
{
	switch (val) {
	case RECEPTACLE_TYPE_INVALID: return "Invalid";
	case RECEPTACLE_TYPE_NEMA_5_15_R: return "NEMA_5_15R";
	case RECEPTACLE_TYPE_NEMA_5_20_R: return "NEMA_5_20R";
	case RECEPTACLE_TYPE_NEMA_L5_20_R: return "NEMA_L5_20R";
	case RECEPTACLE_TYPE_NEMA_L5_30_R: return "NEMA_L5_30R";
	case RECEPTACLE_TYPE_NEMA_L6_20_R: return "NEMA_L6_20R";
	case RECEPTACLE_TYPE_NEMA_L6_30_R: return "NEMA_L6_30R";
	case RECEPTACLE_TYPE_IEC_60320_C13: return "IEC_60320_C13";
	case RECEPTACLE_TYPE_IEC_60320_C15: return "IEC_60320_C15";
	case RECEPTACLE_TYPE_IEC_60320_C19: return "IEC_60320_C19";
	case RECEPTACLE_TYPE_IEC_60320_C21: return "IEC_60320_C21";
	case RECEPTACLE_TYPE_CEE_7__TYPE_E: return "CEE_7_Type_E";
	case RECEPTACLE_TYPE_CEE_7__TYPE_F: return "CEE_7_Type_F";
	case RECEPTACLE_TYPE_SEV_1011_TYPE_12: return "SEV_1011_TYPE_12";
	case RECEPTACLE_TYPE_SEV_1011_TYPE_23: return "SEV_1011_TYPE_23";
	case RECEPTACLE_TYPE_BS_1363__TYPE_G: return "BS_1363_Type_G";
	case RECEPTACLE_TYPE_BUS_CONNECTION: return "BusConnection";
	case RECEPTACLE_TYPE_COMBINATION: return "Combination";
	default: return NULL;
	}
}

/* Outlet.v1_0_0.VoltageType */
enum rf_voltage_type {
	VOLTAGE_TYPE_INVALID = 0,
	VOLTAGE_TYPE_AC = 1,
	VOLTAGE_TYPE_DC = 2,
};

static inline const char *rf_voltage_type_to_str(enum rf_voltage_type val)
{
	switch (val) {
	case VOLTAGE_TYPE_INVALID: return "Invalid";
	case VOLTAGE_TYPE_AC: return "AC";
	case VOLTAGE_TYPE_DC: return "DC";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_OUTLET_H */
