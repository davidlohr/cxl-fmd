/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_PROCESSOR_H
#define REDFISH_ENUM_PROCESSOR_H

#include <stddef.h>

/* Processor.v1_0_0.ProcessorType */
enum rf_processor_type {
	PROCESSOR_TYPE_INVALID = 0,
	PROCESSOR_TYPE_CPU = 1,
	PROCESSOR_TYPE_GPU = 2,
	PROCESSOR_TYPE_FPGA = 3,
	PROCESSOR_TYPE_DSP = 4,
	PROCESSOR_TYPE_ACCELERATOR = 5,
	PROCESSOR_TYPE_CORE = 6,
	PROCESSOR_TYPE_THREAD = 7,
	PROCESSOR_TYPE_PARTITION = 8,
	PROCESSOR_TYPE_OEM = 9,
};

static inline const char *rf_processor_type_to_str(enum rf_processor_type val)
{
	switch (val) {
	case PROCESSOR_TYPE_INVALID: return "Invalid";
	case PROCESSOR_TYPE_CPU: return "CPU";
	case PROCESSOR_TYPE_GPU: return "GPU";
	case PROCESSOR_TYPE_FPGA: return "FPGA";
	case PROCESSOR_TYPE_DSP: return "DSP";
	case PROCESSOR_TYPE_ACCELERATOR: return "Accelerator";
	case PROCESSOR_TYPE_CORE: return "Core";
	case PROCESSOR_TYPE_THREAD: return "Thread";
	case PROCESSOR_TYPE_PARTITION: return "Partition";
	case PROCESSOR_TYPE_OEM: return "OEM";
	default: return NULL;
	}
}

/* Processor.v1_0_0.ProcessorArchitecture */
enum rf_processor_architecture {
	PROCESSOR_ARCHITECTURE_INVALID = 0,
	PROCESSOR_ARCHITECTURE_X86 = 1,
	PROCESSOR_ARCHITECTURE_IA64 = 2,
	PROCESSOR_ARCHITECTURE_ARM = 3,
	PROCESSOR_ARCHITECTURE_MIPS = 4,
	PROCESSOR_ARCHITECTURE_POWER = 5,
	PROCESSOR_ARCHITECTURE_RISCV = 6,
	PROCESSOR_ARCHITECTURE_OEM = 7,
};

static inline const char *rf_processor_architecture_to_str(enum rf_processor_architecture val)
{
	switch (val) {
	case PROCESSOR_ARCHITECTURE_INVALID: return "Invalid";
	case PROCESSOR_ARCHITECTURE_X86: return "x86";
	case PROCESSOR_ARCHITECTURE_IA64: return "IA-64";
	case PROCESSOR_ARCHITECTURE_ARM: return "ARM";
	case PROCESSOR_ARCHITECTURE_MIPS: return "MIPS";
	case PROCESSOR_ARCHITECTURE_POWER: return "Power";
	case PROCESSOR_ARCHITECTURE_RISCV: return "RISC-V";
	case PROCESSOR_ARCHITECTURE_OEM: return "OEM";
	default: return NULL;
	}
}

/* Processor.v1_0_0.InstructionSet */
enum rf_instruction_set {
	INSTRUCTION_SET_INVALID = 0,
	INSTRUCTION_SET_X86 = 1,
	INSTRUCTION_SET_X8664 = 2,
	INSTRUCTION_SET_IA64 = 3,
	INSTRUCTION_SET_ARMA32 = 4,
	INSTRUCTION_SET_ARMA64 = 5,
	INSTRUCTION_SET_MIPS32 = 6,
	INSTRUCTION_SET_MIPS64 = 7,
	INSTRUCTION_SET_POWER_ISA = 8,
	INSTRUCTION_SET_RV32 = 9,
	INSTRUCTION_SET_RV64 = 10,
	INSTRUCTION_SET_OEM = 11,
};

