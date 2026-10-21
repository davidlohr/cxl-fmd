/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_SWITCH_H
#define REDFISH_ENUM_SWITCH_H

#include <stddef.h>

/* Switch.v1_11_0.TargetType */
enum rf_target_type {
	TARGET_TYPE_INVALID = 0,
	TARGET_TYPE_FABRIC_PORT = 1,
	TARGET_TYPE_HOST_EDGE_PORT = 2,
	TARGET_TYPE_DOWNSTREAM_EDGE_PORT = 3,
};

static inline const char *rf_target_type_to_str(enum rf_target_type val)
{
	switch (val) {
	case TARGET_TYPE_INVALID: return "Invalid";
	case TARGET_TYPE_FABRIC_PORT: return "FabricPort";
	case TARGET_TYPE_HOST_EDGE_PORT: return "HostEdgePort";
	case TARGET_TYPE_DOWNSTREAM_EDGE_PORT: return "DownstreamEdgePort";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_SWITCH_H */
