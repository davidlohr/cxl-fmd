/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_COMPUTER_SYSTEM_H
#define REDFISH_ENUM_COMPUTER_SYSTEM_H

#include <stddef.h>

/* ComputerSystem.BootSource */
enum rf_boot_source {
	BOOT_SOURCE_INVALID = 0,
	BOOT_SOURCE_NONE = 1,
	BOOT_SOURCE_PXE = 2,
	BOOT_SOURCE_FLOPPY = 3,
	BOOT_SOURCE_CD = 4,
	BOOT_SOURCE_USB = 5,
	BOOT_SOURCE_HDD = 6,
	BOOT_SOURCE_BIOS_SETUP = 7,
	BOOT_SOURCE_UTILITIES = 8,
	BOOT_SOURCE_DIAGS = 9,
	BOOT_SOURCE_UEFI_SHELL = 10,
	BOOT_SOURCE_UEFI_TARGET = 11,
	BOOT_SOURCE_SD_CARD = 12,
	BOOT_SOURCE_UEFI_HTTP = 13,
	BOOT_SOURCE_REMOTE_DRIVE = 14,
	BOOT_SOURCE_UEFI_BOOT_NEXT = 15,
	BOOT_SOURCE_RECOVERY = 16,
};

static inline const char *rf_boot_source_to_str(enum rf_boot_source val)
{
	switch (val) {
	case BOOT_SOURCE_INVALID: return "Invalid";
	case BOOT_SOURCE_NONE: return "None";
	case BOOT_SOURCE_PXE: return "Pxe";
	case BOOT_SOURCE_FLOPPY: return "Floppy";
	case BOOT_SOURCE_CD: return "Cd";
	case BOOT_SOURCE_USB: return "Usb";
	case BOOT_SOURCE_HDD: return "Hdd";
	case BOOT_SOURCE_BIOS_SETUP: return "BiosSetup";
	case BOOT_SOURCE_UTILITIES: return "Utilities";
	case BOOT_SOURCE_DIAGS: return "Diags";
	case BOOT_SOURCE_UEFI_SHELL: return "UefiShell";
	case BOOT_SOURCE_UEFI_TARGET: return "UefiTarget";
	case BOOT_SOURCE_SD_CARD: return "SDCard";
	case BOOT_SOURCE_UEFI_HTTP: return "UefiHttp";
	case BOOT_SOURCE_REMOTE_DRIVE: return "RemoteDrive";
	case BOOT_SOURCE_UEFI_BOOT_NEXT: return "UefiBootNext";
	case BOOT_SOURCE_RECOVERY: return "Recovery";
	default: return NULL;
	}
}

/* ComputerSystem.v1_0_0.SystemType */
enum rf_system_type {
	SYSTEM_TYPE_INVALID = 0,
	SYSTEM_TYPE_PHYSICAL = 1,
	SYSTEM_TYPE_VIRTUAL = 2,
	SYSTEM_TYPE_OS = 3,
	SYSTEM_TYPE_PHYSICALLY_PARTITIONED = 4,
	SYSTEM_TYPE_VIRTUALLY_PARTITIONED = 5,
	SYSTEM_TYPE_COMPOSED = 6,
	SYSTEM_TYPE_DPU = 7,
};

static inline const char *rf_system_type_to_str(enum rf_system_type val)
{
	switch (val) {
	case SYSTEM_TYPE_INVALID: return "Invalid";
	case SYSTEM_TYPE_PHYSICAL: return "Physical";
	case SYSTEM_TYPE_VIRTUAL: return "Virtual";
	case SYSTEM_TYPE_OS: return "OS";
	case SYSTEM_TYPE_PHYSICALLY_PARTITIONED: return "PhysicallyPartitioned";
	case SYSTEM_TYPE_VIRTUALLY_PARTITIONED: return "VirtuallyPartitioned";
	case SYSTEM_TYPE_COMPOSED: return "Composed";
	case SYSTEM_TYPE_DPU: return "DPU";
	default: return NULL;
	}
}

/* ComputerSystem.v1_0_0.IndicatorLED */
enum rf_indicator_led {
	INDICATOR_LED_INVALID = 0,
	INDICATOR_LED_UNKNOWN = 1,
	INDICATOR_LED_LIT = 2,
	INDICATOR_LED_BLINKING = 3,
	INDICATOR_LED_OFF = 4,
};