static inline const char *rf_instruction_set_to_str(enum rf_instruction_set val)
{
	switch (val) {
	case INSTRUCTION_SET_INVALID: return "Invalid";
	case INSTRUCTION_SET_X86: return "x86";
	case INSTRUCTION_SET_X8664: return "x86-64";
	case INSTRUCTION_SET_IA64: return "IA-64";
	case INSTRUCTION_SET_ARMA32: return "ARM-A32";
	case INSTRUCTION_SET_ARMA64: return "ARM-A64";
	case INSTRUCTION_SET_MIPS32: return "MIPS32";
	case INSTRUCTION_SET_MIPS64: return "MIPS64";
	case INSTRUCTION_SET_POWER_ISA: return "PowerISA";
	case INSTRUCTION_SET_RV32: return "RV32";
	case INSTRUCTION_SET_RV64: return "RV64";
	case INSTRUCTION_SET_OEM: return "OEM";
	default: return NULL;
	}
}

/* Processor.v1_4_0.ProcessorMemoryType */
enum rf_processor_memory_type {
	PROCESSOR_MEMORY_TYPE_INVALID = 0,
	PROCESSOR_MEMORY_TYPE_CACHE = 1,
	PROCESSOR_MEMORY_TYPE_L1_CACHE = 2,
	PROCESSOR_MEMORY_TYPE_L2_CACHE = 3,
	PROCESSOR_MEMORY_TYPE_L3_CACHE = 4,
	PROCESSOR_MEMORY_TYPE_L4_CACHE = 5,
	PROCESSOR_MEMORY_TYPE_L5_CACHE = 6,
	PROCESSOR_MEMORY_TYPE_L6_CACHE = 7,
	PROCESSOR_MEMORY_TYPE_L7_CACHE = 8,
	PROCESSOR_MEMORY_TYPE_HBM1 = 9,
	PROCESSOR_MEMORY_TYPE_HBM2 = 10,
	PROCESSOR_MEMORY_TYPE_HBM2_E = 11,
	PROCESSOR_MEMORY_TYPE_HBM3 = 12,
	PROCESSOR_MEMORY_TYPE_HBM3_E = 13,
	PROCESSOR_MEMORY_TYPE_HBM4 = 14,
	PROCESSOR_MEMORY_TYPE_SGRAM = 15,
	PROCESSOR_MEMORY_TYPE_GDDR = 16,
	PROCESSOR_MEMORY_TYPE_GDDR2 = 17,
	PROCESSOR_MEMORY_TYPE_GDDR3 = 18,
	PROCESSOR_MEMORY_TYPE_GDDR4 = 19,
	PROCESSOR_MEMORY_TYPE_GDDR5 = 20,
	PROCESSOR_MEMORY_TYPE_GDDR5_X = 21,
	PROCESSOR_MEMORY_TYPE_GDDR6 = 22,
	PROCESSOR_MEMORY_TYPE_GDDR7 = 23,
	PROCESSOR_MEMORY_TYPE_DDR = 24,
	PROCESSOR_MEMORY_TYPE_DDR2 = 25,
	PROCESSOR_MEMORY_TYPE_DDR3 = 26,
	PROCESSOR_MEMORY_TYPE_DDR4 = 27,
	PROCESSOR_MEMORY_TYPE_DDR5 = 28,
	PROCESSOR_MEMORY_TYPE_SDRAM = 29,
	PROCESSOR_MEMORY_TYPE_SRAM = 30,
	PROCESSOR_MEMORY_TYPE_FLASH = 31,
	PROCESSOR_MEMORY_TYPE_OEM = 32,
};

