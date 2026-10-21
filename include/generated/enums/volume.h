/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_VOLUME_H
#define REDFISH_ENUM_VOLUME_H

#include <stddef.h>

/* Volume.InitializeType */
enum rf_initialize_type {
	INITIALIZE_TYPE_INVALID = 0,
	INITIALIZE_TYPE_FAST = 1,
	INITIALIZE_TYPE_SLOW = 2,
};

static inline const char *rf_initialize_type_to_str(enum rf_initialize_type val)
{
	switch (val) {
	case INITIALIZE_TYPE_INVALID: return "Invalid";
	case INITIALIZE_TYPE_FAST: return "Fast";
	case INITIALIZE_TYPE_SLOW: return "Slow";
	default: return NULL;
	}
}

/* Volume.InitializeMethod */
enum rf_initialize_method {
	INITIALIZE_METHOD_INVALID = 0,
	INITIALIZE_METHOD_SKIP = 1,
	INITIALIZE_METHOD_BACKGROUND = 2,
	INITIALIZE_METHOD_FOREGROUND = 3,
};

static inline const char *rf_initialize_method_to_str(enum rf_initialize_method val)
{
	switch (val) {
	case INITIALIZE_METHOD_INVALID: return "Invalid";
	case INITIALIZE_METHOD_SKIP: return "Skip";
	case INITIALIZE_METHOD_BACKGROUND: return "Background";
	case INITIALIZE_METHOD_FOREGROUND: return "Foreground";
	default: return NULL;
	}
}

/* Volume.RAIDType */
enum rf_raid_type {
	RAID_TYPE_INVALID = 0,
	RAID_TYPE_RAID0 = 1,
	RAID_TYPE_RAID1 = 2,
	RAID_TYPE_RAID3 = 3,
	RAID_TYPE_RAID4 = 4,
	RAID_TYPE_RAID5 = 5,
	RAID_TYPE_RAID6 = 6,
	RAID_TYPE_RAID10 = 7,
	RAID_TYPE_RAID01 = 8,
	RAID_TYPE_RAID6_TP = 9,
	RAID_TYPE_RAID1_E = 10,
	RAID_TYPE_RAID50 = 11,
	RAID_TYPE_RAID60 = 12,
	RAID_TYPE_RAID00 = 13,
	RAID_TYPE_RAID10_E = 14,
	RAID_TYPE_RAID1_TRIPLE = 15,
	RAID_TYPE_RAID10_TRIPLE = 16,
	RAID_TYPE_NONE = 17,
};

static inline const char *rf_raid_type_to_str(enum rf_raid_type val)
{
	switch (val) {
	case RAID_TYPE_INVALID: return "Invalid";
	case RAID_TYPE_RAID0: return "RAID0";
	case RAID_TYPE_RAID1: return "RAID1";
	case RAID_TYPE_RAID3: return "RAID3";
	case RAID_TYPE_RAID4: return "RAID4";
	case RAID_TYPE_RAID5: return "RAID5";
	case RAID_TYPE_RAID6: return "RAID6";
	case RAID_TYPE_RAID10: return "RAID10";
	case RAID_TYPE_RAID01: return "RAID01";
	case RAID_TYPE_RAID6_TP: return "RAID6TP";
	case RAID_TYPE_RAID1_E: return "RAID1E";
	case RAID_TYPE_RAID50: return "RAID50";
	case RAID_TYPE_RAID60: return "RAID60";
	case RAID_TYPE_RAID00: return "RAID00";
	case RAID_TYPE_RAID10_E: return "RAID10E";
	case RAID_TYPE_RAID1_TRIPLE: return "RAID1Triple";
	case RAID_TYPE_RAID10_TRIPLE: return "RAID10Triple";
	case RAID_TYPE_NONE: return "None";
	default: return NULL;
	}
}

/* Volume.VolumeType */
enum rf_volume_type {
	VOLUME_TYPE_INVALID = 0,
	VOLUME_TYPE_RAW_DEVICE = 1,
	VOLUME_TYPE_NON_REDUNDANT = 2,
	VOLUME_TYPE_MIRRORED = 3,
	VOLUME_TYPE_STRIPED_WITH_PARITY = 4,
	VOLUME_TYPE_SPANNED_MIRRORS = 5,
	VOLUME_TYPE_SPANNED_STRIPES_WITH_PARITY = 6,
};

