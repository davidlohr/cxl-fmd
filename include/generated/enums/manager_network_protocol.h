/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_MANAGER_NETWORK_PROTOCOL_H
#define REDFISH_ENUM_MANAGER_NETWORK_PROTOCOL_H

#include <stddef.h>

/* ManagerNetworkProtocol.v1_0_0.NotifyIPv6Scope */
enum rf_notify_ipv6_scope {
	NOTIFY_IPV6_SCOPE_INVALID = 0,
	NOTIFY_IPV6_SCOPE_LINK = 1,
	NOTIFY_IPV6_SCOPE_SITE = 2,
	NOTIFY_IPV6_SCOPE_ORGANIZATION = 3,
};

static inline const char *rf_notify_ipv6_scope_to_str(enum rf_notify_ipv6_scope val)
{
	switch (val) {
	case NOTIFY_IPV6_SCOPE_INVALID: return "Invalid";
	case NOTIFY_IPV6_SCOPE_LINK: return "Link";
	case NOTIFY_IPV6_SCOPE_SITE: return "Site";
	case NOTIFY_IPV6_SCOPE_ORGANIZATION: return "Organization";
	default: return NULL;
	}
}

/* ManagerNetworkProtocol.v1_5_0.SNMPCommunityAccessMode */
enum rf_snmp_community_access_mode {
	SNMP_COMMUNITY_ACCESS_MODE_INVALID = 0,
	SNMP_COMMUNITY_ACCESS_MODE_FULL = 1,
	SNMP_COMMUNITY_ACCESS_MODE_LIMITED = 2,
};

static inline const char *rf_snmp_community_access_mode_to_str(enum rf_snmp_community_access_mode val)
{
	switch (val) {
	case SNMP_COMMUNITY_ACCESS_MODE_INVALID: return "Invalid";
	case SNMP_COMMUNITY_ACCESS_MODE_FULL: return "Full";
	case SNMP_COMMUNITY_ACCESS_MODE_LIMITED: return "Limited";
	default: return NULL;
	}
}

/* ManagerNetworkProtocol.v1_5_0.SNMPAuthenticationProtocols */
enum rf_snmp_authentication_protocols {
	SNMP_AUTHENTICATION_PROTOCOLS_INVALID = 0,
	SNMP_AUTHENTICATION_PROTOCOLS_ACCOUNT = 1,
	SNMP_AUTHENTICATION_PROTOCOLS_COMMUNITY_STRING = 2,
	SNMP_AUTHENTICATION_PROTOCOLS_HMAC_MD5 = 3,
	SNMP_AUTHENTICATION_PROTOCOLS_HMAC_SHA96 = 4,
	SNMP_AUTHENTICATION_PROTOCOLS_HMAC128_SHA224 = 5,
	SNMP_AUTHENTICATION_PROTOCOLS_HMAC192_SHA256 = 6,
	SNMP_AUTHENTICATION_PROTOCOLS_HMAC256_SHA384 = 7,
	SNMP_AUTHENTICATION_PROTOCOLS_HMAC384_SHA512 = 8,
	SNMP_AUTHENTICATION_PROTOCOLS_NONE = 9,
};

static inline const char *rf_snmp_authentication_protocols_to_str(enum rf_snmp_authentication_protocols val)
{
	switch (val) {
	case SNMP_AUTHENTICATION_PROTOCOLS_INVALID: return "Invalid";
	case SNMP_AUTHENTICATION_PROTOCOLS_ACCOUNT: return "Account";
	case SNMP_AUTHENTICATION_PROTOCOLS_COMMUNITY_STRING: return "CommunityString";
	case SNMP_AUTHENTICATION_PROTOCOLS_HMAC_MD5: return "HMAC_MD5";
	case SNMP_AUTHENTICATION_PROTOCOLS_HMAC_SHA96: return "HMAC_SHA96";
	case SNMP_AUTHENTICATION_PROTOCOLS_HMAC128_SHA224: return "HMAC128_SHA224";
	case SNMP_AUTHENTICATION_PROTOCOLS_HMAC192_SHA256: return "HMAC192_SHA256";
	case SNMP_AUTHENTICATION_PROTOCOLS_HMAC256_SHA384: return "HMAC256_SHA384";
	case SNMP_AUTHENTICATION_PROTOCOLS_HMAC384_SHA512: return "HMAC384_SHA512";
	case SNMP_AUTHENTICATION_PROTOCOLS_NONE: return "None";
	default: return NULL;
	}
}

