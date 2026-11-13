/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_MEMORY_H
#define REDFISH_ENUM_MEMORY_H

#include <stddef.h>

/* Memory.v1_0_0.MemoryType */
enum rf_memory_type {
	MEMORY_TYPE_INVALID = 0,
	MEMORY_TYPE_DRAM = 1,
	MEMORY_TYPE_NVDIMM_N = 2,
	MEMORY_TYPE_NVDIMM_F = 3,
	MEMORY_TYPE_NVDIMM_P = 4,
	MEMORY_TYPE_INTEL_OPTANE = 5,
	MEMORY_TYPE_CACHE = 6,
};

static inline const char *rf_memory_type_to_str(enum rf_memory_type val)
{
	switch (val) {
	case MEMORY_TYPE_INVALID: return "Invalid";
	case MEMORY_TYPE_DRAM: return "DRAM";
	case MEMORY_TYPE_NVDIMM_N: return "NVDIMM_N";
	case MEMORY_TYPE_NVDIMM_F: return "NVDIMM_F";
	case MEMORY_TYPE_NVDIMM_P: return "NVDIMM_P";
	case MEMORY_TYPE_INTEL_OPTANE: return "IntelOptane";
	case MEMORY_TYPE_CACHE: return "Cache";
	default: return NULL;
	}
}

/* Memory.v1_0_0.MemoryDeviceType */
enum rf_memory_device_type {
	MEMORY_DEVICE_TYPE_INVALID = 0,
	MEMORY_DEVICE_TYPE_DDR = 1,
	MEMORY_DEVICE_TYPE_DDR2 = 2,
	MEMORY_DEVICE_TYPE_DDR3 = 3,
	MEMORY_DEVICE_TYPE_DDR4 = 4,
	MEMORY_DEVICE_TYPE_DDR4_SDRAM = 5,
	MEMORY_DEVICE_TYPE_DDR4_E_SDRAM = 6,
	MEMORY_DEVICE_TYPE_LPDDR4_SDRAM = 7,
	MEMORY_DEVICE_TYPE_DDR3_SDRAM = 8,
	MEMORY_DEVICE_TYPE_LPDDR3_SDRAM = 9,
	MEMORY_DEVICE_TYPE_DDR2_SDRAM = 10,
	MEMORY_DEVICE_TYPE_DDR2_SDRAM_FB_DIMM = 11,
	MEMORY_DEVICE_TYPE_DDR2_SDRAM_FB_DIMM_PROBE = 12,
	MEMORY_DEVICE_TYPE_DDR_SGRAM = 13,
	MEMORY_DEVICE_TYPE_DDR_SDRAM = 14,
	MEMORY_DEVICE_TYPE_ROM = 15,
	MEMORY_DEVICE_TYPE_SDRAM = 16,
	MEMORY_DEVICE_TYPE_EDO = 17,
	MEMORY_DEVICE_TYPE_FAST_PAGE_MODE = 18,
	MEMORY_DEVICE_TYPE_PIPELINED_NIBBLE = 19,
	MEMORY_DEVICE_TYPE_LOGICAL = 20,
	MEMORY_DEVICE_TYPE_HBM = 21,
	MEMORY_DEVICE_TYPE_HBM2 = 22,
	MEMORY_DEVICE_TYPE_HBM2_E = 23,
	MEMORY_DEVICE_TYPE_HBM3 = 24,
	MEMORY_DEVICE_TYPE_HBM3_E = 25,
	MEMORY_DEVICE_TYPE_HBM4 = 26,
	MEMORY_DEVICE_TYPE_GDDR = 27,
	MEMORY_DEVICE_TYPE_GDDR2 = 28,
	MEMORY_DEVICE_TYPE_GDDR3 = 29,
	MEMORY_DEVICE_TYPE_GDDR4 = 30,
	MEMORY_DEVICE_TYPE_GDDR5 = 31,
	MEMORY_DEVICE_TYPE_GDDR5_X = 32,
	MEMORY_DEVICE_TYPE_GDDR6 = 33,
	MEMORY_DEVICE_TYPE_GDDR7 = 34,
	MEMORY_DEVICE_TYPE_DDR5 = 35,
	MEMORY_DEVICE_TYPE_OEM = 36,
	MEMORY_DEVICE_TYPE_LPDDR5_SDRAM = 37,
	MEMORY_DEVICE_TYPE_DDR5_MRDIMM = 38,
};

