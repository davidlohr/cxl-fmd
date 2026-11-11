/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Redfish HTTP Server
 *
 * Uses libmicrohttpd to serve Redfish REST API endpoints.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <microhttpd.h>

#include "cxl-fmd.h"

/* Maximum request body size (64KB - sufficient for Redfish PATCH) */
#define FMD_MAX_REQUEST_BODY (64 * 1024)

/* Forward declaration */
static enum MHD_Result handle_request(void *cls,
				      struct MHD_Connection *conn,
				      const char *url,
				      const char *method,
				      const char *version,
				      const char *upload_data,
				      size_t *upload_data_size,
				      void **con_cls);

/*
 * Simple pattern matching for routes like:
 * /redfish/v1/Chassis/{chassis_id}/PCIeDevices/{device_id}
 *
 * Returns true if match, fills params array with extracted values.
 * On failure, any allocated params are freed before returning.
 */
static bool match_route(const char *pattern, const char *url,
			const char **params, int max_params)
{
	const char *p = pattern;
	const char *u = url;
	int param_idx = 0;

	while (*p && *u) {
		if (*p == '{') {
			/* Parameter placeholder */
			const char *end = strchr(p, '}');
			if (!end)
				goto fail;

			/* Extract parameter value until next '/' or end */
			const char *val_start = u;
			while (*u && *u != '/')
				u++;

			if (param_idx < max_params) {
				size_t len = u - val_start;
				char *val = malloc(len + 1);
				if (val) {
					memcpy(val, val_start, len);
					val[len] = '\0';
					params[param_idx++] = val;
				}
			}

			p = end + 1;
		} else if (*p == *u) {
			p++;
			u++;
		} else {
			goto fail;
		}
	}

	/* Both should be at end (or pattern has trailing optional parts) */
	if (*p == '\0' && *u == '\0')
		return true;

fail:
	/* Free any params allocated during partial match */
	for (int i = 0; i < param_idx; i++) {
		free((void *)params[i]);
		params[i] = NULL;
	}
	return false;
}

static void free_params(const char **params, int count)
{
	for (int i = 0; i < count; i++) {
		free((void *)params[i]);
		params[i] = NULL;
	}
}

/*
 * Send JSON response
 */
static enum MHD_Result send_json_response(struct MHD_Connection *conn,
					  unsigned int status,
					  json_t *json)
{
	struct MHD_Response *response;
	enum MHD_Result ret;
	char *body;

	if (json) {
		body = json_dumps(json, JSON_INDENT(2));
		json_decref(json);
	} else {
		body = strdup("{}");
	}

	if (!body)
		return MHD_NO;

	response = MHD_create_response_from_buffer(strlen(body), body,
						   MHD_RESPMEM_MUST_FREE);
	if (!response) {
		free(body);
		return MHD_NO;
	}

	MHD_add_response_header(response, "Content-Type",
				"application/json; charset=utf-8");
	MHD_add_response_header(response, "OData-Version", "4.0");

	ret = MHD_queue_response(conn, status, response);
	MHD_destroy_response(response);

	return ret;
}

static enum MHD_Result send_error(struct MHD_Connection *conn,
				  unsigned int status,
				  const char *message)
{
	json_t *error = json_pack("{s:{s:s, s:s}}",
				  "error",
				  "code", "Base.1.0.GeneralError",
				  "message", message);
	return send_json_response(conn, status, error);
}

/*
 * Map a negative errno from a handler to an HTTP error response
 */
static enum MHD_Result send_errno(struct MHD_Connection *conn, int rc,
				  const char *msg)
{
	unsigned int status;

	switch (-rc) {
	case ENOENT:
		status = MHD_HTTP_NOT_FOUND;
		break;
	case EINVAL:
		status = MHD_HTTP_BAD_REQUEST;
		break;
	case EBUSY:
	case ENOSPC:
	case EEXIST:
		status = MHD_HTTP_CONFLICT;
		break;
	default:
		status = MHD_HTTP_INTERNAL_SERVER_ERROR;
		break;
	}

	return send_error(conn, status, msg ? msg : "Request failed");
}

/*
 * Send 204 No Content response (for DELETE success)
 */
