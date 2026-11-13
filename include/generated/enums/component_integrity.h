/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_COMPONENT_INTEGRITY_H
#define REDFISH_ENUM_COMPONENT_INTEGRITY_H

#include <stddef.h>

/* ComponentIntegrity.v1_0_0.ComponentIntegrityType */
enum rf_component_integrity_type {
	COMPONENT_INTEGRITY_TYPE_INVALID = 0,
	COMPONENT_INTEGRITY_TYPE_SPDM = 1,
	COMPONENT_INTEGRITY_TYPE_TPM = 2,
	COMPONENT_INTEGRITY_TYPE_TCM = 3,
	COMPONENT_INTEGRITY_TYPE_TPCM = 4,
	COMPONENT_INTEGRITY_TYPE_OEM = 5,
};

static inline const char *rf_component_integrity_type_to_str(enum rf_component_integrity_type val)
{
	switch (val) {
	case COMPONENT_INTEGRITY_TYPE_INVALID: return "Invalid";
	case COMPONENT_INTEGRITY_TYPE_SPDM: return "SPDM";
	case COMPONENT_INTEGRITY_TYPE_TPM: return "TPM";
	case COMPONENT_INTEGRITY_TYPE_TCM: return "TCM";
	case COMPONENT_INTEGRITY_TYPE_TPCM: return "TPCM";
	case COMPONENT_INTEGRITY_TYPE_OEM: return "OEM";
	default: return NULL;
	}
}

/* ComponentIntegrity.v1_0_0.MeasurementSpecification */
enum rf_measurement_specification {
	MEASUREMENT_SPECIFICATION_INVALID = 0,
	MEASUREMENT_SPECIFICATION_DMTF = 1,
};

static inline const char *rf_measurement_specification_to_str(enum rf_measurement_specification val)
{
	switch (val) {
	case MEASUREMENT_SPECIFICATION_INVALID: return "Invalid";
	case MEASUREMENT_SPECIFICATION_DMTF: return "DMTF";
	default: return NULL;
	}
}

/* ComponentIntegrity.v1_0_0.SPDMmeasurementSummaryType */
enum rf_spd_mmeasurement_summary_type {
	SPD_MMEASUREMENT_SUMMARY_TYPE_INVALID = 0,
	SPD_MMEASUREMENT_SUMMARY_TYPE_TCB = 1,
	SPD_MMEASUREMENT_SUMMARY_TYPE_ALL = 2,
};

static inline const char *rf_spd_mmeasurement_summary_type_to_str(enum rf_spd_mmeasurement_summary_type val)
{
	switch (val) {
	case SPD_MMEASUREMENT_SUMMARY_TYPE_INVALID: return "Invalid";
	case SPD_MMEASUREMENT_SUMMARY_TYPE_TCB: return "TCB";
	case SPD_MMEASUREMENT_SUMMARY_TYPE_ALL: return "All";
	default: return NULL;
	}
}

/* ComponentIntegrity.v1_0_0.DMTFmeasurementTypes */
enum rf_dmt_fmeasurement_types {
	DMT_FMEASUREMENT_TYPES_INVALID = 0,
	DMT_FMEASUREMENT_TYPES_IMMUTABLE_ROM = 1,
	DMT_FMEASUREMENT_TYPES_MUTABLE_FIRMWARE = 2,
	DMT_FMEASUREMENT_TYPES_HARDWARE_CONFIGURATION = 3,
	DMT_FMEASUREMENT_TYPES_FIRMWARE_CONFIGURATION = 4,
	DMT_FMEASUREMENT_TYPES_MUTABLE_FIRMWARE_VERSION = 5,
	DMT_FMEASUREMENT_TYPES_MUTABLE_FIRMWARE_SECURITY_VERSION_NUMBER = 6,
	DMT_FMEASUREMENT_TYPES_MEASUREMENT_MANIFEST = 7,
	DMT_FMEASUREMENT_TYPES_DEBUG_AND_DEVICE_MODE = 8,
	DMT_FMEASUREMENT_TYPES_HASH_EXTEND_MEASUREMENT = 9,
	DMT_FMEASUREMENT_TYPES_INFORMATIONAL = 10,
	DMT_FMEASUREMENT_TYPES_STRUCTURED_MEASUREMENT_MANIFEST = 11,
};

static inline const char *rf_dmt_fmeasurement_types_to_str(enum rf_dmt_fmeasurement_types val)
{
	switch (val) {
	case DMT_FMEASUREMENT_TYPES_INVALID: return "Invalid";
	case DMT_FMEASUREMENT_TYPES_IMMUTABLE_ROM: return "ImmutableROM";
	case DMT_FMEASUREMENT_TYPES_MUTABLE_FIRMWARE: return "MutableFirmware";
	case DMT_FMEASUREMENT_TYPES_HARDWARE_CONFIGURATION: return "HardwareConfiguration";
	case DMT_FMEASUREMENT_TYPES_FIRMWARE_CONFIGURATION: return "FirmwareConfiguration";
	case DMT_FMEASUREMENT_TYPES_MUTABLE_FIRMWARE_VERSION: return "MutableFirmwareVersion";
	case DMT_FMEASUREMENT_TYPES_MUTABLE_FIRMWARE_SECURITY_VERSION_NUMBER: return "MutableFirmwareSecurityVersionNumber";
	case DMT_FMEASUREMENT_TYPES_MEASUREMENT_MANIFEST: return "MeasurementManifest";
	case DMT_FMEASUREMENT_TYPES_DEBUG_AND_DEVICE_MODE: return "DebugAndDeviceMode";
	case DMT_FMEASUREMENT_TYPES_HASH_EXTEND_MEASUREMENT: return "HashExtendMeasurement";
	case DMT_FMEASUREMENT_TYPES_INFORMATIONAL: return "Informational";
	case DMT_FMEASUREMENT_TYPES_STRUCTURED_MEASUREMENT_MANIFEST: return "StructuredMeasurementManifest";
	default: return NULL;
	}
}

/* ComponentIntegrity.v1_0_0.VerificationStatus */
enum rf_verification_status {
	VERIFICATION_STATUS_INVALID = 0,
	VERIFICATION_STATUS_SUCCESS = 1,
	VERIFICATION_STATUS_FAILED = 2,
};

static inline const char *rf_verification_status_to_str(enum rf_verification_status val)
{
	switch (val) {
	case VERIFICATION_STATUS_INVALID: return "Invalid";
	case VERIFICATION_STATUS_SUCCESS: return "Success";
	case VERIFICATION_STATUS_FAILED: return "Failed";
	default: return NULL;
	}
}

/* ComponentIntegrity.v1_0_0.SecureSessionType */
enum rf_secure_session_type {
	SECURE_SESSION_TYPE_INVALID = 0,
	SECURE_SESSION_TYPE_PLAIN = 1,
	SECURE_SESSION_TYPE_ENCRYPTED_AUTHENTICATED = 2,
	SECURE_SESSION_TYPE_AUTHENTICATED_ONLY = 3,
};

static inline const char *rf_secure_session_type_to_str(enum rf_secure_session_type val)
{
	switch (val) {
	case SECURE_SESSION_TYPE_INVALID: return "Invalid";
	case SECURE_SESSION_TYPE_PLAIN: return "Plain";
	case SECURE_SESSION_TYPE_ENCRYPTED_AUTHENTICATED: return "EncryptedAuthenticated";
	case SECURE_SESSION_TYPE_AUTHENTICATED_ONLY: return "AuthenticatedOnly";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_COMPONENT_INTEGRITY_H */
