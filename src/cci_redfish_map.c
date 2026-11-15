/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - CCI to Redfish Mapping
 *
 * Maps CXL CCI command output values to Redfish schema enum values.
 */

#include "cxl-fmd.h"
#include "generated/enums/resource.h"
#include "generated/enums/cxl_logical_device.h"

/*
 * CXL Health Status bit definitions
 */
#define CXL_HEALTH_MAINTENANCE_NEEDED      (1 << 0)
#define CXL_HEALTH_PERFORMANCE_DEGRADED    (1 << 1)
#define CXL_HEALTH_HW_REPLACEMENT_NEEDED   (1 << 2)
#define CXL_HEALTH_CAPACITY_DEGRADED       (1 << 3)

/*
 * CXL Media Status values (Table 8-315)
 */
#define CXL_MEDIA_NORMAL                   0x00
#define CXL_MEDIA_NOT_READY                0x01
#define CXL_MEDIA_WRITE_PERSISTENCY_LOST   0x02
#define CXL_MEDIA_ALL_DATA_LOST            0x03
#define CXL_MEDIA_WRITE_PERSISTENCY_LOSS_ON_POWER_LOSS 0x04
#define CXL_MEDIA_ALL_DATA_LOSS_IMMINENT   0x09	/* the last defined */

/*
 * Map CXL health_status to Redfish Health enum
 *
 * CXL health_status is a bitmask:
 *   Bit 0: Maintenance Needed → Warning
 *   Bit 1: Performance Degraded → Warning
 *   Bit 2: Hardware Replacement Needed → Critical
 *   Bit 3: Memory Capacity Degraded → Warning
 *
 * (Table 8-315). The Redfish values are cxl-fmd's mapping.
 */
enum rf_health fmd_map_health_status(uint8_t cci_health_status)
{
	if (cci_health_status & CXL_HEALTH_HW_REPLACEMENT_NEEDED)
		return HEALTH_CRITICAL;

	if (cci_health_status & (CXL_HEALTH_MAINTENANCE_NEEDED |
				 CXL_HEALTH_PERFORMANCE_DEGRADED |
				 CXL_HEALTH_CAPACITY_DEGRADED))
		return HEALTH_WARNING;

	return HEALTH_OK;
}

/*
 * Map CXL media_status to Redfish State enum
 *
 * CXL media_status values:
 *   0x00: Normal → Enabled
 *   0x01: Not Ready → Starting
 *   0x02: Write Persistency Lost → Degraded
 *   0x03: All Data Lost → UnavailableOffline
 *   0x04: Write Persistency Loss in the Event of Power Loss ("The device's
 *         ability to persist subsequent write requests may be lost in the
 *         event of a power loss") → Degraded
 *   0x05 to 0x09 → Degraded, below
 *   Reserved ("All other encodings are reserved") → Degraded, which is
 *   cxl-fmd's choice: the media is not reported as Normal, and nothing
 *   more is known of it.
 */
enum rf_state fmd_map_media_status(uint8_t cci_media_status)
{
	switch (cci_media_status) {
	case CXL_MEDIA_NORMAL:
		return STATE_ENABLED;
	case CXL_MEDIA_NOT_READY:
		return STATE_STARTING;
	case CXL_MEDIA_WRITE_PERSISTENCY_LOST:
	case CXL_MEDIA_WRITE_PERSISTENCY_LOSS_ON_POWER_LOSS:
	/*
	 * 05h to 09h (Table 8-315): write persistency or all data may be
	 * lost, at shutdown, on power loss, or imminently. The media still
	 * works and is not in its normal state.
	 */
	case 0x05:
	case 0x06:
	case 0x07:
	case 0x08:
	case CXL_MEDIA_ALL_DATA_LOSS_IMMINENT:
		return STATE_DEGRADED;
	case CXL_MEDIA_ALL_DATA_LOST:
		return STATE_UNAVAILABLE_OFFLINE;
	default:
		/* reserved */
		return STATE_DEGRADED;
	}
}

/*
 * The worse of two Media Status values. They are an enumeration, not a
 * scale: "worse" is by the state each maps to, then by which is abnormal.
 */
uint8_t fmd_worse_media_status(uint8_t a, uint8_t b)
{
	static const int rank[] = {
		[STATE_ENABLED] = 0, [STATE_STARTING] = 1,
		[STATE_DEGRADED] = 2, [STATE_UNAVAILABLE_OFFLINE] = 3,
	};
	enum rf_state sa = fmd_map_media_status(a), sb = fmd_map_media_status(b);
	int ra = (unsigned int)sa < sizeof(rank) / sizeof(rank[0]) ? rank[sa] : 0;
	int rb = (unsigned int)sb < sizeof(rank) / sizeof(rank[0]) ? rank[sb] : 0;

	if (ra != rb)
		return ra > rb ? a : b;
	return a ? a : b;
}