static enum MHD_Result send_no_content(struct MHD_Connection *conn)
{
	struct MHD_Response *response;
	enum MHD_Result ret;

	response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
	if (!response)
		return MHD_NO;

	MHD_add_response_header(response, "OData-Version", "4.0");

	ret = MHD_queue_response(conn, MHD_HTTP_NO_CONTENT, response);
	MHD_destroy_response(response);

	return ret;
}

/*
 * Send JSON response with Location header (for POST/create success)
 */
static enum MHD_Result send_json_response_with_location(
	struct MHD_Connection *conn,
	unsigned int status,
	json_t *json,
	const char *location)
{
	struct MHD_Response *response;
	enum MHD_Result ret;
	char *body;

	if (json) {
		body = json_dumps(json, JSON_INDENT(2));
		json_decref(json);
	} else {
		body = strdup("{}");
	}

	if (!body)
		return MHD_NO;

	response = MHD_create_response_from_buffer(strlen(body), body,
						   MHD_RESPMEM_MUST_FREE);
	if (!response) {
		free(body);
		return MHD_NO;
	}

	MHD_add_response_header(response, "Content-Type",
				"application/json; charset=utf-8");
	MHD_add_response_header(response, "OData-Version", "4.0");
	if (location)
		MHD_add_response_header(response, "Location", location);

	ret = MHD_queue_response(conn, status, response);
	MHD_destroy_response(response);

	return ret;
}

/*
 * Send XML response (for $metadata)
 */
static enum MHD_Result send_xml_response(struct MHD_Connection *conn,
					 unsigned int status,
					 const char *xml)
{
	struct MHD_Response *response;
	enum MHD_Result ret;
	char *body;

	body = strdup(xml);
	if (!body)
		return MHD_NO;

	response = MHD_create_response_from_buffer(strlen(body), body,
						   MHD_RESPMEM_MUST_FREE);
	if (!response) {
		free(body);
		return MHD_NO;
	}

	MHD_add_response_header(response, "Content-Type",
				"application/xml; charset=utf-8");
	MHD_add_response_header(response, "OData-Version", "4.0");

	ret = MHD_queue_response(conn, status, response);
	MHD_destroy_response(response);

	return ret;
}

/*
 * Normalize URL by stripping trailing slash (except for root "/")
 * Returns a newly allocated string that must be freed.
 */
static char *normalize_url(const char *url)
{
	size_t len = strlen(url);
	char *normalized = strdup(url);

	if (normalized && len > 1 && normalized[len - 1] == '/')
		normalized[len - 1] = '\0';

	return normalized;
}

/*
 * Main request handler
 */
/* Per-connection context for request body accumulation */
struct request_context {
	char *body;
	size_t body_size;
	size_t body_alloc;
	int error;  /* 0 = ok, 413 = body too large, 500 = alloc failed */
};

static struct request_context *request_context_new(void)
{
	return calloc(1, sizeof(struct request_context));
}

static void request_context_free(struct request_context *rctx)
{
	if (rctx) {
		free(rctx->body);
		free(rctx);
	}
}

/*
 * Append data to request body buffer.
 * Returns:
 *   0: success
 *  -1: body too large (HTTP 413)
 *  -2: allocation failed (HTTP 500)
 */
static int request_context_append(struct request_context *rctx,
				  const char *data, size_t size)
{
	size_t new_size;

	/* Check for overflow in size calculation */
	if (size > SIZE_MAX - rctx->body_size)
		return -1;

	new_size = rctx->body_size + size;

	/* Enforce maximum body size */
	if (new_size > FMD_MAX_REQUEST_BODY)
		return -1;

	if (new_size > rctx->body_alloc) {
		size_t new_alloc = rctx->body_alloc ? rctx->body_alloc * 2 : 4096;
		while (new_alloc < new_size) {
			/* Check for overflow in doubling */
			if (new_alloc > SIZE_MAX / 2)
				return -2;
			new_alloc *= 2;
		}
		char *new_body = realloc(rctx->body, new_alloc);
		if (!new_body)
			return -2;
		rctx->body = new_body;
		rctx->body_alloc = new_alloc;
	}
	memcpy(rctx->body + rctx->body_size, data, size);
	rctx->body_size = new_size;
	return 0;
}

