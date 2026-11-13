/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_ATTRIBUTE_REGISTRY_H
#define REDFISH_ENUM_ATTRIBUTE_REGISTRY_H

#include <stddef.h>

/* AttributeRegistry.v1_0_0.AttributeType */
enum rf_attribute_type {
	ATTRIBUTE_TYPE_INVALID = 0,
	ATTRIBUTE_TYPE_ENUMERATION = 1,
	ATTRIBUTE_TYPE_STRING = 2,
	ATTRIBUTE_TYPE_INTEGER = 3,
	ATTRIBUTE_TYPE_BOOLEAN = 4,
	ATTRIBUTE_TYPE_PASSWORD = 5,
};

static inline const char *rf_attribute_type_to_str(enum rf_attribute_type val)
{
	switch (val) {
	case ATTRIBUTE_TYPE_INVALID: return "Invalid";
	case ATTRIBUTE_TYPE_ENUMERATION: return "Enumeration";
	case ATTRIBUTE_TYPE_STRING: return "String";
	case ATTRIBUTE_TYPE_INTEGER: return "Integer";
	case ATTRIBUTE_TYPE_BOOLEAN: return "Boolean";
	case ATTRIBUTE_TYPE_PASSWORD: return "Password";
	default: return NULL;
	}
}

/* AttributeRegistry.v1_0_0.DependencyType */
enum rf_dependency_type {
	DEPENDENCY_TYPE_INVALID = 0,
	DEPENDENCY_TYPE_MAP = 1,
};

static inline const char *rf_dependency_type_to_str(enum rf_dependency_type val)
{
	switch (val) {
	case DEPENDENCY_TYPE_INVALID: return "Invalid";
	case DEPENDENCY_TYPE_MAP: return "Map";
	default: return NULL;
	}
}

/* AttributeRegistry.v1_0_0.MapFromCondition */
enum rf_map_from_condition {
	MAP_FROM_CONDITION_INVALID = 0,
	MAP_FROM_CONDITION_EQU = 1,
	MAP_FROM_CONDITION_NEQ = 2,
	MAP_FROM_CONDITION_GTR = 3,
	MAP_FROM_CONDITION_GEQ = 4,
	MAP_FROM_CONDITION_LSS = 5,
	MAP_FROM_CONDITION_LEQ = 6,
	MAP_FROM_CONDITION_BITAND = 7,
	MAP_FROM_CONDITION_BITOR = 8,
};

static inline const char *rf_map_from_condition_to_str(enum rf_map_from_condition val)
{
	switch (val) {
	case MAP_FROM_CONDITION_INVALID: return "Invalid";
	case MAP_FROM_CONDITION_EQU: return "EQU";
	case MAP_FROM_CONDITION_NEQ: return "NEQ";
	case MAP_FROM_CONDITION_GTR: return "GTR";
	case MAP_FROM_CONDITION_GEQ: return "GEQ";
	case MAP_FROM_CONDITION_LSS: return "LSS";
	case MAP_FROM_CONDITION_LEQ: return "LEQ";
	case MAP_FROM_CONDITION_BITAND: return "BITAND";
	case MAP_FROM_CONDITION_BITOR: return "BITOR";
	default: return NULL;
	}
}

/* AttributeRegistry.v1_0_0.MapFromProperty */
enum rf_map_from_property {
	MAP_FROM_PROPERTY_INVALID = 0,
	MAP_FROM_PROPERTY_CURRENT_VALUE = 1,
	MAP_FROM_PROPERTY_DEFAULT_VALUE = 2,
	MAP_FROM_PROPERTY_READ_ONLY = 3,
	MAP_FROM_PROPERTY_WRITE_ONLY = 4,
	MAP_FROM_PROPERTY_GRAY_OUT = 5,
	MAP_FROM_PROPERTY_HIDDEN = 6,
	MAP_FROM_PROPERTY_LOWER_BOUND = 7,
	MAP_FROM_PROPERTY_UPPER_BOUND = 8,
	MAP_FROM_PROPERTY_MIN_LENGTH = 9,
	MAP_FROM_PROPERTY_MAX_LENGTH = 10,
	MAP_FROM_PROPERTY_SCALAR_INCREMENT = 11,
};

