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
 * The headers of every response cxl-fmd makes. OData-Version: "OData
 * version of the payload to which the response conforms", and
 * Cache-Control, which "Shall be supported and indicates whether a
 * response can or cannot be cached" (DSP0266 1.25.0 Table 13, both
 * required). Its value, no-store, is cxl-fmd's choice: a response is the
 * fabric as it was when it was read, which a host, a switch or another
 * request changes under it, and cxl-fmd gives no validator (no ETag, no
 * Last-Modified) a cache could check one it kept against.
 */
static void add_common_headers(struct MHD_Response *response)
{
	MHD_add_response_header(response, "OData-Version", "4.0");
	MHD_add_response_header(response, "Cache-Control", "no-store");
}

/*
 * Add @headers, pairs of name and value ended by a NULL name, if given; a
 * value that is NULL or empty is left out
 */
static void add_headers(struct MHD_Response *response, const char *const *headers)
{
	for (; headers && headers[0]; headers += 2)
		if (headers[1] && headers[1][0])
			MHD_add_response_header(response, headers[0], headers[1]);
}

/*
 * Send @json, which is consumed, with @status and @headers (add_headers())
 */
static enum MHD_Result send_json_headers(struct MHD_Connection *conn,
					 unsigned int status, json_t *json,
					 const char *const *headers)
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
	add_common_headers(response);
	add_headers(response, headers);

	ret = MHD_queue_response(conn, status, response);
	MHD_destroy_response(response);

	return ret;
}

/*
 * Send JSON response
 */
static enum MHD_Result send_json_response(struct MHD_Connection *conn,
					  unsigned int status,
					  json_t *json)
{
	return send_json_headers(conn, status, json, NULL);
}

/*
 * Send @body, or the fallback error body if it is NULL, with @status and,
 * if given, an Allow header
 */
static enum MHD_Result send_error_body(struct MHD_Connection *conn,
				       unsigned int status, json_t *body,
				       const char *allow)
{
	struct MHD_Response *response;
	enum MHD_Result ret;
	char *text = body ? json_dumps(body, JSON_INDENT(2)) : NULL;

	json_decref(body);
	if (!text)
		text = strdup(fmd_redfish_error_fallback);
	if (!text)
		return MHD_NO;
	response = MHD_create_response_from_buffer(strlen(text), text,
						   MHD_RESPMEM_MUST_FREE);
	if (!response) {
		free(text);
		return MHD_NO;
	}
	MHD_add_response_header(response, "Content-Type",
				"application/json; charset=utf-8");
	add_common_headers(response);
	if (allow)
		MHD_add_response_header(response, "Allow", allow);
	ret = MHD_queue_response(conn, status, response);
	MHD_destroy_response(response);
	return ret;
}

/*
 * An error: @message is cxl-fmd's sentence on it, and @info the Base
 * registry message (fmd_redfish_message()), which is consumed.
 */
static enum MHD_Result send_error(struct MHD_Connection *conn,
				  unsigned int status, const char *message,
				  json_t *info)
{
	return send_error_body(conn, status, fmd_redfish_error_body(message, info),
			       NULL);
}

/* A message of the registry with no argument, with its own Resolution */
#define RF_MSG(id) fmd_redfish_message(id, NULL, NULL, 0)
/*
 * GeneralError with @res, cxl-fmd's Resolution, where the registry has no
 * better message: "When an implementation uses GeneralError from the Base
 * Message Registry in ExtendedInfo, the implementation should include a
 * service-defined value for the Resolution property with this error to
 * indicate how to resolve the problem" (DSP0266 1.25.0 Section 9.5.11.2)
 */
#define RF_GENERAL(res) fmd_redfish_message(FMD_RF_GENERAL_ERROR, res, NULL, 0)
/* PropertyValueError of @prop, at /@prop, or at @ptr */
#define RF_PROPERTY(prop) RF_PROPERTY_AT(prop, "/" prop)
#define RF_PROPERTY_AT(prop, ptr) \
	fmd_redfish_message(FMD_RF_PROPERTY_VALUE_ERROR, NULL, ptr, 1, prop)

/* 404 for @url, which is not a resource of this service */
static enum MHD_Result send_not_found(struct MHD_Connection *conn,
				      const char *url, const char *message)
{
	return send_error(conn, MHD_HTTP_NOT_FOUND, message,
			  fmd_redfish_message(FMD_RF_INVALID_URI, NULL, NULL, 1, url));
}

/*
 * A 400 for a request: MalformedJSON if it has a body that is not JSON,
 * else @info (with no body, or none that is read, what was wrong is not
 * the JSON). @info is consumed either way.
 */
static enum MHD_Result send_bad_body(struct MHD_Connection *conn,
				     const char *body, size_t body_size,
				     const char *message, json_t *info)
{
	json_t *root;

	if (body_size) {
		root = json_loadb(body, body_size, 0, NULL);
		if (!root) {
			json_decref(info);
			info = RF_MSG(FMD_RF_MALFORMED_JSON);
		}
		json_decref(root);
	}
	return send_error(conn, MHD_HTTP_BAD_REQUEST, message, info);
}

/*
 * A DC request the device accepted, whose state could not be read back
 * after it (FMD_DC_ACCEPTED_STALE): 200, with "An error response, with a
 * message that indicates success or any additional relevant messages"
 * (DSP0266 Table 11, for an action), here the Success code with @message,
 * and Base Success in its ExtendedInfo.
 * A DELETE and a PATCH answer the same, where DSP0266 says they "should"
 * return the resource or 204: what cxl-fmd has of the resource is what is
 * stale (cxl-fmd's choice).
 */
static enum MHD_Result send_dc_accepted_stale(struct MHD_Connection *conn,
					      const char *message)
{
	return send_json_response(conn, MHD_HTTP_OK, fmd_redfish_success_body(message));
}

/*
 * A Dynamic Capacity add or release the device accepted (FMD_OP_STARTED):
 * 202 Accepted, the Task, and its monitor in Location. "The response to an
 * asynchronous operation shall return the HTTP 202 Accepted status code
 * and set the Location response header to the URI of a task monitor
 * associated with the task ... If a response body is provided, it shall
 * contain a representation of the Task resource" (DSP0266 1.25.0 Section
 * 12.2). A task that could not be shown (out of memory) is 202 with no
 * body and no Location; it is in the Tasks collection all the same.
 */
static enum MHD_Result send_json_response_with_location(struct MHD_Connection *conn,
							unsigned int status, json_t *json,
							const char *location);

static enum MHD_Result send_dc_started(struct MHD_Connection *conn, json_t *task)
{
	char location[256];

	if (!task)
		return send_json_response(conn, MHD_HTTP_ACCEPTED, NULL);
	snprintf(location, sizeof(location), "%s",
		 json_string_value(json_object_get(task, "TaskMonitor")));
	return send_json_response_with_location(conn, MHD_HTTP_ACCEPTED, task,
						location);
}

/*
 * A request that would send a command CXL r4.0 Table 8-308 marks O4 for
 * MCTP where fmd_mctp_restricted_allowed() does not let it go
 * (FMD_MCTP_RESTRICTED): 409, nothing sent. Note 4: "Systems capable of
 * management from Mailbox registers and an MCTP-based CCI shall ensure
 * that these commands are not issued as MCTP messages while a device's
 * mailboxes are operational." Base ResourceInUse: a host may be using the
 * device (cxl-fmd's choice).
 */
static enum MHD_Result send_mctp_restricted(struct MHD_Connection *conn)
{
	return send_error(conn, MHD_HTTP_CONFLICT,
			  "Refused: this would send a command that CXL r4.0 Table 8-308 "
			  "note 4 does not let go as an MCTP message while the device's "
			  "mailboxes are operational, which cxl-fmd cannot tell. Nothing "
			  "was sent. Once no host drives the device, the operator can "
			  "start cxl-fmd with --allow-mctp-restricted",
			  fmd_redfish_message(FMD_RF_RESOURCE_IN_USE,
				"Once no host drives the device, start cxl-fmd with "
				"--allow-mctp-restricted and resubmit the request.",
				NULL, 0));
}

/*
 * The answer to a security action (fmd_redfish_ld_security_action() and
 * the Memory aliases), @action at @url: 200 with no body when it was
 * carried out; 404 if there is no such resource or action; 400 for a
 * request that is not right, or that the device answered Invalid Input,
 * Incorrect Passphrase or Invalid Payload Length; 409 for Invalid Security
 * State, or a command that may not be sent (send_mctp_restricted()); 501
 * if the device's CEL does not list the command, the device answered
 * Unsupported, or cxl-fmd has no route to the LD; 503 for Retry Required
 * or Busy; else 500. cxl-fmd's choices.
 */