static inline const char *rf_memory_device_type_to_str(enum rf_memory_device_type val)
{
	switch (val) {
	case MEMORY_DEVICE_TYPE_INVALID: return "Invalid";
	case MEMORY_DEVICE_TYPE_DDR: return "DDR";
	case MEMORY_DEVICE_TYPE_DDR2: return "DDR2";
	case MEMORY_DEVICE_TYPE_DDR3: return "DDR3";
	case MEMORY_DEVICE_TYPE_DDR4: return "DDR4";
	case MEMORY_DEVICE_TYPE_DDR4_SDRAM: return "DDR4_SDRAM";
	case MEMORY_DEVICE_TYPE_DDR4_E_SDRAM: return "DDR4E_SDRAM";
	case MEMORY_DEVICE_TYPE_LPDDR4_SDRAM: return "LPDDR4_SDRAM";
	case MEMORY_DEVICE_TYPE_DDR3_SDRAM: return "DDR3_SDRAM";
	case MEMORY_DEVICE_TYPE_LPDDR3_SDRAM: return "LPDDR3_SDRAM";
	case MEMORY_DEVICE_TYPE_DDR2_SDRAM: return "DDR2_SDRAM";
	case MEMORY_DEVICE_TYPE_DDR2_SDRAM_FB_DIMM: return "DDR2_SDRAM_FB_DIMM";
	case MEMORY_DEVICE_TYPE_DDR2_SDRAM_FB_DIMM_PROBE: return "DDR2_SDRAM_FB_DIMM_PROBE";
	case MEMORY_DEVICE_TYPE_DDR_SGRAM: return "DDR_SGRAM";
	case MEMORY_DEVICE_TYPE_DDR_SDRAM: return "DDR_SDRAM";
	case MEMORY_DEVICE_TYPE_ROM: return "ROM";
	case MEMORY_DEVICE_TYPE_SDRAM: return "SDRAM";
	case MEMORY_DEVICE_TYPE_EDO: return "EDO";
	case MEMORY_DEVICE_TYPE_FAST_PAGE_MODE: return "FastPageMode";
	case MEMORY_DEVICE_TYPE_PIPELINED_NIBBLE: return "PipelinedNibble";
	case MEMORY_DEVICE_TYPE_LOGICAL: return "Logical";
	case MEMORY_DEVICE_TYPE_HBM: return "HBM";
	case MEMORY_DEVICE_TYPE_HBM2: return "HBM2";
	case MEMORY_DEVICE_TYPE_HBM2_E: return "HBM2E";
	case MEMORY_DEVICE_TYPE_HBM3: return "HBM3";
	case MEMORY_DEVICE_TYPE_HBM3_E: return "HBM3E";
	case MEMORY_DEVICE_TYPE_HBM4: return "HBM4";
	case MEMORY_DEVICE_TYPE_GDDR: return "GDDR";
	case MEMORY_DEVICE_TYPE_GDDR2: return "GDDR2";
	case MEMORY_DEVICE_TYPE_GDDR3: return "GDDR3";
	case MEMORY_DEVICE_TYPE_GDDR4: return "GDDR4";
	case MEMORY_DEVICE_TYPE_GDDR5: return "GDDR5";
	case MEMORY_DEVICE_TYPE_GDDR5_X: return "GDDR5X";
	case MEMORY_DEVICE_TYPE_GDDR6: return "GDDR6";
	case MEMORY_DEVICE_TYPE_GDDR7: return "GDDR7";
	case MEMORY_DEVICE_TYPE_DDR5: return "DDR5";
	case MEMORY_DEVICE_TYPE_OEM: return "OEM";
	case MEMORY_DEVICE_TYPE_LPDDR5_SDRAM: return "LPDDR5_SDRAM";
	case MEMORY_DEVICE_TYPE_DDR5_MRDIMM: return "DDR5_MRDIMM";
	default: return NULL;
	}
}

/* Memory.v1_0_0.BaseModuleType */
enum rf_base_module_type {
	BASE_MODULE_TYPE_INVALID = 0,
	BASE_MODULE_TYPE_RDIMM = 1,
	BASE_MODULE_TYPE_UDIMM = 2,
	BASE_MODULE_TYPE_SO_DIMM = 3,
	BASE_MODULE_TYPE_LRDIMM = 4,
	BASE_MODULE_TYPE_MINI_RDIMM = 5,
	BASE_MODULE_TYPE_MINI_UDIMM = 6,
	BASE_MODULE_TYPE_SO_RDIMM_72B = 7,
	BASE_MODULE_TYPE_SO_UDIMM_72B = 8,
	BASE_MODULE_TYPE_SO_DIMM_16B = 9,
	BASE_MODULE_TYPE_SO_DIMM_32B = 10,
	BASE_MODULE_TYPE_DIE = 11,
	BASE_MODULE_TYPE_CAMM = 12,
};

static inline const char *rf_base_module_type_to_str(enum rf_base_module_type val)
{
	switch (val) {
	case BASE_MODULE_TYPE_INVALID: return "Invalid";
	case BASE_MODULE_TYPE_RDIMM: return "RDIMM";
	case BASE_MODULE_TYPE_UDIMM: return "UDIMM";
	case BASE_MODULE_TYPE_SO_DIMM: return "SO_DIMM";
	case BASE_MODULE_TYPE_LRDIMM: return "LRDIMM";
	case BASE_MODULE_TYPE_MINI_RDIMM: return "Mini_RDIMM";
	case BASE_MODULE_TYPE_MINI_UDIMM: return "Mini_UDIMM";
	case BASE_MODULE_TYPE_SO_RDIMM_72B: return "SO_RDIMM_72b";
	case BASE_MODULE_TYPE_SO_UDIMM_72B: return "SO_UDIMM_72b";
	case BASE_MODULE_TYPE_SO_DIMM_16B: return "SO_DIMM_16b";
	case BASE_MODULE_TYPE_SO_DIMM_32B: return "SO_DIMM_32b";
	case BASE_MODULE_TYPE_DIE: return "Die";
	case BASE_MODULE_TYPE_CAMM: return "CAMM";
	default: return NULL;
	}
}

