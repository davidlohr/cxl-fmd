/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_SOFTWARE_INVENTORY_H
#define REDFISH_ENUM_SOFTWARE_INVENTORY_H

#include <stddef.h>

/* SoftwareInventory.v1_9_0.VersionScheme */
enum rf_version_scheme {
	VERSION_SCHEME_INVALID = 0,
	VERSION_SCHEME_SEM_VER = 1,
	VERSION_SCHEME_DOT_INTEGER_NOTATION = 2,
	VERSION_SCHEME_OEM = 3,
};

static inline const char *rf_version_scheme_to_str(enum rf_version_scheme val)
{
	switch (val) {
	case VERSION_SCHEME_INVALID: return "Invalid";
	case VERSION_SCHEME_SEM_VER: return "SemVer";
	case VERSION_SCHEME_DOT_INTEGER_NOTATION: return "DotIntegerNotation";
	case VERSION_SCHEME_OEM: return "OEM";
	default: return NULL;
	}
}

/* SoftwareInventory.v1_10_0.ReleaseType */
enum rf_release_type {
	RELEASE_TYPE_INVALID = 0,
	RELEASE_TYPE_PRODUCTION = 1,
	RELEASE_TYPE_PROTOTYPE = 2,
	RELEASE_TYPE_OTHER = 3,
};

static inline const char *rf_release_type_to_str(enum rf_release_type val)
{
	switch (val) {
	case RELEASE_TYPE_INVALID: return "Invalid";
	case RELEASE_TYPE_PRODUCTION: return "Production";
	case RELEASE_TYPE_PROTOTYPE: return "Prototype";
	case RELEASE_TYPE_OTHER: return "Other";
	default: return NULL;
	}
}

/* SoftwareInventory.v1_15_0.ImageState */
enum rf_image_state {
	IMAGE_STATE_INVALID = 0,
	IMAGE_STATE_ACTIVE = 1,
	IMAGE_STATE_STAGED = 2,
	IMAGE_STATE_ARMED = 3,
	IMAGE_STATE_TARGET_SPECIFIC = 4,
	IMAGE_STATE_OEM = 5,
};

static inline const char *rf_image_state_to_str(enum rf_image_state val)
{
	switch (val) {
	case IMAGE_STATE_INVALID: return "Invalid";
	case IMAGE_STATE_ACTIVE: return "Active";
	case IMAGE_STATE_STAGED: return "Staged";
	case IMAGE_STATE_ARMED: return "Armed";
	case IMAGE_STATE_TARGET_SPECIFIC: return "TargetSpecific";
	case IMAGE_STATE_OEM: return "OEM";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_SOFTWARE_INVENTORY_H */