static inline const char *rf_indicator_led_to_str(enum rf_indicator_led val)
{
	switch (val) {
	case INDICATOR_LED_INVALID: return "Invalid";
	case INDICATOR_LED_UNKNOWN: return "Unknown";
	case INDICATOR_LED_LIT: return "Lit";
	case INDICATOR_LED_BLINKING: return "Blinking";
	case INDICATOR_LED_OFF: return "Off";
	default: return NULL;
	}
}

/* ComputerSystem.v1_0_0.BootSourceOverrideEnabled */
enum rf_boot_source_override_enabled {
	BOOT_SOURCE_OVERRIDE_ENABLED_INVALID = 0,
	BOOT_SOURCE_OVERRIDE_ENABLED_DISABLED = 1,
	BOOT_SOURCE_OVERRIDE_ENABLED_ONCE = 2,
	BOOT_SOURCE_OVERRIDE_ENABLED_CONTINUOUS = 3,
};

static inline const char *rf_boot_source_override_enabled_to_str(enum rf_boot_source_override_enabled val)
{
	switch (val) {
	case BOOT_SOURCE_OVERRIDE_ENABLED_INVALID: return "Invalid";
	case BOOT_SOURCE_OVERRIDE_ENABLED_DISABLED: return "Disabled";
	case BOOT_SOURCE_OVERRIDE_ENABLED_ONCE: return "Once";
	case BOOT_SOURCE_OVERRIDE_ENABLED_CONTINUOUS: return "Continuous";
	default: return NULL;
	}
}

/* ComputerSystem.v1_1_0.MemoryMirroring */
enum rf_memory_mirroring {
	MEMORY_MIRRORING_INVALID = 0,
	MEMORY_MIRRORING_SYSTEM = 1,
	MEMORY_MIRRORING_DIMM = 2,
	MEMORY_MIRRORING_HYBRID = 3,
	MEMORY_MIRRORING_NONE = 4,
};

static inline const char *rf_memory_mirroring_to_str(enum rf_memory_mirroring val)
{
	switch (val) {
	case MEMORY_MIRRORING_INVALID: return "Invalid";
	case MEMORY_MIRRORING_SYSTEM: return "System";
	case MEMORY_MIRRORING_DIMM: return "DIMM";
	case MEMORY_MIRRORING_HYBRID: return "Hybrid";
	case MEMORY_MIRRORING_NONE: return "None";
	default: return NULL;
	}
}

/* ComputerSystem.v1_1_0.BootSourceOverrideMode */
enum rf_boot_source_override_mode {
	BOOT_SOURCE_OVERRIDE_MODE_INVALID = 0,
	BOOT_SOURCE_OVERRIDE_MODE_LEGACY = 1,
	BOOT_SOURCE_OVERRIDE_MODE_UEFI = 2,
};

static inline const char *rf_boot_source_override_mode_to_str(enum rf_boot_source_override_mode val)
{
	switch (val) {
	case BOOT_SOURCE_OVERRIDE_MODE_INVALID: return "Invalid";
	case BOOT_SOURCE_OVERRIDE_MODE_LEGACY: return "Legacy";
	case BOOT_SOURCE_OVERRIDE_MODE_UEFI: return "UEFI";
	default: return NULL;
	}
}

/* ComputerSystem.v1_1_0.InterfaceType */
enum rf_interface_type {
	INTERFACE_TYPE_INVALID = 0,
	INTERFACE_TYPE_TPM1_2 = 1,
	INTERFACE_TYPE_TPM2_0 = 2,
	INTERFACE_TYPE_TCM1_0 = 3,
};

static inline const char *rf_interface_type_to_str(enum rf_interface_type val)
{
	switch (val) {
	case INTERFACE_TYPE_INVALID: return "Invalid";
	case INTERFACE_TYPE_TPM1_2: return "TPM1_2";
	case INTERFACE_TYPE_TPM2_0: return "TPM2_0";
	case INTERFACE_TYPE_TCM1_0: return "TCM1_0";
	default: return NULL;
	}
}