static enum MHD_Result send_security_result(struct MHD_Connection *conn, int rc,
					    const char *msg, const char *url,
					    const char *action,
					    const char *body, size_t body_size)
{
	char buf[256];
	unsigned int status;
	const char *what;
	json_t *info;

	switch (rc) {
	case 0:
		return send_json_response(conn, MHD_HTTP_OK, NULL);
	case FMD_MCTP_RESTRICTED:
		return send_mctp_restricted(conn);
	case -ENOENT:
		return send_not_found(conn, url, "Resource or action not found");
	case -EINVAL:
		status = MHD_HTTP_BAD_REQUEST;
		what = "The request is not valid";
		info = RF_GENERAL(FMD_RES_CORRECT);
		break;
	case -EBUSY:
		status = MHD_HTTP_CONFLICT;
		what = "The device's security state does not allow it";
		info = RF_GENERAL("Bring the device to a security state that "
				  "allows the action, and resubmit the request.");
		break;
	case -EAGAIN:
		status = MHD_HTTP_SERVICE_UNAVAILABLE;
		what = "The device cannot do it now; try again";
		info = fmd_redfish_message(FMD_RF_RESOURCE_IN_USE, FMD_RES_RETRY, NULL, 0);
		break;
	case -EOPNOTSUPP:
		status = MHD_HTTP_NOT_IMPLEMENTED;
		what = "The device does not support the command";
		info = fmd_redfish_message(FMD_RF_ACTION_NOT_SUPPORTED, NULL, NULL, 1,
					   action);
		break;
	default:
		status = MHD_HTTP_INTERNAL_SERVER_ERROR;
		what = "The command failed";
		info = RF_GENERAL(FMD_RES_FAILED);
		break;
	}
	if (msg)
		snprintf(buf, sizeof(buf), "%s: %s", what, msg);
	else
		snprintf(buf, sizeof(buf), "%s", what);
	if (status == MHD_HTTP_BAD_REQUEST)
		return send_bad_body(conn, body, body_size, buf, info);
	return send_error(conn, status, buf, info);
}

/*
 * An error with the Allow header a 405 has to have (RFC 9110 Section
 * 15.5.6), and Base OperationNotAllowed: "This error message should be used
 * together with HTTP status code 405 Method Not Allowed" (Base.1.24.0.json)
 */
static enum MHD_Result send_error_allow(struct MHD_Connection *conn,
					unsigned int status, const char *message,
					const char *allow)
{
	return send_error_body(conn, status,
			       fmd_redfish_error_body(message,
					RF_MSG(FMD_RF_OPERATION_NOT_ALLOWED)),
			       allow);
}

/*
 * The Link header of a resource whose @odata.type is @type: "A Link
 * header containing rel=describedby shall be returned on GET and HEAD
 * requests for Redfish resources. If the referenced JSON Schema is a
 * versioned schema, it shall match the version contained in the value of
 * the @odata.type property returned in this resource" (DSP0266 1.25.0
 * Section 8.2). It is DMTF's published JSON Schema of that very version
 * ("#Chassis.v1_23_0.Chassis" gives Chassis.v1_23_0.json), or the
 * unversioned one of a collection, at the URI that schema gives as its
 * $id (http://redfish.dmtf.org/schemas/v1/...): cxl-fmd serves no JSON
 * Schema of its own (cxl-fmd's choice). Empty for a body that is not a
 * resource (no @odata.type: /redfish, the OData service document), and
 * for a type of cxl-fmd's Oem schema, which has no JSON Schema.
 */
static void describedby_header(const char *type, char *buf, size_t len)
{
	const char *dot;

	buf[0] = '\0';
	if (!type || type[0] != '#' || !strncmp(type, "#Cxl.", 5))
		return;
	type++;
	dot = strrchr(type, '.');
	if (!dot || dot == type)
		return;
	snprintf(buf, len, "<http://redfish.dmtf.org/schemas/v1/%.*s.json>; rel=describedby",
		 (int)(dot - type), type);
}

/* describedby_header() of the JSON text @data, @n bytes */
static void describedby_of_text(const char *data, size_t n, char *buf, size_t len)
{
	json_t *root = json_loadb(data, n, 0, NULL);

	describedby_header(json_string_value(json_object_get(root, "@odata.type")),
			   buf, len);
	json_decref(root);
}

static bool route_exists(struct fmd_ctx *ctx, const char *url);
static void allow_header(struct fmd_ctx *ctx, const char *url, char *buf, size_t len);

/* A bind or unbind that failed with @err: as its task monitor answers */
/*
 * A chunk POST or DELETE that a switch that can expose the chunk's LD or
 * SLD was read for just before (binding_coverage() in handlers.c), and
 * nothing was sent: a VCS could not be read (503), or the switch, or the
 * heads of the Multi-Headed device, cannot be seen whole (409).
 */
static enum MHD_Result send_chunk_unread(struct MHD_Connection *conn)
{
	return send_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE,
			  "A Virtual CXL Switch that could bind the logical device could not be read: nothing was sent; try again",
			  fmd_redfish_message(FMD_RF_RESOURCE_IN_USE, FMD_RES_UNREAD,
					      NULL, 0));
}

static enum MHD_Result send_chunk_incomplete(struct MHD_Connection *conn)
{
	return send_error(conn, MHD_HTTP_CONFLICT,
			  "Whether a binding exposes the logical device cannot be seen: a switch that could bind it is not read whole (more than 8 Virtual CXL Switches, or more than 32 vPPBs in one), or which head of the Multi-Headed device a binding reaches is not known; nothing was sent",
			  RF_GENERAL("This service does not change memory a binding "
				     "it cannot see may expose: manage this device "
				     "otherwise."));
}

static enum MHD_Result send_vppb_failure(struct MHD_Connection *conn,
					 enum fmd_task_kind kind, int err)
{
	json_t *body;
	unsigned int status = fmd_redfish_vppb_failure(kind, err, &body);

	return send_error_body(conn, status, body, NULL);
}

/* The action an action URI names: its last segment */
static const char *action_of(const char *url)
{
	const char *slash = strrchr(url, '/');

	return slash ? slash + 1 : url;
}

/*
 * Map a negative errno from a DC handler to an HTTP error response, for
 * @url (@action, if the request is one)
 */
static enum MHD_Result send_errno(struct fmd_ctx *ctx,
				  struct MHD_Connection *conn, int rc,
				  const char *msg, const char *url,
				  const char *action,
				  const char *body, size_t body_size)
{
	char target[512];
	char *actions;

	if (!msg)
		msg = "Request failed";
	switch (-rc) {
	case ENOENT:
		/* the resource of the URI, or of the action, or one the body names */
		snprintf(target, sizeof(target), "%s", url);
		actions = strstr(target, "/Actions/");
		if (actions)
			*actions = '\0';
		if (!route_exists(ctx, target))
			return send_not_found(conn, url, msg);
		return send_error(conn, MHD_HTTP_NOT_FOUND, msg,
				  RF_GENERAL("Name resources that exist in the request "
					     "body, and resubmit it."));
	case EINVAL:
		return send_bad_body(conn, body, body_size, msg,
				     RF_GENERAL(FMD_RES_CORRECT));
	case EBUSY:
	case EEXIST:
		return send_error(conn, MHD_HTTP_CONFLICT, msg,
				  RF_MSG(FMD_RF_RESOURCE_IN_USE));
	case EALREADY:	/* a DC task follows the host and Tag: nothing was sent */
		return send_error(conn, MHD_HTTP_CONFLICT, msg,
				  fmd_redfish_message(FMD_RF_RESOURCE_IN_USE,
					"Wait for the Dynamic Capacity task that follows "
					"this host and Tag to end, then resubmit the "
					"request if it still applies.", NULL, 0));
	case ENOSPC:
		snprintf(target, sizeof(target), "%s", url);
		actions = strstr(target, "/Actions/");
		if (actions)
			*actions = '\0';
		return send_error(conn, MHD_HTTP_CONFLICT, msg,
				  fmd_redfish_message(FMD_RF_RESOURCE_EXHAUSTION,
						      NULL, NULL, 1, target));
	case EAGAIN:	/* the device said Retry Required or Busy */
		return send_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE, msg,
				  fmd_redfish_message(FMD_RF_RESOURCE_IN_USE,
						      FMD_RES_RETRY, NULL, 0));
	case EREMOTEIO:	/* whether the device carried it out is not known */
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, msg,
				  RF_GENERAL(FMD_RES_OUTCOME_UNKNOWN));
	case EOPNOTSUPP:	/* no route to the CCI: nothing was sent */
		return send_error(conn, MHD_HTTP_NOT_IMPLEMENTED, msg, action ?
				  fmd_redfish_message(FMD_RF_ACTION_NOT_SUPPORTED,
						      NULL, NULL, 1, action) :
				  RF_GENERAL(FMD_RES_UNSUPPORTED));
	default:
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, msg,
				  RF_GENERAL(FMD_RES_FAILED));
	}
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

	add_common_headers(response);

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
	add_common_headers(response);
	if (location)
		MHD_add_response_header(response, "Location", location);

	ret = MHD_queue_response(conn, status, response);
	MHD_destroy_response(response);

	return ret;
}

