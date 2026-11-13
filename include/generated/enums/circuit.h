/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_CIRCUIT_H
#define REDFISH_ENUM_CIRCUIT_H

#include <stddef.h>

/* Circuit.PowerState */
enum rf_power_state {
	POWER_STATE_INVALID = 0,
	POWER_STATE_ON = 1,
	POWER_STATE_OFF = 2,
	POWER_STATE_POWER_CYCLE = 3,
};

static inline const char *rf_power_state_to_str(enum rf_power_state val)
{
	switch (val) {
	case POWER_STATE_INVALID: return "Invalid";
	case POWER_STATE_ON: return "On";
	case POWER_STATE_OFF: return "Off";
	case POWER_STATE_POWER_CYCLE: return "PowerCycle";
	default: return NULL;
	}
}

/* Circuit.BreakerStates */
enum rf_breaker_states {
	BREAKER_STATES_INVALID = 0,
	BREAKER_STATES_NORMAL = 1,
	BREAKER_STATES_TRIPPED = 2,
	BREAKER_STATES_OFF = 3,
};

static inline const char *rf_breaker_states_to_str(enum rf_breaker_states val)
{
	switch (val) {
	case BREAKER_STATES_INVALID: return "Invalid";
	case BREAKER_STATES_NORMAL: return "Normal";
	case BREAKER_STATES_TRIPPED: return "Tripped";
	case BREAKER_STATES_OFF: return "Off";
	default: return NULL;
	}
}

/* Circuit.PowerRestorePolicyTypes */
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

/* Circuit.PhaseWiringType */
enum rf_phase_wiring_type {
	PHASE_WIRING_TYPE_INVALID = 0,
	PHASE_WIRING_TYPE_ONE_PHASE3_WIRE = 1,
	PHASE_WIRING_TYPE_TWO_PHASE3_WIRE = 2,
	PHASE_WIRING_TYPE_ONE_OR_TWO_PHASE3_WIRE = 3,
	PHASE_WIRING_TYPE_TWO_PHASE4_WIRE = 4,
	PHASE_WIRING_TYPE_THREE_PHASE4_WIRE = 5,
	PHASE_WIRING_TYPE_THREE_PHASE5_WIRE = 6,
};

static inline const char *rf_phase_wiring_type_to_str(enum rf_phase_wiring_type val)
{
	switch (val) {
	case PHASE_WIRING_TYPE_INVALID: return "Invalid";
	case PHASE_WIRING_TYPE_ONE_PHASE3_WIRE: return "OnePhase3Wire";
	case PHASE_WIRING_TYPE_TWO_PHASE3_WIRE: return "TwoPhase3Wire";
	case PHASE_WIRING_TYPE_ONE_OR_TWO_PHASE3_WIRE: return "OneOrTwoPhase3Wire";
	case PHASE_WIRING_TYPE_TWO_PHASE4_WIRE: return "TwoPhase4Wire";
	case PHASE_WIRING_TYPE_THREE_PHASE4_WIRE: return "ThreePhase4Wire";
	case PHASE_WIRING_TYPE_THREE_PHASE5_WIRE: return "ThreePhase5Wire";
	default: return NULL;
	}
}