/* ComputerSystem.v1_2_0.HostingRole */
enum rf_hosting_role {
	HOSTING_ROLE_INVALID = 0,
	HOSTING_ROLE_APPLICATION_SERVER = 1,
	HOSTING_ROLE_STORAGE_SERVER = 2,
	HOSTING_ROLE_SWITCH = 3,
	HOSTING_ROLE_APPLIANCE = 4,
	HOSTING_ROLE_BARE_METAL_SERVER = 5,
	HOSTING_ROLE_VIRTUAL_MACHINE_SERVER = 6,
	HOSTING_ROLE_CONTAINER_SERVER = 7,
};

static inline const char *rf_hosting_role_to_str(enum rf_hosting_role val)
{
	switch (val) {
	case HOSTING_ROLE_INVALID: return "Invalid";
	case HOSTING_ROLE_APPLICATION_SERVER: return "ApplicationServer";
	case HOSTING_ROLE_STORAGE_SERVER: return "StorageServer";
	case HOSTING_ROLE_SWITCH: return "Switch";
	case HOSTING_ROLE_APPLIANCE: return "Appliance";
	case HOSTING_ROLE_BARE_METAL_SERVER: return "BareMetalServer";
	case HOSTING_ROLE_VIRTUAL_MACHINE_SERVER: return "VirtualMachineServer";
	case HOSTING_ROLE_CONTAINER_SERVER: return "ContainerServer";
	default: return NULL;
	}
}

/* ComputerSystem.v1_3_0.InterfaceTypeSelection */
enum rf_interface_type_selection {
	INTERFACE_TYPE_SELECTION_INVALID = 0,
	INTERFACE_TYPE_SELECTION_NONE = 1,
	INTERFACE_TYPE_SELECTION_FIRMWARE_UPDATE = 2,
	INTERFACE_TYPE_SELECTION_BIOS_SETTING = 3,
	INTERFACE_TYPE_SELECTION_OEM_METHOD = 4,
};

static inline const char *rf_interface_type_selection_to_str(enum rf_interface_type_selection val)
{
	switch (val) {
	case INTERFACE_TYPE_SELECTION_INVALID: return "Invalid";
	case INTERFACE_TYPE_SELECTION_NONE: return "None";
	case INTERFACE_TYPE_SELECTION_FIRMWARE_UPDATE: return "FirmwareUpdate";
	case INTERFACE_TYPE_SELECTION_BIOS_SETTING: return "BiosSetting";
	case INTERFACE_TYPE_SELECTION_OEM_METHOD: return "OemMethod";
	default: return NULL;
	}
}

/* ComputerSystem.v1_5_0.WatchdogWarningActions */
enum rf_watchdog_warning_actions {
	WATCHDOG_WARNING_ACTIONS_INVALID = 0,
	WATCHDOG_WARNING_ACTIONS_NONE = 1,
	WATCHDOG_WARNING_ACTIONS_DIAGNOSTIC_INTERRUPT = 2,
	WATCHDOG_WARNING_ACTIONS_SMI = 3,
	WATCHDOG_WARNING_ACTIONS_MESSAGING_INTERRUPT = 4,
	WATCHDOG_WARNING_ACTIONS_SCI = 5,
	WATCHDOG_WARNING_ACTIONS_OEM = 6,
};

static inline const char *rf_watchdog_warning_actions_to_str(enum rf_watchdog_warning_actions val)
{
	switch (val) {
	case WATCHDOG_WARNING_ACTIONS_INVALID: return "Invalid";
	case WATCHDOG_WARNING_ACTIONS_NONE: return "None";
	case WATCHDOG_WARNING_ACTIONS_DIAGNOSTIC_INTERRUPT: return "DiagnosticInterrupt";
	case WATCHDOG_WARNING_ACTIONS_SMI: return "SMI";
	case WATCHDOG_WARNING_ACTIONS_MESSAGING_INTERRUPT: return "MessagingInterrupt";
	case WATCHDOG_WARNING_ACTIONS_SCI: return "SCI";
	case WATCHDOG_WARNING_ACTIONS_OEM: return "OEM";
	default: return NULL;
	}
}

/* ComputerSystem.v1_5_0.WatchdogTimeoutActions */
enum rf_watchdog_timeout_actions {
	WATCHDOG_TIMEOUT_ACTIONS_INVALID = 0,
	WATCHDOG_TIMEOUT_ACTIONS_NONE = 1,
	WATCHDOG_TIMEOUT_ACTIONS_RESET_SYSTEM = 2,
	WATCHDOG_TIMEOUT_ACTIONS_POWER_CYCLE = 3,
	WATCHDOG_TIMEOUT_ACTIONS_POWER_DOWN = 4,
	WATCHDOG_TIMEOUT_ACTIONS_OEM = 5,
};