/*
 * Send @len bytes of @data, which stay valid (a file compiled in), as
 * @type, with @headers (add_headers())
 */
static enum MHD_Result send_static_response(struct MHD_Connection *conn,
					    unsigned int status,
					    const char *data, size_t len,
					    const char *type,
					    const char *const *headers)
{
	struct MHD_Response *response;
	enum MHD_Result ret;

	response = MHD_create_response_from_buffer(len, (void *)data,
						   MHD_RESPMEM_PERSISTENT);
	if (!response)
		return MHD_NO;

	MHD_add_response_header(response, "Content-Type", type);
	add_common_headers(response);
	add_headers(response, headers);

	ret = MHD_queue_response(conn, status, response);
	MHD_destroy_response(response);

	return ret;
}

/*
 * Send XML response (for $metadata), with @headers (add_headers())
 */
static enum MHD_Result send_xml_response(struct MHD_Connection *conn,
					 unsigned int status,
					 const char *xml,
					 const char *const *headers)
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
	add_common_headers(response);
	add_headers(response, headers);

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
/*
 * GET @url. With @exists, nothing is sent: *@exists is whether @url is a
 * resource this service serves, for route_exists(). The builders called
 * here only read what cxl-fmd holds: none sends a command.
 */
static enum MHD_Result route_get(struct fmd_ctx *ctx,
				 struct MHD_Connection *conn,
				 const char *url, bool *exists)
{
	const char *params[8] = {0};
	json_t *response = NULL;
	char *norm_url;
	/*
	 * A 200 has an Allow header: it "Shall be returned with the HTTP 200
	 * (OK) status code with any GET or HEAD operation to indicate the
	 * other allowable operations for this resource" (DSP0266 1.25.0
	 * Table 13), as a 405 of the resource has it (allow_header())
	 */
	char allow[64];
	char link[256] = "";
	const char *const headers[] = { "Allow", allow, "Link", link, NULL };

	norm_url = normalize_url(url);
	if (!norm_url) {
		if (exists) {
			*exists = false;
			return MHD_YES;
		}
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Memory allocation failed",
				  RF_MSG(FMD_RF_INTERNAL_ERROR));
	}

	if (!exists)
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
		char *metadata;

		if (exists) {
			free(norm_url);
			*exists = true;
			return MHD_YES;
		}
		metadata = fmd_redfish_metadata(ctx);
		free(norm_url);
		if (!metadata)
			return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "Failed to generate metadata",
					  RF_MSG(FMD_RF_INTERNAL_ERROR));
		allow_header(ctx, url, allow, sizeof(allow));
		enum MHD_Result ret = send_xml_response(conn, MHD_HTTP_OK, metadata,
							headers);
		free(metadata);
		return ret;
	}

	if (match_route("/redfish/v1/Schemas/{file}", norm_url, params, 8)) {
		size_t len;
		const char *data = fmd_redfish_schema_file(params[0], &len);

		free_params(params, 8);
		if (!data)
			goto not_found;
		free(norm_url);
		if (exists) {
			*exists = true;
			return MHD_YES;
		}
		allow_header(ctx, url, allow, sizeof(allow));
		return send_static_response(conn, MHD_HTTP_OK, data, len,
					    "application/xml; charset=utf-8", headers);
	}

	if (strcmp(norm_url, "/redfish/v1/Registries") == 0) {
		response = fmd_redfish_registries(ctx);
		goto ok;
	}

	if (match_route("/redfish/v1/Registries/{file}", norm_url, params, 8)) {
		size_t len;
		const char *data = fmd_redfish_registry_file(params[0], &len);

		if (data) {
			free_params(params, 8);
			free(norm_url);
			if (exists) {
				*exists = true;
				return MHD_YES;
			}
			allow_header(ctx, url, allow, sizeof(allow));
			describedby_of_text(data, len, link, sizeof(link));
			return send_static_response(conn, MHD_HTTP_OK, data, len,
						    "application/json; charset=utf-8",
						    headers);
		}
		response = fmd_redfish_registry(ctx, params[0]);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
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

	/* Tasks: binds and unbinds a switch carries out in the background */
	if (strcmp(norm_url, "/redfish/v1/TaskService") == 0) {
		response = fmd_redfish_task_service(ctx);
		if (response)
			goto ok;
		goto not_found;
	}

	if (strcmp(norm_url, "/redfish/v1/TaskService/Tasks") == 0) {
		response = fmd_redfish_tasks(ctx);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/TaskService/Tasks/{task}", norm_url, params, 8)) {
		response = fmd_redfish_task(ctx, atoi(params[0]));
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	/*
	 * The task monitor: 202 and the task while the operation runs, then
	 * what the request that started it would have been answered with.
	 */
	if (match_route("/redfish/v1/TaskService/TaskMonitors/{task}", norm_url,
			params, 8)) {
		char location[256];
		int status = fmd_redfish_task_monitor(ctx, atoi(params[0]), &response,
						      location, sizeof(location));

		free_params(params, 8);
		free(norm_url);
		if (exists) {
			json_decref(response);
			*exists = status != 404;
			return MHD_YES;
		}
		if (status == 404)
			return send_not_found(conn, url, "Task not found");
		if (status == 204)
			return send_no_content(conn);
		if (status >= 400)
			return send_error_body(conn, status, response, NULL);
		if (location[0])
			return send_json_response_with_location(conn, status,
								response, location);
		if (status == MHD_HTTP_OK) {
			allow_header(ctx, url, allow, sizeof(allow));
			describedby_header(json_string_value(json_object_get(response,
							"@odata.type")), link, sizeof(link));
			return send_json_headers(conn, status, response, headers);
		}
		return send_json_response(conn, status, response);
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

	/*
	 * CXL event records: the log services of a switch's Chassis, and the
	 * DeviceLog of each CXL logical device (src/event_log.c)
	 */
	if (match_route("/redfish/v1/Chassis/{id}/LogServices", norm_url, params, 8)) {
		response = fmd_redfish_switch_log(ctx, params[0], NULL, "services", 0);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/LogServices/{log}", norm_url, params, 8)) {
		response = fmd_redfish_switch_log(ctx, params[0], params[1], "service", 0);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/LogServices/{log}/Entries",
			norm_url, params, 8)) {
		response = fmd_redfish_switch_log(ctx, params[0], params[1], "entries", 0);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/LogServices/{log}/Entries/{entry}",
			norm_url, params, 8)) {
		response = fmd_redfish_switch_log(ctx, params[0], params[1], "entry",
						  atoi(params[2]));
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/DeviceLog",
			norm_url, params, 8)) {
		response = fmd_redfish_ld_log(ctx, params[0], params[1], params[2],
					      "service", 0);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/DeviceLog/Entries",
			norm_url, params, 8)) {
		response = fmd_redfish_ld_log(ctx, params[0], params[1], params[2],
					      "entries", 0);
		free_params(params, 8);
		if (response)
			goto ok;
		goto not_found;
	}

	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/DeviceLog/Entries/{entry}",
			norm_url, params, 8)) {
		response = fmd_redfish_ld_log(ctx, params[0], params[1], params[2],
					      "entry", atoi(params[3]));
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
	if (exists) {
		*exists = false;
		return MHD_YES;
	}
	return send_not_found(conn, url, "Resource not found");

ok:
	free(norm_url);
	if (exists) {
		*exists = response != NULL;
		json_decref(response);
		return MHD_YES;
	}
	allow_header(ctx, url, allow, sizeof(allow));
	describedby_header(json_string_value(json_object_get(response, "@odata.type")),
			   link, sizeof(link));
	return send_json_headers(conn, MHD_HTTP_OK, response, headers);
}

