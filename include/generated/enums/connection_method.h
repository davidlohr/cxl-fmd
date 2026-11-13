/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_CONNECTION_METHOD_H
#define REDFISH_ENUM_CONNECTION_METHOD_H

#include <stddef.h>

/* ConnectionMethod.v1_0_0.ConnectionMethodType */
enum rf_connection_method_type {
	CONNECTION_METHOD_TYPE_INVALID = 0,
	CONNECTION_METHOD_TYPE_REDFISH = 1,
	CONNECTION_METHOD_TYPE_SNMP = 2,
	CONNECTION_METHOD_TYPE_SNM_PV1 = 3,
	CONNECTION_METHOD_TYPE_SNM_PV2C = 4,
	CONNECTION_METHOD_TYPE_SNM_PV3 = 5,
	CONNECTION_METHOD_TYPE_IPMI15 = 6,
	CONNECTION_METHOD_TYPE_IPMI20 = 7,
	CONNECTION_METHOD_TYPE_NETCONF = 8,
	CONNECTION_METHOD_TYPE_OEM = 9,
	CONNECTION_METHOD_TYPE_MODBUS_SERIAL = 10,
	CONNECTION_METHOD_TYPE_MODBUS_TCP = 11,
};

static inline const char *rf_connection_method_type_to_str(enum rf_connection_method_type val)
{
	switch (val) {
	case CONNECTION_METHOD_TYPE_INVALID: return "Invalid";
	case CONNECTION_METHOD_TYPE_REDFISH: return "Redfish";
	case CONNECTION_METHOD_TYPE_SNMP: return "SNMP";
	case CONNECTION_METHOD_TYPE_SNM_PV1: return "SNMPv1";
	case CONNECTION_METHOD_TYPE_SNM_PV2C: return "SNMPv2c";
	case CONNECTION_METHOD_TYPE_SNM_PV3: return "SNMPv3";
	case CONNECTION_METHOD_TYPE_IPMI15: return "IPMI15";
	case CONNECTION_METHOD_TYPE_IPMI20: return "IPMI20";
	case CONNECTION_METHOD_TYPE_NETCONF: return "NETCONF";
	case CONNECTION_METHOD_TYPE_OEM: return "OEM";
	case CONNECTION_METHOD_TYPE_MODBUS_SERIAL: return "ModbusSerial";
	case CONNECTION_METHOD_TYPE_MODBUS_TCP: return "ModbusTCP";
	default: return NULL;
	}
}

/* ConnectionMethod.v1_1_0.TunnelingProtocolType */
enum rf_tunneling_protocol_type {
	TUNNELING_PROTOCOL_TYPE_INVALID = 0,
	TUNNELING_PROTOCOL_TYPE_SSH = 1,
	TUNNELING_PROTOCOL_TYPE_OEM = 2,
};

static inline const char *rf_tunneling_protocol_type_to_str(enum rf_tunneling_protocol_type val)
{
	switch (val) {
	case TUNNELING_PROTOCOL_TYPE_INVALID: return "Invalid";
	case TUNNELING_PROTOCOL_TYPE_SSH: return "SSH";
	case TUNNELING_PROTOCOL_TYPE_OEM: return "OEM";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_CONNECTION_METHOD_H */