static inline const char *rf_watchdog_timeout_actions_to_str(enum rf_watchdog_timeout_actions val)
{
	switch (val) {
	case WATCHDOG_TIMEOUT_ACTIONS_INVALID: return "Invalid";
	case WATCHDOG_TIMEOUT_ACTIONS_NONE: return "None";
	case WATCHDOG_TIMEOUT_ACTIONS_RESET_SYSTEM: return "ResetSystem";
	case WATCHDOG_TIMEOUT_ACTIONS_POWER_CYCLE: return "PowerCycle";
	case WATCHDOG_TIMEOUT_ACTIONS_POWER_DOWN: return "PowerDown";
	case WATCHDOG_TIMEOUT_ACTIONS_OEM: return "OEM";
	default: return NULL;
	}
}

/* ComputerSystem.v1_6_0.PowerRestorePolicyTypes */
enum rf_power_restore_policy_types {
	POWER_RESTORE_POLICY_TYPES_INVALID = 0,
	POWER_RESTORE_POLICY_TYPES_ALWAYS_ON = 1,
	POWER_RESTORE_POLICY_TYPES_ALWAYS_OFF = 2,
	POWER_RESTORE_POLICY_TYPES_LAST_STATE = 3,
};

static inline const char *rf_power_restore_policy_types_to_str(enum rf_power_restore_policy_types val)
{
	switch (val) {
	case POWER_RESTORE_POLICY_TYPES_INVALID: return "Invalid";
	case POWER_RESTORE_POLICY_TYPES_ALWAYS_ON: return "AlwaysOn";
	case POWER_RESTORE_POLICY_TYPES_ALWAYS_OFF: return "AlwaysOff";
	case POWER_RESTORE_POLICY_TYPES_LAST_STATE: return "LastState";
	default: return NULL;
	}
}

/* ComputerSystem.v1_6_0.BootOrderTypes */
enum rf_boot_order_types {
	BOOT_ORDER_TYPES_INVALID = 0,
	BOOT_ORDER_TYPES_BOOT_ORDER = 1,
	BOOT_ORDER_TYPES_ALIAS_BOOT_ORDER = 2,
};

static inline const char *rf_boot_order_types_to_str(enum rf_boot_order_types val)
{
	switch (val) {
	case BOOT_ORDER_TYPES_INVALID: return "Invalid";
	case BOOT_ORDER_TYPES_BOOT_ORDER: return "BootOrder";
	case BOOT_ORDER_TYPES_ALIAS_BOOT_ORDER: return "AliasBootOrder";
	default: return NULL;
	}
}

/* ComputerSystem.v1_11_0.AutomaticRetryConfig */
enum rf_automatic_retry_config {
	AUTOMATIC_RETRY_CONFIG_INVALID = 0,
	AUTOMATIC_RETRY_CONFIG_DISABLED = 1,
	AUTOMATIC_RETRY_CONFIG_RETRY_ATTEMPTS = 2,
	AUTOMATIC_RETRY_CONFIG_RETRY_ALWAYS = 3,
};

static inline const char *rf_automatic_retry_config_to_str(enum rf_automatic_retry_config val)
{
	switch (val) {
	case AUTOMATIC_RETRY_CONFIG_INVALID: return "Invalid";
	case AUTOMATIC_RETRY_CONFIG_DISABLED: return "Disabled";
	case AUTOMATIC_RETRY_CONFIG_RETRY_ATTEMPTS: return "RetryAttempts";
	case AUTOMATIC_RETRY_CONFIG_RETRY_ALWAYS: return "RetryAlways";
	default: return NULL;
	}
}