static enum MHD_Result handle_request_get(struct fmd_ctx *ctx,
					  struct MHD_Connection *conn,
					  const char *url)
{
	return route_get(ctx, conn, url, NULL);
}

/* Whether @url is a resource a GET of answers 200 (or 202, a task monitor) */
static bool route_exists(struct fmd_ctx *ctx, const char *url)
{
	bool exists = false;

	route_get(ctx, NULL, url, &exists);
	return exists;
}

/*
 * The routes of resources that take more than GET, as the handlers below
 * serve them. A direct-attached Connection takes no DELETE (rc -4 there),
 * so allow_header() leaves it out for one.
 */
static const char *const post_routes[] = {
	"/redfish/v1/Fabrics/{id}/Connections",
	"/redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks",
};

static const char *const patch_routes[] = {
	"/redfish/v1/Chassis/{id}/PCIeDevices/{dev}",
	"/redfish/v1/Chassis/{id}/Memory/{mem}",
	"/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}",
};

static const char *const delete_routes[] = {
	"/redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}",
	"/redfish/v1/Fabrics/{id}/Connections/{conn}",
	"/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}",
};

#define ROUTES(a) (sizeof(a) / sizeof((a)[0]))

static bool matches_any(const char *const *routes, size_t n, const char *url)
{
	const char *params[8] = {0};

	for (size_t i = 0; i < n; i++) {
		if (match_route(routes[i], url, params, 8)) {
			free_params(params, 8);
			return true;
		}
	}
	return false;
}

/* A switch-attached Connection: one a DELETE unbinds */
static bool deletable(struct fmd_ctx *ctx, const char *url)
{
	const char *params[8] = {0};
	json_t *conn;
	bool ok;

	if (!match_route("/redfish/v1/Fabrics/{id}/Connections/{conn}", url, params, 8))
		return true;
	conn = fmd_redfish_connection(ctx, params[0], params[1]);
	free_params(params, 8);
	/* only a vPPB binding has ForceUnbind (conn_json()) */
	ok = conn && json_object_get(conn, "Actions");
	json_decref(conn);
	return ok;
}

/*
 * The Allow header of the resource at @url: the methods it takes, which a
 * 405 has to list ("The response shall include an Allow header that
 * provides a list of methods that the resource identified by the URI in
 * the client request supports", DSP0266 1.25.0, Table 13), and a 200 to a
 * GET too (route_get()).
 */
static void allow_header(struct fmd_ctx *ctx, const char *url, char *buf, size_t len)
{
	char *norm_url = normalize_url(url);
	const char *u = norm_url ? norm_url : url;

	snprintf(buf, len, "GET%s%s%s",
		 matches_any(post_routes, ROUTES(post_routes), u) ? ", POST" : "",
		 matches_any(patch_routes, ROUTES(patch_routes), u) ? ", PATCH" : "",
		 matches_any(delete_routes, ROUTES(delete_routes), u) &&
		 deletable(ctx, u) ? ", DELETE" : "");
	free(norm_url);
}

/*
 * The answer to @method on @url when no handler takes it: 405 with an
 * Allow header if @url is a resource ("If the resource exists but does not
 * support the requested operation, services shall return the HTTP 405
 * Method Not Allowed status code", DSP0266 1.25.0 Section 7.5.3), else 404.
 */
static enum MHD_Result send_method_not_allowed(struct fmd_ctx *ctx,
					       struct MHD_Connection *conn,
					       const char *method, const char *url)
{
	char allow[64], msg[96];

	if (!route_exists(ctx, url))
		return send_not_found(conn, url, "Resource not found");
	allow_header(ctx, url, allow, sizeof(allow));
	snprintf(msg, sizeof(msg), "%.16s is not supported for this resource", method);
	return send_error_allow(conn, MHD_HTTP_METHOD_NOT_ALLOWED, msg, allow);
}

/*
 * Whether @method on @url is one a handler takes, of a resource that is
 * there: what would be answered 2xx were it not for a precondition, and
 * not 404 or 405. An action is taken to be there when its resource is.
 */
static bool takes_method(struct fmd_ctx *ctx, const char *method,
			 const char *url)
{
	char *norm_url = normalize_url(url);
	char *actions;
	bool takes = false;

	if (!norm_url)
		return false;
	if (!strcmp(method, "POST")) {
		actions = strstr(norm_url, "/Actions/");
		if (actions) {
			*actions = '\0';
			takes = route_exists(ctx, norm_url);
		} else {
			takes = matches_any(post_routes, ROUTES(post_routes), norm_url) &&
				route_exists(ctx, norm_url);
		}
	} else if (!strcmp(method, "PATCH")) {
		takes = matches_any(patch_routes, ROUTES(patch_routes), norm_url) &&
			route_exists(ctx, norm_url);
	} else if (!strcmp(method, "DELETE")) {
		takes = matches_any(delete_routes, ROUTES(delete_routes), norm_url) &&
			route_exists(ctx, norm_url) && deletable(ctx, norm_url);
	}
	free(norm_url);
	return takes;
}

/*
 * A request that changes something, with If-Match or If-None-Match:
 * cxl-fmd returns no ETag, so it cannot compare the one asked for with
 * the resource's, and would otherwise carry the request out as if it had
 * none. It is answered 412, nothing done: "412 Precondition Failed:
 * Precondition check, such as check of the OData-Version, If-Match, or
 * If-None-Match header, failed." (DSP0266 1.25.0 Table 14). DSP0266 asks
 * If-Match only of services that return ETags ("PUT and PATCH requests for
 * resources for which a service returns ETags shall support If-Match",
 * Table 6), so a 412 for any precondition, whatever its value, is
 * cxl-fmd's choice: a client that asks for one never gets a write it did
 * not condition. A request answered 404 or 405 without the header is
 * answered so with it ("A server MUST ignore all received preconditions if
 * its response to the same request without those conditions, prior to
 * processing the request content, would have been a status code other
 * than a 2xx (Successful) or 412 (Precondition Failed)", RFC 9110 Section
 * 13.2.1); an action is judged by its resource only, so an action a
 * resource that is there does not have is 412 with the header, and 404
 * without (cxl-fmd's simplification). A GET is not checked: a conditional GET is "If supported by the
 * service" (DSP0266 Section 7.2.1), and cxl-fmd answers it as one without.
 * Base PreconditionFailed is not used: "The use of this message shall
 * only be used in responses for deep operations, since HTTP status code
 * 412 is typically used for this" (Base.1.24.0.json). Returns true, the
 * answer sent and *@result what sending it gave, if the request is
 * answered so.
 */
