/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Redfish error responses
 *
 * "The @Message.ExtendedInfo property should be present in all error
 * responses" (DSP0266 1.25.0 Section 9.5.10). Each message object cxl-fmd
 * makes is one of the DMTF Base registry, Base.1.24.0.json (a copy is in
 * schema/dmtf/registries): its MessageId, its Message with the arguments
 * put in, its MessageSeverity and its Resolution, which cxl-fmd replaces
 * where it has a better one ("Services can replace the value of the
 * Resolution property defined in the message registry with a
 * service-defined resolution", Table 20).
 *
 * A MessageId is "<MessageRegistryPrefix>.<MajorVersion>.<MinorVersion>.
 * <MessageKey>" (Section 9.5.11.2): Base.1.24.<key>. The code of an error
 * is GeneralError: "If no better message exists or the ExtendedInfo array
 * contains multiple messages, use GeneralError from the Base Message
 * Registry only in the code property of the error object" (Section
 * 9.5.11.2). The error's message is cxl-fmd's own sentence on what
 * happened.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cxl-fmd.h"

struct rf_registry_msg {
	const char *key;
	int nargs;
	const char *severity;
	const char *message;
	const char *resolution;
};

/* Copied from Base.1.24.0.json; tests/test_errors.py compares them */
static const struct rf_registry_msg base_msgs[FMD_RF_MSG_COUNT] = {
	[FMD_RF_GENERAL_ERROR] = {
		"GeneralError", 0, "Critical",
		"A general error has occurred.  See Resolution for information on how to resolve the error, or @Message.ExtendedInfo if Resolution is not provided.",
		"None." },
	[FMD_RF_SUCCESS] = {
		"Success", 0, "OK",
		"The request completed successfully.",
		"None." },
	[FMD_RF_INVALID_URI] = {
		"InvalidURI", 1, "Critical",
		"The URI '%1' was not found.",
		"Provide a valid URI and resubmit the request." },
	[FMD_RF_OPERATION_NOT_ALLOWED] = {
		"OperationNotAllowed", 0, "Critical",
		"The HTTP method is not allowed on this resource.",
		"None." },
	[FMD_RF_PAYLOAD_TOO_LARGE] = {
		"PayloadTooLarge", 0, "Critical",
		"The supplied payload exceeds the maximum size supported by the service.",
		"Check that the supplied payload is correct and supported by this service." },
	[FMD_RF_MALFORMED_JSON] = {
		"MalformedJSON", 0, "Critical",
		"The request body submitted was malformed JSON and could not be parsed by the receiving service.",
		"Ensure that the request body is valid JSON and resubmit the request." },
	[FMD_RF_INTERNAL_ERROR] = {
		"InternalError", 0, "Critical",
		"The request failed due to an internal service error.  The service is still operational.",
		"Resubmit the request.  If the problem persists, consider resetting the service." },
	[FMD_RF_RESOURCE_IN_USE] = {
		"ResourceInUse", 0, "Warning",
		"The change to the requested resource failed because the resource is in use or in transition.",
		"Remove the condition and resubmit the request if the operation failed." },
	[FMD_RF_RESOURCE_EXHAUSTION] = {
		"ResourceExhaustion", 1, "Critical",
		"The resource '%1' was unable to satisfy the request due to unavailability of resources.",
		"Ensure that the resources are available and resubmit the request." },
	[FMD_RF_PROPERTY_MISSING] = {
		"PropertyMissing", 1, "Warning",
		"The property '%1' is a required property and must be included in the request.",
		"Ensure that the property is in the request body and has a valid value and resubmit the request if the operation failed." },
	[FMD_RF_PROPERTY_VALUE_ERROR] = {
		"PropertyValueError", 1, "Warning",
		"The value provided for the property '%1' is not valid.",
		"Correct the value for the property in the request body and resubmit the request if the operation failed." },
	[FMD_RF_ACTION_NOT_SUPPORTED] = {
		"ActionNotSupported", 1, "Critical",
		"The action '%1' is not supported by the resource.",
		"Check the Actions property in the resource for the supported actions." },
	[FMD_RF_RESOURCE_CANNOT_BE_DELETED] = {
		"ResourceCannotBeDeleted", 0, "Critical",
		"The delete request failed because the resource requested cannot be deleted.",
		"Do not attempt to delete a non-deletable resource." },
	[FMD_RF_QUERY_PARAMETER_UNSUPPORTED] = {
		"QueryParameterUnsupported", 1, "Warning",
		"Query parameter '%1' is not supported.",
		"Correct or remove the query parameter and resubmit the request." },
};

