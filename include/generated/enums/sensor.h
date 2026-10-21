/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_SENSOR_H
#define REDFISH_ENUM_SENSOR_H

#include <stddef.h>

/* Sensor.VoltageType */
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

/* Sensor.ElectricalContext */
enum rf_electrical_context {
	ELECTRICAL_CONTEXT_INVALID = 0,
	ELECTRICAL_CONTEXT_LINE1 = 1,
	ELECTRICAL_CONTEXT_LINE2 = 2,
	ELECTRICAL_CONTEXT_LINE3 = 3,
	ELECTRICAL_CONTEXT_NEUTRAL = 4,
	ELECTRICAL_CONTEXT_LINE_TO_LINE = 5,
	ELECTRICAL_CONTEXT_LINE1_TO_LINE2 = 6,
	ELECTRICAL_CONTEXT_LINE2_TO_LINE3 = 7,
	ELECTRICAL_CONTEXT_LINE3_TO_LINE1 = 8,
	ELECTRICAL_CONTEXT_LINE_TO_NEUTRAL = 9,
	ELECTRICAL_CONTEXT_LINE1_TO_NEUTRAL = 10,
	ELECTRICAL_CONTEXT_LINE2_TO_NEUTRAL = 11,
	ELECTRICAL_CONTEXT_LINE3_TO_NEUTRAL = 12,
	ELECTRICAL_CONTEXT_LINE1_TO_NEUTRAL_AND_L1_L2 = 13,
	ELECTRICAL_CONTEXT_LINE2_TO_NEUTRAL_AND_L1_L2 = 14,
	ELECTRICAL_CONTEXT_LINE2_TO_NEUTRAL_AND_L2_L3 = 15,
	ELECTRICAL_CONTEXT_LINE3_TO_NEUTRAL_AND_L3_L1 = 16,
	ELECTRICAL_CONTEXT_TOTAL = 17,
};

static inline const char *rf_electrical_context_to_str(enum rf_electrical_context val)
{
	switch (val) {
	case ELECTRICAL_CONTEXT_INVALID: return "Invalid";
	case ELECTRICAL_CONTEXT_LINE1: return "Line1";
	case ELECTRICAL_CONTEXT_LINE2: return "Line2";
	case ELECTRICAL_CONTEXT_LINE3: return "Line3";
	case ELECTRICAL_CONTEXT_NEUTRAL: return "Neutral";
	case ELECTRICAL_CONTEXT_LINE_TO_LINE: return "LineToLine";
	case ELECTRICAL_CONTEXT_LINE1_TO_LINE2: return "Line1ToLine2";
	case ELECTRICAL_CONTEXT_LINE2_TO_LINE3: return "Line2ToLine3";
	case ELECTRICAL_CONTEXT_LINE3_TO_LINE1: return "Line3ToLine1";
	case ELECTRICAL_CONTEXT_LINE_TO_NEUTRAL: return "LineToNeutral";
	case ELECTRICAL_CONTEXT_LINE1_TO_NEUTRAL: return "Line1ToNeutral";
	case ELECTRICAL_CONTEXT_LINE2_TO_NEUTRAL: return "Line2ToNeutral";
	case ELECTRICAL_CONTEXT_LINE3_TO_NEUTRAL: return "Line3ToNeutral";
	case ELECTRICAL_CONTEXT_LINE1_TO_NEUTRAL_AND_L1_L2: return "Line1ToNeutralAndL1L2";
	case ELECTRICAL_CONTEXT_LINE2_TO_NEUTRAL_AND_L1_L2: return "Line2ToNeutralAndL1L2";
	case ELECTRICAL_CONTEXT_LINE2_TO_NEUTRAL_AND_L2_L3: return "Line2ToNeutralAndL2L3";
	case ELECTRICAL_CONTEXT_LINE3_TO_NEUTRAL_AND_L3_L1: return "Line3ToNeutralAndL3L1";
	case ELECTRICAL_CONTEXT_TOTAL: return "Total";
	default: return NULL;
	}
}

/* Sensor.v1_0_0.ThresholdActivation */
enum rf_threshold_activation {
	THRESHOLD_ACTIVATION_INVALID = 0,
	THRESHOLD_ACTIVATION_INCREASING = 1,
	THRESHOLD_ACTIVATION_DECREASING = 2,
	THRESHOLD_ACTIVATION_EITHER = 3,
	THRESHOLD_ACTIVATION_DISABLED = 4,
};

static inline const char *rf_threshold_activation_to_str(enum rf_threshold_activation val)
{
	switch (val) {
	case THRESHOLD_ACTIVATION_INVALID: return "Invalid";
	case THRESHOLD_ACTIVATION_INCREASING: return "Increasing";
	case THRESHOLD_ACTIVATION_DECREASING: return "Decreasing";
	case THRESHOLD_ACTIVATION_EITHER: return "Either";
	case THRESHOLD_ACTIVATION_DISABLED: return "Disabled";
	default: return NULL;
	}
}