static bool send_precondition_failed(struct fmd_ctx *ctx,
				     struct MHD_Connection *conn,
				     const char *method, const char *url,
				     enum MHD_Result *result)
{
	if (!MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "If-Match") &&
	    !MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "If-None-Match"))
		return false;
	if (!takes_method(ctx, method, url))
		return false;
	*result = send_error(conn, MHD_HTTP_PRECONDITION_FAILED,
			     "If-Match and If-None-Match are not supported: cxl-fmd "
			     "returns no ETags, and cannot compare one. Nothing was done",
			     RF_GENERAL("Resubmit the request without If-Match or "
					"If-None-Match: cxl-fmd returns no ETags."));
	return true;
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
				  "Memory allocation failed",
				  RF_MSG(FMD_RF_INTERNAL_ERROR));

	/*
	 * LogService.ClearLog: the records cxl-fmd keeps. The component's
	 * own logs were cleared as they were read.
	 */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/DeviceLog/Actions/LogService.ClearLog",
			norm_url, params, 8)) {
		rc = fmd_redfish_ld_log_clear(ctx, params[0], params[1], params[2]);
		free_params(params, 8);
		free(norm_url);
		if (rc)
			return send_not_found(conn, url, "Log service not found");
		return send_no_content(conn);
	}

	if (match_route("/redfish/v1/Chassis/{id}/LogServices/{log}/Actions/LogService.ClearLog",
			norm_url, params, 8)) {
		rc = fmd_redfish_switch_log_clear(ctx, params[0], params[1]);
		free_params(params, 8);
		free(norm_url);
		if (rc)
			return send_not_found(conn, url, "Log service not found");
		return send_no_content(conn);
	}

	/* POST /redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.ScanMedia */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/Memory.ScanMedia",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_scan_media(ctx, params[0], params[1],
						   body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		if (rc == FMD_MCTP_RESTRICTED)
			return send_mctp_restricted(conn);
		return send_bad_body(conn, body, body_size, "Action failed",
				     RF_GENERAL(FMD_RES_CORRECT));
	}

	/*
	 * POST .../Memory/{mem}/Actions/Memory.{action}: the deprecated
	 * aliases of the CXLLogicalDevice security actions
	 */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}/Actions/{action}",
			norm_url, params, 8)) {
		static const char prefix[] = "Memory.";
		const char *action = params[2];
		const char *msg = NULL;

		rc = -ENOENT;
		if (action && !strncmp(action, prefix, strlen(prefix))) {
			action += strlen(prefix);
			if (!strcmp(action, "SetPassphrase"))
				rc = fmd_redfish_action_set_passphrase(ctx, params[0], params[1],
								       body, body_size, &msg);
			else if (!strcmp(action, "SetMasterPassphrase"))
				rc = fmd_redfish_action_set_master_passphrase(ctx, params[0],
						params[1], body, body_size, &msg);
			else if (!strcmp(action, "DisablePassphrase"))
				rc = fmd_redfish_action_disable_passphrase(ctx, params[0],
						params[1], body, body_size, &msg);
			else if (!strcmp(action, "DisableMasterPassphrase"))
				rc = fmd_redfish_action_disable_master_passphrase(ctx, params[0],
						params[1], body, body_size, &msg);
			else if (!strcmp(action, "FreezeSecurityState"))
				rc = fmd_redfish_action_freeze_security_state(ctx, params[0],
						params[1], &msg);
			else if (!strcmp(action, "SecureEraseUnit"))
				rc = fmd_redfish_action_secure_erase_unit(ctx, params[0],
						params[1], body, body_size, &msg);
		}
		free_params(params, 8);
		free(norm_url);
		return send_security_result(conn, rc, msg, url, action_of(url),
					    body, body_size);
	}

	/* POST /redfish/v1/Fabrics/{fab}/Switches/{sw}/Ports/{port}/Actions/Port.ResetPPB */
	if (match_route("/redfish/v1/Fabrics/{fab}/Switches/{sw}/Ports/{port}/Actions/Port.ResetPPB",
			norm_url, params, 8)) {
		rc = fmd_redfish_action_reset_ppb(ctx, params[1], params[2]);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		if (rc == -2)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "Reset PPB is offered only on a downstream port",
					  fmd_redfish_message(FMD_RF_ACTION_NOT_SUPPORTED,
							      NULL, NULL, 1, action_of(url)));
		return send_error(conn, MHD_HTTP_BAD_REQUEST, "Action failed",
				  RF_GENERAL(FMD_RES_FAILED));
	}

	/* POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityAdd",
			norm_url, params, 8)) {
		const char *msg = NULL;
		json_t *task = NULL;

		rc = fmd_redfish_action_dc_add(ctx, params[0], params[1], params[2],
					       body, body_size, &msg, &task);
		free_params(params, 8);
		free(norm_url);

		if (rc == FMD_OP_STARTED)
			return send_dc_started(conn, task);
		return send_errno(ctx, conn, rc, msg, url, action_of(url),
				  body, body_size);
	}

	/* POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.DynamicCapacityRelease",
			norm_url, params, 8)) {
		const char *msg = NULL;
		json_t *task = NULL;

		rc = fmd_redfish_action_dc_release(ctx, params[0], params[1], params[2],
						   body, body_size, &msg, &task);
		free_params(params, 8);
		free(norm_url);

		if (rc == FMD_OP_STARTED)
			return send_dc_started(conn, task);
		return send_errno(ctx, conn, rc, msg, url, action_of(url),
				  body, body_size);
	}

	/*
	 * POST .../CXLLogicalDevices/{ld}/Actions/CXLLogicalDevice.{action}:
	 * the security actions (DSP0288 1.4.0 Table 1). The DC actions are
	 * matched above.
	 */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/Actions/{action}",
			norm_url, params, 8)) {
		static const char prefix[] = "CXLLogicalDevice.";
		const char *msg = NULL;

		rc = -ENOENT;
		if (params[3] && !strncmp(params[3], prefix, strlen(prefix)))
			rc = fmd_redfish_ld_security_action(ctx, params[0], params[1],
							    params[2], params[3] + strlen(prefix),
							    body, body_size, &msg);
		free_params(params, 8);
		free(norm_url);
		return send_security_result(conn, rc, msg, url, action_of(url),
					    body, body_size);
	}

	/* POST .../PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/Actions/PCIeDevice.CXLReferenceDCMemory",
			norm_url, params, 8)) {
		const char *msg = NULL;

		rc = fmd_redfish_action_dc_reference(ctx, params[0], params[1],
						     body, body_size, &msg);
		free_params(params, 8);
		free(norm_url);

		if (rc == FMD_DC_ACCEPTED_STALE)
			return send_dc_accepted_stale(conn, msg);
		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_errno(ctx, conn, rc, msg, url, action_of(url),
				  body, body_size);
	}

	/* POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks - create chunk */
	if (match_route("/redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks",
			norm_url, params, 8)) {
		json_t *response = NULL;
		char location[512];

		rc = fmd_redfish_post_memory_chunk(ctx, params[0], params[1],
						   body, body_size, &response);

		/* made now (201), or there already (200): where it is */
		if ((rc == 0 || rc == FMD_CONN_EXISTS) && response) {
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
			return send_not_found(conn, url, "Chassis or MemoryDomain not found");
		if (rc == FMD_MCTP_RESTRICTED)
			return send_mctp_restricted(conn);
		if (rc == -2)
			return send_bad_body(conn, body, body_size,
					     "Invalid request body - MemoryChunkSizeMiB required",
					     fmd_redfish_message(FMD_RF_PROPERTY_MISSING, NULL,
								 "/MemoryChunkSizeMiB", 1,
								 "MemoryChunkSizeMiB"));
		if (rc == -3) {
			/* the domain: the collection's parent */
			char domain[512];
			char *slash;

			snprintf(domain, sizeof(domain), "%s", url);
			slash = strstr(domain, "/MemoryChunks");
			if (slash)
				*slash = '\0';
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "Insufficient capacity in memory domain",
					  fmd_redfish_message(FMD_RF_RESOURCE_EXHAUSTION,
							      NULL, NULL, 1, domain));
		}
		if (rc == -4)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "Address range overlaps with existing chunk, or no free range of that size",
					  RF_GENERAL("Choose an AddressRangeOffsetMiB and a size "
						     "that no chunk of the domain overlaps, and "
						     "resubmit the request."));
		if (rc == -6)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The device cannot be given that size",
					  RF_PROPERTY("MemoryChunkSizeMiB"));
		if (rc == -8)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "AddressRangeType must be PMEM or Volatile",
					  RF_PROPERTY("AddressRangeType"));
		if (rc == -9)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "AddressRangeOffsetMiB must be an integer from 0 to the domain's MemorySizeMiB less the chunk's size",
					  RF_PROPERTY("AddressRangeOffsetMiB"));
		if (rc == -10)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "A size or offset in MiB must be an integer from 0 to 17592186044415, and MemoryChunkSizeMiB greater than 0",
					  RF_GENERAL(FMD_RES_CORRECT));
		if (rc == -7)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "A binding exposes the logical device: a Connection, or a vPPB the switch reads bound to it, or Busy; nothing was sent",
					  fmd_redfish_message(FMD_RF_RESOURCE_IN_USE,
						"Delete the Connection that binds the logical "
						"device, or wait for the switch's vPPB to be "
						"unbound or no longer Busy, and resubmit the "
						"request.", NULL, 0));
		if (rc == -11)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "Name exactly one LD of the device: InterleaveSets[0].Memory as the URI of its Memory, or Links.CXLLogicalDevices[0] as the URI of its CXLLogicalDevice",
					  RF_GENERAL(FMD_RES_CORRECT));
		if (rc == -12)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "An InterleaveSets entry sets no OffsetMiB, RegionId or MemoryLevel, and its SizeMiB is MemoryChunkSizeMiB",
					  RF_PROPERTY("InterleaveSets"));
		if (rc == -13)
			return send_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
					  "No LD Pool CCI of the Multi-Headed device is known: its allocations cannot be changed",
					  RF_GENERAL(FMD_RES_UNSUPPORTED));
		if (rc == -15)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "Name the AddressRangeType: the memory of the device is not all of one type, or its type is not known",
					  fmd_redfish_message(FMD_RF_PROPERTY_MISSING, NULL,
							      "/AddressRangeType", 1,
							      "AddressRangeType"));
		if (rc == -16)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The device or LD has no memory of that AddressRangeType, and cannot be partitioned to have it",
					  RF_PROPERTY("AddressRangeType"));
		if (rc == -14)
			return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "Outcome unknown: Set LD Allocations may or may not have been carried out, and the allocation could not be read back. The chunk is kept, and no chunk of the device is changed until the allocation is read: check the logical device before sending this again",
					  RF_GENERAL(FMD_RES_OUTCOME_UNKNOWN));
		if (rc == -17)
			return send_chunk_unread(conn);
		if (rc == -18)
			return send_chunk_incomplete(conn);
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Failed to create memory chunk", RF_GENERAL(FMD_RES_FAILED));
	}

	/*
	 * POST .../Connections/{conn}/Actions/Oem/Cxl.ForceUnbind: unbind
	 * with Simulate Surprise Hot-Remove (cxl-fmd's Oem action). The body,
	 * if any, is not read: the action has no parameters.
	 */
	if (match_route("/redfish/v1/Fabrics/{id}/Connections/{conn}/Actions/Oem/Cxl.ForceUnbind",
			norm_url, params, 8)) {
		json_t *task = NULL;

		rc = fmd_redfish_action_force_unbind(ctx, params[0], params[1], &task);
		free_params(params, 8);
		free(norm_url);

		if (rc == FMD_OP_STARTED && task) {
			char location[256];

			snprintf(location, sizeof(location), "%s",
				 json_string_value(json_object_get(task, "TaskMonitor")));
			return send_json_response_with_location(conn, MHD_HTTP_ACCEPTED,
								task, location);
		}
		json_decref(task);
		if (rc == 0)
			return send_no_content(conn);
		if (rc == -1)
			return send_not_found(conn, url, "Connection not found");
		if (rc == -3)
			return send_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE,
					  "The switch is carrying out another operation; try again",
					  fmd_redfish_message(FMD_RF_RESOURCE_IN_USE,
							      FMD_RES_RETRY, NULL, 0));
		if (rc == -7)
			return send_vppb_failure(conn, FMD_TASK_UNBIND, -ESTALE);
		if (rc == -4)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "ForceUnbind is offered only on a switch-attached connection",
					  fmd_redfish_message(FMD_RF_ACTION_NOT_SUPPORTED,
							      NULL, NULL, 1, action_of(url)));
		if (rc == -5)
			return send_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
					  "The switch does not support Unbind vPPB (Unsupported)",
					  fmd_redfish_message(FMD_RF_ACTION_NOT_SUPPORTED,
							      NULL, NULL, 1, action_of(url)));
		if (rc == -6)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The switch refused the unbind as Invalid Input",
					  RF_GENERAL(FMD_RES_REFUSED));
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Failed to unbind the connection", RF_GENERAL(FMD_RES_FAILED));
	}

	/* POST /redfish/v1/Fabrics/{id}/Connections - create connection */
	if (match_route("/redfish/v1/Fabrics/{id}/Connections", norm_url, params, 8)) {
		json_t *response = NULL;
		char location[512];

		rc = fmd_redfish_post_connection(ctx, params[0], body, body_size, &response);

		/*
		 * The switch is binding in the background: 202 Accepted, the
		 * task, and where to ask how it went.
		 */
		if (rc == FMD_OP_STARTED && response) {
			snprintf(location, sizeof(location), "%s",
				 json_string_value(json_object_get(response, "TaskMonitor")));
			free_params(params, 8);
			free(norm_url);
			return send_json_response_with_location(conn, MHD_HTTP_ACCEPTED,
								response, location);
		}

		/* made now (201), or there already (200): where it is */
		if ((rc == 0 || rc == FMD_CONN_EXISTS) && response) {
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
			return send_json_response_with_location(conn,
				rc == 0 ? MHD_HTTP_CREATED : MHD_HTTP_OK, response, location);
		}
		json_decref(response);

		free_params(params, 8);
		free(norm_url);

		if (rc == -1)
			return send_not_found(conn, url, "Fabric not found");
		if (rc == -2)
			return send_bad_body(conn, body, body_size, "Invalid request body",
					     RF_GENERAL(FMD_RES_CORRECT));
		if (rc == -3)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Host system (initiator) not found",
					  RF_PROPERTY_AT("InitiatorEndpoints",
							 "/Links/InitiatorEndpoints"));
		if (rc == -4)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Target device not found",
					  RF_PROPERTY_AT("TargetEndpoints",
							 "/Links/TargetEndpoints"));
		if (rc == -5)
			return send_error(conn, MHD_HTTP_NOT_FOUND,
					  "Memory chunk not found",
					  RF_PROPERTY("MemoryChunkInfo"));
		if (rc == -8)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The host is not attached to that device or head: it cannot reach this memory",
					  RF_GENERAL(FMD_RES_CORRECT));
		if (rc == -11)
			return send_vppb_failure(conn, FMD_TASK_BIND, -EAGAIN);
		if (rc == -26)
			return send_vppb_failure(conn, FMD_TASK_BIND, -ESTALE);
		if (rc == -27)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "This service does not read the host's Virtual CXL Switch whole (more than 32 vPPBs, or past the 8 VCSs it reads): a binding past them would not be seen",
					  RF_GENERAL(FMD_RES_INCOMPLETE));
		if (rc == -28)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "What the logical device's head presents, an SLD or an MLD, is not known: the device's LD Map, or its heads' port numbers, could not be read whole (CXL r4.0 Section 2.5)",
					  RF_GENERAL(FMD_RES_HEAD_UNKNOWN));
		if (rc == -12)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "The host's Virtual CXL Switch has no unbound vPPB",
					  RF_GENERAL("Delete a Connection of the host, and "
						     "resubmit the request."));
		if (rc == -16)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "The host's connection to the target is being unbound",
					  RF_MSG(FMD_RF_RESOURCE_IN_USE));
		if (rc == -17)
			return send_vppb_failure(conn, FMD_TASK_BIND, -EOPNOTSUPP);
		if (rc == -18)
			return send_vppb_failure(conn, FMD_TASK_BIND, -EPROTO);
		if (rc == -19)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The memory chunks are on more than one logical device",
					  RF_PROPERTY("MemoryChunkInfo"));
		if (rc == -20)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "AccessCapabilities can only be Read and Write: a binding gives the whole of its target",
					  RF_PROPERTY_AT("AccessCapabilities",
							 "/MemoryChunkInfo"));
		if (rc == -24)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "VolumeInfo is not supported: a binding gives memory, and this service has no Volume",
					  RF_PROPERTY("VolumeInfo"));
		if (rc == -25)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "AccessState can only be Optimized: what a binding is once it is made",
					  RF_PROPERTY_AT("AccessState",
							 "/MemoryChunkInfo"));
		if (rc == -21)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "A link is not the URI of an Endpoint or a MemoryChunk as this service serves it",
					  RF_GENERAL(FMD_RES_CORRECT));
		if (rc == -22)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "InitiatorEndpointGroups and TargetEndpointGroups are not supported: name InitiatorEndpoints and TargetEndpoints",
					  RF_GENERAL(FMD_RES_CORRECT));
		if (rc == -23)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "The memory chunk is staged: its partition is active from the device's next reset",
					  fmd_redfish_message(FMD_RF_RESOURCE_IN_USE,
						"Resubmit the request once the device has been "
						"reset.", NULL, 0));
		if (rc == -13)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The memory chunk names no logical device that the device's port can bind",
					  RF_PROPERTY("MemoryChunkInfo"));
		if (rc == -14)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The head presents an MLD: which pool LD each of its LD-IDs is, is not known (CXL r4.0 Section 2.5.1)",
					  RF_GENERAL(FMD_RES_UNSUPPORTED));
		if (rc == -15)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The memory chunk is not on the target",
					  RF_PROPERTY("MemoryChunkInfo"));
		if (rc == -10)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The host and the device are not on the same switch",
					  RF_GENERAL(FMD_RES_CORRECT));
		if (rc == -9)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "The device or logical device is bound, or being bound, to another vPPB",
					  RF_MSG(FMD_RF_RESOURCE_IN_USE));
		return send_vppb_failure(conn, FMD_TASK_BIND, -EIO);
	}

	free(norm_url);
	return send_method_not_allowed(ctx, conn, "POST", url);
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

	/* no resource takes a PUT */
	return send_method_not_allowed(ctx, conn, "PUT", url);
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
				  "Memory allocation failed",
				  RF_MSG(FMD_RF_INTERNAL_ERROR));

	/*
	 * PATCH of a Connection: everything in it is what the switch says
	 * (cxl-fmd's choice, though Connection_v1 lets a service update it).
	 */
	if (match_route("/redfish/v1/Fabrics/{id}/Connections/{conn}", norm_url, params, 8)) {
		char allow[64];

		free_params(params, 8);
		if (!route_exists(ctx, norm_url)) {
			free(norm_url);
			return send_not_found(conn, url, "Resource not found");
		}
		allow_header(ctx, norm_url, allow, sizeof(allow));
		free(norm_url);
		return send_error_allow(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
					"A Connection is the switch's binding: POST and DELETE change it",
					allow);
	}

	/* PATCH /redfish/v1/Chassis/{id}/PCIeDevices/{dev} */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}", norm_url, params, 8)) {
		rc = fmd_redfish_patch_pcie_device(ctx, params[0], params[1], body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		if (rc == -1)
			return send_not_found(conn, url, "Resource not found");
		if (rc == -2)
			return send_bad_body(conn, body, body_size, "Invalid request body",
					     RF_PROPERTY_AT("Timestamp",
							    "/CXLDevice/Timestamp"));
		if (rc == -4)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The device cannot be reached: an SLD on a switch port",
					  RF_GENERAL(FMD_RES_UNSUPPORTED));
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "CCI command failed",
				  RF_GENERAL(FMD_RES_FAILED));
	}

	/* PATCH /redfish/v1/Chassis/{id}/Memory/{mem} */
	if (match_route("/redfish/v1/Chassis/{id}/Memory/{mem}", norm_url, params, 8)) {
		rc = fmd_redfish_patch_memory(ctx, params[0], params[1], body, body_size);
		free_params(params, 8);
		free(norm_url);

		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		if (rc == -1)
			return send_not_found(conn, url, "Resource not found");
		if (rc == FMD_MCTP_RESTRICTED)
			return send_mctp_restricted(conn);
		if (rc == -2)
			return send_bad_body(conn, body, body_size,
					     "Invalid request body, or a capacity the device cannot be partitioned to",
					     RF_GENERAL(FMD_RES_CORRECT));
		if (rc == -4)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The device cannot be reached: an SLD on a switch port",
					  RF_GENERAL(FMD_RES_UNSUPPORTED));
		if (rc == -5)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The device has no partitionable capacity (Partition Alignment is 0)",
					  RF_GENERAL(FMD_RES_UNSUPPORTED));
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "CCI command failed",
				  RF_GENERAL(FMD_RES_FAILED));
	}

	/* PATCH .../CXLLogicalDevices/{ld}/MemoryRegions/{region} - Set DC Region Config */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/CXLLogicalDevices/{ld}/MemoryRegions/{region}",
			norm_url, params, 8)) {
		const char *msg = NULL;

		rc = fmd_redfish_patch_dc_memory_region(ctx, params[0], params[1], params[2],
							params[3], body, body_size, &msg);
		free_params(params, 8);
		free(norm_url);

		if (rc == FMD_DC_ACCEPTED_STALE)
			return send_dc_accepted_stale(conn, msg);
		if (rc == 0)
			return send_json_response(conn, MHD_HTTP_OK, NULL);
		return send_errno(ctx, conn, rc, msg, url, NULL,
				  body, body_size);
	}

	free(norm_url);
	return send_method_not_allowed(ctx, conn, "PATCH", url);
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
				  "Memory allocation failed",
				  RF_MSG(FMD_RF_INTERNAL_ERROR));

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
			return send_not_found(conn, url, "Memory chunk not found");
		if (rc == FMD_MCTP_RESTRICTED)
			return send_mctp_restricted(conn);
		if (rc == -2)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "Chunk does not belong to specified domain",
					  fmd_redfish_message(FMD_RF_INVALID_URI, NULL,
							      NULL, 1, url));
		if (rc == -4)
			return send_error(conn, MHD_HTTP_BAD_REQUEST,
					  "The device cannot be given that partition",
					  RF_GENERAL(FMD_RES_UNSUPPORTED));
		if (rc == -5)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "A binding exposes the memory chunk: a Connection, or a vPPB the switch reads bound to its device, or Busy; nothing was sent",
					  fmd_redfish_message(FMD_RF_RESOURCE_IN_USE,
						"Delete the Connection that exposes the "
						"memory chunk, or wait for the switch's vPPB "
						"to be unbound or no longer Busy, and resubmit "
						"the request.", NULL, 0));
		if (rc == -6)
			return send_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
					  "cxl-fmd cannot give back the allocation of this memory chunk",
					  RF_MSG(FMD_RF_RESOURCE_CANNOT_BE_DELETED));
		if (rc == -7)
			return send_error(conn, MHD_HTTP_CONFLICT,
					  "The logical device has less memory allocated than the chunk",
					  RF_GENERAL(FMD_RES_FAILED));
		if (rc == -8)
			return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "Outcome unknown: Set LD Allocations may or may not have been carried out, and the allocation could not be read back. The chunk is kept, and no chunk of the device is changed until the allocation is read: check the logical device before sending this again",
					  RF_GENERAL(FMD_RES_OUTCOME_UNKNOWN));
		if (rc == -9)
			return send_chunk_unread(conn);
		if (rc == -10)
			return send_chunk_incomplete(conn);
		return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
				  "Failed to delete memory chunk", RF_GENERAL(FMD_RES_FAILED));
	}

	/* DELETE /redfish/v1/Fabrics/{id}/Connections/{conn} */
	if (match_route("/redfish/v1/Fabrics/{id}/Connections/{conn}",
			norm_url, params, 8)) {
		json_t *task = NULL;

		rc = fmd_redfish_delete_connection(ctx, params[0], params[1], &task);
		free_params(params, 8);
		free(norm_url);

		/* the switch is unbinding in the background */
		if (rc == FMD_OP_STARTED && task) {
			char location[256];

			snprintf(location, sizeof(location), "%s",
				 json_string_value(json_object_get(task, "TaskMonitor")));
			return send_json_response_with_location(conn, MHD_HTTP_ACCEPTED,
								task, location);
		}
		json_decref(task);
		if (rc == -3)
			return send_vppb_failure(conn, FMD_TASK_UNBIND, -EAGAIN);
		if (rc == -7)
			return send_vppb_failure(conn, FMD_TASK_UNBIND, -ESTALE);

		if (rc == 0)
			return send_no_content(conn);
		if (rc == -1)
			return send_not_found(conn, url, "Connection not found");
		if (rc == -4)
			return send_error_allow(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
						"A direct-attached connection has no vPPB to unbind",
						"GET");
		if (rc == -5)
			return send_vppb_failure(conn, FMD_TASK_UNBIND, -EOPNOTSUPP);
		if (rc == -6)
			return send_vppb_failure(conn, FMD_TASK_UNBIND, -EPROTO);
		return send_vppb_failure(conn, FMD_TASK_UNBIND, -EIO);
	}

	/* DELETE .../PCIeDevices/{dev}/MemoryExtents/{ext} - release the extent */
	if (match_route("/redfish/v1/Chassis/{id}/PCIeDevices/{dev}/MemoryExtents/{ext}",
			norm_url, params, 8)) {
		const char *msg = NULL;
		json_t *task = NULL;

		rc = fmd_redfish_delete_dc_memory_extent(ctx, params[0], params[1],
							 params[2], &msg, &task);
		free_params(params, 8);
		free(norm_url);

		if (rc == FMD_OP_STARTED)
			return send_dc_started(conn, task);
		return send_errno(ctx, conn, rc, msg, url, NULL, NULL, 0);
	}

	free(norm_url);
	return send_method_not_allowed(ctx, conn, "DELETE", url);
}

