/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_RESOLUTION_STEP_H
#define REDFISH_ENUM_RESOLUTION_STEP_H

#include <stddef.h>

/* ResolutionStep.v1_0_0.ResolutionType */
enum rf_resolution_type {
	RESOLUTION_TYPE_INVALID = 0,
	RESOLUTION_TYPE_CONTACT_VENDOR = 1,
	RESOLUTION_TYPE_REPLACE_COMPONENT = 2,
	RESOLUTION_TYPE_FIRMWARE_UPDATE = 3,
	RESOLUTION_TYPE_RESET = 4,
	RESOLUTION_TYPE_POWER_CYCLE = 5,
	RESOLUTION_TYPE_RESET_TO_DEFAULTS = 6,
	RESOLUTION_TYPE_COLLECT_DIAGNOSTIC_DATA = 7,
	RESOLUTION_TYPE_OEM = 8,
	RESOLUTION_TYPE_RETRY_OPERATION = 9,
};

static inline const char *rf_resolution_type_to_str(enum rf_resolution_type val)
{
	switch (val) {
	case RESOLUTION_TYPE_INVALID: return "Invalid";
	case RESOLUTION_TYPE_CONTACT_VENDOR: return "ContactVendor";
	case RESOLUTION_TYPE_REPLACE_COMPONENT: return "ReplaceComponent";
	case RESOLUTION_TYPE_FIRMWARE_UPDATE: return "FirmwareUpdate";
	case RESOLUTION_TYPE_RESET: return "Reset";
	case RESOLUTION_TYPE_POWER_CYCLE: return "PowerCycle";
	case RESOLUTION_TYPE_RESET_TO_DEFAULTS: return "ResetToDefaults";
	case RESOLUTION_TYPE_COLLECT_DIAGNOSTIC_DATA: return "CollectDiagnosticData";
	case RESOLUTION_TYPE_OEM: return "OEM";
	case RESOLUTION_TYPE_RETRY_OPERATION: return "RetryOperation";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_RESOLUTION_STEP_H */