/* ComputerSystem.v1_13_0.BootProgressTypes */
enum rf_boot_progress_types {
	BOOT_PROGRESS_TYPES_INVALID = 0,
	BOOT_PROGRESS_TYPES_NONE = 1,
	BOOT_PROGRESS_TYPES_PRIMARY_PROCESSOR_INITIALIZATION_STARTED = 2,
	BOOT_PROGRESS_TYPES_BUS_INITIALIZATION_STARTED = 3,
	BOOT_PROGRESS_TYPES_MEMORY_INITIALIZATION_STARTED = 4,
	BOOT_PROGRESS_TYPES_SECONDARY_PROCESSOR_INITIALIZATION_STARTED = 5,
	BOOT_PROGRESS_TYPES_PCI_RESOURCE_CONFIG_STARTED = 6,
	BOOT_PROGRESS_TYPES_SYSTEM_HARDWARE_INITIALIZATION_COMPLETE = 7,
	BOOT_PROGRESS_TYPES_SETUP_ENTERED = 8,
	BOOT_PROGRESS_TYPES_OS_BOOT_STARTED = 9,
	BOOT_PROGRESS_TYPES_OS_RUNNING = 10,
	BOOT_PROGRESS_TYPES_OEM = 11,
};

static inline const char *rf_boot_progress_types_to_str(enum rf_boot_progress_types val)
{
	switch (val) {
	case BOOT_PROGRESS_TYPES_INVALID: return "Invalid";
	case BOOT_PROGRESS_TYPES_NONE: return "None";
	case BOOT_PROGRESS_TYPES_PRIMARY_PROCESSOR_INITIALIZATION_STARTED: return "PrimaryProcessorInitializationStarted";
	case BOOT_PROGRESS_TYPES_BUS_INITIALIZATION_STARTED: return "BusInitializationStarted";
	case BOOT_PROGRESS_TYPES_MEMORY_INITIALIZATION_STARTED: return "MemoryInitializationStarted";
	case BOOT_PROGRESS_TYPES_SECONDARY_PROCESSOR_INITIALIZATION_STARTED: return "SecondaryProcessorInitializationStarted";
	case BOOT_PROGRESS_TYPES_PCI_RESOURCE_CONFIG_STARTED: return "PCIResourceConfigStarted";
	case BOOT_PROGRESS_TYPES_SYSTEM_HARDWARE_INITIALIZATION_COMPLETE: return "SystemHardwareInitializationComplete";
	case BOOT_PROGRESS_TYPES_SETUP_ENTERED: return "SetupEntered";
	case BOOT_PROGRESS_TYPES_OS_BOOT_STARTED: return "OSBootStarted";
	case BOOT_PROGRESS_TYPES_OS_RUNNING: return "OSRunning";
	case BOOT_PROGRESS_TYPES_OEM: return "OEM";
	default: return NULL;
	}
}

/* ComputerSystem.v1_13_0.GraphicalConnectTypesSupported */
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
	case GRAPHICAL_CONNECT_TYPES_SUPPORTED_OEM: return "OEM";
	default: return NULL;
	}
}

/* ComputerSystem.v1_14_0.TrustedModuleRequiredToBoot */
enum rf_trusted_module_required_to_boot {
	TRUSTED_MODULE_REQUIRED_TO_BOOT_INVALID = 0,
	TRUSTED_MODULE_REQUIRED_TO_BOOT_DISABLED = 1,
	TRUSTED_MODULE_REQUIRED_TO_BOOT_REQUIRED = 2,
};

static inline const char *rf_trusted_module_required_to_boot_to_str(enum rf_trusted_module_required_to_boot val)
{
	switch (val) {
	case TRUSTED_MODULE_REQUIRED_TO_BOOT_INVALID: return "Invalid";
	case TRUSTED_MODULE_REQUIRED_TO_BOOT_DISABLED: return "Disabled";
	case TRUSTED_MODULE_REQUIRED_TO_BOOT_REQUIRED: return "Required";
	default: return NULL;
	}
}

/* ComputerSystem.v1_15_0.StopBootOnFault */
enum rf_stop_boot_on_fault {
	STOP_BOOT_ON_FAULT_INVALID = 0,
	STOP_BOOT_ON_FAULT_NEVER = 1,
	STOP_BOOT_ON_FAULT_ANY_FAULT = 2,
};

static inline const char *rf_stop_boot_on_fault_to_str(enum rf_stop_boot_on_fault val)
{
	switch (val) {
	case STOP_BOOT_ON_FAULT_INVALID: return "Invalid";
	case STOP_BOOT_ON_FAULT_NEVER: return "Never";
	case STOP_BOOT_ON_FAULT_ANY_FAULT: return "AnyFault";
	default: return NULL;
	}
}

