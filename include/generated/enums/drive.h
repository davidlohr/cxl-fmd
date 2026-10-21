/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_DRIVE_H
#define REDFISH_ENUM_DRIVE_H

#include <stddef.h>

/* Drive.v1_0_0.MediaType */
enum rf_media_type {
	MEDIA_TYPE_INVALID = 0,
	MEDIA_TYPE_HDD = 1,
	MEDIA_TYPE_SSD = 2,
	MEDIA_TYPE_SMR = 3,
};

static inline const char *rf_media_type_to_str(enum rf_media_type val)
{
	switch (val) {
	case MEDIA_TYPE_INVALID: return "Invalid";
	case MEDIA_TYPE_HDD: return "HDD";
	case MEDIA_TYPE_SSD: return "SSD";
	case MEDIA_TYPE_SMR: return "SMR";
	default: return NULL;
	}
}

/* Drive.v1_0_0.HotspareType */
enum rf_hotspare_type {
	HOTSPARE_TYPE_INVALID = 0,
	HOTSPARE_TYPE_NONE = 1,
	HOTSPARE_TYPE_GLOBAL = 2,
	HOTSPARE_TYPE_CHASSIS = 3,
	HOTSPARE_TYPE_DEDICATED = 4,
};

static inline const char *rf_hotspare_type_to_str(enum rf_hotspare_type val)
{
	switch (val) {
	case HOTSPARE_TYPE_INVALID: return "Invalid";
	case HOTSPARE_TYPE_NONE: return "None";
	case HOTSPARE_TYPE_GLOBAL: return "Global";
	case HOTSPARE_TYPE_CHASSIS: return "Chassis";
	case HOTSPARE_TYPE_DEDICATED: return "Dedicated";
	default: return NULL;
	}
}

/* Drive.v1_0_0.EncryptionAbility */
enum rf_encryption_ability {
	ENCRYPTION_ABILITY_INVALID = 0,
	ENCRYPTION_ABILITY_NONE = 1,
	ENCRYPTION_ABILITY_SELF_ENCRYPTING_DRIVE = 2,
	ENCRYPTION_ABILITY_OTHER = 3,
};

static inline const char *rf_encryption_ability_to_str(enum rf_encryption_ability val)
{
	switch (val) {
	case ENCRYPTION_ABILITY_INVALID: return "Invalid";
	case ENCRYPTION_ABILITY_NONE: return "None";
	case ENCRYPTION_ABILITY_SELF_ENCRYPTING_DRIVE: return "SelfEncryptingDrive";
	case ENCRYPTION_ABILITY_OTHER: return "Other";
	default: return NULL;
	}
}

/* Drive.v1_0_0.EncryptionStatus */
enum rf_encryption_status {
	ENCRYPTION_STATUS_INVALID = 0,
	ENCRYPTION_STATUS_UNECRYPTED = 1,
	ENCRYPTION_STATUS_UNLOCKED = 2,
	ENCRYPTION_STATUS_LOCKED = 3,
	ENCRYPTION_STATUS_FOREIGN = 4,
	ENCRYPTION_STATUS_UNENCRYPTED = 5,
};

static inline const char *rf_encryption_status_to_str(enum rf_encryption_status val)
{
	switch (val) {
	case ENCRYPTION_STATUS_INVALID: return "Invalid";
	case ENCRYPTION_STATUS_UNECRYPTED: return "Unecrypted";
	case ENCRYPTION_STATUS_UNLOCKED: return "Unlocked";
	case ENCRYPTION_STATUS_LOCKED: return "Locked";
	case ENCRYPTION_STATUS_FOREIGN: return "Foreign";
	case ENCRYPTION_STATUS_UNENCRYPTED: return "Unencrypted";
	default: return NULL;
	}
}

/* Drive.v1_0_0.StatusIndicator */
enum rf_status_indicator {
	STATUS_INDICATOR_INVALID = 0,
	STATUS_INDICATOR_OK = 1,
	STATUS_INDICATOR_FAIL = 2,
	STATUS_INDICATOR_REBUILD = 3,
	STATUS_INDICATOR_PREDICTIVE_FAILURE_ANALYSIS = 4,
	STATUS_INDICATOR_HOTSPARE = 5,
	STATUS_INDICATOR_IN_A_CRITICAL_ARRAY = 6,
	STATUS_INDICATOR_IN_A_FAILED_ARRAY = 7,
};

