/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_POWER_SUBSYSTEM_H
#define REDFISH_ENUM_POWER_SUBSYSTEM_H

#include <stddef.h>

/* PowerSubsystem.v1_2_0.ThresholdLevel */
enum rf_threshold_level {
	THRESHOLD_LEVEL_INVALID = 0,
	THRESHOLD_LEVEL_DISABLED = 1,
	THRESHOLD_LEVEL_UPPER_CAUTION = 2,
	THRESHOLD_LEVEL_UPPER_CRITICAL = 3,
};

static inline const char *rf_threshold_level_to_str(enum rf_threshold_level val)
{
	switch (val) {
	case THRESHOLD_LEVEL_INVALID: return "Invalid";
	case THRESHOLD_LEVEL_DISABLED: return "Disabled";
	case THRESHOLD_LEVEL_UPPER_CAUTION: return "UpperCaution";
	case THRESHOLD_LEVEL_UPPER_CRITICAL: return "UpperCritical";
	default: return NULL;
	}
}

/* PowerSubsystem.v1_2_0.PowerBrakeReason */
enum rf_power_brake_reason {
	POWER_BRAKE_REASON_INVALID = 0,
	POWER_BRAKE_REASON_LOAD_PERCENT = 1,
	POWER_BRAKE_REASON_TEMPERATURE = 2,
	POWER_BRAKE_REASON_LOSS_OF_COMMS = 3,
	POWER_BRAKE_REASON_EXTERNAL_SIGNAL = 4,
};

static inline const char *rf_power_brake_reason_to_str(enum rf_power_brake_reason val)
{
	switch (val) {
	case POWER_BRAKE_REASON_INVALID: return "Invalid";
	case POWER_BRAKE_REASON_LOAD_PERCENT: return "LoadPercent";
	case POWER_BRAKE_REASON_TEMPERATURE: return "Temperature";
	case POWER_BRAKE_REASON_LOSS_OF_COMMS: return "LossOfComms";
	case POWER_BRAKE_REASON_EXTERNAL_SIGNAL: return "ExternalSignal";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_POWER_SUBSYSTEM_H */
