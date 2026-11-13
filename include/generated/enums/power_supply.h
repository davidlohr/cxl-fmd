/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_POWER_SUPPLY_H
#define REDFISH_ENUM_POWER_SUPPLY_H

#include <stddef.h>

/* PowerSupply.v1_0_0.PowerSupplyType */
enum rf_power_supply_type {
	POWER_SUPPLY_TYPE_INVALID = 0,
	POWER_SUPPLY_TYPE_AC = 1,
	POWER_SUPPLY_TYPE_DC = 2,
	POWER_SUPPLY_TYPE_A_COR_DC = 3,
	POWER_SUPPLY_TYPE_DC_REGULATOR = 4,
	POWER_SUPPLY_TYPE_PSU = 5,
};

static inline const char *rf_power_supply_type_to_str(enum rf_power_supply_type val)
{
	switch (val) {
	case POWER_SUPPLY_TYPE_INVALID: return "Invalid";
	case POWER_SUPPLY_TYPE_AC: return "AC";
	case POWER_SUPPLY_TYPE_DC: return "DC";
	case POWER_SUPPLY_TYPE_A_COR_DC: return "ACorDC";
	case POWER_SUPPLY_TYPE_DC_REGULATOR: return "DCRegulator";
	case POWER_SUPPLY_TYPE_PSU: return "PSU";
	default: return NULL;
	}
}

/* PowerSupply.v1_3_0.LineStatus */
enum rf_line_status {
	LINE_STATUS_INVALID = 0,
	LINE_STATUS_NORMAL = 1,
	LINE_STATUS_LOSS_OF_INPUT = 2,
	LINE_STATUS_OUT_OF_RANGE = 3,
};

static inline const char *rf_line_status_to_str(enum rf_line_status val)
{
	switch (val) {
	case LINE_STATUS_INVALID: return "Invalid";
	case LINE_STATUS_NORMAL: return "Normal";
	case LINE_STATUS_LOSS_OF_INPUT: return "LossOfInput";
	case LINE_STATUS_OUT_OF_RANGE: return "OutOfRange";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_POWER_SUPPLY_H */