/* Circuit.NominalVoltageType */
enum rf_nominal_voltage_type {
	NOMINAL_VOLTAGE_TYPE_INVALID = 0,
	NOMINAL_VOLTAGE_TYPE_AC100_TO127_V = 1,
	NOMINAL_VOLTAGE_TYPE_AC100_TO240_V = 2,
	NOMINAL_VOLTAGE_TYPE_AC100_TO277_V = 3,
	NOMINAL_VOLTAGE_TYPE_AC120_V = 4,
	NOMINAL_VOLTAGE_TYPE_AC200_TO240_V = 5,
	NOMINAL_VOLTAGE_TYPE_AC200_TO277_V = 6,
	NOMINAL_VOLTAGE_TYPE_AC208_V = 7,
	NOMINAL_VOLTAGE_TYPE_AC230_V = 8,
	NOMINAL_VOLTAGE_TYPE_AC240_V = 9,
	NOMINAL_VOLTAGE_TYPE_AC240_AND_DC380_V = 10,
	NOMINAL_VOLTAGE_TYPE_AC277_V = 11,
	NOMINAL_VOLTAGE_TYPE_AC277_AND_DC380_V = 12,
	NOMINAL_VOLTAGE_TYPE_AC400_V = 13,
	NOMINAL_VOLTAGE_TYPE_AC480_V = 14,
	NOMINAL_VOLTAGE_TYPE_DC48_V = 15,
	NOMINAL_VOLTAGE_TYPE_OPEN_RACK_V3 = 16,
	NOMINAL_VOLTAGE_TYPE_DC240_V = 17,
	NOMINAL_VOLTAGE_TYPE_DC380_V = 18,
	NOMINAL_VOLTAGE_TYPE_DC400_V = 19,
	NOMINAL_VOLTAGE_TYPE_DC800_V = 20,
	NOMINAL_VOLTAGE_TYPE_DC_NEG48_V = 21,
	NOMINAL_VOLTAGE_TYPE_DC16_V = 22,
	NOMINAL_VOLTAGE_TYPE_DC12_V = 23,
	NOMINAL_VOLTAGE_TYPE_DC9_V = 24,
	NOMINAL_VOLTAGE_TYPE_DC5_V = 25,
	NOMINAL_VOLTAGE_TYPE_DC3_3_V = 26,
	NOMINAL_VOLTAGE_TYPE_DC1_8_V = 27,
};

static inline const char *rf_nominal_voltage_type_to_str(enum rf_nominal_voltage_type val)
{
	switch (val) {
	case NOMINAL_VOLTAGE_TYPE_INVALID: return "Invalid";
	case NOMINAL_VOLTAGE_TYPE_AC100_TO127_V: return "AC100To127V";
	case NOMINAL_VOLTAGE_TYPE_AC100_TO240_V: return "AC100To240V";
	case NOMINAL_VOLTAGE_TYPE_AC100_TO277_V: return "AC100To277V";
	case NOMINAL_VOLTAGE_TYPE_AC120_V: return "AC120V";
	case NOMINAL_VOLTAGE_TYPE_AC200_TO240_V: return "AC200To240V";
	case NOMINAL_VOLTAGE_TYPE_AC200_TO277_V: return "AC200To277V";
	case NOMINAL_VOLTAGE_TYPE_AC208_V: return "AC208V";
	case NOMINAL_VOLTAGE_TYPE_AC230_V: return "AC230V";
	case NOMINAL_VOLTAGE_TYPE_AC240_V: return "AC240V";
	case NOMINAL_VOLTAGE_TYPE_AC240_AND_DC380_V: return "AC240AndDC380V";
	case NOMINAL_VOLTAGE_TYPE_AC277_V: return "AC277V";
	case NOMINAL_VOLTAGE_TYPE_AC277_AND_DC380_V: return "AC277AndDC380V";
	case NOMINAL_VOLTAGE_TYPE_AC400_V: return "AC400V";
	case NOMINAL_VOLTAGE_TYPE_AC480_V: return "AC480V";
	case NOMINAL_VOLTAGE_TYPE_DC48_V: return "DC48V";
	case NOMINAL_VOLTAGE_TYPE_OPEN_RACK_V3: return "OpenRackV3";
	case NOMINAL_VOLTAGE_TYPE_DC240_V: return "DC240V";
	case NOMINAL_VOLTAGE_TYPE_DC380_V: return "DC380V";
	case NOMINAL_VOLTAGE_TYPE_DC400_V: return "DC400V";
	case NOMINAL_VOLTAGE_TYPE_DC800_V: return "DC800V";
	case NOMINAL_VOLTAGE_TYPE_DC_NEG48_V: return "DCNeg48V";
	case NOMINAL_VOLTAGE_TYPE_DC16_V: return "DC16V";
	case NOMINAL_VOLTAGE_TYPE_DC12_V: return "DC12V";
	case NOMINAL_VOLTAGE_TYPE_DC9_V: return "DC9V";
	case NOMINAL_VOLTAGE_TYPE_DC5_V: return "DC5V";
	case NOMINAL_VOLTAGE_TYPE_DC3_3_V: return "DC3_3V";
	case NOMINAL_VOLTAGE_TYPE_DC1_8_V: return "DC1_8V";
	default: return NULL;
	}
}