static inline const char *rf_map_from_property_to_str(enum rf_map_from_property val)
{
	switch (val) {
	case MAP_FROM_PROPERTY_INVALID: return "Invalid";
	case MAP_FROM_PROPERTY_CURRENT_VALUE: return "CurrentValue";
	case MAP_FROM_PROPERTY_DEFAULT_VALUE: return "DefaultValue";
	case MAP_FROM_PROPERTY_READ_ONLY: return "ReadOnly";
	case MAP_FROM_PROPERTY_WRITE_ONLY: return "WriteOnly";
	case MAP_FROM_PROPERTY_GRAY_OUT: return "GrayOut";
	case MAP_FROM_PROPERTY_HIDDEN: return "Hidden";
	case MAP_FROM_PROPERTY_LOWER_BOUND: return "LowerBound";
	case MAP_FROM_PROPERTY_UPPER_BOUND: return "UpperBound";
	case MAP_FROM_PROPERTY_MIN_LENGTH: return "MinLength";
	case MAP_FROM_PROPERTY_MAX_LENGTH: return "MaxLength";
	case MAP_FROM_PROPERTY_SCALAR_INCREMENT: return "ScalarIncrement";
	default: return NULL;
	}
}

/* AttributeRegistry.v1_0_0.MapTerms */
enum rf_map_terms {
	MAP_TERMS_INVALID = 0,
	MAP_TERMS_AND = 1,
	MAP_TERMS_OR = 2,
	MAP_TERMS_NOT = 3,
};

static inline const char *rf_map_terms_to_str(enum rf_map_terms val)
{
	switch (val) {
	case MAP_TERMS_INVALID: return "Invalid";
	case MAP_TERMS_AND: return "AND";
	case MAP_TERMS_OR: return "OR";
	case MAP_TERMS_NOT: return "NOT";
	default: return NULL;
	}
}

/* AttributeRegistry.v1_0_0.MapToProperty */
enum rf_map_to_property {
	MAP_TO_PROPERTY_INVALID = 0,
	MAP_TO_PROPERTY_CURRENT_VALUE = 1,
	MAP_TO_PROPERTY_DEFAULT_VALUE = 2,
	MAP_TO_PROPERTY_READ_ONLY = 3,
	MAP_TO_PROPERTY_WRITE_ONLY = 4,
	MAP_TO_PROPERTY_GRAY_OUT = 5,
	MAP_TO_PROPERTY_HIDDEN = 6,
	MAP_TO_PROPERTY_IMMUTABLE = 7,
	MAP_TO_PROPERTY_HELP_TEXT = 8,
	MAP_TO_PROPERTY_WARNING_TEXT = 9,
	MAP_TO_PROPERTY_DISPLAY_NAME = 10,
	MAP_TO_PROPERTY_DISPLAY_ORDER = 11,
	MAP_TO_PROPERTY_LOWER_BOUND = 12,
	MAP_TO_PROPERTY_UPPER_BOUND = 13,
	MAP_TO_PROPERTY_MIN_LENGTH = 14,
	MAP_TO_PROPERTY_MAX_LENGTH = 15,
	MAP_TO_PROPERTY_SCALAR_INCREMENT = 16,
	MAP_TO_PROPERTY_VALUE_EXPRESSION = 17,
};

static inline const char *rf_map_to_property_to_str(enum rf_map_to_property val)
{
	switch (val) {
	case MAP_TO_PROPERTY_INVALID: return "Invalid";
	case MAP_TO_PROPERTY_CURRENT_VALUE: return "CurrentValue";
	case MAP_TO_PROPERTY_DEFAULT_VALUE: return "DefaultValue";
	case MAP_TO_PROPERTY_READ_ONLY: return "ReadOnly";
	case MAP_TO_PROPERTY_WRITE_ONLY: return "WriteOnly";
	case MAP_TO_PROPERTY_GRAY_OUT: return "GrayOut";
	case MAP_TO_PROPERTY_HIDDEN: return "Hidden";
	case MAP_TO_PROPERTY_IMMUTABLE: return "Immutable";
	case MAP_TO_PROPERTY_HELP_TEXT: return "HelpText";
	case MAP_TO_PROPERTY_WARNING_TEXT: return "WarningText";
	case MAP_TO_PROPERTY_DISPLAY_NAME: return "DisplayName";
	case MAP_TO_PROPERTY_DISPLAY_ORDER: return "DisplayOrder";
	case MAP_TO_PROPERTY_LOWER_BOUND: return "LowerBound";
	case MAP_TO_PROPERTY_UPPER_BOUND: return "UpperBound";
	case MAP_TO_PROPERTY_MIN_LENGTH: return "MinLength";
	case MAP_TO_PROPERTY_MAX_LENGTH: return "MaxLength";
	case MAP_TO_PROPERTY_SCALAR_INCREMENT: return "ScalarIncrement";
	case MAP_TO_PROPERTY_VALUE_EXPRESSION: return "ValueExpression";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_ATTRIBUTE_REGISTRY_H */