static inline const char *rf_volume_type_to_str(enum rf_volume_type val)
{
	switch (val) {
	case VOLUME_TYPE_INVALID: return "Invalid";
	case VOLUME_TYPE_RAW_DEVICE: return "RawDevice";
	case VOLUME_TYPE_NON_REDUNDANT: return "NonRedundant";
	case VOLUME_TYPE_MIRRORED: return "Mirrored";
	case VOLUME_TYPE_STRIPED_WITH_PARITY: return "StripedWithParity";
	case VOLUME_TYPE_SPANNED_MIRRORS: return "SpannedMirrors";
	case VOLUME_TYPE_SPANNED_STRIPES_WITH_PARITY: return "SpannedStripesWithParity";
	default: return NULL;
	}
}

/* Volume.EncryptionTypes */
enum rf_encryption_types {
	ENCRYPTION_TYPES_INVALID = 0,
	ENCRYPTION_TYPES_NATIVE_DRIVE_ENCRYPTION = 1,
	ENCRYPTION_TYPES_CONTROLLER_ASSISTED = 2,
	ENCRYPTION_TYPES_SOFTWARE_ASSISTED = 3,
};

static inline const char *rf_encryption_types_to_str(enum rf_encryption_types val)
{
	switch (val) {
	case ENCRYPTION_TYPES_INVALID: return "Invalid";
	case ENCRYPTION_TYPES_NATIVE_DRIVE_ENCRYPTION: return "NativeDriveEncryption";
	case ENCRYPTION_TYPES_CONTROLLER_ASSISTED: return "ControllerAssisted";
	case ENCRYPTION_TYPES_SOFTWARE_ASSISTED: return "SoftwareAssisted";
	default: return NULL;
	}
}

/* Volume.WriteHoleProtectionPolicyType */
enum rf_write_hole_protection_policy_type {
	WRITE_HOLE_PROTECTION_POLICY_TYPE_INVALID = 0,
	WRITE_HOLE_PROTECTION_POLICY_TYPE_OFF = 1,
	WRITE_HOLE_PROTECTION_POLICY_TYPE_JOURNALING = 2,
	WRITE_HOLE_PROTECTION_POLICY_TYPE_DISTRIBUTED_LOG = 3,
	WRITE_HOLE_PROTECTION_POLICY_TYPE_OEM = 4,
};

static inline const char *rf_write_hole_protection_policy_type_to_str(enum rf_write_hole_protection_policy_type val)
{
	switch (val) {
	case WRITE_HOLE_PROTECTION_POLICY_TYPE_INVALID: return "Invalid";
	case WRITE_HOLE_PROTECTION_POLICY_TYPE_OFF: return "Off";
	case WRITE_HOLE_PROTECTION_POLICY_TYPE_JOURNALING: return "Journaling";
	case WRITE_HOLE_PROTECTION_POLICY_TYPE_DISTRIBUTED_LOG: return "DistributedLog";
	case WRITE_HOLE_PROTECTION_POLICY_TYPE_OEM: return "Oem";
	default: return NULL;
	}
}

/* Volume.VolumeUsageType */
enum rf_volume_usage_type {
	VOLUME_USAGE_TYPE_INVALID = 0,
	VOLUME_USAGE_TYPE_DATA = 1,
	VOLUME_USAGE_TYPE_SYSTEM_DATA = 2,
	VOLUME_USAGE_TYPE_CACHE_ONLY = 3,
	VOLUME_USAGE_TYPE_SYSTEM_RESERVE = 4,
	VOLUME_USAGE_TYPE_REPLICATION_RESERVE = 5,
};

static inline const char *rf_volume_usage_type_to_str(enum rf_volume_usage_type val)
{
	switch (val) {
	case VOLUME_USAGE_TYPE_INVALID: return "Invalid";
	case VOLUME_USAGE_TYPE_DATA: return "Data";
	case VOLUME_USAGE_TYPE_SYSTEM_DATA: return "SystemData";
	case VOLUME_USAGE_TYPE_CACHE_ONLY: return "CacheOnly";
	case VOLUME_USAGE_TYPE_SYSTEM_RESERVE: return "SystemReserve";
	case VOLUME_USAGE_TYPE_REPLICATION_RESERVE: return "ReplicationReserve";
	default: return NULL;
	}
}

