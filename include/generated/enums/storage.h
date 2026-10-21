/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_STORAGE_H
#define REDFISH_ENUM_STORAGE_H

#include <stddef.h>

/* Storage.v1_11_0.ResetToDefaultsType */
enum rf_reset_to_defaults_type {
	RESET_TO_DEFAULTS_TYPE_INVALID = 0,
	RESET_TO_DEFAULTS_TYPE_RESET_ALL = 1,
	RESET_TO_DEFAULTS_TYPE_PRESERVE_VOLUMES = 2,
};

static inline const char *rf_reset_to_defaults_type_to_str(enum rf_reset_to_defaults_type val)
{
	switch (val) {
	case RESET_TO_DEFAULTS_TYPE_INVALID: return "Invalid";
	case RESET_TO_DEFAULTS_TYPE_RESET_ALL: return "ResetAll";
	case RESET_TO_DEFAULTS_TYPE_PRESERVE_VOLUMES: return "PreserveVolumes";
	default: return NULL;
	}
}

/* Storage.v1_14_0.HotspareActivationPolicy */
enum rf_hotspare_activation_policy {
	HOTSPARE_ACTIVATION_POLICY_INVALID = 0,
	HOTSPARE_ACTIVATION_POLICY_ON_DRIVE_FAILURE = 1,
	HOTSPARE_ACTIVATION_POLICY_ON_DRIVE_PREDICTED_FAILURE = 2,
	HOTSPARE_ACTIVATION_POLICY_OEM = 3,
};

static inline const char *rf_hotspare_activation_policy_to_str(enum rf_hotspare_activation_policy val)
{
	switch (val) {
	case HOTSPARE_ACTIVATION_POLICY_INVALID: return "Invalid";
	case HOTSPARE_ACTIVATION_POLICY_ON_DRIVE_FAILURE: return "OnDriveFailure";
	case HOTSPARE_ACTIVATION_POLICY_ON_DRIVE_PREDICTED_FAILURE: return "OnDrivePredictedFailure";
	case HOTSPARE_ACTIVATION_POLICY_OEM: return "OEM";
	default: return NULL;
	}
}

/* Storage.v1_14_0.EncryptionMode */
enum rf_encryption_mode {
	ENCRYPTION_MODE_INVALID = 0,
	ENCRYPTION_MODE_DISABLED = 1,
	ENCRYPTION_MODE_USE_EXTERNAL_KEY = 2,
	ENCRYPTION_MODE_USE_LOCAL_KEY = 3,
	ENCRYPTION_MODE_PASSWORD_ONLY = 4,
	ENCRYPTION_MODE_PASSWORD_WITH_EXTERNAL_KEY = 5,
	ENCRYPTION_MODE_PASSWORD_WITH_LOCAL_KEY = 6,
};

static inline const char *rf_encryption_mode_to_str(enum rf_encryption_mode val)
{
	switch (val) {
	case ENCRYPTION_MODE_INVALID: return "Invalid";
	case ENCRYPTION_MODE_DISABLED: return "Disabled";
	case ENCRYPTION_MODE_USE_EXTERNAL_KEY: return "UseExternalKey";
	case ENCRYPTION_MODE_USE_LOCAL_KEY: return "UseLocalKey";
	case ENCRYPTION_MODE_PASSWORD_ONLY: return "PasswordOnly";
	case ENCRYPTION_MODE_PASSWORD_WITH_EXTERNAL_KEY: return "PasswordWithExternalKey";
	case ENCRYPTION_MODE_PASSWORD_WITH_LOCAL_KEY: return "PasswordWithLocalKey";
	default: return NULL;
	}
}

/* Storage.v1_15_0.AutoVolumeCreate */
enum rf_auto_volume_create {
	AUTO_VOLUME_CREATE_INVALID = 0,
	AUTO_VOLUME_CREATE_DISABLED = 1,
	AUTO_VOLUME_CREATE_NON_RAID = 2,
	AUTO_VOLUME_CREATE_RAID0 = 3,
	AUTO_VOLUME_CREATE_RAID1 = 4,
};

