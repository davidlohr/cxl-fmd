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

/*
 * The Health a Media Status adds to that of the Health Status bits:
 * Critical for 03h, All Data Lost; OK for 00h, Normal, and 01h, Not Ready
 * (its State is Starting); Warning for any other, write persistency or
 * data lost or about to be, and reserved values. cxl-fmd's mapping:
 * DSP0288 maps Health Status to Status ("Get Health Info, Health Status,
 * Many, Status", Comment 3), and Media Status to nothing.
 */
static enum rf_health media_health(uint8_t cci_media_status)
{
	switch (cci_media_status) {
	case CXL_MEDIA_NORMAL:
	case CXL_MEDIA_NOT_READY:
		return HEALTH_OK;
	case CXL_MEDIA_ALL_DATA_LOST:
		return HEALTH_CRITICAL;
	default:
		return HEALTH_WARNING;
	}
}

/* Add the Get Health Info of a device or LD to @r; @valid if it was read */
void fmd_rollup_add(struct fmd_status_rollup *r,
		    const struct cxlmi_cmd_memdev_get_health_info_rsp *h, bool valid)
{
	if (!valid) {
		r->unknown++;
		return;
	}
	/* Health Status is a bit field, Media Status an enumeration */
	r->health_bits |= h->health_status;
	r->media = r->known ? fmd_worse_media_status(r->media, h->media_status) :
			      h->media_status;
	r->known++;
}

/*
 * The Status of what @r adds up. *@state is the State of the worst Media
 * Status read (fmd_map_media_status(): only 03h is UnavailableOffline),
 * Enabled if none was. *@health is the worse of what the Health Status
 * bits read map to (fmd_map_health_status()) and what the Media Status
 * adds (media_health()). Returns whether *@health holds: not if a Get
 * Health Info was not read and the ones that were are all OK, as the one
 * that was not may not be. A known Warning or Critical is reported though
 * another read failed: it is no better than that. cxl-fmd's rules.
 */
bool fmd_rollup_status(const struct fmd_status_rollup *r,
		       enum rf_health *health, enum rf_state *state)
{
	enum rf_health h = HEALTH_OK, m;

	*state = STATE_ENABLED;
	if (r->known) {
		*state = fmd_map_media_status(r->media);
		h = fmd_map_health_status(r->health_bits);
		m = media_health(r->media);
		if (m > h)
			h = m;
	}
	*health = h;
	return !r->unknown || h != HEALTH_OK;
}
