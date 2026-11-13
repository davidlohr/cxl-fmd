/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_MANAGER_H
#define REDFISH_ENUM_MANAGER_H

#include <stddef.h>

/* Manager.v1_0_0.ManagerType */
enum rf_manager_type {
	MANAGER_TYPE_INVALID = 0,
	MANAGER_TYPE_MANAGEMENT_CONTROLLER = 1,
	MANAGER_TYPE_ENCLOSURE_MANAGER = 2,
	MANAGER_TYPE_BMC = 3,
	MANAGER_TYPE_RACK_MANAGER = 4,
	MANAGER_TYPE_AUXILIARY_CONTROLLER = 5,
	MANAGER_TYPE_SERVICE = 6,
	MANAGER_TYPE_FABRIC_MANAGER = 7,
};

static inline const char *rf_manager_type_to_str(enum rf_manager_type val)
{
	switch (val) {
	case MANAGER_TYPE_INVALID: return "Invalid";
	case MANAGER_TYPE_MANAGEMENT_CONTROLLER: return "ManagementController";
	case MANAGER_TYPE_ENCLOSURE_MANAGER: return "EnclosureManager";
	case MANAGER_TYPE_BMC: return "BMC";
	case MANAGER_TYPE_RACK_MANAGER: return "RackManager";
	case MANAGER_TYPE_AUXILIARY_CONTROLLER: return "AuxiliaryController";
	case MANAGER_TYPE_SERVICE: return "Service";
	case MANAGER_TYPE_FABRIC_MANAGER: return "FabricManager";
	default: return NULL;
	}
}

/* Manager.v1_0_0.SerialConnectTypesSupported */
enum rf_serial_connect_types_supported {
	SERIAL_CONNECT_TYPES_SUPPORTED_INVALID = 0,
	SERIAL_CONNECT_TYPES_SUPPORTED_SSH = 1,
	SERIAL_CONNECT_TYPES_SUPPORTED_TELNET = 2,
	SERIAL_CONNECT_TYPES_SUPPORTED_IPMI = 3,
	SERIAL_CONNECT_TYPES_SUPPORTED_OEM = 4,
};

static inline const char *rf_serial_connect_types_supported_to_str(enum rf_serial_connect_types_supported val)
{
	switch (val) {
	case SERIAL_CONNECT_TYPES_SUPPORTED_INVALID: return "Invalid";
	case SERIAL_CONNECT_TYPES_SUPPORTED_SSH: return "SSH";
	case SERIAL_CONNECT_TYPES_SUPPORTED_TELNET: return "Telnet";
	case SERIAL_CONNECT_TYPES_SUPPORTED_IPMI: return "IPMI";
	case SERIAL_CONNECT_TYPES_SUPPORTED_OEM: return "Oem";
	default: return NULL;
	}
}

/* Manager.v1_0_0.CommandConnectTypesSupported */
enum rf_command_connect_types_supported {
	COMMAND_CONNECT_TYPES_SUPPORTED_INVALID = 0,
	COMMAND_CONNECT_TYPES_SUPPORTED_SSH = 1,
	COMMAND_CONNECT_TYPES_SUPPORTED_TELNET = 2,
	COMMAND_CONNECT_TYPES_SUPPORTED_IPMI = 3,
	COMMAND_CONNECT_TYPES_SUPPORTED_OEM = 4,
};

static inline const char *rf_command_connect_types_supported_to_str(enum rf_command_connect_types_supported val)
{
	switch (val) {
	case COMMAND_CONNECT_TYPES_SUPPORTED_INVALID: return "Invalid";
	case COMMAND_CONNECT_TYPES_SUPPORTED_SSH: return "SSH";
	case COMMAND_CONNECT_TYPES_SUPPORTED_TELNET: return "Telnet";
	case COMMAND_CONNECT_TYPES_SUPPORTED_IPMI: return "IPMI";
	case COMMAND_CONNECT_TYPES_SUPPORTED_OEM: return "Oem";
	default: return NULL;
	}
}

/* Manager.v1_0_0.GraphicalConnectTypesSupported */
enum rf_graphical_connect_types_supported {
	GRAPHICAL_CONNECT_TYPES_SUPPORTED_INVALID = 0,
	GRAPHICAL_CONNECT_TYPES_SUPPORTED_KVMIP = 1,
	GRAPHICAL_CONNECT_TYPES_SUPPORTED_OEM = 2,
};