static inline const char *rf_status_indicator_to_str(enum rf_status_indicator val)
{
	switch (val) {
	case STATUS_INDICATOR_INVALID: return "Invalid";
	case STATUS_INDICATOR_OK: return "OK";
	case STATUS_INDICATOR_FAIL: return "Fail";
	case STATUS_INDICATOR_REBUILD: return "Rebuild";
	case STATUS_INDICATOR_PREDICTIVE_FAILURE_ANALYSIS: return "PredictiveFailureAnalysis";
	case STATUS_INDICATOR_HOTSPARE: return "Hotspare";
	case STATUS_INDICATOR_IN_A_CRITICAL_ARRAY: return "InACriticalArray";
	case STATUS_INDICATOR_IN_A_FAILED_ARRAY: return "InAFailedArray";
	default: return NULL;
	}
}

/* Drive.v1_5_0.HotspareReplacementModeType */
enum rf_hotspare_replacement_mode_type {
	HOTSPARE_REPLACEMENT_MODE_TYPE_INVALID = 0,
	HOTSPARE_REPLACEMENT_MODE_TYPE_REVERTIBLE = 1,
	HOTSPARE_REPLACEMENT_MODE_TYPE_NON_REVERTIBLE = 2,
};

static inline const char *rf_hotspare_replacement_mode_type_to_str(enum rf_hotspare_replacement_mode_type val)
{
	switch (val) {
	case HOTSPARE_REPLACEMENT_MODE_TYPE_INVALID: return "Invalid";
	case HOTSPARE_REPLACEMENT_MODE_TYPE_REVERTIBLE: return "Revertible";
	case HOTSPARE_REPLACEMENT_MODE_TYPE_NON_REVERTIBLE: return "NonRevertible";
	default: return NULL;
	}
}

/* Drive.v1_15_0.DataSanitizationType */
enum rf_data_sanitization_type {
	DATA_SANITIZATION_TYPE_INVALID = 0,
	DATA_SANITIZATION_TYPE_BLOCK_ERASE = 1,
	DATA_SANITIZATION_TYPE_CRYPTOGRAPHIC_ERASE = 2,
	DATA_SANITIZATION_TYPE_OVERWRITE = 3,
};

static inline const char *rf_data_sanitization_type_to_str(enum rf_data_sanitization_type val)
{
	switch (val) {
	case DATA_SANITIZATION_TYPE_INVALID: return "Invalid";
	case DATA_SANITIZATION_TYPE_BLOCK_ERASE: return "BlockErase";
	case DATA_SANITIZATION_TYPE_CRYPTOGRAPHIC_ERASE: return "CryptographicErase";
	case DATA_SANITIZATION_TYPE_OVERWRITE: return "Overwrite";
	default: return NULL;
	}
}

/* Drive.v1_16_0.FormFactor */
enum rf_form_factor {
	FORM_FACTOR_INVALID = 0,
	FORM_FACTOR_DRIVE3_5 = 1,
	FORM_FACTOR_DRIVE2_5 = 2,
	FORM_FACTOR_EDSFF = 3,
	FORM_FACTOR_EDSFF_1_U__LONG = 4,
	FORM_FACTOR_EDSFF_1_U__SHORT = 5,
	FORM_FACTOR_EDSFF_E3__SHORT = 6,
	FORM_FACTOR_EDSFF_E3__LONG = 7,
	FORM_FACTOR_M2 = 8,
	FORM_FACTOR_M2_2230 = 9,
	FORM_FACTOR_M2_2242 = 10,
	FORM_FACTOR_M2_2260 = 11,
	FORM_FACTOR_M2_2280 = 12,
	FORM_FACTOR_M2_22110 = 13,
	FORM_FACTOR_U2 = 14,
	FORM_FACTOR_PCIE_SLOT_FULL_LENGTH = 15,
	FORM_FACTOR_PCIE_SLOT_LOW_PROFILE = 16,
	FORM_FACTOR_PCIE_HALF_LENGTH = 17,
	FORM_FACTOR_OEM = 18,
};

