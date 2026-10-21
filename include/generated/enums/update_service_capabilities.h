/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_UPDATE_SERVICE_CAPABILITIES_H
#define REDFISH_ENUM_UPDATE_SERVICE_CAPABILITIES_H

#include <stddef.h>

/* UpdateServiceCapabilities.v1_1_0.AllowableTargetReferenceType */
enum rf_allowable_target_reference_type {
	ALLOWABLE_TARGET_REFERENCE_TYPE_INVALID = 0,
	ALLOWABLE_TARGET_REFERENCE_TYPE_SOFTWARE_INVENTORY = 1,
	ALLOWABLE_TARGET_REFERENCE_TYPE_DEVICE_RESOURCE = 2,
	ALLOWABLE_TARGET_REFERENCE_TYPE_RESOURCE_COLLECTION = 3,
	ALLOWABLE_TARGET_REFERENCE_TYPE_AGGREGATE = 4,
	ALLOWABLE_TARGET_REFERENCE_TYPE_COMPUTER_SYSTEM = 5,
};

static inline const char *rf_allowable_target_reference_type_to_str(enum rf_allowable_target_reference_type val)
{
	switch (val) {
	case ALLOWABLE_TARGET_REFERENCE_TYPE_INVALID: return "Invalid";
	case ALLOWABLE_TARGET_REFERENCE_TYPE_SOFTWARE_INVENTORY: return "SoftwareInventory";
	case ALLOWABLE_TARGET_REFERENCE_TYPE_DEVICE_RESOURCE: return "DeviceResource";
	case ALLOWABLE_TARGET_REFERENCE_TYPE_RESOURCE_COLLECTION: return "ResourceCollection";
	case ALLOWABLE_TARGET_REFERENCE_TYPE_AGGREGATE: return "Aggregate";
	case ALLOWABLE_TARGET_REFERENCE_TYPE_COMPUTER_SYSTEM: return "ComputerSystem";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_UPDATE_SERVICE_CAPABILITIES_H */