/* Circuit.PlugType */
enum rf_plug_type {
	PLUG_TYPE_INVALID = 0,
	PLUG_TYPE_NEMA_5_15_P = 1,
	PLUG_TYPE_NEMA_L5_15_P = 2,
	PLUG_TYPE_NEMA_5_20_P = 3,
	PLUG_TYPE_NEMA_L5_20_P = 4,
	PLUG_TYPE_NEMA_L5_30_P = 5,
	PLUG_TYPE_NEMA_6_15_P = 6,
	PLUG_TYPE_NEMA_L6_15_P = 7,
	PLUG_TYPE_NEMA_6_20_P = 8,
	PLUG_TYPE_NEMA_L6_20_P = 9,
	PLUG_TYPE_NEMA_L6_30_P = 10,
	PLUG_TYPE_NEMA_L14_20_P = 11,
	PLUG_TYPE_NEMA_L14_30_P = 12,
	PLUG_TYPE_NEMA_L15_20_P = 13,
	PLUG_TYPE_NEMA_L15_30_P = 14,
	PLUG_TYPE_NEMA_L21_20_P = 15,
	PLUG_TYPE_NEMA_L21_30_P = 16,
	PLUG_TYPE_NEMA_L22_20_P = 17,
	PLUG_TYPE_NEMA_L22_30_P = 18,
	PLUG_TYPE_CALIFORNIA_CS8265 = 19,
	PLUG_TYPE_CALIFORNIA_CS8365 = 20,
	PLUG_TYPE_IEC_60320_C14 = 21,
	PLUG_TYPE_IEC_60320_C20 = 22,
	PLUG_TYPE_IEC_60309_316_P6 = 23,
	PLUG_TYPE_IEC_60309_332_P6 = 24,
	PLUG_TYPE_IEC_60309_363_P6 = 25,
	PLUG_TYPE_IEC_60309_516_P6 = 26,
	PLUG_TYPE_IEC_60309_532_P6 = 27,
	PLUG_TYPE_IEC_60309_563_P6 = 28,
	PLUG_TYPE_IEC_60309_460_P9 = 29,
	PLUG_TYPE_IEC_60309_560_P9 = 30,
	PLUG_TYPE_FIELD_208_V_3_P4_W_60_A = 31,
	PLUG_TYPE_FIELD_400_V_3_P5_W_32_A = 32,
};