static inline const char *rf_form_factor_to_str(enum rf_form_factor val)
{
	switch (val) {
	case FORM_FACTOR_INVALID: return "Invalid";
	case FORM_FACTOR_DRIVE3_5: return "Drive3_5";
	case FORM_FACTOR_DRIVE2_5: return "Drive2_5";
	case FORM_FACTOR_EDSFF: return "EDSFF";
	case FORM_FACTOR_EDSFF_1_U__LONG: return "EDSFF_1U_Long";
	case FORM_FACTOR_EDSFF_1_U__SHORT: return "EDSFF_1U_Short";
	case FORM_FACTOR_EDSFF_E3__SHORT: return "EDSFF_E3_Short";
	case FORM_FACTOR_EDSFF_E3__LONG: return "EDSFF_E3_Long";
	case FORM_FACTOR_M2: return "M2";
	case FORM_FACTOR_M2_2230: return "M2_2230";
	case FORM_FACTOR_M2_2242: return "M2_2242";
	case FORM_FACTOR_M2_2260: return "M2_2260";
	case FORM_FACTOR_M2_2280: return "M2_2280";
	case FORM_FACTOR_M2_22110: return "M2_22110";
	case FORM_FACTOR_U2: return "U2";
	case FORM_FACTOR_PCIE_SLOT_FULL_LENGTH: return "PCIeSlotFullLength";
	case FORM_FACTOR_PCIE_SLOT_LOW_PROFILE: return "PCIeSlotLowProfile";
	case FORM_FACTOR_PCIE_HALF_LENGTH: return "PCIeHalfLength";
	case FORM_FACTOR_OEM: return "OEM";
	default: return NULL;
	}
}

/* Drive.v1_19_0.ConfigurationLock */
enum rf_configuration_lock {
	CONFIGURATION_LOCK_INVALID = 0,
	CONFIGURATION_LOCK_ENABLED = 1,
	CONFIGURATION_LOCK_DISABLED = 2,
	CONFIGURATION_LOCK_PARTIAL = 3,
};

static inline const char *rf_configuration_lock_to_str(enum rf_configuration_lock val)
{
	switch (val) {
	case CONFIGURATION_LOCK_INVALID: return "Invalid";
	case CONFIGURATION_LOCK_ENABLED: return "Enabled";
	case CONFIGURATION_LOCK_DISABLED: return "Disabled";
	case CONFIGURATION_LOCK_PARTIAL: return "Partial";
	default: return NULL;
	}
}

/* Drive.v1_20_0.TargetConfigurationLockLevel */
enum rf_target_configuration_lock_level {
	TARGET_CONFIGURATION_LOCK_LEVEL_INVALID = 0,
	TARGET_CONFIGURATION_LOCK_LEVEL_BASELINE = 1,
};

static inline const char *rf_target_configuration_lock_level_to_str(enum rf_target_configuration_lock_level val)
{
	switch (val) {
	case TARGET_CONFIGURATION_LOCK_LEVEL_INVALID: return "Invalid";
	case TARGET_CONFIGURATION_LOCK_LEVEL_BASELINE: return "Baseline";
	default: return NULL;
	}
}

/* Drive.v1_20_0.ConfigLockOptions */
enum rf_config_lock_options {
	CONFIG_LOCK_OPTIONS_INVALID = 0,
	CONFIG_LOCK_OPTIONS_UNLOCKED = 1,
	CONFIG_LOCK_OPTIONS_LOCKED = 2,
	CONFIG_LOCK_OPTIONS_LOCKDOWN_UNSUPPORTED = 3,
	CONFIG_LOCK_OPTIONS_COMMAND_UNSUPPORTED = 4,
};