static inline const char *rf_graphical_connect_types_supported_to_str(enum rf_graphical_connect_types_supported val)
{
	switch (val) {
	case GRAPHICAL_CONNECT_TYPES_SUPPORTED_INVALID: return "Invalid";
	case GRAPHICAL_CONNECT_TYPES_SUPPORTED_KVMIP: return "KVMIP";
	case GRAPHICAL_CONNECT_TYPES_SUPPORTED_OEM: return "Oem";
	default: return NULL;
	}
}

/* Manager.v1_8_0.ResetToDefaultsType */
enum rf_reset_to_defaults_type {
	RESET_TO_DEFAULTS_TYPE_INVALID = 0,
	RESET_TO_DEFAULTS_TYPE_RESET_ALL = 1,
	RESET_TO_DEFAULTS_TYPE_PRESERVE_NETWORK_AND_USERS = 2,
	RESET_TO_DEFAULTS_TYPE_PRESERVE_NETWORK = 3,
	RESET_TO_DEFAULTS_TYPE_SENSOR_THRESHOLDS = 4,
};

static inline const char *rf_reset_to_defaults_type_to_str(enum rf_reset_to_defaults_type val)
{
	switch (val) {
	case RESET_TO_DEFAULTS_TYPE_INVALID: return "Invalid";
	case RESET_TO_DEFAULTS_TYPE_RESET_ALL: return "ResetAll";
	case RESET_TO_DEFAULTS_TYPE_PRESERVE_NETWORK_AND_USERS: return "PreserveNetworkAndUsers";
	case RESET_TO_DEFAULTS_TYPE_PRESERVE_NETWORK: return "PreserveNetwork";
	case RESET_TO_DEFAULTS_TYPE_SENSOR_THRESHOLDS: return "SensorThresholds";
	default: return NULL;
	}
}

/* Manager.v1_20_0.DateTimeSource */
enum rf_date_time_source {
	DATE_TIME_SOURCE_INVALID = 0,
	DATE_TIME_SOURCE_RTC = 1,
	DATE_TIME_SOURCE_FIRMWARE = 2,
	DATE_TIME_SOURCE_HOST = 3,
	DATE_TIME_SOURCE_NTP = 4,
	DATE_TIME_SOURCE_PTP = 5,
	DATE_TIME_SOURCE_DHCP = 6,
};

static inline const char *rf_date_time_source_to_str(enum rf_date_time_source val)
{
	switch (val) {
	case DATE_TIME_SOURCE_INVALID: return "Invalid";
	case DATE_TIME_SOURCE_RTC: return "RTC";
	case DATE_TIME_SOURCE_FIRMWARE: return "Firmware";
	case DATE_TIME_SOURCE_HOST: return "Host";
	case DATE_TIME_SOURCE_NTP: return "NTP";
	case DATE_TIME_SOURCE_PTP: return "PTP";
	case DATE_TIME_SOURCE_DHCP: return "DHCP";
	default: return NULL;
	}
}

/* Manager.v1_21_0.SecurityModeTypes */
enum rf_security_mode_types {
	SECURITY_MODE_TYPES_INVALID = 0,
	SECURITY_MODE_TYPES_FIPS_140_2 = 1,
	SECURITY_MODE_TYPES_FIPS_140_3 = 2,
	SECURITY_MODE_TYPES_CNSA_1_0 = 3,
	SECURITY_MODE_TYPES_CNSA_2_0 = 4,
	SECURITY_MODE_TYPES_SUITE_B = 5,
	SECURITY_MODE_TYPES_OEM = 6,
	SECURITY_MODE_TYPES_DEFAULT = 7,
};

static inline const char *rf_security_mode_types_to_str(enum rf_security_mode_types val)
{
	switch (val) {
	case SECURITY_MODE_TYPES_INVALID: return "Invalid";
	case SECURITY_MODE_TYPES_FIPS_140_2: return "FIPS_140_2";
	case SECURITY_MODE_TYPES_FIPS_140_3: return "FIPS_140_3";
	case SECURITY_MODE_TYPES_CNSA_1_0: return "CNSA_1_0";
	case SECURITY_MODE_TYPES_CNSA_2_0: return "CNSA_2_0";
	case SECURITY_MODE_TYPES_SUITE_B: return "SuiteB";
	case SECURITY_MODE_TYPES_OEM: return "OEM";
	case SECURITY_MODE_TYPES_DEFAULT: return "Default";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_MANAGER_H */