/* How many query parameters of a request start with $; nothing allocated */
static enum MHD_Result count_dollar_param(void *cls, enum MHD_ValueKind kind,
					  const char *key, const char *value)
{
	size_t *n = cls;

	(void)kind;
	(void)value;
	if (key && key[0] == '$')
		(*n)++;
	return MHD_YES;
}

/* The query parameters of a request that start with $ */
static enum MHD_Result collect_dollar_param(void *cls, enum MHD_ValueKind kind,
					    const char *key, const char *value)
{
	json_t *found = cls;

	(void)kind;
	(void)value;
	if (key && key[0] == '$')
		json_array_append_new(found, json_string(key));
	return MHD_YES;
}

/*
 * The body of the 501 for the $-parameters @found, which must hold all @n
 * of them (a string each): NULL if it does not (one could not be
 * collected), or if any of the body could not be made. Never a body with
 * a message missing.
 */
json_t *fmd_redfish_unsupported_query_body(json_t *found, size_t n)
{
	json_t *info, *body, *v;
	char code[64];
	size_t i;

	if (!found || !n || json_array_size(found) != n)
		return NULL;
	info = json_array();
	if (!info)
		return NULL;
	json_array_foreach(found, i, v) {
		json_t *m;

		if (!json_is_string(v))
			goto fail;
		m = fmd_redfish_message(FMD_RF_QUERY_PARAMETER_UNSUPPORTED, NULL,
					NULL, 1, json_string_value(v));
		/* json_array_append_new() releases @m when it fails */
		if (!m || json_array_append_new(info, m))
			goto fail;
	}
	/* more than one message may be given: the code is GeneralError */
	fmd_redfish_message_id(FMD_RF_GENERAL_ERROR, code, sizeof(code));
	body = json_pack("{s:{s:s, s:s, s:O}}", "error",
			 "code", code,
			 "message", "Query parameters that start with $ are not supported",
			 "@Message.ExtendedInfo", info);
	json_decref(info);
	return body;
fail:
	json_decref(info);
	return NULL;
}

