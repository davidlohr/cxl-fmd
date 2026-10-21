/* SPDX-License-Identifier: LGPL-2.1 */
/* Auto-generated from DMTF CSDL schemas - DO NOT EDIT */

#ifndef REDFISH_ENUM_RESOURCE_H
#define REDFISH_ENUM_RESOURCE_H

#include <stddef.h>

/* Resource.State */
enum rf_state {
	STATE_INVALID = 0,
	STATE_ENABLED = 1,
	STATE_DISABLED = 2,
	STATE_STANDBY_OFFLINE = 3,
	STATE_STANDBY_SPARE = 4,
	STATE_IN_TEST = 5,
	STATE_STARTING = 6,
	STATE_ABSENT = 7,
	STATE_UNAVAILABLE_OFFLINE = 8,
	STATE_DEFERRING = 9,
	STATE_QUIESCED = 10,
	STATE_UPDATING = 11,
	STATE_QUALIFIED = 12,
	STATE_DEGRADED = 13,
};

static inline const char *rf_state_to_str(enum rf_state val)
{
	switch (val) {
	case STATE_INVALID: return "Invalid";
	case STATE_ENABLED: return "Enabled";
	case STATE_DISABLED: return "Disabled";
	case STATE_STANDBY_OFFLINE: return "StandbyOffline";
	case STATE_STANDBY_SPARE: return "StandbySpare";
	case STATE_IN_TEST: return "InTest";
	case STATE_STARTING: return "Starting";
	case STATE_ABSENT: return "Absent";
	case STATE_UNAVAILABLE_OFFLINE: return "UnavailableOffline";
	case STATE_DEFERRING: return "Deferring";
	case STATE_QUIESCED: return "Quiesced";
	case STATE_UPDATING: return "Updating";
	case STATE_QUALIFIED: return "Qualified";
	case STATE_DEGRADED: return "Degraded";
	default: return NULL;
	}
}

/* Resource.Health */
enum rf_health {
	HEALTH_INVALID = 0,
	HEALTH_OK = 1,
	HEALTH_WARNING = 2,
	HEALTH_CRITICAL = 3,
};

static inline const char *rf_health_to_str(enum rf_health val)
{
	switch (val) {
	case HEALTH_INVALID: return "Invalid";
	case HEALTH_OK: return "OK";
	case HEALTH_WARNING: return "Warning";
	case HEALTH_CRITICAL: return "Critical";
	default: return NULL;
	}
}

/* Resource.ResetType */
enum rf_reset_type {
	RESET_TYPE_INVALID = 0,
	RESET_TYPE_ON = 1,
	RESET_TYPE_FORCE_OFF = 2,
	RESET_TYPE_GRACEFUL_SHUTDOWN = 3,
	RESET_TYPE_GRACEFUL_RESTART = 4,
	RESET_TYPE_FORCE_RESTART = 5,
	RESET_TYPE_NMI = 6,
	RESET_TYPE_FORCE_ON = 7,
	RESET_TYPE_PUSH_POWER_BUTTON = 8,
	RESET_TYPE_POWER_CYCLE = 9,
	RESET_TYPE_SUSPEND = 10,
	RESET_TYPE_PAUSE = 11,
	RESET_TYPE_RESUME = 12,
	RESET_TYPE_FULL_POWER_CYCLE = 13,
	RESET_TYPE_SLEEP = 14,
	RESET_TYPE_HIBERNATE = 15,
};

static inline const char *rf_reset_type_to_str(enum rf_reset_type val)
{
	switch (val) {
	case RESET_TYPE_INVALID: return "Invalid";
	case RESET_TYPE_ON: return "On";
	case RESET_TYPE_FORCE_OFF: return "ForceOff";
	case RESET_TYPE_GRACEFUL_SHUTDOWN: return "GracefulShutdown";
	case RESET_TYPE_GRACEFUL_RESTART: return "GracefulRestart";
	case RESET_TYPE_FORCE_RESTART: return "ForceRestart";
	case RESET_TYPE_NMI: return "Nmi";
	case RESET_TYPE_FORCE_ON: return "ForceOn";
	case RESET_TYPE_PUSH_POWER_BUTTON: return "PushPowerButton";
	case RESET_TYPE_POWER_CYCLE: return "PowerCycle";
	case RESET_TYPE_SUSPEND: return "Suspend";
	case RESET_TYPE_PAUSE: return "Pause";
	case RESET_TYPE_RESUME: return "Resume";
	case RESET_TYPE_FULL_POWER_CYCLE: return "FullPowerCycle";
	case RESET_TYPE_SLEEP: return "Sleep";
	case RESET_TYPE_HIBERNATE: return "Hibernate";
	default: return NULL;
	}
}