static inline const char *rf_processor_memory_type_to_str(enum rf_processor_memory_type val)
{
	switch (val) {
	case PROCESSOR_MEMORY_TYPE_INVALID: return "Invalid";
	case PROCESSOR_MEMORY_TYPE_CACHE: return "Cache";
	case PROCESSOR_MEMORY_TYPE_L1_CACHE: return "L1Cache";
	case PROCESSOR_MEMORY_TYPE_L2_CACHE: return "L2Cache";
	case PROCESSOR_MEMORY_TYPE_L3_CACHE: return "L3Cache";
	case PROCESSOR_MEMORY_TYPE_L4_CACHE: return "L4Cache";
	case PROCESSOR_MEMORY_TYPE_L5_CACHE: return "L5Cache";
	case PROCESSOR_MEMORY_TYPE_L6_CACHE: return "L6Cache";
	case PROCESSOR_MEMORY_TYPE_L7_CACHE: return "L7Cache";
	case PROCESSOR_MEMORY_TYPE_HBM1: return "HBM1";
	case PROCESSOR_MEMORY_TYPE_HBM2: return "HBM2";
	case PROCESSOR_MEMORY_TYPE_HBM2_E: return "HBM2E";
	case PROCESSOR_MEMORY_TYPE_HBM3: return "HBM3";
	case PROCESSOR_MEMORY_TYPE_HBM3_E: return "HBM3E";
	case PROCESSOR_MEMORY_TYPE_HBM4: return "HBM4";
	case PROCESSOR_MEMORY_TYPE_SGRAM: return "SGRAM";
	case PROCESSOR_MEMORY_TYPE_GDDR: return "GDDR";
	case PROCESSOR_MEMORY_TYPE_GDDR2: return "GDDR2";
	case PROCESSOR_MEMORY_TYPE_GDDR3: return "GDDR3";
	case PROCESSOR_MEMORY_TYPE_GDDR4: return "GDDR4";
	case PROCESSOR_MEMORY_TYPE_GDDR5: return "GDDR5";
	case PROCESSOR_MEMORY_TYPE_GDDR5_X: return "GDDR5X";
	case PROCESSOR_MEMORY_TYPE_GDDR6: return "GDDR6";
	case PROCESSOR_MEMORY_TYPE_GDDR7: return "GDDR7";
	case PROCESSOR_MEMORY_TYPE_DDR: return "DDR";
	case PROCESSOR_MEMORY_TYPE_DDR2: return "DDR2";
	case PROCESSOR_MEMORY_TYPE_DDR3: return "DDR3";
	case PROCESSOR_MEMORY_TYPE_DDR4: return "DDR4";
	case PROCESSOR_MEMORY_TYPE_DDR5: return "DDR5";
	case PROCESSOR_MEMORY_TYPE_SDRAM: return "SDRAM";
	case PROCESSOR_MEMORY_TYPE_SRAM: return "SRAM";
	case PROCESSOR_MEMORY_TYPE_FLASH: return "Flash";
	case PROCESSOR_MEMORY_TYPE_OEM: return "OEM";
	default: return NULL;
	}
}

/* Processor.v1_4_0.FpgaType */
enum rf_fpga_type {
	FPGA_TYPE_INVALID = 0,
	FPGA_TYPE_INTEGRATED = 1,
	FPGA_TYPE_DISCRETE = 2,
};

static inline const char *rf_fpga_type_to_str(enum rf_fpga_type val)
{
	switch (val) {
	case FPGA_TYPE_INVALID: return "Invalid";
	case FPGA_TYPE_INTEGRATED: return "Integrated";
	case FPGA_TYPE_DISCRETE: return "Discrete";
	default: return NULL;
	}
}

/* Processor.v1_4_0.SystemInterfaceType */
enum rf_system_interface_type {
	SYSTEM_INTERFACE_TYPE_INVALID = 0,
	SYSTEM_INTERFACE_TYPE_QPI = 1,
	SYSTEM_INTERFACE_TYPE_UPI = 2,
	SYSTEM_INTERFACE_TYPE_PCIE = 3,
	SYSTEM_INTERFACE_TYPE_ETHERNET = 4,
	SYSTEM_INTERFACE_TYPE_AMBA = 5,
	SYSTEM_INTERFACE_TYPE_CCIX = 6,
	SYSTEM_INTERFACE_TYPE_CXL = 7,
	SYSTEM_INTERFACE_TYPE_OEM = 8,
};

