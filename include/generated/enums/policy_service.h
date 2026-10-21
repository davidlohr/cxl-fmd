/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_POLICY_SERVICE_H
#define REDFISH_ENUM_POLICY_SERVICE_H

#include <stddef.h>

/* PolicyService.v1_0_0.OperatingMode */
enum rf_operating_mode {
	OPERATING_MODE_INVALID = 0,
	OPERATING_MODE_DISABLED = 1,
	OPERATING_MODE_ALERT_ONLY = 2,
	OPERATING_MODE_ENABLED = 3,
};

static inline const char *rf_operating_mode_to_str(enum rf_operating_mode val)
{
	switch (val) {
	case OPERATING_MODE_INVALID: return "Invalid";
	case OPERATING_MODE_DISABLED: return "Disabled";
	case OPERATING_MODE_ALERT_ONLY: return "AlertOnly";
	case OPERATING_MODE_ENABLED: return "Enabled";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_POLICY_SERVICE_H */