/*
 * Handle GET request - route to appropriate handler
 */
static enum MHD_Result handle_request_get(struct fmd_ctx *ctx,
					  struct MHD_Connection *conn,
					  const char *url)
{
	const char *params[8] = {0};
	json_t *response = NULL;
	char *norm_url;

	norm_url = normalize_url(url);
	if (!norm_url)
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Memory allocation failed");

	fmd_dbg(ctx, "GET %s", norm_url);

	/* Route matching */
	if (strcmp(norm_url, "/redfish") == 0) {
		response = json_pack("{s:s}", "v1", "/redfish/v1/");
		goto ok;
	}

	if (strcmp(norm_url, "/redfish/v1") == 0) {
		response = fmd_redfish_service_root(ctx);
		goto ok;
	}

	if (strcmp(norm_url, "/redfish/v1/odata") == 0) {
		response = fmd_redfish_odata(ctx);
		goto ok;
	}

	if (strcmp(norm_url, "/redfish/v1/$metadata") == 0) {
		char *metadata = fmd_redfish_metadata(ctx);
		free(norm_url);
		if (!metadata)
			return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "Failed to generate metadata");
		enum MHD_Result ret = send_xml_response(conn, MHD_HTTP_OK, metadata);
		free(metadata);
		return ret;
	}

	if (strcmp(norm_url, "/redfish/v1/Systems") == 0) {
		response = fmd_redfish_systems_collection(ctx);
		goto ok;
	}

	if (match_route("/redfish/v1/Systems/{sys}", norm_url, params, 8)) {
		response = fmd_redfish_system(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Systems/{sys}/MemoryDomains", norm_url, params, 8)) {
		response = fmd_redfish_system_memory_domains(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Systems/{sys}/MemoryDomains/{dom}", norm_url, params, 8)) {
		response = fmd_redfish_system_memory_domain(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Systems/{sys}/MemoryDomains/{dom}/MemoryChunks", norm_url, params, 8)) {
		response = fmd_redfish_system_memory_chunks(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Systems/{sys}/MemoryDomains/{dom}/MemoryChunks/{chunk}", norm_url, params, 8)) {
		response = fmd_redfish_system_memory_chunk(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (strcmp(norm_url, "/redfish/v1/Managers") == 0) {
		response = fmd_redfish_managers_collection(ctx);
		goto ok;
	}

	if (strcmp(norm_url, "/redfish/v1/SessionService/Sessions") == 0) {
		response = fmd_redfish_sessions_collection(ctx);
		goto ok;
	}

	if (strcmp(norm_url, "/redfish/v1/Fabrics") == 0) {
		response = fmd_redfish_fabrics_collection(ctx);
		goto ok;
	}

	if (match_route("/redfish/v1/Fabrics/{id}", norm_url, params, 8)) {
		response = fmd_redfish_fabric(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches", norm_url, params, 8)) {
		response = fmd_redfish_switches_collection(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches/{sw}", norm_url, params, 8)) {
		response = fmd_redfish_switch(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches/{sw}/Ports", norm_url, params, 8)) {
		response = fmd_redfish_ports_collection(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches/{sw}/Ports/{port}", norm_url, params, 8)) {
		response = fmd_redfish_port(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches/{sw}/Ports/{port}/Metrics", norm_url, params, 8)) {
		response = fmd_redfish_port_metrics(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches/{sw}/VCSs", norm_url, params, 8)) {
		response = fmd_redfish_vcs_collection(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches/{sw}/VCSs/{vcs}", norm_url, params, 8)) {
		response = fmd_redfish_vcs(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches/{sw}/VCSs/{vcs}/VPPBs", norm_url, params, 8)) {
		response = fmd_redfish_vppb_collection(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Switches/{sw}/VCSs/{vcs}/VPPBs/{vppb}", norm_url, params, 8)) {
		response = fmd_redfish_vppb(ctx, params[0], params[1], params[2], params[3]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Endpoints", norm_url, params, 8)) {
		response = fmd_redfish_endpoints_collection(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Endpoints/{ep}", norm_url, params, 8)) {
		response = fmd_redfish_endpoint(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Connections", norm_url, params, 8)) {
		response = fmd_redfish_connections_collection(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Connections/{conn}", norm_url, params, 8)) {
		response = fmd_redfish_connection(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Zones", norm_url, params, 8)) {
		response = fmd_redfish_zones_collection(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Fabrics/{id}/Zones/{zone}", norm_url, params, 8)) {
		response = fmd_redfish_zone(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (strcmp(norm_url, "/redfish/v1/Chassis") == 0) {
		response = fmd_redfish_chassis_collection(ctx);
		goto ok;
	}

	if (match_route("/redfish/v1/Chassis/{id}", norm_url, params, 8)) {
		response = fmd_redfish_chassis(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices", norm_url, params, 8)) {
		response = fmd_redfish_pcie_devices(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}", norm_url, params, 8)) {
		response = fmd_redfish_pcie_device(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices",
			norm_url, params, 8)) {
		response = fmd_redfish_cxl_logical_devices(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}",
			norm_url, params, 8)) {
		response = fmd_redfish_cxl_logical_device(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	/* Dynamic capacity: MemoryRegions (per LD) and MemoryExtents (per device) */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions",
			norm_url, params, 8)) {
		response = fmd_redfish_dc_memory_regions(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}",
			norm_url, params, 8)) {
		response = fmd_redfish_dc_memory_region(ctx, params[0], params[1],
							params[2], params[3]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents",
			norm_url, params, 8)) {
		response = fmd_redfish_dc_memory_extents(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}",
			norm_url, params, 8)) {
		response = fmd_redfish_dc_memory_extent(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	/* Memory Collection and Memory resources */
	if (match_route("/redfish/v1/Chassis/{id}/Memory", norm_url, params, 8)) {
		response = fmd_redfish_memory_collection(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}", norm_url, params, 8)) {
		response = fmd_redfish_memory(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/MemoryMetrics",
			norm_url, params, 8)) {
		response = fmd_redfish_memory_metrics(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/MemoryDomains", norm_url, params, 8)) {
		response = fmd_redfish_memory_domains(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/MemoryDomains/{dom}", norm_url, params, 8)) {
		response = fmd_redfish_memory_domain(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks",
			norm_url, params, 8)) {
		response = fmd_redfish_memory_chunks(ctx, params[0], params[1]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}",
			norm_url, params, 8)) {
		response = fmd_redfish_memory_chunk(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

not_found:
	free(norm_url);
	return send_error(conn, MHD_HTTP_NOT_FOUND, "Resource not found");

ok:
	free(norm_url);
	return send_json_response(conn, MHD_HTTP_OK, response);
}

/*
 * Handle POST request
 */
static enum MHD_Result handle_request_post(struct fmd_ctx *ctx,
					   struct MHD_Connection *conn,
					   const char *url,
					   const char *body,
					   size_t body_size)
{
	const char *params[8] = {0};
	char *norm_url;
	int rc;

	fmd_dbg(ctx, "POST %s (%zu bytes body)", url, body_size);

	norm_url = normalize_url(url);
	if (!norm_url)
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Memory allocation failed");

	/* POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.ScanMedia */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.ScanMedia",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_scan_media(ctx, params[0], params[1],
						   body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_error(conn, MHD_HTTP_BAD_REQUEST,
				  "Action failed");
	}

	/* POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetPassphrase */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetPassphrase",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_set_passphrase(ctx, params[0], params[1],
						       body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_error(conn, MHD_HTTP_BAD_REQUEST,
				  "Action failed");
	}

	/* POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetMasterPassphrase */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SetMasterPassphrase",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_set_master_passphrase(ctx, params[0], params[1],
							      body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_error(conn, MHD_HTTP_BAD_REQUEST,
				  "Action failed");
	}

	/* POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisablePassphrase */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisablePassphrase",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_disable_passphrase(ctx, params[0], params[1],
							   body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_error(conn, MHD_HTTP_BAD_REQUEST,
				  "Action failed");
	}

	/* POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisableMasterPassphrase */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.DisableMasterPassphrase",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_disable_master_passphrase(ctx, params[0], params[1],
								  body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_error(conn, MHD_HTTP_BAD_REQUEST,
				  "Action failed");
	}

	/* POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.FreezeSecurityState */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.FreezeSecurityState",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_freeze_security_state(ctx, params[0], params[1]);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_error(conn, MHD_HTTP_BAD_REQUEST,
				  "Action failed");
	}

	/* POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SecureEraseUnit */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.SecureEraseUnit",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_secure_erase_unit(ctx, params[0], params[1],
							  body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_error(conn, MHD_HTTP_BAD_REQUEST,
				  "Action failed");
	}

	/* POST /redfish/v1/Fabrics/{fab}/Switches/{sw}/Ports/{port}/Actions/Port.ResetPPB */
	if (match_route("/redfish/v1/Fabrics/{fab}/Switches/{sw}/Ports/{port}/Actions/Port.ResetPPB",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_reset_ppb(ctx, params[1], params[2]);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_error(conn, MHD_HTTP_BAD_REQUEST,
				  "Action failed");
	}

	/* POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd",
			norm_url, params, 8)) {
		const char *msg = NULL;

		rc = fmd_redfish_action_dc_add(ctx, params[0], params[1], params[2],
					       body, body_size, &msg);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_errno(conn, rc, msg);
	}

	/* POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease",
			norm_url, params, 8)) {
		const char *msg = NULL;

		rc = fmd_redfish_action_dc_release(ctx, params[0], params[1], params[2],
						   body, body_size, &msg);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_errno(conn, rc, msg);
	}

	/* POST .../PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory",
			norm_url, params, 8)) {
		const char *msg = NULL;

		rc = fmd_redfish_action_dc_reference(ctx, params[0], params[1],
						     body, body_size, &msg);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_errno(conn, rc, msg);
	}

	/* POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks - create chunk */
	if (match_route("/redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks",
			norm_url, params, 8)) {
		json_t *response = NULL;
		char location[512];

		rc = fmd_redfish_post_memory_chunk(ctx, params[0], params[1],
						   body, body_size, &response);

		if (rc == 0 && response) {
			/* Extract @odata.id for Location header */
			json_t *odata_id = json_object_get(response, "@odata.id");
			if (odata_id && json_is_string(odata_id)) {
				snprintf(location, sizeof(location), "%s",
					 json_string_value(odata_id));
			} else {
				location[0] = '\0';
			}

			free_params(params, 8);
			free(norm_url);
			return send_json_response_with_location(conn, MHD_HTTP_CREATED,
								response, location);
		}

		free_params(params, 8);
		free(norm_url);

		if (rc == -1)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Chassis or MemoryDomain not found");
		if (rc == -2)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "Invalid request body - MemoryChunkSizeMiB required");
		if (rc == -3)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "Insufficient capacity in memory domain");
		if (rc == -4)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "Address range overlaps with existing chunk");
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Failed to create memory chunk");
	}

	/* POST /redfish/v1/Fabrics/{id}/Connections - create connection */
	if (match_route("/redfish/v1/Fabrics/{id}/Connections", norm_url, params, 8)) {
		json_t *response = NULL;
		char location[512];

		rc = fmd_redfish_post_connection(ctx, params[0], body, body_size, &response);

		if (rc == 0 && response) {
			/* Extract @odata.id for Location header */
			json_t *odata_id = json_object_get(response, "@odata.id");
			if (odata_id && json_is_string(odata_id)) {
				snprintf(location, sizeof(location), "%s",
					 json_string_value(odata_id));
			} else {
				location[0] = '\0';
			}

			free_params(params, 8);
			free(norm_url);
			return send_json_response_with_location(conn, MHD_HTTP_CREATED,
								response, location);
		}

		free_params(params, 8);
		free(norm_url);

		if (rc == -1)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Fabric not found");
		if (rc == -2)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "Invalid request body");
		if (rc == -3)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Host system (initiator) not found");
		if (rc == -4)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Target device not found");
		if (rc == -5)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Memory chunk not found");
		if (rc == -8)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "Cross-head access denied: host cannot access this memory");
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Failed to create connection");
	}

	free(norm_url);
	return send_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
			  "POST not supported for this resource");
}

/*
 * Handle PUT request
 */
static enum MHD_Result handle_request_put(struct fmd_ctx *ctx,
					  struct MHD_Connection *conn,
					  const char *url,
					  const char *body,
					  size_t body_size)
{
	(void)body;

	fmd_dbg(ctx, "PUT %s (%zu bytes body)", url, body_size);

	/* TODO: implement PUT handlers */
	return send_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
			  "PUT not yet implemented");
}

/*
 * Handle PATCH request
 */
static enum MHD_Result handle_request_patch(struct fmd_ctx *ctx,
					    struct MHD_Connection *conn,
					    const char *url,
					    const char *body,
					    size_t body_size)
{
	const char *params[8] = {0};
	char *norm_url;
	int rc;

	fmd_dbg(ctx, "PATCH %s (%zu bytes body)", url, body_size);

	norm_url = normalize_url(url);
	if (!norm_url)
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Memory allocation failed");

	/* PATCH /redfish/v1/Chassis/{id}/PCIeDevices/{dev} */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}", norm_url, params, 8)) {
		rc = fmd_redfish_patch_pcie_device(ctx, params[0], params[1], body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		if (rc == -1)
			return send_error(conn, MHD_HTTP_NOT_FOUND, "Resource not found");
		if (rc == -2)
			return send_error(conn, MHD_HTTP_BAD_REQUEST, "Invalid request body");
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "CCI command failed");
	}

	/* PATCH /redfish/v1/Chassis/{id}/Memory/{mem} */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}", norm_url, params, 8)) {
		rc = fmd_redfish_patch_memory(ctx, params[0], params[1], body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		if (rc == -1)
			return send_error(conn, MHD_HTTP_NOT_FOUND, "Resource not found");
		if (rc == -2)
			return send_error(conn, MHD_HTTP_BAD_REQUEST, "Invalid request body");
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "CCI command failed");
	}

	/* PATCH .../CXLLogicalDevices/{ld}/MemoryRegions/{region} - Set DC Region Config */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}",
			norm_url, params, 8)) {
		const char *msg = NULL;

		rc = fmd_redfish_patch_dc_memory_region(ctx, params[0], params[1], params[2],
							params[3], body, body_size, &msg);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_errno(conn, rc, msg);
	}

	free(norm_url);
	return send_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
			  "PATCH not supported for this resource");
}

/*
 * Handle DELETE request
 */
static enum MHD_Result handle_request_delete(struct fmd_ctx *ctx,
					     struct MHD_Connection *conn,
					     const char *url)
{
	const char *params[8] = {0};
	char *norm_url;
	int rc;

	fmd_dbg(ctx, "DELETE %s", url);

	norm_url = normalize_url(url);
	if (!norm_url)
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Memory allocation failed");

	/* DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk} */
	if (match_route("/redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}",
			norm_url, params, 8)) {
		rc = fmd_redfish_delete_memory_chunk(ctx, params[0], params[1],
						     params[2]);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_no_content(conn);
		if (rc == -1)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Memory chunk not found");
		if (rc == -2)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "Chunk does not belong to specified domain");
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Failed to delete memory chunk");
	}

	/* DELETE /redfish/v1/Fabrics/{id}/Connections/{conn} */
	if (match_route("/redfish/v1/Fabrics/{id}/Connections/{conn}",
			norm_url, params, 8)) {
		rc = fmd_redfish_delete_connection(ctx, params[0], params[1]);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_no_content(conn);
		if (rc == -1)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Connection not found");
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Failed to delete connection");
	}

	/* DELETE .../PCIeDevices/{dev}/MemoryExtents/{ext} - release the extent */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}",
			norm_url, params, 8)) {
		const char *msg = NULL;

		rc = fmd_redfish_delete_dc_memory_extent(ctx, params[0], params[1],
							 params[2], &msg);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_no_content(conn);
		return send_errno(conn, rc, msg);
	}

	free(norm_url);
	return send_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
			  "DELETE not supported for this resource");
}

