/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_PCIE_DEVICE_H
#define REDFISH_ENUM_PCIE_DEVICE_H

#include <stddef.h>

/* PCIeDevice.PCIeTypes */
enum rf_pcie_types {
	PCIE_TYPES_INVALID = 0,
	PCIE_TYPES_GEN1 = 1,
	PCIE_TYPES_GEN2 = 2,
	PCIE_TYPES_GEN3 = 3,
	PCIE_TYPES_GEN4 = 4,
	PCIE_TYPES_GEN5 = 5,
	PCIE_TYPES_GEN6 = 6,
};

static inline const char *rf_pcie_types_to_str(enum rf_pcie_types val)
{
	switch (val) {
	case PCIE_TYPES_INVALID: return "Invalid";
	case PCIE_TYPES_GEN1: return "Gen1";
	case PCIE_TYPES_GEN2: return "Gen2";
	case PCIE_TYPES_GEN3: return "Gen3";
	case PCIE_TYPES_GEN4: return "Gen4";
	case PCIE_TYPES_GEN5: return "Gen5";
	case PCIE_TYPES_GEN6: return "Gen6";
	default: return NULL;
	}
}

/* PCIeDevice.CXLDynamicCapacityPolicies */
enum rf_cxl_dynamic_capacity_policies {
	CXL_DYNAMIC_CAPACITY_POLICIES_INVALID = 0,
	CXL_DYNAMIC_CAPACITY_POLICIES_FREE = 1,
	CXL_DYNAMIC_CAPACITY_POLICIES_CONTIGUOUS = 2,
	CXL_DYNAMIC_CAPACITY_POLICIES_PRESCRIPTIVE = 3,
	CXL_DYNAMIC_CAPACITY_POLICIES_TAG_BASED = 4,
	CXL_DYNAMIC_CAPACITY_POLICIES_ENABLE_SHARED_ACCESS = 5,
};

static inline const char *rf_cxl_dynamic_capacity_policies_to_str(enum rf_cxl_dynamic_capacity_policies val)
{
	switch (val) {
	case CXL_DYNAMIC_CAPACITY_POLICIES_INVALID: return "Invalid";
	case CXL_DYNAMIC_CAPACITY_POLICIES_FREE: return "Free";
	case CXL_DYNAMIC_CAPACITY_POLICIES_CONTIGUOUS: return "Contiguous";
	case CXL_DYNAMIC_CAPACITY_POLICIES_PRESCRIPTIVE: return "Prescriptive";
	case CXL_DYNAMIC_CAPACITY_POLICIES_TAG_BASED: return "TagBased";
	case CXL_DYNAMIC_CAPACITY_POLICIES_ENABLE_SHARED_ACCESS: return "EnableSharedAccess";
	default: return NULL;
	}
}

/* PCIeDevice.v1_0_0.DeviceType */
enum rf_device_type {
	DEVICE_TYPE_INVALID = 0,
	DEVICE_TYPE_SINGLE_FUNCTION = 1,
	DEVICE_TYPE_MULTI_FUNCTION = 2,
	DEVICE_TYPE_SIMULATED = 3,
	DEVICE_TYPE_RETIMER = 4,
};

static inline const char *rf_device_type_to_str(enum rf_device_type val)
{
	switch (val) {
	case DEVICE_TYPE_INVALID: return "Invalid";
	case DEVICE_TYPE_SINGLE_FUNCTION: return "SingleFunction";
	case DEVICE_TYPE_MULTI_FUNCTION: return "MultiFunction";
	case DEVICE_TYPE_SIMULATED: return "Simulated";
	case DEVICE_TYPE_RETIMER: return "Retimer";
	default: return NULL;
	}
}

/* PCIeDevice.v1_9_0.SlotType */
enum rf_slot_type {
	SLOT_TYPE_INVALID = 0,
	SLOT_TYPE_FULL_LENGTH = 1,
	SLOT_TYPE_HALF_LENGTH = 2,
	SLOT_TYPE_LOW_PROFILE = 3,
	SLOT_TYPE_MINI = 4,
	SLOT_TYPE_M2 = 5,
	SLOT_TYPE_OEM = 6,
	SLOT_TYPE_OCP3_SMALL = 7,
	SLOT_TYPE_OCP3_LARGE = 8,
	SLOT_TYPE_U2 = 9,
	SLOT_TYPE_EDSFF = 10,
};