static inline const char *rf_plug_type_to_str(enum rf_plug_type val)
{
	switch (val) {
	case PLUG_TYPE_INVALID: return "Invalid";
	case PLUG_TYPE_NEMA_5_15_P: return "NEMA_5_15P";
	case PLUG_TYPE_NEMA_L5_15_P: return "NEMA_L5_15P";
	case PLUG_TYPE_NEMA_5_20_P: return "NEMA_5_20P";
	case PLUG_TYPE_NEMA_L5_20_P: return "NEMA_L5_20P";
	case PLUG_TYPE_NEMA_L5_30_P: return "NEMA_L5_30P";
	case PLUG_TYPE_NEMA_6_15_P: return "NEMA_6_15P";
	case PLUG_TYPE_NEMA_L6_15_P: return "NEMA_L6_15P";
	case PLUG_TYPE_NEMA_6_20_P: return "NEMA_6_20P";
	case PLUG_TYPE_NEMA_L6_20_P: return "NEMA_L6_20P";
	case PLUG_TYPE_NEMA_L6_30_P: return "NEMA_L6_30P";
	case PLUG_TYPE_NEMA_L14_20_P: return "NEMA_L14_20P";
	case PLUG_TYPE_NEMA_L14_30_P: return "NEMA_L14_30P";
	case PLUG_TYPE_NEMA_L15_20_P: return "NEMA_L15_20P";
	case PLUG_TYPE_NEMA_L15_30_P: return "NEMA_L15_30P";
	case PLUG_TYPE_NEMA_L21_20_P: return "NEMA_L21_20P";
	case PLUG_TYPE_NEMA_L21_30_P: return "NEMA_L21_30P";
	case PLUG_TYPE_NEMA_L22_20_P: return "NEMA_L22_20P";
	case PLUG_TYPE_NEMA_L22_30_P: return "NEMA_L22_30P";
	case PLUG_TYPE_CALIFORNIA_CS8265: return "California_CS8265";
	case PLUG_TYPE_CALIFORNIA_CS8365: return "California_CS8365";
	case PLUG_TYPE_IEC_60320_C14: return "IEC_60320_C14";
	case PLUG_TYPE_IEC_60320_C20: return "IEC_60320_C20";
	case PLUG_TYPE_IEC_60309_316_P6: return "IEC_60309_316P6";
	case PLUG_TYPE_IEC_60309_332_P6: return "IEC_60309_332P6";
	case PLUG_TYPE_IEC_60309_363_P6: return "IEC_60309_363P6";
	case PLUG_TYPE_IEC_60309_516_P6: return "IEC_60309_516P6";
	case PLUG_TYPE_IEC_60309_532_P6: return "IEC_60309_532P6";
	case PLUG_TYPE_IEC_60309_563_P6: return "IEC_60309_563P6";
	case PLUG_TYPE_IEC_60309_460_P9: return "IEC_60309_460P9";
	case PLUG_TYPE_IEC_60309_560_P9: return "IEC_60309_560P9";
	case PLUG_TYPE_FIELD_208_V_3_P4_W_60_A: return "Field_208V_3P4W_60A";
	case PLUG_TYPE_FIELD_400_V_3_P5_W_32_A: return "Field_400V_3P5W_32A";
	default: return NULL;
	}
}

/* Circuit.v1_0_0.CircuitType */
enum rf_circuit_type {
	CIRCUIT_TYPE_INVALID = 0,
	CIRCUIT_TYPE_MAINS = 1,
	CIRCUIT_TYPE_BRANCH = 2,
	CIRCUIT_TYPE_SUBFEED = 3,
	CIRCUIT_TYPE_FEEDER = 4,
	CIRCUIT_TYPE_BUS = 5,
};

static inline const char *rf_circuit_type_to_str(enum rf_circuit_type val)
{
	switch (val) {
	case CIRCUIT_TYPE_INVALID: return "Invalid";
	case CIRCUIT_TYPE_MAINS: return "Mains";
	case CIRCUIT_TYPE_BRANCH: return "Branch";
	case CIRCUIT_TYPE_SUBFEED: return "Subfeed";
	case CIRCUIT_TYPE_FEEDER: return "Feeder";
	case CIRCUIT_TYPE_BUS: return "Bus";
	default: return NULL;
	}
}

/* Circuit.v1_0_0.VoltageType */
enum rf_voltage_type {
	VOLTAGE_TYPE_INVALID = 0,
	VOLTAGE_TYPE_AC = 1,
	VOLTAGE_TYPE_DC = 2,
};

static inline const char *rf_voltage_type_to_str(enum rf_voltage_type val)
{
	switch (val) {
	case VOLTAGE_TYPE_INVALID: return "Invalid";
	case VOLTAGE_TYPE_AC: return "AC";
	case VOLTAGE_TYPE_DC: return "DC";
	default: return NULL;
	}
}

/* Circuit.v1_10_0.PowerFlowDirection */
enum rf_power_flow_direction {
	POWER_FLOW_DIRECTION_INVALID = 0,
	POWER_FLOW_DIRECTION_SOURCE = 1,
	POWER_FLOW_DIRECTION_LOAD = 2,
	POWER_FLOW_DIRECTION_BIDIRECTIONAL = 3,
};

static inline const char *rf_power_flow_direction_to_str(enum rf_power_flow_direction val)
{
	switch (val) {
	case POWER_FLOW_DIRECTION_INVALID: return "Invalid";
	case POWER_FLOW_DIRECTION_SOURCE: return "Source";
	case POWER_FLOW_DIRECTION_LOAD: return "Load";
	case POWER_FLOW_DIRECTION_BIDIRECTIONAL: return "Bidirectional";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_CIRCUIT_H */