/* Memory.v1_0_0.MemoryMedia */
enum rf_memory_media {
	MEMORY_MEDIA_INVALID = 0,
	MEMORY_MEDIA_DRAM = 1,
	MEMORY_MEDIA_NAND = 2,
	MEMORY_MEDIA_INTEL3_DX_POINT = 3,
	MEMORY_MEDIA_PROPRIETARY = 4,
};

static inline const char *rf_memory_media_to_str(enum rf_memory_media val)
{
	switch (val) {
	case MEMORY_MEDIA_INVALID: return "Invalid";
	case MEMORY_MEDIA_DRAM: return "DRAM";
	case MEMORY_MEDIA_NAND: return "NAND";
	case MEMORY_MEDIA_INTEL3_DX_POINT: return "Intel3DXPoint";
	case MEMORY_MEDIA_PROPRIETARY: return "Proprietary";
	default: return NULL;
	}
}

/* Memory.v1_0_0.SecurityStates */
enum rf_security_states {
	SECURITY_STATES_INVALID = 0,
	SECURITY_STATES_ENABLED = 1,
	SECURITY_STATES_DISABLED = 2,
	SECURITY_STATES_UNLOCKED = 3,
	SECURITY_STATES_LOCKED = 4,
	SECURITY_STATES_FROZEN = 5,
	SECURITY_STATES_PASSPHRASELIMIT = 6,
};

static inline const char *rf_security_states_to_str(enum rf_security_states val)
{
	switch (val) {
	case SECURITY_STATES_INVALID: return "Invalid";
	case SECURITY_STATES_ENABLED: return "Enabled";
	case SECURITY_STATES_DISABLED: return "Disabled";
	case SECURITY_STATES_UNLOCKED: return "Unlocked";
	case SECURITY_STATES_LOCKED: return "Locked";
	case SECURITY_STATES_FROZEN: return "Frozen";
	case SECURITY_STATES_PASSPHRASELIMIT: return "Passphraselimit";
	default: return NULL;
	}
}

/* Memory.v1_0_0.ErrorCorrection */
enum rf_error_correction {
	ERROR_CORRECTION_INVALID = 0,
	ERROR_CORRECTION_NO_ECC = 1,
	ERROR_CORRECTION_SINGLE_BIT_ECC = 2,
	ERROR_CORRECTION_MULTI_BIT_ECC = 3,
	ERROR_CORRECTION_ADDRESS_PARITY = 4,
};

static inline const char *rf_error_correction_to_str(enum rf_error_correction val)
{
	switch (val) {
	case ERROR_CORRECTION_INVALID: return "Invalid";
	case ERROR_CORRECTION_NO_ECC: return "NoECC";
	case ERROR_CORRECTION_SINGLE_BIT_ECC: return "SingleBitECC";
	case ERROR_CORRECTION_MULTI_BIT_ECC: return "MultiBitECC";
	case ERROR_CORRECTION_ADDRESS_PARITY: return "AddressParity";
	default: return NULL;
	}
}

/* Memory.v1_0_0.MemoryClassification */
enum rf_memory_classification {
	MEMORY_CLASSIFICATION_INVALID = 0,
	MEMORY_CLASSIFICATION_VOLATILE = 1,
	MEMORY_CLASSIFICATION_BYTE_ACCESSIBLE_PERSISTENT = 2,
	MEMORY_CLASSIFICATION_BLOCK = 3,
};

static inline const char *rf_memory_classification_to_str(enum rf_memory_classification val)
{
	switch (val) {
	case MEMORY_CLASSIFICATION_INVALID: return "Invalid";
	case MEMORY_CLASSIFICATION_VOLATILE: return "Volatile";
	case MEMORY_CLASSIFICATION_BYTE_ACCESSIBLE_PERSISTENT: return "ByteAccessiblePersistent";
	case MEMORY_CLASSIFICATION_BLOCK: return "Block";
	default: return NULL;
	}
}

/* Memory.v1_0_0.OperatingMemoryModes */
enum rf_operating_memory_modes {
	OPERATING_MEMORY_MODES_INVALID = 0,
	OPERATING_MEMORY_MODES_VOLATILE = 1,
	OPERATING_MEMORY_MODES_PMEM = 2,
	OPERATING_MEMORY_MODES_BLOCK = 3,
};

static inline const char *rf_operating_memory_modes_to_str(enum rf_operating_memory_modes val)
{
	switch (val) {
	case OPERATING_MEMORY_MODES_INVALID: return "Invalid";
	case OPERATING_MEMORY_MODES_VOLATILE: return "Volatile";
	case OPERATING_MEMORY_MODES_PMEM: return "PMEM";
	case OPERATING_MEMORY_MODES_BLOCK: return "Block";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_MEMORY_H */
