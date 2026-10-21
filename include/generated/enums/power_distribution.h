/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_POWER_DISTRIBUTION_H
#define REDFISH_ENUM_POWER_DISTRIBUTION_H

#include <stddef.h>

/* PowerDistribution.PowerState */
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

/* PowerDistribution.v1_0_0.PowerEquipmentType */
enum rf_power_equipment_type {
	POWER_EQUIPMENT_TYPE_INVALID = 0,
	POWER_EQUIPMENT_TYPE_RACK_PDU = 1,
	POWER_EQUIPMENT_TYPE_FLOOR_PDU = 2,
	POWER_EQUIPMENT_TYPE_MANUAL_TRANSFER_SWITCH = 3,
	POWER_EQUIPMENT_TYPE_AUTOMATIC_TRANSFER_SWITCH = 4,
	POWER_EQUIPMENT_TYPE_SWITCHGEAR = 5,
	POWER_EQUIPMENT_TYPE_POWER_SHELF = 6,
	POWER_EQUIPMENT_TYPE_BUS = 7,
	POWER_EQUIPMENT_TYPE_BATTERY_SHELF = 8,
};

static inline const char *rf_power_equipment_type_to_str(enum rf_power_equipment_type val)
{
	switch (val) {
	case POWER_EQUIPMENT_TYPE_INVALID: return "Invalid";
	case POWER_EQUIPMENT_TYPE_RACK_PDU: return "RackPDU";
	case POWER_EQUIPMENT_TYPE_FLOOR_PDU: return "FloorPDU";
	case POWER_EQUIPMENT_TYPE_MANUAL_TRANSFER_SWITCH: return "ManualTransferSwitch";
	case POWER_EQUIPMENT_TYPE_AUTOMATIC_TRANSFER_SWITCH: return "AutomaticTransferSwitch";
	case POWER_EQUIPMENT_TYPE_SWITCHGEAR: return "Switchgear";
	case POWER_EQUIPMENT_TYPE_POWER_SHELF: return "PowerShelf";
	case POWER_EQUIPMENT_TYPE_BUS: return "Bus";
	case POWER_EQUIPMENT_TYPE_BATTERY_SHELF: return "BatteryShelf";
	default: return NULL;
	}
}

/* PowerDistribution.v1_0_0.TransferSensitivityType */
enum rf_transfer_sensitivity_type {
	TRANSFER_SENSITIVITY_TYPE_INVALID = 0,
	TRANSFER_SENSITIVITY_TYPE_HIGH = 1,
	TRANSFER_SENSITIVITY_TYPE_MEDIUM = 2,
	TRANSFER_SENSITIVITY_TYPE_LOW = 3,
};

static inline const char *rf_transfer_sensitivity_type_to_str(enum rf_transfer_sensitivity_type val)
{
	switch (val) {
	case TRANSFER_SENSITIVITY_TYPE_INVALID: return "Invalid";
	case TRANSFER_SENSITIVITY_TYPE_HIGH: return "High";
	case TRANSFER_SENSITIVITY_TYPE_MEDIUM: return "Medium";
	case TRANSFER_SENSITIVITY_TYPE_LOW: return "Low";
	default: return NULL;
	}
}

/* PowerDistribution.v1_5_0.ExportType */
enum rf_export_type {
	EXPORT_TYPE_INVALID = 0,
	EXPORT_TYPE_CLONE = 1,
	EXPORT_TYPE_REPLACEMENT = 2,
};

static inline const char *rf_export_type_to_str(enum rf_export_type val)
{
	switch (val) {
	case EXPORT_TYPE_INVALID: return "Invalid";
	case EXPORT_TYPE_CLONE: return "Clone";
	case EXPORT_TYPE_REPLACEMENT: return "Replacement";
	default: return NULL;
	}
}

/* PowerDistribution.v1_5_0.Component */
enum rf_component {
	COMPONENT_INVALID = 0,
	COMPONENT_ALL = 1,
	COMPONENT_MANAGER = 2,
	COMPONENT_MANAGER_ACCOUNTS = 3,
	COMPONENT_POWER_DISTRIBUTION = 4,
};

static inline const char *rf_component_to_str(enum rf_component val)
{
	switch (val) {
	case COMPONENT_INVALID: return "Invalid";
	case COMPONENT_ALL: return "All";
	case COMPONENT_MANAGER: return "Manager";
	case COMPONENT_MANAGER_ACCOUNTS: return "ManagerAccounts";
	case COMPONENT_POWER_DISTRIBUTION: return "PowerDistribution";
	default: return NULL;
	}
}

/* PowerDistribution.v1_5_0.ExportSecurity */
enum rf_export_security {
	EXPORT_SECURITY_INVALID = 0,
	EXPORT_SECURITY_INCLUDE_SENSITIVE_DATA = 1,
	EXPORT_SECURITY_HASHED_DATA_ONLY = 2,
	EXPORT_SECURITY_EXCLUDE_SENSITIVE_DATA = 3,
};

static inline const char *rf_export_security_to_str(enum rf_export_security val)
{
	switch (val) {
	case EXPORT_SECURITY_INVALID: return "Invalid";
	case EXPORT_SECURITY_INCLUDE_SENSITIVE_DATA: return "IncludeSensitiveData";
	case EXPORT_SECURITY_HASHED_DATA_ONLY: return "HashedDataOnly";
	case EXPORT_SECURITY_EXCLUDE_SENSITIVE_DATA: return "ExcludeSensitiveData";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_POWER_DISTRIBUTION_H */