/* Sensor.v1_0_0.ReadingType */
enum rf_reading_type {
	READING_TYPE_INVALID = 0,
	READING_TYPE_TEMPERATURE = 1,
	READING_TYPE_HUMIDITY = 2,
	READING_TYPE_POWER = 3,
	READING_TYPE_ENERGYK_WH = 4,
	READING_TYPE_ENERGY_JOULES = 5,
	READING_TYPE_ENERGY_WH = 6,
	READING_TYPE_CHARGE_AH = 7,
	READING_TYPE_VOLTAGE = 8,
	READING_TYPE_CURRENT = 9,
	READING_TYPE_FREQUENCY = 10,
	READING_TYPE_PRESSURE = 11,
	READING_TYPE_PRESSUREK_PA = 12,
	READING_TYPE_PRESSURE_PA = 13,
	READING_TYPE_LIQUID_LEVEL = 14,
	READING_TYPE_ROTATIONAL = 15,
	READING_TYPE_AIR_FLOW = 16,
	READING_TYPE_AIR_FLOW_CMM = 17,
	READING_TYPE_LIQUID_FLOW = 18,
	READING_TYPE_LIQUID_FLOW_LPM = 19,
	READING_TYPE_BAROMETRIC = 20,
	READING_TYPE_ALTITUDE = 21,
	READING_TYPE_PERCENT = 22,
	READING_TYPE_ABSOLUTE_HUMIDITY = 23,
	READING_TYPE_HEAT = 24,
	READING_TYPE_LINEAR_POSITION = 25,
	READING_TYPE_LINEAR_VELOCITY = 26,
	READING_TYPE_LINEAR_ACCELERATION = 27,
	READING_TYPE_ROTATIONAL_POSITION = 28,
	READING_TYPE_ROTATIONAL_VELOCITY = 29,
	READING_TYPE_ROTATIONAL_ACCELERATION = 30,
	READING_TYPE_VALVE = 31,
	READING_TYPE_TURBIDITY = 32,
	READING_TYPE_LIQUID_CONDUCTIVITY = 33,
	READING_TYPE_P_H = 34,
};

static inline const char *rf_reading_type_to_str(enum rf_reading_type val)
{
	switch (val) {
	case READING_TYPE_INVALID: return "Invalid";
	case READING_TYPE_TEMPERATURE: return "Temperature";
	case READING_TYPE_HUMIDITY: return "Humidity";
	case READING_TYPE_POWER: return "Power";
	case READING_TYPE_ENERGYK_WH: return "EnergykWh";
	case READING_TYPE_ENERGY_JOULES: return "EnergyJoules";
	case READING_TYPE_ENERGY_WH: return "EnergyWh";
	case READING_TYPE_CHARGE_AH: return "ChargeAh";
	case READING_TYPE_VOLTAGE: return "Voltage";
	case READING_TYPE_CURRENT: return "Current";
	case READING_TYPE_FREQUENCY: return "Frequency";
	case READING_TYPE_PRESSURE: return "Pressure";
	case READING_TYPE_PRESSUREK_PA: return "PressurekPa";
	case READING_TYPE_PRESSURE_PA: return "PressurePa";
	case READING_TYPE_LIQUID_LEVEL: return "LiquidLevel";
	case READING_TYPE_ROTATIONAL: return "Rotational";
	case READING_TYPE_AIR_FLOW: return "AirFlow";
	case READING_TYPE_AIR_FLOW_CMM: return "AirFlowCMM";
	case READING_TYPE_LIQUID_FLOW: return "LiquidFlow";
	case READING_TYPE_LIQUID_FLOW_LPM: return "LiquidFlowLPM";
	case READING_TYPE_BAROMETRIC: return "Barometric";
	case READING_TYPE_ALTITUDE: return "Altitude";
	case READING_TYPE_PERCENT: return "Percent";
	case READING_TYPE_ABSOLUTE_HUMIDITY: return "AbsoluteHumidity";
	case READING_TYPE_HEAT: return "Heat";
	case READING_TYPE_LINEAR_POSITION: return "LinearPosition";
	case READING_TYPE_LINEAR_VELOCITY: return "LinearVelocity";
	case READING_TYPE_LINEAR_ACCELERATION: return "LinearAcceleration";
	case READING_TYPE_ROTATIONAL_POSITION: return "RotationalPosition";
	case READING_TYPE_ROTATIONAL_VELOCITY: return "RotationalVelocity";
	case READING_TYPE_ROTATIONAL_ACCELERATION: return "RotationalAcceleration";
	case READING_TYPE_VALVE: return "Valve";
	case READING_TYPE_TURBIDITY: return "Turbidity";
	case READING_TYPE_LIQUID_CONDUCTIVITY: return "LiquidConductivity";
	case READING_TYPE_P_H: return "pH";
	default: return NULL;
	}
}

/* Sensor.v1_1_0.ImplementationType */
enum rf_implementation_type {
	IMPLEMENTATION_TYPE_INVALID = 0,
	IMPLEMENTATION_TYPE_PHYSICAL_SENSOR = 1,
	IMPLEMENTATION_TYPE_SYNTHESIZED = 2,
	IMPLEMENTATION_TYPE_REPORTED = 3,
};

static inline const char *rf_implementation_type_to_str(enum rf_implementation_type val)
{
	switch (val) {
	case IMPLEMENTATION_TYPE_INVALID: return "Invalid";
	case IMPLEMENTATION_TYPE_PHYSICAL_SENSOR: return "PhysicalSensor";
	case IMPLEMENTATION_TYPE_SYNTHESIZED: return "Synthesized";
	case IMPLEMENTATION_TYPE_REPORTED: return "Reported";
	default: return NULL;
	}
}

/* Sensor.v1_7_0.ReadingBasisType */
enum rf_reading_basis_type {
	READING_BASIS_TYPE_INVALID = 0,
	READING_BASIS_TYPE_ZERO = 1,
	READING_BASIS_TYPE_DELTA = 2,
	READING_BASIS_TYPE_HEADROOM = 3,
};

static inline const char *rf_reading_basis_type_to_str(enum rf_reading_basis_type val)
{
	switch (val) {
	case READING_BASIS_TYPE_INVALID: return "Invalid";
	case READING_BASIS_TYPE_ZERO: return "Zero";
	case READING_BASIS_TYPE_DELTA: return "Delta";
	case READING_BASIS_TYPE_HEADROOM: return "Headroom";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_SENSOR_H */