/* ManagerNetworkProtocol.v1_5_0.SNMPEncryptionProtocols */
enum rf_snmp_encryption_protocols {
	SNMP_ENCRYPTION_PROTOCOLS_INVALID = 0,
	SNMP_ENCRYPTION_PROTOCOLS_NONE = 1,
	SNMP_ENCRYPTION_PROTOCOLS_ACCOUNT = 2,
	SNMP_ENCRYPTION_PROTOCOLS_CBC_DES = 3,
	SNMP_ENCRYPTION_PROTOCOLS_CFB128_AES128 = 4,
	SNMP_ENCRYPTION_PROTOCOLS_CFB128_AES192 = 5,
	SNMP_ENCRYPTION_PROTOCOLS_CFB128_AES256 = 6,
};

static inline const char *rf_snmp_encryption_protocols_to_str(enum rf_snmp_encryption_protocols val)
{
	switch (val) {
	case SNMP_ENCRYPTION_PROTOCOLS_INVALID: return "Invalid";
	case SNMP_ENCRYPTION_PROTOCOLS_NONE: return "None";
	case SNMP_ENCRYPTION_PROTOCOLS_ACCOUNT: return "Account";
	case SNMP_ENCRYPTION_PROTOCOLS_CBC_DES: return "CBC_DES";
	case SNMP_ENCRYPTION_PROTOCOLS_CFB128_AES128: return "CFB128_AES128";
	case SNMP_ENCRYPTION_PROTOCOLS_CFB128_AES192: return "CFB128_AES192";
	case SNMP_ENCRYPTION_PROTOCOLS_CFB128_AES256: return "CFB128_AES256";
	default: return NULL;
	}
}

/* ManagerNetworkProtocol.v1_13_0.IPv6AddressGenerationMode */
enum rf_ipv6_address_generation_mode {
	IPV6_ADDRESS_GENERATION_MODE_INVALID = 0,
	IPV6_ADDRESS_GENERATION_MODE_STABLE_PRIVACY = 1,
	IPV6_ADDRESS_GENERATION_MODE_EUI64 = 2,
};

static inline const char *rf_ipv6_address_generation_mode_to_str(enum rf_ipv6_address_generation_mode val)
{
	switch (val) {
	case IPV6_ADDRESS_GENERATION_MODE_INVALID: return "Invalid";
	case IPV6_ADDRESS_GENERATION_MODE_STABLE_PRIVACY: return "StablePrivacy";
	case IPV6_ADDRESS_GENERATION_MODE_EUI64: return "EUI64";
	default: return NULL;
	}
}

/* ManagerNetworkProtocol.v1_13_0.SSHPreferredAuthentication */
enum rf_ssh_preferred_authentication {
	SSH_PREFERRED_AUTHENTICATION_INVALID = 0,
	SSH_PREFERRED_AUTHENTICATION_PASSWORD = 1,
	SSH_PREFERRED_AUTHENTICATION_PUBLIC_KEY = 2,
	SSH_PREFERRED_AUTHENTICATION_KEYBOARD_INTERACTIVE = 3,
	SSH_PREFERRED_AUTHENTICATION_GSSAPI_WITH_MIC = 4,
	SSH_PREFERRED_AUTHENTICATION_HOST_BASED = 5,
};

static inline const char *rf_ssh_preferred_authentication_to_str(enum rf_ssh_preferred_authentication val)
{
	switch (val) {
	case SSH_PREFERRED_AUTHENTICATION_INVALID: return "Invalid";
	case SSH_PREFERRED_AUTHENTICATION_PASSWORD: return "Password";
	case SSH_PREFERRED_AUTHENTICATION_PUBLIC_KEY: return "PublicKey";
	case SSH_PREFERRED_AUTHENTICATION_KEYBOARD_INTERACTIVE: return "KeyboardInteractive";
	case SSH_PREFERRED_AUTHENTICATION_GSSAPI_WITH_MIC: return "GSSAPIWithMIC";
	case SSH_PREFERRED_AUTHENTICATION_HOST_BASED: return "HostBased";
	default: return NULL;
	}
}

/* ManagerNetworkProtocol.v1_14_0.NTPState */
enum rf_ntp_state {
	NTP_STATE_INVALID = 0,
	NTP_STATE_SYNCHRONIZED = 1,
	NTP_STATE_NOT_SYNCHRONIZED = 2,
	NTP_STATE_DISABLED = 3,
	NTP_STATE_UNREACHABLE = 4,
};

static inline const char *rf_ntp_state_to_str(enum rf_ntp_state val)
{
	switch (val) {
	case NTP_STATE_INVALID: return "Invalid";
	case NTP_STATE_SYNCHRONIZED: return "Synchronized";
	case NTP_STATE_NOT_SYNCHRONIZED: return "NotSynchronized";
	case NTP_STATE_DISABLED: return "Disabled";
	case NTP_STATE_UNREACHABLE: return "Unreachable";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_MANAGER_NETWORK_PROTOCOL_H */