static inline const char *rf_slot_type_to_str(enum rf_slot_type val)
{
	switch (val) {
	case SLOT_TYPE_INVALID: return "Invalid";
	case SLOT_TYPE_FULL_LENGTH: return "FullLength";
	case SLOT_TYPE_HALF_LENGTH: return "HalfLength";
	case SLOT_TYPE_LOW_PROFILE: return "LowProfile";
	case SLOT_TYPE_MINI: return "Mini";
	case SLOT_TYPE_M2: return "M2";
	case SLOT_TYPE_OEM: return "OEM";
	case SLOT_TYPE_OCP3_SMALL: return "OCP3Small";
	case SLOT_TYPE_OCP3_LARGE: return "OCP3Large";
	case SLOT_TYPE_U2: return "U2";
	case SLOT_TYPE_EDSFF: return "EDSFF";
	default: return NULL;
	}
}

/* PCIeDevice.v1_9_0.LaneSplittingType */
enum rf_lane_splitting_type {
	LANE_SPLITTING_TYPE_INVALID = 0,
	LANE_SPLITTING_TYPE_NONE = 1,
	LANE_SPLITTING_TYPE_BRIDGED = 2,
	LANE_SPLITTING_TYPE_BIFURCATED = 3,
};

static inline const char *rf_lane_splitting_type_to_str(enum rf_lane_splitting_type val)
{
	switch (val) {
	case LANE_SPLITTING_TYPE_INVALID: return "Invalid";
	case LANE_SPLITTING_TYPE_NONE: return "None";
	case LANE_SPLITTING_TYPE_BRIDGED: return "Bridged";
	case LANE_SPLITTING_TYPE_BIFURCATED: return "Bifurcated";
	default: return NULL;
	}
}

/* PCIeDevice.v1_11_0.CXLDeviceType */
enum rf_cxl_device_type {
	CXL_DEVICE_TYPE_INVALID = 0,
	CXL_DEVICE_TYPE_TYPE1 = 1,
	CXL_DEVICE_TYPE_TYPE2 = 2,
	CXL_DEVICE_TYPE_TYPE3 = 3,
};

static inline const char *rf_cxl_device_type_to_str(enum rf_cxl_device_type val)
{
	switch (val) {
	case CXL_DEVICE_TYPE_INVALID: return "Invalid";
	case CXL_DEVICE_TYPE_TYPE1: return "Type1";
	case CXL_DEVICE_TYPE_TYPE2: return "Type2";
	case CXL_DEVICE_TYPE_TYPE3: return "Type3";
	default: return NULL;
	}
}

/* PCIeDevice.v1_18_0.CXLProtocolVersion */
enum rf_cxl_protocol_version {
	CXL_PROTOCOL_VERSION_INVALID = 0,
	CXL_PROTOCOL_VERSION_CXL1_1 = 1,
	CXL_PROTOCOL_VERSION_CXL2_0 = 2,
	CXL_PROTOCOL_VERSION_CXL3_0 = 3,
	CXL_PROTOCOL_VERSION_CXL3_1 = 4,
	CXL_PROTOCOL_VERSION_CXL3_2 = 5,
};

static inline const char *rf_cxl_protocol_version_to_str(enum rf_cxl_protocol_version val)
{
	switch (val) {
	case CXL_PROTOCOL_VERSION_INVALID: return "Invalid";
	case CXL_PROTOCOL_VERSION_CXL1_1: return "CXL1_1";
	case CXL_PROTOCOL_VERSION_CXL2_0: return "CXL2_0";
	case CXL_PROTOCOL_VERSION_CXL3_0: return "CXL3_0";
	case CXL_PROTOCOL_VERSION_CXL3_1: return "CXL3_1";
	case CXL_PROTOCOL_VERSION_CXL3_2: return "CXL3_2";
	default: return NULL;
	}
}

/* PCIeDevice.v1_23_0.CXLDeviceClass */
enum rf_cxl_device_class {
	CXL_DEVICE_CLASS_INVALID = 0,
	CXL_DEVICE_CLASS_SLD = 1,
	CXL_DEVICE_CLASS_MLD = 2,
	CXL_DEVICE_CLASS_MH_SLD = 3,
	CXL_DEVICE_CLASS_MH_MLD = 4,
};

static inline const char *rf_cxl_device_class_to_str(enum rf_cxl_device_class val)
{
	switch (val) {
	case CXL_DEVICE_CLASS_INVALID: return "Invalid";
	case CXL_DEVICE_CLASS_SLD: return "SLD";
	case CXL_DEVICE_CLASS_MLD: return "MLD";
	case CXL_DEVICE_CLASS_MH_SLD: return "MH_SLD";
	case CXL_DEVICE_CLASS_MH_MLD: return "MH_MLD";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_PCIE_DEVICE_H */