/* ComputerSystem.v1_15_0.PowerMode */
enum rf_power_mode {
	POWER_MODE_INVALID = 0,
	POWER_MODE_MAXIMUM_PERFORMANCE = 1,
	POWER_MODE_BALANCED_PERFORMANCE = 2,
	POWER_MODE_POWER_SAVING = 3,
	POWER_MODE_STATIC = 4,
	POWER_MODE_OS_CONTROLLED = 5,
	POWER_MODE_OEM = 6,
	POWER_MODE_EFFICIENCY_FAVOR_POWER = 7,
	POWER_MODE_EFFICIENCY_FAVOR_PERFORMANCE = 8,
};

static inline const char *rf_power_mode_to_str(enum rf_power_mode val)
{
	switch (val) {
	case POWER_MODE_INVALID: return "Invalid";
	case POWER_MODE_MAXIMUM_PERFORMANCE: return "MaximumPerformance";
	case POWER_MODE_BALANCED_PERFORMANCE: return "BalancedPerformance";
	case POWER_MODE_POWER_SAVING: return "PowerSaving";
	case POWER_MODE_STATIC: return "Static";
	case POWER_MODE_OS_CONTROLLED: return "OSControlled";
	case POWER_MODE_OEM: return "OEM";
	case POWER_MODE_EFFICIENCY_FAVOR_POWER: return "EfficiencyFavorPower";
	case POWER_MODE_EFFICIENCY_FAVOR_PERFORMANCE: return "EfficiencyFavorPerformance";
	default: return NULL;
	}
}

/* ComputerSystem.v1_18_0.CompositionUseCase */
enum rf_composition_use_case {
	COMPOSITION_USE_CASE_INVALID = 0,
	COMPOSITION_USE_CASE_RESOURCE_BLOCK_CAPABLE = 1,
	COMPOSITION_USE_CASE_EXPANDABLE_SYSTEM = 2,
};

static inline const char *rf_composition_use_case_to_str(enum rf_composition_use_case val)
{
	switch (val) {
	case COMPOSITION_USE_CASE_INVALID: return "Invalid";
	case COMPOSITION_USE_CASE_RESOURCE_BLOCK_CAPABLE: return "ResourceBlockCapable";
	case COMPOSITION_USE_CASE_EXPANDABLE_SYSTEM: return "ExpandableSystem";
	default: return NULL;
	}
}

/* ComputerSystem.v1_20_0.KMIPCachePolicy */
enum rf_kmip_cache_policy {
	KMIP_CACHE_POLICY_INVALID = 0,
	KMIP_CACHE_POLICY_NONE = 1,
	KMIP_CACHE_POLICY_AFTER_FIRST_USE = 2,
};

static inline const char *rf_kmip_cache_policy_to_str(enum rf_kmip_cache_policy val)
{
	switch (val) {
	case KMIP_CACHE_POLICY_INVALID: return "Invalid";
	case KMIP_CACHE_POLICY_NONE: return "None";
	case KMIP_CACHE_POLICY_AFTER_FIRST_USE: return "AfterFirstUse";
	default: return NULL;
	}
}

/* ComputerSystem.v1_21_0.DecommissionType */
enum rf_decommission_type {
	DECOMMISSION_TYPE_INVALID = 0,
	DECOMMISSION_TYPE_ALL = 1,
	DECOMMISSION_TYPE_USER_DATA = 2,
	DECOMMISSION_TYPE_MANAGER_CONFIG = 3,
	DECOMMISSION_TYPE_BIOS_CONFIG = 4,
	DECOMMISSION_TYPE_NETWORK_CONFIG = 5,
	DECOMMISSION_TYPE_STORAGE_CONFIG = 6,
	DECOMMISSION_TYPE_LOGS = 7,
	DECOMMISSION_TYPE_TPM = 8,
};

static inline const char *rf_decommission_type_to_str(enum rf_decommission_type val)
{
	switch (val) {
	case DECOMMISSION_TYPE_INVALID: return "Invalid";
	case DECOMMISSION_TYPE_ALL: return "All";
	case DECOMMISSION_TYPE_USER_DATA: return "UserData";
	case DECOMMISSION_TYPE_MANAGER_CONFIG: return "ManagerConfig";
	case DECOMMISSION_TYPE_BIOS_CONFIG: return "BIOSConfig";
	case DECOMMISSION_TYPE_NETWORK_CONFIG: return "NetworkConfig";
	case DECOMMISSION_TYPE_STORAGE_CONFIG: return "StorageConfig";
	case DECOMMISSION_TYPE_LOGS: return "Logs";
	case DECOMMISSION_TYPE_TPM: return "TPM";
	default: return NULL;
	}
}