static inline const char *rf_config_lock_options_to_str(enum rf_config_lock_options val)
{
	switch (val) {
	case CONFIG_LOCK_OPTIONS_INVALID: return "Invalid";
	case CONFIG_LOCK_OPTIONS_UNLOCKED: return "Unlocked";
	case CONFIG_LOCK_OPTIONS_LOCKED: return "Locked";
	case CONFIG_LOCK_OPTIONS_LOCKDOWN_UNSUPPORTED: return "LockdownUnsupported";
	case CONFIG_LOCK_OPTIONS_COMMAND_UNSUPPORTED: return "CommandUnsupported";
	default: return NULL;
	}
}

/* Drive.v1_22_0.NMVeMinimumRequiredResetType */
enum rf_nm_ve_minimum_required_reset_type {
	NM_VE_MINIMUM_REQUIRED_RESET_TYPE_INVALID = 0,
	NM_VE_MINIMUM_REQUIRED_RESET_TYPE_NO_RESET = 1,
	NM_VE_MINIMUM_REQUIRED_RESET_TYPE_CONTROLLER_LEVEL = 2,
	NM_VE_MINIMUM_REQUIRED_RESET_TYPE_LIMITED_CONTROLLER_LEVEL = 3,
	NM_VE_MINIMUM_REQUIRED_RESET_TYPE_NVM_SUBSYSTEM_RESET = 4,
	NM_VE_MINIMUM_REQUIRED_RESET_TYPE_POWER_CYCLE = 5,
};

static inline const char *rf_nm_ve_minimum_required_reset_type_to_str(enum rf_nm_ve_minimum_required_reset_type val)
{
	switch (val) {
	case NM_VE_MINIMUM_REQUIRED_RESET_TYPE_INVALID: return "Invalid";
	case NM_VE_MINIMUM_REQUIRED_RESET_TYPE_NO_RESET: return "NoReset";
	case NM_VE_MINIMUM_REQUIRED_RESET_TYPE_CONTROLLER_LEVEL: return "ControllerLevel";
	case NM_VE_MINIMUM_REQUIRED_RESET_TYPE_LIMITED_CONTROLLER_LEVEL: return "LimitedControllerLevel";
	case NM_VE_MINIMUM_REQUIRED_RESET_TYPE_NVM_SUBSYSTEM_RESET: return "NVMSubsystemReset";
	case NM_VE_MINIMUM_REQUIRED_RESET_TYPE_POWER_CYCLE: return "PowerCycle";
	default: return NULL;
	}
}

/* Drive.v1_22_0.NMVeUnfreezeAuthMode */
enum rf_nm_ve_unfreeze_auth_mode {
	NM_VE_UNFREEZE_AUTH_MODE_INVALID = 0,
	NM_VE_UNFREEZE_AUTH_MODE_PROGRAMMED_KEY = 1,
	NM_VE_UNFREEZE_AUTH_MODE_PHYSICAL_ID = 2,
};

static inline const char *rf_nm_ve_unfreeze_auth_mode_to_str(enum rf_nm_ve_unfreeze_auth_mode val)
{
	switch (val) {
	case NM_VE_UNFREEZE_AUTH_MODE_INVALID: return "Invalid";
	case NM_VE_UNFREEZE_AUTH_MODE_PROGRAMMED_KEY: return "ProgrammedKey";
	case NM_VE_UNFREEZE_AUTH_MODE_PHYSICAL_ID: return "PhysicalId";
	default: return NULL;
	}
}

/* Drive.v1_22_0.NMVePersonalityKeyAlgorithm */
enum rf_nm_ve_personality_key_algorithm {
	NM_VE_PERSONALITY_KEY_ALGORITHM_INVALID = 0,
	NM_VE_PERSONALITY_KEY_ALGORITHM_HMAC_SHA384 = 1,
};

static inline const char *rf_nm_ve_personality_key_algorithm_to_str(enum rf_nm_ve_personality_key_algorithm val)
{
	switch (val) {
	case NM_VE_PERSONALITY_KEY_ALGORITHM_INVALID: return "Invalid";
	case NM_VE_PERSONALITY_KEY_ALGORITHM_HMAC_SHA384: return "HMAC_SHA384";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_DRIVE_H */