/*
 * Main request handler - dispatches to method-specific handlers
 */
static enum MHD_Result handle_request(void *cls,
				      struct MHD_Connection *conn,
				      const char *url,
				      const char *method,
				      const char *version,
				      const char *upload_data,
				      size_t *upload_data_size,
				      void **con_cls)
{
	struct fmd_ctx *ctx = cls;
	struct request_context *rctx = *con_cls;
	bool has_body;

	/* Require HTTP/1.x */
	if (strncmp(version, "HTTP/1.", 7) != 0) {
		return send_error(conn, MHD_HTTP_HTTP_VERSION_NOT_SUPPORTED,
				  "HTTP version not supported");
	}

	/* Check if this is a method that may have a request body */
	has_body = (strcmp(method, "POST") == 0 ||
		    strcmp(method, "PUT") == 0 ||
		    strcmp(method, "PATCH") == 0);

	/*
	 * MHD callback pattern:
	 * - GET/DELETE: Handle immediately (no body expected)
	 * - POST/PUT/PATCH: Wait for body data across multiple calls
	 */
	if (has_body) {
		if (!rctx) {
			rctx = request_context_new();
			if (!rctx)
				return MHD_NO;
			*con_cls = rctx;
			return MHD_YES;
		}

		if (*upload_data_size > 0) {
			/*
			 * Only append if we haven't already hit an error.
			 * If error is set, just drain the remaining data.
			 */
			if (rctx->error == 0) {
				int rc = request_context_append(rctx, upload_data,
								*upload_data_size);
				if (rc == -1) {
					rctx->error = MHD_HTTP_CONTENT_TOO_LARGE;
				} else if (rc == -2) {
					rctx->error = MHD_HTTP_INTERNAL_SERVER_ERROR;
				} else {
					fmd_dbg(ctx, "%s %s: buffered %zu bytes (total: %zu)",
						method, url, *upload_data_size, rctx->body_size);
				}
			}
			*upload_data_size = 0;
			return MHD_YES;
		}

		/* Body complete - check for errors before dispatching */
		if (rctx->error == MHD_HTTP_CONTENT_TOO_LARGE) {
			return send_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
					  "Request body too large (max 64KB)");
		} else if (rctx->error == MHD_HTTP_INTERNAL_SERVER_ERROR) {
			return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "Failed to buffer request body");
		}
	}

	/* Dispatch to method-specific handler */
	if (strcmp(method, "GET") == 0)
		return handle_request_get(ctx, conn, url);
	else if (strcmp(method, "POST") == 0)
		return handle_request_post(ctx, conn, url,
					   rctx->body, rctx->body_size);
	else if (strcmp(method, "PUT") == 0)
		return handle_request_put(ctx, conn, url,
					  rctx->body, rctx->body_size);
	else if (strcmp(method, "PATCH") == 0)
		return handle_request_patch(ctx, conn, url,
					    rctx->body, rctx->body_size);
	else if (strcmp(method, "DELETE") == 0)
		return handle_request_delete(ctx, conn, url);

	return send_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
			  "Method not allowed");
}

static void request_completed(void *cls,
			      struct MHD_Connection *conn,
			      void **con_cls,
			      enum MHD_RequestTerminationCode toe)
{
	struct request_context *rctx = *con_cls;

	(void)cls;
	(void)conn;
	(void)toe;

	request_context_free(rctx);
	*con_cls = NULL;
}

int fmd_redfish_start(struct fmd_ctx *ctx)
{
	if (!ctx)
		return -1;

	ctx->redfish.port = ctx->config.port;

	ctx->redfish.daemon = MHD_start_daemon(
		MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
		ctx->redfish.port,
		NULL, NULL,                    /* accept policy */
		handle_request, ctx,           /* request handler */
		MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
		MHD_OPTION_END);

	if (!ctx->redfish.daemon) {
		fmd_err(ctx, "Failed to start HTTP daemon on port %d",
			ctx->redfish.port);
		return -1;
	}

	ctx->redfish.running = true;
	return 0;
}

void fmd_redfish_stop(struct fmd_ctx *ctx)
{
	if (!ctx || !ctx->redfish.daemon)
		return;

	MHD_stop_daemon(ctx->redfish.daemon);
	ctx->redfish.daemon = NULL;
	ctx->redfish.running = false;
}