static inline const char *rf_auto_volume_create_to_str(enum rf_auto_volume_create val)
{
	switch (val) {
	case AUTO_VOLUME_CREATE_INVALID: return "Invalid";
	case AUTO_VOLUME_CREATE_DISABLED: return "Disabled";
	case AUTO_VOLUME_CREATE_NON_RAID: return "NonRAID";
	case AUTO_VOLUME_CREATE_RAID0: return "RAID0";
	case AUTO_VOLUME_CREATE_RAID1: return "RAID1";
	default: return NULL;
	}
}

/* Storage.v1_16_0.ConfigurationLock */
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

/* Storage.v1_17_0.TargetConfigurationLockLevel */
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

/* Storage.v1_17_0.ConfigLockOptions */
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

/* Storage.v1_20_0.VolumeAssignmentPolicy */
enum rf_volume_assignment_policy {
	VOLUME_ASSIGNMENT_POLICY_INVALID = 0,
	VOLUME_ASSIGNMENT_POLICY_UNASSIGNED = 1,
	VOLUME_ASSIGNMENT_POLICY_SUPERVISOR = 2,
	VOLUME_ASSIGNMENT_POLICY_WEIGHTED_ROUND_ROBIN = 3,
};

static inline const char *rf_volume_assignment_policy_to_str(enum rf_volume_assignment_policy val)
{
	switch (val) {
	case VOLUME_ASSIGNMENT_POLICY_INVALID: return "Invalid";
	case VOLUME_ASSIGNMENT_POLICY_UNASSIGNED: return "Unassigned";
	case VOLUME_ASSIGNMENT_POLICY_SUPERVISOR: return "Supervisor";
	case VOLUME_ASSIGNMENT_POLICY_WEIGHTED_ROUND_ROBIN: return "WeightedRoundRobin";
	default: return NULL;
	}
}

/* Storage.v1_21_0.NMVeMinimumRequiredResetType */
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

/* Storage.v1_21_0.NMVeUnfreezeAuthMode */
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

/* Storage.v1_21_0.NMVePersonalityKeyAlgorithm */
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

/* Storage.v1_23_0.ReservationCapabilities */
enum rf_reservation_capabilities {
	RESERVATION_CAPABILITIES_INVALID = 0,
	RESERVATION_CAPABILITIES_COMPATIBLE_RESERVATION_HANDLING = 1,
	RESERVATION_CAPABILITIES_SPECIFY_INITIATOR_PORTS = 2,
	RESERVATION_CAPABILITIES_MULTIPLE_TARGET_PORTS = 3,
	RESERVATION_CAPABILITIES_PERSIST_THROUGH_POWER_LOSS = 4,
	RESERVATION_CAPABILITIES_SUPPORT_CLEAR_RESERVATIONS_OUT_OF_BAND = 5,
};

static inline const char *rf_reservation_capabilities_to_str(enum rf_reservation_capabilities val)
{
	switch (val) {
	case RESERVATION_CAPABILITIES_INVALID: return "Invalid";
	case RESERVATION_CAPABILITIES_COMPATIBLE_RESERVATION_HANDLING: return "CompatibleReservationHandling";
	case RESERVATION_CAPABILITIES_SPECIFY_INITIATOR_PORTS: return "SpecifyInitiatorPorts";
	case RESERVATION_CAPABILITIES_MULTIPLE_TARGET_PORTS: return "MultipleTargetPorts";
	case RESERVATION_CAPABILITIES_PERSIST_THROUGH_POWER_LOSS: return "PersistThroughPowerLoss";
	case RESERVATION_CAPABILITIES_SUPPORT_CLEAR_RESERVATIONS_OUT_OF_BAND: return "SupportClearReservationsOutOfBand";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_STORAGE_H */