/* Resource.IndicatorLED */
enum rf_indicator_led {
	INDICATOR_LED_INVALID = 0,
	INDICATOR_LED_LIT = 1,
	INDICATOR_LED_BLINKING = 2,
	INDICATOR_LED_OFF = 3,
};

static inline const char *rf_indicator_led_to_str(enum rf_indicator_led val)
{
	switch (val) {
	case INDICATOR_LED_INVALID: return "Invalid";
	case INDICATOR_LED_LIT: return "Lit";
	case INDICATOR_LED_BLINKING: return "Blinking";
	case INDICATOR_LED_OFF: return "Off";
	default: return NULL;
	}
}

/* Resource.PowerState */
enum rf_power_state {
	POWER_STATE_INVALID = 0,
	POWER_STATE_ON = 1,
	POWER_STATE_OFF = 2,
	POWER_STATE_POWERING_ON = 3,
	POWER_STATE_POWERING_OFF = 4,
	POWER_STATE_PAUSED = 5,
	POWER_STATE_SLEEPING = 6,
	POWER_STATE_HIBERNATING = 7,
};

static inline const char *rf_power_state_to_str(enum rf_power_state val)
{
	switch (val) {
	case POWER_STATE_INVALID: return "Invalid";
	case POWER_STATE_ON: return "On";
	case POWER_STATE_OFF: return "Off";
	case POWER_STATE_POWERING_ON: return "PoweringOn";
	case POWER_STATE_POWERING_OFF: return "PoweringOff";
	case POWER_STATE_PAUSED: return "Paused";
	case POWER_STATE_SLEEPING: return "Sleeping";
	case POWER_STATE_HIBERNATING: return "Hibernating";
	default: return NULL;
	}
}

/* Resource.v1_1_0.DurableNameFormat */
enum rf_durable_name_format {
	DURABLE_NAME_FORMAT_INVALID = 0,
	DURABLE_NAME_FORMAT_NAA = 1,
	DURABLE_NAME_FORMAT_I_QN = 2,
	DURABLE_NAME_FORMAT_FC_WWN = 3,
	DURABLE_NAME_FORMAT_UUID = 4,
	DURABLE_NAME_FORMAT_EUI = 5,
	DURABLE_NAME_FORMAT_NQN = 6,
	DURABLE_NAME_FORMAT_NSID = 7,
	DURABLE_NAME_FORMAT_NGUID = 8,
	DURABLE_NAME_FORMAT_MAC_ADDRESS = 9,
	DURABLE_NAME_FORMAT_GCXLID = 10,
};

static inline const char *rf_durable_name_format_to_str(enum rf_durable_name_format val)
{
	switch (val) {
	case DURABLE_NAME_FORMAT_INVALID: return "Invalid";
	case DURABLE_NAME_FORMAT_NAA: return "NAA";
	case DURABLE_NAME_FORMAT_I_QN: return "iQN";
	case DURABLE_NAME_FORMAT_FC_WWN: return "FC_WWN";
	case DURABLE_NAME_FORMAT_UUID: return "UUID";
	case DURABLE_NAME_FORMAT_EUI: return "EUI";
	case DURABLE_NAME_FORMAT_NQN: return "NQN";
	case DURABLE_NAME_FORMAT_NSID: return "NSID";
	case DURABLE_NAME_FORMAT_NGUID: return "NGUID";
	case DURABLE_NAME_FORMAT_MAC_ADDRESS: return "MACAddress";
	case DURABLE_NAME_FORMAT_GCXLID: return "GCXLID";
	default: return NULL;
	}
}

/* Resource.v1_3_0.RackUnits */
enum rf_rack_units {
	RACK_UNITS_INVALID = 0,
	RACK_UNITS_OPEN_U = 1,
	RACK_UNITS_EIA_310 = 2,
};