/*
 * cxl-fmd supports no query parameter, and its service root has no
 * ProtocolFeaturesSupported ("Shall include the ProtocolFeaturesSupported
 * object in the service root, if the service supports query parameters",
 * DSP0266 1.25.0 Section 7.3.1). That clause has services return "The HTTP
 * 501 Not Implemented status code for any unsupported query parameters
 * that start with $" and "An extended error that indicates the
 * unsupported query parameters for this resource"; the others are
 * ignored ("Shall ignore unknown or unsupported query parameters that do
 * not begin with $"). The extended error has one Base
 * QueryParameterUnsupported message for each (src/redfish/messages.c). A
 * request of any method is answered so: cxl-fmd's choice, as the clause
 * has query parameters only on GET. Returns false if there is none, and the request is handled as
 * usual; else true, the answer sent, and *@result what sending it gave.
 * Whether there is one is found with nothing allocated, so that running
 * out of memory never lets such a request through; a body that cannot be
 * made in full is replaced by a GeneralError one, still with 501
 * (cxl-fmd's choice).
 */
static bool send_unsupported_query(struct MHD_Connection *conn,
				   enum MHD_Result *result)
{
	json_t *found, *body = NULL;
	size_t n = 0;

	MHD_get_connection_values(conn, MHD_GET_ARGUMENT_KIND,
				  count_dollar_param, &n);
	if (!n)
		return false;
	found = json_array();
	if (found) {
		MHD_get_connection_values(conn, MHD_GET_ARGUMENT_KIND,
					  collect_dollar_param, found);
		body = fmd_redfish_unsupported_query_body(found, n);
		json_decref(found);
	}
	if (body)
		*result = send_json_response(conn, MHD_HTTP_NOT_IMPLEMENTED, body);
	else
		*result = send_error(conn, MHD_HTTP_NOT_IMPLEMENTED,
				     "Query parameters that start with $ are not supported",
				     RF_GENERAL("Remove the query parameters that start "
						"with $, and resubmit the request."));
	return true;
}