/* ComputerSystem.v1_23_0.LastResetCauses */
enum rf_last_reset_causes {
	LAST_RESET_CAUSES_INVALID = 0,
	LAST_RESET_CAUSES_POWER_BUTTON_PRESS = 1,
	LAST_RESET_CAUSES_MANAGEMENT_COMMAND = 2,
	LAST_RESET_CAUSES_POWER_RESTORE_POLICY = 3,
	LAST_RESET_CAUSES_RTC_WAKEUP = 4,
	LAST_RESET_CAUSES_WATCHDOG_EXPIRATION = 5,
	LAST_RESET_CAUSES_OS_SOFT_RESTART = 6,
	LAST_RESET_CAUSES_SYSTEM_CRASH = 7,
	LAST_RESET_CAUSES_THERMAL_EVENT = 8,
	LAST_RESET_CAUSES_POWER_EVENT = 9,
	LAST_RESET_CAUSES_UNKNOWN = 10,
};

static inline const char *rf_last_reset_causes_to_str(enum rf_last_reset_causes val)
{
	switch (val) {
	case LAST_RESET_CAUSES_INVALID: return "Invalid";
	case LAST_RESET_CAUSES_POWER_BUTTON_PRESS: return "PowerButtonPress";
	case LAST_RESET_CAUSES_MANAGEMENT_COMMAND: return "ManagementCommand";
	case LAST_RESET_CAUSES_POWER_RESTORE_POLICY: return "PowerRestorePolicy";
	case LAST_RESET_CAUSES_RTC_WAKEUP: return "RTCWakeup";
	case LAST_RESET_CAUSES_WATCHDOG_EXPIRATION: return "WatchdogExpiration";
	case LAST_RESET_CAUSES_OS_SOFT_RESTART: return "OSSoftRestart";
	case LAST_RESET_CAUSES_SYSTEM_CRASH: return "SystemCrash";
	case LAST_RESET_CAUSES_THERMAL_EVENT: return "ThermalEvent";
	case LAST_RESET_CAUSES_POWER_EVENT: return "PowerEvent";
	case LAST_RESET_CAUSES_UNKNOWN: return "Unknown";
	default: return NULL;
	}
}

/* ComputerSystem.v1_26_0.ExportType */
enum rf_export_type {
	EXPORT_TYPE_INVALID = 0,
	EXPORT_TYPE_NON_DESTRUCTIVE = 1,
	EXPORT_TYPE_CLONE_WITHIN_FABRIC = 2,
	EXPORT_TYPE_REPLACEMENT = 3,
};

static inline const char *rf_export_type_to_str(enum rf_export_type val)
{
	switch (val) {
	case EXPORT_TYPE_INVALID: return "Invalid";
	case EXPORT_TYPE_NON_DESTRUCTIVE: return "NonDestructive";
	case EXPORT_TYPE_CLONE_WITHIN_FABRIC: return "CloneWithinFabric";
	case EXPORT_TYPE_REPLACEMENT: return "Replacement";
	default: return NULL;
	}
}

/* ComputerSystem.v1_26_0.Component */
enum rf_component {
	COMPONENT_INVALID = 0,
	COMPONENT_ALL = 1,
	COMPONENT_MANAGER = 2,
	COMPONENT_BIOS = 3,
	COMPONENT_NETWORK = 4,
	COMPONENT_STORAGE = 5,
};

static inline const char *rf_component_to_str(enum rf_component val)
{
	switch (val) {
	case COMPONENT_INVALID: return "Invalid";
	case COMPONENT_ALL: return "All";
	case COMPONENT_MANAGER: return "Manager";
	case COMPONENT_BIOS: return "BIOS";
	case COMPONENT_NETWORK: return "Network";
	case COMPONENT_STORAGE: return "Storage";
	default: return NULL;
	}
}

/* ComputerSystem.v1_26_0.ExportSecurity */
enum rf_export_security {
	EXPORT_SECURITY_INVALID = 0,
	EXPORT_SECURITY_INCLUDE_SENSITIVE_DATA = 1,
	EXPORT_SECURITY_HASHED_DATA_ONLY = 2,
	EXPORT_SECURITY_EXCLUDE_SENSITIVE_DATA = 3,
};