/* Volume.ReadCachePolicyType */
enum rf_read_cache_policy_type {
	READ_CACHE_POLICY_TYPE_INVALID = 0,
	READ_CACHE_POLICY_TYPE_READ_AHEAD = 1,
	READ_CACHE_POLICY_TYPE_ADAPTIVE_READ_AHEAD = 2,
	READ_CACHE_POLICY_TYPE_OFF = 3,
};

static inline const char *rf_read_cache_policy_type_to_str(enum rf_read_cache_policy_type val)
{
	switch (val) {
	case READ_CACHE_POLICY_TYPE_INVALID: return "Invalid";
	case READ_CACHE_POLICY_TYPE_READ_AHEAD: return "ReadAhead";
	case READ_CACHE_POLICY_TYPE_ADAPTIVE_READ_AHEAD: return "AdaptiveReadAhead";
	case READ_CACHE_POLICY_TYPE_OFF: return "Off";
	default: return NULL;
	}
}

/* Volume.WriteCachePolicyType */
enum rf_write_cache_policy_type {
	WRITE_CACHE_POLICY_TYPE_INVALID = 0,
	WRITE_CACHE_POLICY_TYPE_WRITE_THROUGH = 1,
	WRITE_CACHE_POLICY_TYPE_PROTECTED_WRITE_BACK = 2,
	WRITE_CACHE_POLICY_TYPE_UNPROTECTED_WRITE_BACK = 3,
	WRITE_CACHE_POLICY_TYPE_OFF = 4,
};

static inline const char *rf_write_cache_policy_type_to_str(enum rf_write_cache_policy_type val)
{
	switch (val) {
	case WRITE_CACHE_POLICY_TYPE_INVALID: return "Invalid";
	case WRITE_CACHE_POLICY_TYPE_WRITE_THROUGH: return "WriteThrough";
	case WRITE_CACHE_POLICY_TYPE_PROTECTED_WRITE_BACK: return "ProtectedWriteBack";
	case WRITE_CACHE_POLICY_TYPE_UNPROTECTED_WRITE_BACK: return "UnprotectedWriteBack";
	case WRITE_CACHE_POLICY_TYPE_OFF: return "Off";
	default: return NULL;
	}
}

/* Volume.WriteCacheStateType */
enum rf_write_cache_state_type {
	WRITE_CACHE_STATE_TYPE_INVALID = 0,
	WRITE_CACHE_STATE_TYPE_UNPROTECTED = 1,
	WRITE_CACHE_STATE_TYPE_PROTECTED = 2,
	WRITE_CACHE_STATE_TYPE_DEGRADED = 3,
};

static inline const char *rf_write_cache_state_type_to_str(enum rf_write_cache_state_type val)
{
	switch (val) {
	case WRITE_CACHE_STATE_TYPE_INVALID: return "Invalid";
	case WRITE_CACHE_STATE_TYPE_UNPROTECTED: return "Unprotected";
	case WRITE_CACHE_STATE_TYPE_PROTECTED: return "Protected";
	case WRITE_CACHE_STATE_TYPE_DEGRADED: return "Degraded";
	default: return NULL;
	}
}

/* Volume.LBAFormatType */
enum rf_lba_format_type {
	LBA_FORMAT_TYPE_INVALID = 0,
	LBA_FORMAT_TYPE_LBA_FORMAT0 = 1,
	LBA_FORMAT_TYPE_LBA_FORMAT1 = 2,
	LBA_FORMAT_TYPE_LBA_FORMAT2 = 3,
	LBA_FORMAT_TYPE_LBA_FORMAT3 = 4,
	LBA_FORMAT_TYPE_LBA_FORMAT4 = 5,
	LBA_FORMAT_TYPE_LBA_FORMAT5 = 6,
	LBA_FORMAT_TYPE_LBA_FORMAT6 = 7,
	LBA_FORMAT_TYPE_LBA_FORMAT7 = 8,
	LBA_FORMAT_TYPE_LBA_FORMAT8 = 9,
	LBA_FORMAT_TYPE_LBA_FORMAT9 = 10,
	LBA_FORMAT_TYPE_LBA_FORMAT10 = 11,
	LBA_FORMAT_TYPE_LBA_FORMAT11 = 12,
	LBA_FORMAT_TYPE_LBA_FORMAT12 = 13,
	LBA_FORMAT_TYPE_LBA_FORMAT13 = 14,
	LBA_FORMAT_TYPE_LBA_FORMAT14 = 15,
	LBA_FORMAT_TYPE_LBA_FORMAT15 = 16,
};