/*
 * The collection a POST to @url creates in, if @url is the Members of one
 * that takes a POST: "Submitting a POST request to a resource collection
 * is equivalent to submitting the same request to the Members property of
 * that resource collection. Services that support the addition of Members
 * to a resource collection shall support both forms." (DSP0266 1.25.0
 * Section 7.9). Allocated; NULL if @url is not such a Members.
 */
static char *members_collection(const char *url)
{
	char *norm_url = normalize_url(url);
	size_t len = norm_url ? strlen(norm_url) : 0;
	const size_t suffix = strlen("/Members");

	if (len > suffix && !strcmp(norm_url + len - suffix, "/Members")) {
		norm_url[len - suffix] = '\0';
		if (matches_any(post_routes, ROUTES(post_routes), norm_url))
			return norm_url;
	}
	free(norm_url);
	return NULL;
}

/*
 * A request whose OData-Version is not 4.0: "Services shall reject
 * requests that specify an unsupported OData version. If a service
 * encounters an unsupported OData version, it should reject the request
 * with the HTTP 412 Precondition Failed status code." (DSP0266 1.25.0
 * Table 12). cxl-fmd supports 4.0 only, the version it answers with. A
 * request with none is taken as one of 4.0 ("Removed requirement for
 * clients to include the OData-Version HTTP header in all requests",
 * DSP0266's change log). Any method and any URI, before anything else is
 * looked at, so nothing is done. Base HeaderInvalid, whose argument
 * "shall contain the full request header, including the field name and
 * field value" (Base.1.24.0.json). Returns true, the answer sent and
 * *@result what sending it gave, if the request is answered so.
 */
static bool send_unsupported_odata_version(struct MHD_Connection *conn,
					   enum MHD_Result *result)
{
	const char *v = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
						    "OData-Version");
	char header[128];

	if (!v || !strcmp(v, "4.0"))
		return false;
	snprintf(header, sizeof(header), "OData-Version: %.100s", v);
	*result = send_error(conn, MHD_HTTP_PRECONDITION_FAILED,
			     "OData-Version is not supported: cxl-fmd supports 4.0 only. "
			     "Nothing was done",
			     fmd_redfish_message(FMD_RF_HEADER_INVALID,
				"Resubmit the request with OData-Version 4.0, or without "
				"the header.", NULL, 1, header));
	return true;
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
				  "HTTP version not supported",
				  RF_GENERAL("Resubmit the request with HTTP/1.1."));
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
					  "Request body too large (max 64KB)",
					  RF_MSG(FMD_RF_PAYLOAD_TOO_LARGE));
		} else if (rctx->error == MHD_HTTP_INTERNAL_SERVER_ERROR) {
			return send_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "Failed to buffer request body",
					  RF_MSG(FMD_RF_INTERNAL_ERROR));
		}
	}

	/*
	 * Dispatch to method-specific handler under fabric_lock: shared for
	 * a GET, which only reads, and exclusive for anything else. The body
	 * of a request has arrived by now, so the lock is not held while a
	 * client is slow to send one.
	 */
	enum MHD_Result result;

	if (send_unsupported_odata_version(conn, &result))
		return result;
	if (send_unsupported_query(conn, &result))
		return result;

	if (strcmp(method, "GET") == 0) {
		pthread_rwlock_rdlock(&ctx->fabric_lock);
		result = handle_request_get(ctx, conn, url);
		pthread_rwlock_unlock(&ctx->fabric_lock);
		return result;
	}

	/* a POST to the Members of a collection is one to the collection */
	char *collection = strcmp(method, "POST") ? NULL : members_collection(url);

	if (collection)
		url = collection;

	pthread_rwlock_wrlock(&ctx->fabric_lock);
	if (send_precondition_failed(ctx, conn, method, url, &result))
		;
	else if (strcmp(method, "POST") == 0)
		result = handle_request_post(ctx, conn, url,
					     rctx->body, rctx->body_size);
	else if (strcmp(method, "PUT") == 0)
		result = handle_request_put(ctx, conn, url,
					    rctx->body, rctx->body_size);
	else if (strcmp(method, "PATCH") == 0)
		result = handle_request_patch(ctx, conn, url,
					      rctx->body, rctx->body_size);
	else if (strcmp(method, "DELETE") == 0)
		result = handle_request_delete(ctx, conn, url);
	else
		result = send_method_not_allowed(ctx, conn, method, url);
	pthread_rwlock_unlock(&ctx->fabric_lock);
	free(collection);

	return result;
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
	unsigned int threads;

	if (!ctx)
		return -1;

	ctx->redfish.port = ctx->config.port;

	/*
	 * A pool of threads, each serving connections of its own. What may
	 * run at once is decided by fabric_lock, not here. libmicrohttpd
	 * makes a pool only of two threads or more: one is the single
	 * internal thread it has without the option.
	 */
	threads = ctx->config.http_threads ? ctx->config.http_threads :
					     FMD_DEFAULT_HTTP_THREADS;
	if (threads > 1)
		ctx->redfish.daemon = MHD_start_daemon(
			MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
			ctx->redfish.port,
			NULL, NULL,                    /* accept policy */
			handle_request, ctx,           /* request handler */
			MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
			MHD_OPTION_THREAD_POOL_SIZE, threads,
			MHD_OPTION_END);
	else
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

	fmd_info(ctx, "Serving requests on %u thread%s", threads,
		 threads == 1 ? "" : "s");

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
