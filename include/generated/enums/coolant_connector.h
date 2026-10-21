/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_COOLANT_CONNECTOR_H
#define REDFISH_ENUM_COOLANT_CONNECTOR_H

#include <stddef.h>

/* CoolantConnector.v1_0_0.CoolantConnectorType */
enum rf_coolant_connector_type {
	COOLANT_CONNECTOR_TYPE_INVALID = 0,
	COOLANT_CONNECTOR_TYPE_PAIR = 1,
	COOLANT_CONNECTOR_TYPE_SUPPLY = 2,
	COOLANT_CONNECTOR_TYPE_RETURN = 3,
	COOLANT_CONNECTOR_TYPE_INLINE = 4,
	COOLANT_CONNECTOR_TYPE_CLOSED = 5,
	COOLANT_CONNECTOR_TYPE_PAIR_GROUP_MEMBER = 6,
	COOLANT_CONNECTOR_TYPE_SUPPLY_GROUP_MEMBER = 7,
	COOLANT_CONNECTOR_TYPE_RETURN_GROUP_MEMBER = 8,
};

static inline const char *rf_coolant_connector_type_to_str(enum rf_coolant_connector_type val)
{
	switch (val) {
	case COOLANT_CONNECTOR_TYPE_INVALID: return "Invalid";
	case COOLANT_CONNECTOR_TYPE_PAIR: return "Pair";
	case COOLANT_CONNECTOR_TYPE_SUPPLY: return "Supply";
	case COOLANT_CONNECTOR_TYPE_RETURN: return "Return";
	case COOLANT_CONNECTOR_TYPE_INLINE: return "Inline";
	case COOLANT_CONNECTOR_TYPE_CLOSED: return "Closed";
	case COOLANT_CONNECTOR_TYPE_PAIR_GROUP_MEMBER: return "PairGroupMember";
	case COOLANT_CONNECTOR_TYPE_SUPPLY_GROUP_MEMBER: return "SupplyGroupMember";
	case COOLANT_CONNECTOR_TYPE_RETURN_GROUP_MEMBER: return "ReturnGroupMember";
	default: return NULL;
	}
}

/* CoolantConnector.v1_3_0.ValveState */
enum rf_valve_state {
	VALVE_STATE_INVALID = 0,
	VALVE_STATE_OPEN = 1,
	VALVE_STATE_CLOSED = 2,
};

static inline const char *rf_valve_state_to_str(enum rf_valve_state val)
{
	switch (val) {
	case VALVE_STATE_INVALID: return "Invalid";
	case VALVE_STATE_OPEN: return "Open";
	case VALVE_STATE_CLOSED: return "Closed";
	default: return NULL;
	}
}

/* CoolantConnector.v1_3_0.ValveStateReason */
enum rf_valve_state_reason {
	VALVE_STATE_REASON_INVALID = 0,
	VALVE_STATE_REASON_NORMAL = 1,
	VALVE_STATE_REASON_NOT_IN_USE = 2,
	VALVE_STATE_REASON_LEAK_DETECTED = 3,
};

static inline const char *rf_valve_state_reason_to_str(enum rf_valve_state_reason val)
{
	switch (val) {
	case VALVE_STATE_REASON_INVALID: return "Invalid";
	case VALVE_STATE_REASON_NORMAL: return "Normal";
	case VALVE_STATE_REASON_NOT_IN_USE: return "NotInUse";
	case VALVE_STATE_REASON_LEAK_DETECTED: return "LeakDetected";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_COOLANT_CONNECTOR_H */