static inline const char *rf_lba_format_type_to_str(enum rf_lba_format_type val)
{
	switch (val) {
	case LBA_FORMAT_TYPE_INVALID: return "Invalid";
	case LBA_FORMAT_TYPE_LBA_FORMAT0: return "LBAFormat0";
	case LBA_FORMAT_TYPE_LBA_FORMAT1: return "LBAFormat1";
	case LBA_FORMAT_TYPE_LBA_FORMAT2: return "LBAFormat2";
	case LBA_FORMAT_TYPE_LBA_FORMAT3: return "LBAFormat3";
	case LBA_FORMAT_TYPE_LBA_FORMAT4: return "LBAFormat4";
	case LBA_FORMAT_TYPE_LBA_FORMAT5: return "LBAFormat5";
	case LBA_FORMAT_TYPE_LBA_FORMAT6: return "LBAFormat6";
	case LBA_FORMAT_TYPE_LBA_FORMAT7: return "LBAFormat7";
	case LBA_FORMAT_TYPE_LBA_FORMAT8: return "LBAFormat8";
	case LBA_FORMAT_TYPE_LBA_FORMAT9: return "LBAFormat9";
	case LBA_FORMAT_TYPE_LBA_FORMAT10: return "LBAFormat10";
	case LBA_FORMAT_TYPE_LBA_FORMAT11: return "LBAFormat11";
	case LBA_FORMAT_TYPE_LBA_FORMAT12: return "LBAFormat12";
	case LBA_FORMAT_TYPE_LBA_FORMAT13: return "LBAFormat13";
	case LBA_FORMAT_TYPE_LBA_FORMAT14: return "LBAFormat14";
	case LBA_FORMAT_TYPE_LBA_FORMAT15: return "LBAFormat15";
	default: return NULL;
	}
}

/* Volume.NamespaceType */
enum rf_namespace_type {
	NAMESPACE_TYPE_INVALID = 0,
	NAMESPACE_TYPE_BLOCK = 1,
	NAMESPACE_TYPE_KEY_VALUE = 2,
	NAMESPACE_TYPE_ZNS = 3,
	NAMESPACE_TYPE_COMPUTATIONAL = 4,
};

static inline const char *rf_namespace_type_to_str(enum rf_namespace_type val)
{
	switch (val) {
	case NAMESPACE_TYPE_INVALID: return "Invalid";
	case NAMESPACE_TYPE_BLOCK: return "Block";
	case NAMESPACE_TYPE_KEY_VALUE: return "KeyValue";
	case NAMESPACE_TYPE_ZNS: return "ZNS";
	case NAMESPACE_TYPE_COMPUTATIONAL: return "Computational";
	default: return NULL;
	}
}

/* Volume.OperationType */
enum rf_operation_type {
	OPERATION_TYPE_INVALID = 0,
	OPERATION_TYPE_DEDUPLICATE = 1,
	OPERATION_TYPE_CHECK_CONSISTENCY = 2,
	OPERATION_TYPE_INITIALIZE = 3,
	OPERATION_TYPE_REPLICATE = 4,
	OPERATION_TYPE_DELETE = 5,
	OPERATION_TYPE_CHANGE_RAID_TYPE = 6,
	OPERATION_TYPE_REBUILD = 7,
	OPERATION_TYPE_ENCRYPT = 8,
	OPERATION_TYPE_DECRYPT = 9,
	OPERATION_TYPE_RESIZE = 10,
	OPERATION_TYPE_COMPRESS = 11,
	OPERATION_TYPE_SANITIZE = 12,
	OPERATION_TYPE_FORMAT = 13,
	OPERATION_TYPE_CHANGE_STRIP_SIZE = 14,
};

