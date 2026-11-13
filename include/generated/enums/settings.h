/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_SETTINGS_H
#define REDFISH_ENUM_SETTINGS_H

#include <stddef.h>

/* Settings.OperationApplyTime */
enum rf_operation_apply_time {
	OPERATION_APPLY_TIME_INVALID = 0,
	OPERATION_APPLY_TIME_IMMEDIATE = 1,
	OPERATION_APPLY_TIME_ON_RESET = 2,
	OPERATION_APPLY_TIME_AT_MAINTENANCE_WINDOW_START = 3,
	OPERATION_APPLY_TIME_IN_MAINTENANCE_WINDOW_ON_RESET = 4,
	OPERATION_APPLY_TIME_ON_START_UPDATE_REQUEST = 5,
	OPERATION_APPLY_TIME_ON_TARGET_RESET = 6,
};

static inline const char *rf_operation_apply_time_to_str(enum rf_operation_apply_time val)
{
	switch (val) {
	case OPERATION_APPLY_TIME_INVALID: return "Invalid";
	case OPERATION_APPLY_TIME_IMMEDIATE: return "Immediate";
	case OPERATION_APPLY_TIME_ON_RESET: return "OnReset";
	case OPERATION_APPLY_TIME_AT_MAINTENANCE_WINDOW_START: return "AtMaintenanceWindowStart";
	case OPERATION_APPLY_TIME_IN_MAINTENANCE_WINDOW_ON_RESET: return "InMaintenanceWindowOnReset";
	case OPERATION_APPLY_TIME_ON_START_UPDATE_REQUEST: return "OnStartUpdateRequest";
	case OPERATION_APPLY_TIME_ON_TARGET_RESET: return "OnTargetReset";
	default: return NULL;
	}
}

/* Settings.v1_1_0.ApplyTime */
enum rf_apply_time {
	APPLY_TIME_INVALID = 0,
	APPLY_TIME_IMMEDIATE = 1,
	APPLY_TIME_ON_RESET = 2,
	APPLY_TIME_AT_MAINTENANCE_WINDOW_START = 3,
	APPLY_TIME_IN_MAINTENANCE_WINDOW_ON_RESET = 4,
	APPLY_TIME_DEFERRED = 5,
};

static inline const char *rf_apply_time_to_str(enum rf_apply_time val)
{
	switch (val) {
	case APPLY_TIME_INVALID: return "Invalid";
	case APPLY_TIME_IMMEDIATE: return "Immediate";
	case APPLY_TIME_ON_RESET: return "OnReset";
	case APPLY_TIME_AT_MAINTENANCE_WINDOW_START: return "AtMaintenanceWindowStart";
	case APPLY_TIME_IN_MAINTENANCE_WINDOW_ON_RESET: return "InMaintenanceWindowOnReset";
	case APPLY_TIME_DEFERRED: return "Deferred";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_SETTINGS_H */
