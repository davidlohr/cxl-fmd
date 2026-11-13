/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_POLICY_H
#define REDFISH_ENUM_POLICY_H

#include <stddef.h>

/* Policy.ConditionType */
enum rf_condition_type {
	CONDITION_TYPE_INVALID = 0,
	CONDITION_TYPE_AND = 1,
	CONDITION_TYPE_OR = 2,
	CONDITION_TYPE_COMPARE = 3,
	CONDITION_TYPE_READ = 4,
	CONDITION_TYPE_SUM = 5,
	CONDITION_TYPE_AVERAGE = 6,
	CONDITION_TYPE_EVENT = 7,
};

static inline const char *rf_condition_type_to_str(enum rf_condition_type val)
{
	switch (val) {
	case CONDITION_TYPE_INVALID: return "Invalid";
	case CONDITION_TYPE_AND: return "And";
	case CONDITION_TYPE_OR: return "Or";
	case CONDITION_TYPE_COMPARE: return "Compare";
	case CONDITION_TYPE_READ: return "Read";
	case CONDITION_TYPE_SUM: return "Sum";
	case CONDITION_TYPE_AVERAGE: return "Average";
	case CONDITION_TYPE_EVENT: return "Event";
	default: return NULL;
	}
}

/* Policy.ResponseAction */
enum rf_response_action {
	RESPONSE_ACTION_INVALID = 0,
	RESPONSE_ACTION_RECHECK = 1,
	RESPONSE_ACTION_EVENT = 2,
	RESPONSE_ACTION_RESET = 3,
	RESPONSE_ACTION_HTTP = 4,
	RESPONSE_ACTION_OEM = 5,
};

static inline const char *rf_response_action_to_str(enum rf_response_action val)
{
	switch (val) {
	case RESPONSE_ACTION_INVALID: return "Invalid";
	case RESPONSE_ACTION_RECHECK: return "Recheck";
	case RESPONSE_ACTION_EVENT: return "Event";
	case RESPONSE_ACTION_RESET: return "Reset";
	case RESPONSE_ACTION_HTTP: return "HTTP";
	case RESPONSE_ACTION_OEM: return "OEM";
	default: return NULL;
	}
}

/* Policy.v1_0_0.Operator */
enum rf_operator {
	OPERATOR_INVALID = 0,
	OPERATOR_EQUAL = 1,
	OPERATOR_NOT_EQUAL = 2,
	OPERATOR_GREATER_THAN = 3,
	OPERATOR_LESS_THAN = 4,
	OPERATOR_GREATER_THAN_OR_EQUAL = 5,
	OPERATOR_LESS_THAN_OR_EQUAL = 6,
};

static inline const char *rf_operator_to_str(enum rf_operator val)
{
	switch (val) {
	case OPERATOR_INVALID: return "Invalid";
	case OPERATOR_EQUAL: return "Equal";
	case OPERATOR_NOT_EQUAL: return "NotEqual";
	case OPERATOR_GREATER_THAN: return "GreaterThan";
	case OPERATOR_LESS_THAN: return "LessThan";
	case OPERATOR_GREATER_THAN_OR_EQUAL: return "GreaterThanOrEqual";
	case OPERATOR_LESS_THAN_OR_EQUAL: return "LessThanOrEqual";
	default: return NULL;
	}
}

/* Policy.v1_0_0.HTTPVerb */
enum rf_http_verb {
	HTTP_VERB_INVALID = 0,
	HTTP_VERB_GET = 1,
	HTTP_VERB_PATCH = 2,
	HTTP_VERB_POST = 3,
	HTTP_VERB_DELETE = 4,
};

static inline const char *rf_http_verb_to_str(enum rf_http_verb val)
{
	switch (val) {
	case HTTP_VERB_INVALID: return "Invalid";
	case HTTP_VERB_GET: return "GET";
	case HTTP_VERB_PATCH: return "PATCH";
	case HTTP_VERB_POST: return "POST";
	case HTTP_VERB_DELETE: return "DELETE";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_POLICY_H */