static inline const char *rf_system_interface_type_to_str(enum rf_system_interface_type val)
{
	switch (val) {
	case SYSTEM_INTERFACE_TYPE_INVALID: return "Invalid";
	case SYSTEM_INTERFACE_TYPE_QPI: return "QPI";
	case SYSTEM_INTERFACE_TYPE_UPI: return "UPI";
	case SYSTEM_INTERFACE_TYPE_PCIE: return "PCIe";
	case SYSTEM_INTERFACE_TYPE_ETHERNET: return "Ethernet";
	case SYSTEM_INTERFACE_TYPE_AMBA: return "AMBA";
	case SYSTEM_INTERFACE_TYPE_CCIX: return "CCIX";
	case SYSTEM_INTERFACE_TYPE_CXL: return "CXL";
	case SYSTEM_INTERFACE_TYPE_OEM: return "OEM";
	default: return NULL;
	}
}

/* Processor.v1_9_0.TurboState */
enum rf_turbo_state {
	TURBO_STATE_INVALID = 0,
	TURBO_STATE_ENABLED = 1,
	TURBO_STATE_DISABLED = 2,
};

static inline const char *rf_turbo_state_to_str(enum rf_turbo_state val)
{
	switch (val) {
	case TURBO_STATE_INVALID: return "Invalid";
	case TURBO_STATE_ENABLED: return "Enabled";
	case TURBO_STATE_DISABLED: return "Disabled";
	default: return NULL;
	}
}

/* Processor.v1_9_0.BaseSpeedPriorityState */
enum rf_base_speed_priority_state {
	BASE_SPEED_PRIORITY_STATE_INVALID = 0,
	BASE_SPEED_PRIORITY_STATE_ENABLED = 1,
	BASE_SPEED_PRIORITY_STATE_DISABLED = 2,
};

static inline const char *rf_base_speed_priority_state_to_str(enum rf_base_speed_priority_state val)
{
	switch (val) {
	case BASE_SPEED_PRIORITY_STATE_INVALID: return "Invalid";
	case BASE_SPEED_PRIORITY_STATE_ENABLED: return "Enabled";
	case BASE_SPEED_PRIORITY_STATE_DISABLED: return "Disabled";
	default: return NULL;
	}
}

/* Processor.v1_16_0.ThrottleCause */
enum rf_throttle_cause {
	THROTTLE_CAUSE_INVALID = 0,
	THROTTLE_CAUSE_POWER_LIMIT = 1,
	THROTTLE_CAUSE_THERMAL_LIMIT = 2,
	THROTTLE_CAUSE_CLOCK_LIMIT = 3,
	THROTTLE_CAUSE_MANAGEMENT_DETECTED_FAULT = 4,
	THROTTLE_CAUSE_UNKNOWN = 5,
	THROTTLE_CAUSE_OEM = 6,
};

static inline const char *rf_throttle_cause_to_str(enum rf_throttle_cause val)
{
	switch (val) {
	case THROTTLE_CAUSE_INVALID: return "Invalid";
	case THROTTLE_CAUSE_POWER_LIMIT: return "PowerLimit";
	case THROTTLE_CAUSE_THERMAL_LIMIT: return "ThermalLimit";
	case THROTTLE_CAUSE_CLOCK_LIMIT: return "ClockLimit";
	case THROTTLE_CAUSE_MANAGEMENT_DETECTED_FAULT: return "ManagementDetectedFault";
	case THROTTLE_CAUSE_UNKNOWN: return "Unknown";
	case THROTTLE_CAUSE_OEM: return "OEM";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_PROCESSOR_H */