static inline const char *rf_rack_units_to_str(enum rf_rack_units val)
{
	switch (val) {
	case RACK_UNITS_INVALID: return "Invalid";
	case RACK_UNITS_OPEN_U: return "OpenU";
	case RACK_UNITS_EIA_310: return "EIA_310";
	default: return NULL;
	}
}

/* Resource.v1_5_0.LocationType */
enum rf_location_type {
	LOCATION_TYPE_INVALID = 0,
	LOCATION_TYPE_SLOT = 1,
	LOCATION_TYPE_BAY = 2,
	LOCATION_TYPE_CONNECTOR = 3,
	LOCATION_TYPE_SOCKET = 4,
	LOCATION_TYPE_BACKPLANE = 5,
	LOCATION_TYPE_EMBEDDED = 6,
};

static inline const char *rf_location_type_to_str(enum rf_location_type val)
{
	switch (val) {
	case LOCATION_TYPE_INVALID: return "Invalid";
	case LOCATION_TYPE_SLOT: return "Slot";
	case LOCATION_TYPE_BAY: return "Bay";
	case LOCATION_TYPE_CONNECTOR: return "Connector";
	case LOCATION_TYPE_SOCKET: return "Socket";
	case LOCATION_TYPE_BACKPLANE: return "Backplane";
	case LOCATION_TYPE_EMBEDDED: return "Embedded";
	default: return NULL;
	}
}

/* Resource.v1_5_0.Reference */
enum rf_reference {
	REFERENCE_INVALID = 0,
	REFERENCE_TOP = 1,
	REFERENCE_BOTTOM = 2,
	REFERENCE_FRONT = 3,
	REFERENCE_REAR = 4,
	REFERENCE_LEFT = 5,
	REFERENCE_RIGHT = 6,
	REFERENCE_MIDDLE = 7,
};

static inline const char *rf_reference_to_str(enum rf_reference val)
{
	switch (val) {
	case REFERENCE_INVALID: return "Invalid";
	case REFERENCE_TOP: return "Top";
	case REFERENCE_BOTTOM: return "Bottom";
	case REFERENCE_FRONT: return "Front";
	case REFERENCE_REAR: return "Rear";
	case REFERENCE_LEFT: return "Left";
	case REFERENCE_RIGHT: return "Right";
	case REFERENCE_MIDDLE: return "Middle";
	default: return NULL;
	}
}

/* Resource.v1_5_0.Orientation */
enum rf_orientation {
	ORIENTATION_INVALID = 0,
	ORIENTATION_FRONT_TO_BACK = 1,
	ORIENTATION_BACK_TO_FRONT = 2,
	ORIENTATION_TOP_TO_BOTTOM = 3,
	ORIENTATION_BOTTOM_TO_TOP = 4,
	ORIENTATION_LEFT_TO_RIGHT = 5,
	ORIENTATION_RIGHT_TO_LEFT = 6,
};

static inline const char *rf_orientation_to_str(enum rf_orientation val)
{
	switch (val) {
	case ORIENTATION_INVALID: return "Invalid";
	case ORIENTATION_FRONT_TO_BACK: return "FrontToBack";
	case ORIENTATION_BACK_TO_FRONT: return "BackToFront";
	case ORIENTATION_TOP_TO_BOTTOM: return "TopToBottom";
	case ORIENTATION_BOTTOM_TO_TOP: return "BottomToTop";
	case ORIENTATION_LEFT_TO_RIGHT: return "LeftToRight";
	case ORIENTATION_RIGHT_TO_LEFT: return "RightToLeft";
	default: return NULL;
	}
}

/* Resource.v1_22_0.ConditionType */
enum rf_condition_type {
	CONDITION_TYPE_INVALID = 0,
	CONDITION_TYPE_ALERT = 1,
	CONDITION_TYPE_INFORMATIONAL = 2,
	CONDITION_TYPE_SUBSYSTEM = 3,
};

static inline const char *rf_condition_type_to_str(enum rf_condition_type val)
{
	switch (val) {
	case CONDITION_TYPE_INVALID: return "Invalid";
	case CONDITION_TYPE_ALERT: return "Alert";
	case CONDITION_TYPE_INFORMATIONAL: return "Informational";
	case CONDITION_TYPE_SUBSYSTEM: return "Subsystem";
	default: return NULL;
	}
}

#endif /* REDFISH_ENUM_RESOURCE_H */
