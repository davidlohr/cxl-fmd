/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_REDFISH_EXTENSIONS_H
#define REDFISH_ENUM_REDFISH_EXTENSIONS_H

#include <stddef.h>

/* RedfishExtensions.v1_0_0.ClientContextStatusType */
enum rf_client_context_status_type {
	CLIENT_CONTEXT_STATUS_TYPE_APPLIED = 0,
	CLIENT_CONTEXT_STATUS_TYPE_INVALID = 1,
	CLIENT_CONTEXT_STATUS_TYPE_NOT_FOUND = 2,
	CLIENT_CONTEXT_STATUS_TYPE_UNSUPPORTED = 3,
};

static inline const char *rf_client_context_status_type_to_str(enum rf_client_context_status_type val)
{
	switch (val) {
	case CLIENT_CONTEXT_STATUS_TYPE_APPLIED: return "Applied";
	case CLIENT_CONTEXT_STATUS_TYPE_INVALID: return "Invalid";
	case CLIENT_CONTEXT_STATUS_TYPE_NOT_FOUND: return "NotFound";
	case CLIENT_CONTEXT_STATUS_TYPE_UNSUPPORTED: return "Unsupported";
	default: return NULL;
	}
}

/* RedfishExtensions.v1_0_0.ReleaseStatusType */
enum rf_release_status_type {
	RELEASE_STATUS_TYPE_INVALID = 0,
	RELEASE_STATUS_TYPE_STANDARD = 1,
	RELEASE_STATUS_TYPE_INFORMATIONAL = 2,
	RELEASE_STATUS_TYPE_WORK_IN_PROGRESS = 3,
	RELEASE_STATUS_TYPE_IN_DEVELOPMENT = 4,
};

static inline const char *rf_release_status_type_to_str(enum rf_release_status_type val)
{
	switch (val) {
	case RELEASE_STATUS_TYPE_INVALID: return "Invalid";
	case RELEASE_STATUS_TYPE_STANDARD: return "Standard";
	case RELEASE_STATUS_TYPE_INFORMATIONAL: return "Informational";
	case RELEASE_STATUS_TYPE_WORK_IN_PROGRESS: return "WorkInProgress";
	case RELEASE_STATUS_TYPE_IN_DEVELOPMENT: return "InDevelopment";
	default: return NULL;
	}
}

/* RedfishExtensions.v1_0_0.RevisionKind */
enum rf_revision_kind {
	REVISION_KIND_INVALID = 0,
	REVISION_KIND_ADDED = 1,
	REVISION_KIND_MODIFIED = 2,
	REVISION_KIND_DEPRECATED = 3,
};

static inline const char *rf_revision_kind_to_str(enum rf_revision_kind val)
{
	switch (val) {
	case REVISION_KIND_INVALID: return "Invalid";
	case REVISION_KIND_ADDED: return "Added";
	case REVISION_KIND_MODIFIED: return "Modified";
	case REVISION_KIND_DEPRECATED: return "Deprecated";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_REDFISH_EXTENSIONS_H */