void fmd_redfish_message_id(enum fmd_rf_msg id, char *buf, size_t len)
{
	snprintf(buf, len, "%s.%s", FMD_RF_REGISTRY,
		 id < FMD_RF_MSG_COUNT ? base_msgs[id].key : "GeneralError");
}

/* @tmpl with %1 .. %9 replaced by @args (of @nargs); NULL if out of memory */
static char *substitute(const char *tmpl, const char *const *args, int nargs)
{
	size_t len = 1;
	const char *p;
	char *out, *o;

	for (p = tmpl; *p; p++) {
		if (p[0] == '%' && p[1] >= '1' && p[1] <= '9' && p[1] - '1' < nargs) {
			len += strlen(args[p[1] - '1']);
			p++;
		} else {
			len++;
		}
	}
	out = malloc(len);
	if (!out)
		return NULL;
	for (p = tmpl, o = out; *p; p++) {
		if (p[0] == '%' && p[1] >= '1' && p[1] <= '9' && p[1] - '1' < nargs) {
			size_t n = strlen(args[p[1] - '1']);

			memcpy(o, args[p[1] - '1'], n);
			o += n;
			p++;
		} else {
			*o++ = *p;
		}
	}
	*o = '\0';
	return out;
}

json_t *fmd_redfish_message(enum fmd_rf_msg id, const char *resolution,
			    const char *related, int nargs, ...)
{
	const char *args[9] = { 0 };
	const struct rf_registry_msg *m;
	json_t *obj, *margs;
	char msgid[96];
	char *text;
	va_list ap;

	if (id >= FMD_RF_MSG_COUNT || nargs != base_msgs[id].nargs || nargs > 9)
		return NULL;
	m = &base_msgs[id];
	va_start(ap, nargs);
	for (int i = 0; i < nargs; i++) {
		args[i] = va_arg(ap, const char *);
		if (!args[i])
			args[i] = "";
	}
	va_end(ap);

	text = substitute(m->message, args, nargs);
	if (!text)
		return NULL;
	fmd_redfish_message_id(id, msgid, sizeof(msgid));
	/*
	 * Severity is deprecated for MessageSeverity (Table 20); both are
	 * given, as in the example of Section 9.5.10 (cxl-fmd's choice)
	 */
	obj = json_pack("{s:s, s:s, s:s, s:s, s:s}",
			"MessageId", msgid,
			"Message", text,
			"MessageSeverity", m->severity,
			"Severity", m->severity,
			"Resolution", resolution ? resolution : m->resolution);
	free(text);
	if (!obj)
		return NULL;
	/* "If the parameterized message defines a MessageId, the service
	 * shall include the MessageArgs in the response" (Table 20) */
	if (nargs) {
		margs = json_array();
		for (int i = 0; margs && i < nargs; i++) {
			if (json_array_append_new(margs, json_string(args[i]))) {
				json_decref(margs);
				margs = NULL;
			}
		}
		if (!margs || json_object_set_new(obj, "MessageArgs", margs)) {
			json_decref(obj);
			return NULL;
		}
	}
	if (related &&
	    json_object_set_new(obj, "RelatedProperties", json_pack("[s]", related))) {
		json_decref(obj);
		return NULL;
	}
	return obj;
}

json_t *fmd_redfish_error_body(const char *message, json_t *info)
{
	char code[96];
	json_t *body;

	if (!info)
		return NULL;
	fmd_redfish_message_id(FMD_RF_GENERAL_ERROR, code, sizeof(code));
	body = json_pack("{s:{s:s, s:s, s:[O]}}", "error",
			 "code", code,
			 "message", message ? message : "",
			 "@Message.ExtendedInfo", info);
	json_decref(info);
	return body;
}

/*
 * What is sent when an error body cannot be made (out of memory): a
 * complete one, with the error's status all the same (cxl-fmd's choice).
 */
const char fmd_redfish_error_fallback[] =
	"{\"error\":{\"code\":\"" FMD_RF_REGISTRY ".GeneralError\","
	"\"message\":\"The service ran out of memory while answering\","
	"\"@Message.ExtendedInfo\":[{\"MessageId\":\"" FMD_RF_REGISTRY ".InternalError\","
	"\"Message\":\"The request failed due to an internal service error.  "
	"The service is still operational.\","
	"\"MessageSeverity\":\"Critical\",\"Severity\":\"Critical\","
	"\"Resolution\":\"Read the resource before the request is sent again: "
	"whether it was carried out is not said.\"}]}}";