static inline const char *rf_operation_type_to_str(enum rf_operation_type val)
{
	switch (val) {
	case OPERATION_TYPE_INVALID: return "Invalid";
	case OPERATION_TYPE_DEDUPLICATE: return "Deduplicate";
	case OPERATION_TYPE_CHECK_CONSISTENCY: return "CheckConsistency";
	case OPERATION_TYPE_INITIALIZE: return "Initialize";
	case OPERATION_TYPE_REPLICATE: return "Replicate";
	case OPERATION_TYPE_DELETE: return "Delete";
	case OPERATION_TYPE_CHANGE_RAID_TYPE: return "ChangeRAIDType";
	case OPERATION_TYPE_REBUILD: return "Rebuild";
	case OPERATION_TYPE_ENCRYPT: return "Encrypt";
	case OPERATION_TYPE_DECRYPT: return "Decrypt";
	case OPERATION_TYPE_RESIZE: return "Resize";
	case OPERATION_TYPE_COMPRESS: return "Compress";
	case OPERATION_TYPE_SANITIZE: return "Sanitize";
	case OPERATION_TYPE_FORMAT: return "Format";
	case OPERATION_TYPE_CHANGE_STRIP_SIZE: return "ChangeStripSize";
	default: return NULL;
	}
}

/* Volume.LBARelativePerformanceType */
enum rf_lba_relative_performance_type {
	LBA_RELATIVE_PERFORMANCE_TYPE_INVALID = 0,
	LBA_RELATIVE_PERFORMANCE_TYPE_BEST = 1,
	LBA_RELATIVE_PERFORMANCE_TYPE_BETTER = 2,
	LBA_RELATIVE_PERFORMANCE_TYPE_GOOD = 3,
	LBA_RELATIVE_PERFORMANCE_TYPE_DEGRADED = 4,
};

static inline const char *rf_lba_relative_performance_type_to_str(enum rf_lba_relative_performance_type val)
{
	switch (val) {
	case LBA_RELATIVE_PERFORMANCE_TYPE_INVALID: return "Invalid";
	case LBA_RELATIVE_PERFORMANCE_TYPE_BEST: return "Best";
	case LBA_RELATIVE_PERFORMANCE_TYPE_BETTER: return "Better";
	case LBA_RELATIVE_PERFORMANCE_TYPE_GOOD: return "Good";
	case LBA_RELATIVE_PERFORMANCE_TYPE_DEGRADED: return "Degraded";
	default: return NULL;
	}
}

/* Volume.ReservationType */
enum rf_reservation_type {
	RESERVATION_TYPE_INVALID = 0,
	RESERVATION_TYPE_WRITE_EXCLUSIVE = 1,
	RESERVATION_TYPE_EXCLUSIVE_ACCESS = 2,
	RESERVATION_TYPE_WRITE_EXCLUSIVE_REGISTRANTS_ONLY = 3,
	RESERVATION_TYPE_EXCLUSIVE_ACCESS_REGISTRANTS_ONLY = 4,
	RESERVATION_TYPE_WRITE_EXCLUSIVE_ALL_REGISTRANTS = 5,
	RESERVATION_TYPE_EXCLUSIVE_ACCESS_ALL_REGISTRANTS = 6,
};

static inline const char *rf_reservation_type_to_str(enum rf_reservation_type val)
{
	switch (val) {
	case RESERVATION_TYPE_INVALID: return "Invalid";
	case RESERVATION_TYPE_WRITE_EXCLUSIVE: return "WriteExclusive";
	case RESERVATION_TYPE_EXCLUSIVE_ACCESS: return "ExclusiveAccess";
	case RESERVATION_TYPE_WRITE_EXCLUSIVE_REGISTRANTS_ONLY: return "WriteExclusiveRegistrantsOnly";
	case RESERVATION_TYPE_EXCLUSIVE_ACCESS_REGISTRANTS_ONLY: return "ExclusiveAccessRegistrantsOnly";
	case RESERVATION_TYPE_WRITE_EXCLUSIVE_ALL_REGISTRANTS: return "WriteExclusiveAllRegistrants";
	case RESERVATION_TYPE_EXCLUSIVE_ACCESS_ALL_REGISTRANTS: return "ExclusiveAccessAllRegistrants";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_VOLUME_H */