static inline const char *rf_export_security_to_str(enum rf_export_security val)
{
	switch (val) {
	case EXPORT_SECURITY_INVALID: return "Invalid";
	case EXPORT_SECURITY_INCLUDE_SENSITIVE_DATA: return "IncludeSensitiveData";
	case EXPORT_SECURITY_HASHED_DATA_ONLY: return "HashedDataOnly";
	case EXPORT_SECURITY_EXCLUDE_SENSITIVE_DATA: return "ExcludeSensitiveData";
	default: return NULL;
	}
}

/* ComputerSystem.v1_28_0.ImageChecksumAlgorithm */
enum rf_image_checksum_algorithm {
	IMAGE_CHECKSUM_ALGORITHM_INVALID = 0,
	IMAGE_CHECKSUM_ALGORITHM_SHA256 = 1,
};

static inline const char *rf_image_checksum_algorithm_to_str(enum rf_image_checksum_algorithm val)
{
	switch (val) {
	case IMAGE_CHECKSUM_ALGORITHM_INVALID: return "Invalid";
	case IMAGE_CHECKSUM_ALGORITHM_SHA256: return "SHA256";
	default: return NULL;
	}
}

/* ComputerSystem.v1_29_0.ConsoleAuthenticationMethod */
enum rf_console_authentication_method {
	CONSOLE_AUTHENTICATION_METHOD_INVALID = 0,
	CONSOLE_AUTHENTICATION_METHOD_EXPLICIT_LOGIN = 1,
	CONSOLE_AUTHENTICATION_METHOD_ONE_TIME_URL = 2,
	CONSOLE_AUTHENTICATION_METHOD_ONE_TIME_PASSWORD = 3,
};

static inline const char *rf_console_authentication_method_to_str(enum rf_console_authentication_method val)
{
	switch (val) {
	case CONSOLE_AUTHENTICATION_METHOD_INVALID: return "Invalid";
	case CONSOLE_AUTHENTICATION_METHOD_EXPLICIT_LOGIN: return "ExplicitLogin";
	case CONSOLE_AUTHENTICATION_METHOD_ONE_TIME_URL: return "OneTimeURL";
	case CONSOLE_AUTHENTICATION_METHOD_ONE_TIME_PASSWORD: return "OneTimePassword";
	default: return NULL;
	}
}

/* ComputerSystem.v1_29_0.ConsoleAccessMode */
enum rf_console_access_mode {
	CONSOLE_ACCESS_MODE_INVALID = 0,
	CONSOLE_ACCESS_MODE_READ_WRITE = 1,
	CONSOLE_ACCESS_MODE_READ_ONLY = 2,
};

static inline const char *rf_console_access_mode_to_str(enum rf_console_access_mode val)
{
	switch (val) {
	case CONSOLE_ACCESS_MODE_INVALID: return "Invalid";
	case CONSOLE_ACCESS_MODE_READ_WRITE: return "ReadWrite";
	case CONSOLE_ACCESS_MODE_READ_ONLY: return "ReadOnly";
	default: return NULL;
	}
}

/* ComputerSystem.v1_29_0.GraphicalConsoleType */
enum rf_graphical_console_type {
	GRAPHICAL_CONSOLE_TYPE_INVALID = 0,
	GRAPHICAL_CONSOLE_TYPE_VNC = 1,
	GRAPHICAL_CONSOLE_TYPE_RDP = 2,
	GRAPHICAL_CONSOLE_TYPE_HTML5 = 3,
	GRAPHICAL_CONSOLE_TYPE_KVMIP = 4,
};

static inline const char *rf_graphical_console_type_to_str(enum rf_graphical_console_type val)
{
	switch (val) {
	case GRAPHICAL_CONSOLE_TYPE_INVALID: return "Invalid";
	case GRAPHICAL_CONSOLE_TYPE_VNC: return "VNC";
	case GRAPHICAL_CONSOLE_TYPE_RDP: return "RDP";
	case GRAPHICAL_CONSOLE_TYPE_HTML5: return "HTML5";
	case GRAPHICAL_CONSOLE_TYPE_KVMIP: return "KVMIP";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_COMPUTER_SYSTEM_H */
