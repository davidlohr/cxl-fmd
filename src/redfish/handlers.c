/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - Redfish Resource Handlers
 *
 * Generates JSON responses for Redfish API endpoints.
 *
 * Resource Hierarchy:
 * - ServiceRoot
 * - Fabrics Collection / Fabric / Switches / Ports
 * - Chassis Collection / Chassis (SLD/MLD)
 * - PCIeDevices Collection / PCIeDevice
 * - CXLLogicalDevices Collection / CXLLogicalDevice
 * - MemoryDomains Collection / MemoryDomain
 * - MemoryChunks Collection
 */

#include "handlers_internal.h"

/*
 * Forward declarations for internal helpers
 */
static json_t *build_sld_semantics_array(struct fmd_sld *sld);
static json_t *build_mld_semantics_array(struct fmd_mld *mld);
static json_t *build_mhd_semantics_array(struct fmd_mhd *mhd);
static void append_conn_targets(json_t *arr, const struct fmd_conn_view *v);
static void append_conn_lds(json_t *arr, const struct fmd_conn_view *v);
static int memory_endpoint_setup(struct fmd_ctx *ctx, const char *chassis_id,
				 const char *mem_id, struct cxlmi_endpoint **ep_out,
				 struct cxlmi_tunnel_info *ti_buf,
				 struct cxlmi_tunnel_info **ti_out);
static void sec_memory_actions(struct fmd_ctx *ctx, const char *chassis_id,
			       const char *mem_id, json_t *actions);
static void sec_ld_actions(struct fmd_ctx *ctx, struct fmd_component *dev,
			   int ld_idx, const char *uri, json_t *actions);

/*
 * OData Service Document - /redfish/v1/odata
 *
 * Returns the OData service document listing available resources.
 * Required for Redfish validation.
 */
json_t *fmd_redfish_odata(struct fmd_ctx *ctx)
{
	(void)ctx;

	json_t *values = json_array();

	/* Add all top-level resources */
	json_array_append_new(values, json_pack(
		"{s:s, s:s, s:s}",
		"name", "Service",
		"kind", "Singleton",
		"url", "/redfish/v1"));

	json_array_append_new(values, json_pack(
		"{s:s, s:s, s:s}",
		"name", "Fabrics",
		"kind", "Singleton",
		"url", "/redfish/v1/Fabrics"));

	json_array_append_new(values, json_pack(
		"{s:s, s:s, s:s}",
		"name", "Chassis",
		"kind", "Singleton",
		"url", "/redfish/v1/Chassis"));

	json_array_append_new(values, json_pack(
		"{s:s, s:s, s:s}",
		"name", "Systems",
		"kind", "Singleton",
		"url", "/redfish/v1/Systems"));

	json_array_append_new(values, json_pack(
		"{s:s, s:s, s:s}",
		"name", "Managers",
		"kind", "Singleton",
		"url", "/redfish/v1/Managers"));

	json_array_append_new(values, json_pack(
		"{s:s, s:s, s:s}",
		"name", "Registries",
		"kind", "Singleton",
		"url", "/redfish/v1/Registries"));

	return json_pack(
		"{"
		"  s:s,"    /* @odata.context */
		"  s:o"     /* value */
		"}",
		"@odata.context", "/redfish/v1/$metadata",
		"value", values
	);
}

/*
 * ServiceRoot - /redfish/v1/
 *
 * The Links.Sessions is mandatory per Redfish spec.
 */
json_t *fmd_redfish_service_root(struct fmd_ctx *ctx)
{
	(void)ctx;

	return json_pack(
		"{"
		"  s:s,"  /* @odata.id */
		"  s:s,"  /* @odata.type */
		"  s:s,"  /* Id */
		"  s:s,"  /* Name */
		"  s:s,"  /* RedfishVersion */
		"  s:{s:s},"  /* Fabrics */
		"  s:{s:s},"  /* Chassis */
		"  s:{s:s},"  /* Systems */
		"  s:{s:s},"  /* Managers */
		"  s:{s:s},"  /* Tasks */
		"  s:{s:s},"  /* Registries */
		"  s:{s:{s:s}}"  /* Links.Sessions (mandatory) */
		"}",
		"@odata.id", "/redfish/v1",
		"@odata.type", "#ServiceRoot.v1_15_0.ServiceRoot",
		"Id", "RootService",
		"Name", "CXL Fabric Manager Service",
		"RedfishVersion", "1.15.0",
		"Fabrics", "@odata.id", "/redfish/v1/Fabrics",
		"Chassis", "@odata.id", "/redfish/v1/Chassis",
		"Systems", "@odata.id", "/redfish/v1/Systems",
		"Managers", "@odata.id", "/redfish/v1/Managers",
		"Tasks", "@odata.id", "/redfish/v1/TaskService",
		"Registries", "@odata.id", "/redfish/v1/Registries",
		"Links", "Sessions", "@odata.id", "/redfish/v1/SessionService/Sessions"
	);
}

/*
 * Systems Collection - /redfish/v1/Systems
 *
 * Returns collection of host systems that can access CXL memory.
 */
json_t *fmd_redfish_systems_collection(struct fmd_ctx *ctx)
{
	struct rf_collection coll;
	struct fmd_component *comp;

	rf_collection_init(&coll, "/redfish/v1/Systems",
			   "#ComputerSystemCollection.ComputerSystemCollection",
			   "Computer System Collection");

	fmd_topology_rdlock(&ctx->topology);
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_HOST)
		rf_collection_add(&coll, "/redfish/v1/Systems/%s", comp->id);
	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * Individual System - /redfish/v1/Systems/{systemId}
 *
 * Returns a host system with links to MemoryDomains (connected CXL memory).
 */
json_t *fmd_redfish_system(struct fmd_ctx *ctx, const char *system_id)
{
	struct fmd_host *host;
	char odata_id[128];
	char mem_domains_id[128];

	host = fmd_topology_find_host(&ctx->topology, system_id);
	if (!host)
		return NULL;

	snprintf(odata_id, sizeof(odata_id), "/redfish/v1/Systems/%s", system_id);
	snprintf(mem_domains_id, sizeof(mem_domains_id),
		 "/redfish/v1/Systems/%s/MemoryDomains", system_id);

	/*
	 * No PowerState: nothing the FM reads (Identify Switch Device, Get
	 * Virtual CXL Switch Info, Get Physical Port State) says whether a
	 * host is powered on.
	 */
	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:{s:s}"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#ComputerSystem.v1_22_0.ComputerSystem",
		"Id", system_id,
		"Name", host->name,
		"SystemType", "Physical",
		"MemoryDomains", "@odata.id", mem_domains_id
	);
}

/*
 * A host's MemoryDomains are its Connections that are Enabled, one each,
 * with the Connection's ID: what the binding makes reachable. A
 * Connection being made or removed, Absent, or whose target is not known
 * (Enabled with Health Warning) gives none: no memory is reachable through
 * it, or none can be named. Which ones give one is cxl-fmd's choice. It is
 * not what the host has mapped: the FM does not see that.
 *
 * Each of these takes the topology read lock; the caller holds fabric_lock.
 */
static bool host_domain(const struct fmd_conn_view *v, const char *system_id)
{
	return v->state == FMD_CONN_ENABLED && v->host &&
	       !strcmp(v->host->base.id, system_id);
}

struct domain_walk {
	const char *system_id;
	json_t *members;
};

static int domain_member(const struct fmd_conn_view *v, void *arg)
{
	struct domain_walk *w = arg;

	if (host_domain(v, w->system_id))
		json_array_append_new(w->members,
			rf_link("/redfish/v1/Systems/%s/MemoryDomains/%s",
				w->system_id, v->id));
	return 0;
}

/*
 * System MemoryDomains Collection - /redfish/v1/Systems/{systemId}/MemoryDomains
 */
json_t *fmd_redfish_system_memory_domains(struct fmd_ctx *ctx, const char *system_id)
{
	struct domain_walk w = { .system_id = system_id };
	char odata_id[128];

	if (!fmd_topology_find_host(&ctx->topology, system_id))
		return NULL;

	snprintf(odata_id, sizeof(odata_id),
		 "/redfish/v1/Systems/%s/MemoryDomains", system_id);

	w.members = json_array();
	fmd_topology_rdlock(&ctx->topology);
	fmd_conn_for_each(ctx, domain_member, &w);
	fmd_topology_rdunlock(&ctx->topology);

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:I,"
		"  s:o"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#MemoryDomainCollection.MemoryDomainCollection",
		"Name", "Host Memory Domains",
		"Members@odata.count", (json_int_t)json_array_size(w.members),
		"Members", w.members
	);
}

/* The Connection @domain_id, if it is a MemoryDomain of host @system_id */
static int host_domain_view(struct fmd_ctx *ctx, const char *system_id,
			    const char *domain_id, struct fmd_conn_view *v)
{
	if (!fmd_topology_find_host(&ctx->topology, system_id))
		return -ENOENT;
	if (fmd_conn_lookup(ctx, domain_id, v) || !host_domain(v, system_id))
		return -ENOENT;
	return 0;
}

static int sum_chunk(const struct fmd_mem_chunk *chunk, void *arg)
{
	*(uint64_t *)arg += chunk->size;
	return 0;
}

/*
 * System MemoryDomain - /redfish/v1/Systems/{systemId}/MemoryDomains/{domainId}
 */
json_t *fmd_redfish_system_memory_domain(struct fmd_ctx *ctx,
					 const char *system_id,
					 const char *domain_id)
{
	struct fmd_conn_view v;
	char odata_id[256];
	char chunks_id[256];
	uint64_t size = 0;
	json_t *result = NULL, *lds;

	fmd_topology_rdlock(&ctx->topology);
	if (host_domain_view(ctx, system_id, domain_id, &v))
		goto out;

	snprintf(odata_id, sizeof(odata_id),
		 "/redfish/v1/Systems/%s/MemoryDomains/%s", system_id, domain_id);
	snprintf(chunks_id, sizeof(chunks_id),
		 "/redfish/v1/Systems/%s/MemoryDomains/%s/MemoryChunks",
		 system_id, domain_id);

	/* its chunks; an LD that has none is its own size (cxl-fmd's choice) */
	fmd_conn_for_each_chunk(&v, sum_chunk, &size);
	if (!size && v.ld)
		size = v.ld->memory_size;

	lds = json_array();
	append_conn_lds(lds, &v);

	result = json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:I,"
		"  s:{s:s},"
		"  s:{s:o}"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#MemoryDomain.v1_5_1.MemoryDomain",
		"Id", domain_id,
		"Name", v.target ? v.target->id : domain_id,
		"MemorySizeMiB", (json_int_t)(size / (1024 * 1024)),
		"MemoryChunks", "@odata.id", chunks_id,
		"Links", "CXLLogicalDevices", lds
	);
	/*
	 * What the host does with it the FM does not see (cxl-fmd's Oem):
	 * "The switch prohibits any CXL.io access from the FM to a bound SLD
	 * port and any CXL.io access from the FM to a bound LD within an MLD
	 * component" (CXL r4.0 Section 7.2.3.1).
	 */
	if (result)
		json_object_set_new(result, "Oem", json_pack("{s:{s:s, s:s}}", "Cxl",
			"@odata.type", "#Cxl.v1_0_0.MemoryDomain",
			"HostState", "NotObserved"));
out:
	fmd_topology_rdunlock(&ctx->topology);
	return result;
}

struct host_chunks {
	const char *base;
	json_t *members;
};

static int host_chunk_member(const struct fmd_mem_chunk *chunk, void *arg)
{
	struct host_chunks *h = arg;

	json_array_append_new(h->members, rf_link("%s/%s", h->base, chunk->base.id));
	return 0;
}

/*
 * System MemoryChunks Collection - /redfish/v1/Systems/{systemId}/MemoryDomains/{domainId}/MemoryChunks
 *
 * One member per chunk the Connection makes reachable.
 */
json_t *fmd_redfish_system_memory_chunks(struct fmd_ctx *ctx,
					 const char *system_id,
					 const char *domain_id)
{
	struct host_chunks h;
	struct fmd_conn_view v;
	char odata_id[256];
	json_t *result = NULL;

	fmd_topology_rdlock(&ctx->topology);
	if (host_domain_view(ctx, system_id, domain_id, &v))
		goto out;

	snprintf(odata_id, sizeof(odata_id),
		 "/redfish/v1/Systems/%s/MemoryDomains/%s/MemoryChunks",
		 system_id, domain_id);
	h.base = odata_id;
	h.members = json_array();
	fmd_conn_for_each_chunk(&v, host_chunk_member, &h);

	result = json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:I,"
		"  s:o"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#MemoryChunksCollection.MemoryChunksCollection",
		"Name", "Host Memory Chunks",
		"Members@odata.count", (json_int_t)json_array_size(h.members),
		"Members", h.members
	);
out:
	fmd_topology_rdunlock(&ctx->topology);
	return result;
}

struct chunk_find {
	const char *id;
	const struct fmd_mem_chunk *found;
};

static int chunk_match(const struct fmd_mem_chunk *chunk, void *arg)
{
	struct chunk_find *f = arg;

	if (strcmp(chunk->base.id, f->id))
		return 0;
	f->found = chunk;
	return 1;
}

/*
 * System MemoryChunk - /redfish/v1/Systems/{systemId}/MemoryDomains/{domainId}/MemoryChunks/{chunkId}
 */
json_t *fmd_redfish_system_memory_chunk(struct fmd_ctx *ctx,
					const char *system_id,
					const char *domain_id,
					const char *chunk_id)
{
	struct chunk_find f = { .id = chunk_id };
	struct fmd_conn_view v;
	char odata_id[384];
	json_t *result = NULL, *targets;

	fmd_topology_rdlock(&ctx->topology);
	if (host_domain_view(ctx, system_id, domain_id, &v))
		goto out;
	fmd_conn_for_each_chunk(&v, chunk_match, &f);
	if (!f.found)
		goto out;

	snprintf(odata_id, sizeof(odata_id),
		 "/redfish/v1/Systems/%s/MemoryDomains/%s/MemoryChunks/%s",
		 system_id, domain_id, chunk_id);

	targets = json_array();
	append_conn_targets(targets, &v);

	/*
	 * How the host may use it is the Connection's: AccessCapabilities of
	 * its MemoryChunkInfo. MemoryChunks has no property for it. Remote:
	 * "created using remote media accessible through a fabric"
	 * (MemoryChunks_v1), as the host sees it.
	 */
	result = json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:I,"
		"  s:o,"
		"  s:s,"
		"  s:{s:o}"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#MemoryChunks.v1_6_1.MemoryChunks",
		"Id", chunk_id,
		"Name", chunk_id,
		"MemoryChunkSizeMiB", (json_int_t)(f.found->size / (1024 * 1024)),
		"AddressRangeType", fmd_chunk_range_type(f.found),
		"MediaLocation", "Remote",
		"Links", "Endpoints", targets
	);
out:
	fmd_topology_rdunlock(&ctx->topology);
	return result;
}

/*
 * Managers Collection - /redfish/v1/Managers
 *
 * TODO: Empty collection for now.
 */
json_t *fmd_redfish_managers_collection(struct fmd_ctx *ctx)
{
	(void)ctx;

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:i,"
		"  s:[]"
		"}",
		"@odata.id", "/redfish/v1/Managers",
		"@odata.type", "#ManagerCollection.ManagerCollection",
		"Name", "Manager Collection",
		"Members@odata.count", 0,
		"Members"
	);
}

/*
 * ============================================================================
 * Tasks
 * ============================================================================
 *
 * A bind or unbind that a switch carries out in the background is a Redfish
 * task (DSP0266, asynchronous operations): the request is answered with 202
 * Accepted and the Location of a task monitor. A GET of the monitor answers
 * 202 while the operation runs, and with what the request would have been
 * answered with once it is over.
 */

/* TaskService - /redfish/v1/TaskService */
json_t *fmd_redfish_task_service(struct fmd_ctx *ctx)
{
	char now[32];

	(void)ctx;
	rf_date_time(time(NULL), now, sizeof(now));

	return json_pack("{s:s, s:s, s:s, s:s, s:b, s:s, s:o, s:{s:s}}",
		"@odata.id", "/redfish/v1/TaskService",
		"@odata.type", "#TaskService.v1_3_0.TaskService",
		"Id", "TaskService",
		"Name", "Task Service",
		"ServiceEnabled", 1,
		"DateTime", now,
		"Status", rf_status("Enabled", "OK"),
		"Tasks", "@odata.id", "/redfish/v1/TaskService/Tasks");
}

/* Tasks - /redfish/v1/TaskService/Tasks */
json_t *fmd_redfish_tasks(struct fmd_ctx *ctx)
{
	struct rf_collection coll;
	struct fmd_task *task;

	rf_collection_init(&coll, "/redfish/v1/TaskService/Tasks",
			   "#TaskCollection.TaskCollection", "Task Collection");
	for (task = ctx->tasks; task; task = task->next)
		rf_collection_add(&coll, "/redfish/v1/TaskService/Tasks/%d", task->id);

	return rf_collection_finish(&coll);
}

static void task_connection_uri(struct fmd_task *task, char *buf, size_t len)
{
	snprintf(buf, len, "/redfish/v1/Fabrics/CXL/Connections/%s",
		 task->connection_id);
}

/*
 * Where the request of a DC task went: the action, or the MemoryExtent of
 * a DELETE
 */
static void dc_task_target(const struct fmd_task *task, char *buf, size_t len)
{
	const struct fmd_dc_task *dc = task->dc;

	if (dc->ext_id[0])
		snprintf(buf, len, "/redfish/v1/Chassis/%s/PCIeDevices/%s/MemoryExtents/%s",
			 dc->chassis_id, dc->dev_id, dc->ext_id);
	else
		snprintf(buf, len, "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s"
			 "/Actions/CXLLogicalDevice.%s", dc->chassis_id, dc->dev_id,
			 dc->ld_id, task->kind == FMD_TASK_DC_ADD ?
			 "DynamicCapacityAdd" : "DynamicCapacityRelease");
}

/*
 * A Dynamic Capacity task: Payload is the request as it came (TargetUri,
 * HttpOperation, and JsonBody, "the JSON payload to use in the execution
 * of this task", Task_v1). There is no PercentComplete: what share of the
 * work a host has done is not known (cxl-fmd's choice). Messages says how
 * it goes while it runs, and how it ended. A partial add is Completed with
 * TaskStatus Warning.
 */
static json_t *dc_task(struct fmd_ctx *ctx, const struct fmd_task *task,
		       const char *uri, const char *monitor, const char *start)
{
	const struct fmd_dc_task *dc = task->dc;
	char target[512], msgid[64];
	json_t *result, *payload;
	const char *status;

	dc_task_target(task, target, sizeof(target));
	payload = json_pack("{s:s, s:s}", "TargetUri", target,
			    "HttpOperation", dc->ext_id[0] ? "DELETE" : "POST");
	if (dc->body)
		json_object_set_new(payload, "JsonBody", json_string(dc->body));

	result = json_pack("{s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:o, s:o}",
		"@odata.id", uri,
		"@odata.type", "#Task.v1_7_4.Task",
		"Id", uri + strlen("/redfish/v1/TaskService/Tasks/"),
		"Name", task->kind == FMD_TASK_DC_ADD ? "Dynamic Capacity Add" :
			"Dynamic Capacity Release",
		"TaskState", task->state == FMD_TASK_RUNNING ? "Running" :
			     task->state == FMD_TASK_COMPLETED ? "Completed" : "Exception",
		"StartTime", start,
		"TaskMonitor", monitor,
		"Payload", payload,
		"Oem", fmd_redfish_dc_task_oem(ctx, task));

	status = task->state == FMD_TASK_EXCEPTION ? "Critical" :
		 dc->partial ? "Warning" : "OK";
	fmd_redfish_message_id(task->state == FMD_TASK_EXCEPTION ? FMD_RF_GENERAL_ERROR :
			       FMD_RF_SUCCESS, msgid, sizeof(msgid));
	if (task->message[0])
		json_object_set_new(result, "Messages",
			json_pack("[{s:s, s:s}]", "MessageId", msgid,
				  "Message", task->message));
	if (task->state != FMD_TASK_RUNNING) {
		char end[32];

		rf_date_time(task->end, end, sizeof(end));
		json_object_set_new(result, "EndTime", json_string(end));
		json_object_set_new(result, "TaskStatus", json_string(status));
	}
	if (task->kind == FMD_TASK_DC_ADD && task->state != FMD_TASK_EXCEPTION)
		json_object_set_new(result, "Links", json_pack("{s:o}",
			"CreatedResources", fmd_redfish_dc_task_created(ctx, task)));
	return result;
}

/* Task - /redfish/v1/TaskService/Tasks/{id} */
json_t *fmd_redfish_task(struct fmd_ctx *ctx, int id)
{
	struct fmd_task *task = fmd_task_find(ctx, id);
	char uri[64], monitor[64], start[32], conn_uri[256];
	json_t *result;

	if (!task)
		return NULL;
	if (fmd_task_is_dc(task)) {
		snprintf(uri, sizeof(uri), "/redfish/v1/TaskService/Tasks/%d", id);
		snprintf(monitor, sizeof(monitor), "/redfish/v1/TaskService/TaskMonitors/%d", id);
		rf_date_time(task->start, start, sizeof(start));
		return dc_task(ctx, task, uri, monitor, start);
	}

	snprintf(uri, sizeof(uri), "/redfish/v1/TaskService/Tasks/%d", id);
	snprintf(monitor, sizeof(monitor), "/redfish/v1/TaskService/TaskMonitors/%d", id);
	rf_date_time(task->start, start, sizeof(start));
	task_connection_uri(task, conn_uri, sizeof(conn_uri));

	result = json_pack("{s:s, s:s, s:s, s:s, s:s, s:i, s:s, s:s, s:{s:s, s:s}}",
		"@odata.id", uri,
		"@odata.type", "#Task.v1_7_4.Task",
		"Id", uri + strlen("/redfish/v1/TaskService/Tasks/"),
		"Name", task->kind == FMD_TASK_BIND ? "Bind vPPB" : "Unbind vPPB",
		"TaskState", task->state == FMD_TASK_RUNNING ? "Running" :
			     task->state == FMD_TASK_COMPLETED ? "Completed" :
			     task->state == FMD_TASK_INTERRUPTED ? "Interrupted" :
			     "Exception",
		/* Percentage Complete of Background Operation Status */
		"PercentComplete", task->percent,
		"StartTime", start,
		"TaskMonitor", monitor,
		"Payload",
			"TargetUri", task->kind == FMD_TASK_BIND ?
				"/redfish/v1/Fabrics/CXL/Connections" : conn_uri,
			"HttpOperation", task->kind == FMD_TASK_BIND ? "POST" : "DELETE");

	/*
	 * TaskStatus "shall not be set until the task completes", and EndTime
	 * "shall not appear if the task is running or otherwise has not been
	 * completed" (Task_v1)
	 */
	if (task->state == FMD_TASK_INTERRUPTED) {
		char msgid[64];

		/* not complete: why the switch is no longer asked */
		fmd_redfish_message_id(FMD_RF_GENERAL_ERROR, msgid, sizeof(msgid));
		json_object_set_new(result, "Messages",
			json_pack("[{s:s, s:s}]",
				  "MessageId", msgid,
				  "Message", task->message));
	} else if (task->state != FMD_TASK_RUNNING) {
		bool ok = task->state == FMD_TASK_COMPLETED;
		char end[32], msgid[64];

		rf_date_time(task->end, end, sizeof(end));
		fmd_redfish_message_id(ok ? FMD_RF_SUCCESS : FMD_RF_GENERAL_ERROR,
				       msgid, sizeof(msgid));
		json_object_set_new(result, "EndTime", json_string(end));
		json_object_set_new(result, "TaskStatus", json_string(ok ? "OK" : "Critical"));
		json_object_set_new(result, "Messages",
			json_pack("[{s:s, s:s}]",
				  "MessageId", msgid,
				  "Message", task->message));
	}

	/*
	 * A bind makes its Connection as soon as it starts, Starting until it
	 * is over (cxl-fmd's choice); one that failed made nothing.
	 */
	if (task->kind == FMD_TASK_BIND && task->state != FMD_TASK_EXCEPTION)
		json_object_set_new(result, "Links",
			json_pack("{s:[{s:s}]}", "CreatedResources",
				  "@odata.id", conn_uri));

	return result;
}

unsigned int fmd_redfish_vppb_failure(enum fmd_task_kind kind, int err,
				      json_t **body)
{
	bool bind = kind == FMD_TASK_BIND;
	unsigned int status;
	const char *msg;
	json_t *info;

	switch (err) {
	case -EAGAIN:		/* Busy, or Retry Required twice */
		status = 503;
		msg = bind ? "The switch cannot bind now; try again" :
			     "The switch is carrying out another operation; try again";
		info = fmd_redfish_message(FMD_RF_RESOURCE_IN_USE, FMD_RES_RETRY,
					   NULL, 0);
		break;
	case -EOPNOTSUPP:	/* Unsupported: the command is optional */
		status = 501;
		msg = bind ? "The switch does not support Bind vPPB (Unsupported)" :
			     "The switch does not support Unbind vPPB (Unsupported)";
		info = bind ? fmd_redfish_message(FMD_RF_GENERAL_ERROR,
						  FMD_RES_UNSUPPORTED, NULL, 0) :
			      fmd_redfish_message(FMD_RF_RESOURCE_CANNOT_BE_DELETED,
						  NULL, NULL, 0);
		break;
	case -EPROTO:		/* Invalid Input */
		status = 400;
		msg = bind ? "The switch refused the bind as Invalid Input" :
			     "The switch refused the unbind as Invalid Input";
		info = fmd_redfish_message(FMD_RF_GENERAL_ERROR, FMD_RES_REFUSED,
					   NULL, 0);
		break;
	case -ESTALE:		/* the VCS could not be read: nothing sent */
		status = 503;
		msg = "The switch's Virtual CXL Switch could not be read: "
		      "nothing was sent; try again";
		info = fmd_redfish_message(FMD_RF_GENERAL_ERROR, FMD_RES_UNREAD,
					   NULL, 0);
		break;
	default:
		status = 500;
		msg = bind ? "Failed to create connection" : "Failed to delete connection";
		info = fmd_redfish_message(FMD_RF_GENERAL_ERROR, FMD_RES_FAILED,
					   NULL, 0);
		break;
	}
	*body = fmd_redfish_error_body(msg, info);
	return status;
}

/*
 * Task monitor - /redfish/v1/TaskService/TaskMonitors/{id}
 *
 * Returns the HTTP status to answer with, and for all but 204 a body in
 * @response. @location is the monitor while the task runs, and the
 * connection once a bind has made it.
 *
 * Once the task is over, "the task monitor shall return: • The appropriate
 * HTTP status code ... • The headers and response body of the initial
 * operation, as if it had completed synchronously. If the initial
 * operation fails, the response body shall contain an error response"
 * (DSP0266 1.25.0 Section 12.2). An unbind is answered as its DELETE,
 * which the Task's Payload names, even when #Cxl.ForceUnbind sent it
 * (cxl-fmd's choice). A failure with the switch's return code is answered
 * as the request would have been had the switch answered with it at once
 * (fmd_redfish_vppb_failure()); one with none (the switch logged the
 * operation failed, or the vPPB is not what was asked) is 500 with what
 * the vPPB showed; and one whose switch is gone, or whose vPPB, the switch
 * not asked, reads what was asked as it did before the command
 * (FMD_TASK_UNPROVEN), is 500 "Outcome unknown", as whether the switch did
 * it is not known (cxl-fmd's choices).
 */
/*
 * The monitor of a DC task that is over answers as the request would have,
 * had what the host did been known at once (DSP0266 1.25.0 Section 12.2,
 * quoted above): a DELETE 204, an action 200 with a Success message that
 * says what the host accepted or released. A host seen to accept none of
 * an add is 409 (the host's state does not allow it; cxl-fmd's choice); a
 * device that is gone is 500 "Outcome unknown".
 */
static int dc_task_monitor(const struct fmd_task *task, json_t **response)
{
	char msg[sizeof(task->message) + 128];

	if (task->state == FMD_TASK_COMPLETED) {
		if (task->dc->ext_id[0])
			return 204;
		*response = fmd_redfish_success_body(task->message);
		return 200;
	}
	if (task->outcome == FMD_TASK_DC_REFUSED) {
		*response = fmd_redfish_error_body(task->message,
			fmd_redfish_message(FMD_RF_GENERAL_ERROR,
				"Read the host's MemoryExtents and the tag's PendingLDs: "
				"the host did not accept the capacity. Resubmit the request "
				"if it still applies.", NULL, 0));
		return 409;
	}
	snprintf(msg, sizeof(msg), "Outcome unknown: %s", task->message);
	*response = fmd_redfish_error_body(msg,
		fmd_redfish_message(FMD_RF_GENERAL_ERROR, FMD_RES_OUTCOME_UNKNOWN,
				    NULL, 0));
	return 500;
}

int fmd_redfish_task_monitor(struct fmd_ctx *ctx, int id, json_t **response,
			     char *location, size_t len)
{
	struct fmd_task *task = fmd_task_find(ctx, id);

	*response = NULL;
	location[0] = '\0';
	if (!task)
		return 404;

	/* Interrupted is not complete either (Task_v1) */
	if (task->state == FMD_TASK_RUNNING ||
	    task->state == FMD_TASK_INTERRUPTED) {
		snprintf(location, len, "/redfish/v1/TaskService/TaskMonitors/%d", id);
		*response = fmd_redfish_task(ctx, id);
		return 202;
	}

	if (fmd_task_is_dc(task))
		return dc_task_monitor(task, response);

	if (task->state == FMD_TASK_EXCEPTION) {
		char msg[sizeof(task->message) + 128];

		switch (task->outcome) {
		case FMD_TASK_RC:
			return (int)fmd_redfish_vppb_failure(task->kind,
						fmd_vppb_errno(task->rc), response);
		case FMD_TASK_UNKNOWN:
			snprintf(msg, sizeof(msg), "Outcome unknown: %s, and whether "
				 "it %s the vPPB before it went is not known",
				 task->message,
				 task->kind == FMD_TASK_BIND ? "bound" : "unbound");
			*response = fmd_redfish_error_body(msg,
				fmd_redfish_message(FMD_RF_GENERAL_ERROR,
						    FMD_RES_OUTCOME_UNKNOWN, NULL, 0));
			return 500;
		case FMD_TASK_UNPROVEN:
			snprintf(msg, sizeof(msg), "Outcome unknown: %s",
				 task->message);
			*response = fmd_redfish_error_body(msg,
				fmd_redfish_message(FMD_RF_GENERAL_ERROR,
						    FMD_RES_OUTCOME_UNKNOWN, NULL, 0));
			return 500;
		default:
			*response = fmd_redfish_error_body(task->message,
				fmd_redfish_message(FMD_RF_GENERAL_ERROR,
						    FMD_RES_FAILED, NULL, 0));
			return 500;
		}
	}

	if (task->kind == FMD_TASK_UNBIND)
		return 204;

	task_connection_uri(task, location, len);
	*response = fmd_redfish_connection(ctx, "CXL", task->connection_id);
	return *response ? 201 : 404;
}

/*
 * Sessions Collection - /redfish/v1/SessionService/Sessions
 *
 * TODO: Empty collection - we don't use session-based auth.
 */
json_t *fmd_redfish_sessions_collection(struct fmd_ctx *ctx)
{
	(void)ctx;

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:i,"
		"  s:[]"
		"}",
		"@odata.id", "/redfish/v1/SessionService/Sessions",
		"@odata.type", "#SessionCollection.SessionCollection",
		"Name", "Session Collection",
		"Members@odata.count", 0,
		"Members"
	);
}


/*
 * Fabrics Collection - /redfish/v1/Fabrics
 */
json_t *fmd_redfish_fabrics_collection(struct fmd_ctx *ctx)
{
	json_t *members = json_array();

	/* We have a single CXL fabric */
	if (ctx->topology.root)
		json_array_append_new(members, rf_link("/redfish/v1/Fabrics/CXL"));

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:i,"
		"  s:o"
		"}",
		"@odata.id", "/redfish/v1/Fabrics",
		"@odata.type", "#FabricCollection.FabricCollection",
		"Name", "Fabric Collection",
		"Members@odata.count", ctx->topology.root ? 1 : 0,
		"Members", members
	);
}

/*
 * Fabric - /redfish/v1/Fabrics/{fabric_id}
 */
json_t *fmd_redfish_fabric(struct fmd_ctx *ctx, const char *fabric_id)
{
	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_fabric *fabric = ctx->topology.root;
	if (!fabric)
		return NULL;

	return json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* FabricType */
		"  s:{s:s},"  /* Switches */
		"  s:{s:s},"  /* Endpoints */
		"  s:{s:s},"  /* Connections */
		"  s:{s:s},"  /* Zones */
		"  s:o"    /* Status */
		"}",
		"@odata.id", "/redfish/v1/Fabrics/CXL",
		"@odata.type", "#Fabric.v1_3_0.Fabric",
		"Id", "CXL",
		"Name", "CXL Fabric",
		"FabricType", "CXL",
		"Switches", "@odata.id", "/redfish/v1/Fabrics/CXL/Switches",
		"Endpoints", "@odata.id", "/redfish/v1/Fabrics/CXL/Endpoints",
		"Connections", "@odata.id", "/redfish/v1/Fabrics/CXL/Connections",
		"Zones", "@odata.id", "/redfish/v1/Fabrics/CXL/Zones",
		"Status", rf_status("Enabled", "OK")
	);
}

/*
 * Per-LD Endpoints (cxl-fmd's choice: Endpoint_v1 allows an Endpoint per
 * logical device and does not require it).
 *
 * A host is bound to an SLD whole, and to an MLD one LD at a time: "If
 * the bind target is a physical port connected to an MLD, then a
 * corresponding LD-ID must also be specified" (CXL r4.0 Section
 * 7.6.7.2.2), and "For a DSP above an MLD, there is a vPPB for each LD"
 * (Table 7-93,
 * HBR Switch Routing for CXL.mem). So the target Endpoint of an SLD is the
 * device, Id the device's, and that of an MLD or a Multi-Headed device is
 * each of its LDs, Id the LD's ("{dev}_ld{N}"); the device has none of
 * its own.
 *
 * A head of a Multi-Headed device that presents an MLD is bound by an
 * LD-ID "from 0 to n-1 relative to that head", which "The MH-MLD maps
 * ... to the device-wide LD index in the MH-MLD's LD pool" (Section
 * 2.5.1) by no rule the FM is told. Such a head has an Endpoint of its
 * own ("{mhd}_head{H}"), EntityType Memory, which is what a binding of
 * an LD of that head is shown to reach.
 */

/* Whether @comp has a target Endpoint of its own */
/*
 * The URIs Endpoints and memory chunks are served at. The GET handlers
 * build them here, and a request's links are held to them (see
 * resolve_endpoint() and resolve_chunk()). Each returns 0, or
 * -ENAMETOOLONG if @len is too short for it.
 */
#define RF_ENDPOINTS "/redfish/v1/Fabrics/CXL/Endpoints/"
#define RF_CHUNK_URI_LEN (sizeof("/redfish/v1/Chassis//MemoryDomains//MemoryChunks/") + \
			  3 * FMD_MAX_ID_LEN)

static int endpoint_uri(char *buf, size_t len, const char *id)
{
	return (size_t)snprintf(buf, len, RF_ENDPOINTS "%s", id) < len ? 0 : -ENAMETOOLONG;
}

/* A chunk is served under its device's Chassis, in its MemoryDomain */
static int chunk_uri(char *buf, size_t len, const struct fmd_mem_chunk *chunk)
{
	const struct fmd_component *dom = chunk->base.parent;

	if (!dom || !dom->parent)
		return -ENOENT;
	return (size_t)snprintf(buf, len,
				"/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s",
				dom->parent->id, dom->id, chunk->base.id) < len ?
		0 : -ENAMETOOLONG;
}

static bool is_target_endpoint(const struct fmd_component *comp)
{
	if (!comp)
		return false;
	switch (comp->type) {
	case FMD_COMPONENT_SLD:
		return true;
	case FMD_COMPONENT_LD:
		return comp->parent && (comp->parent->type == FMD_COMPONENT_MLD ||
					comp->parent->type == FMD_COMPONENT_MHD);
	case FMD_COMPONENT_HEAD:
		return fmd_to_head((struct fmd_component *)comp)->presentation ==
		       FMD_HEAD_MLD;
	default:
		return false;
	}
}

/*
 * The target Endpoint a vPPB is bound to, NULL if none: it is not bound
 * (Bound Port ID is "Valid only when Binding Status is 02h or 03h",
 * Table 7-34), or what is on the port is not a memory
 * device cxl-fmd knows.
 *
 *  - 02h, an SLD on the port: the SLD;
 *  - 03h, an MLD on the port: its LD with the Bound LD ID;
 *  - 02h, the port is a head of a Multi-Headed device that is bound
 *    whole: the pool LD mapped to that head;
 *  - 03h, the port is a head that presents an MLD: the head.
 *
 * The rule is fmd_bind_target()'s, which the Connections computed from
 * the switch use too. Caller holds the topology read lock.
 */
static struct fmd_component *vppb_target(const struct fmd_vppb *vppb)
{
	if (vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT)
		return fmd_bind_target(vppb->port, FMD_LD_NONE);
	if (vppb->binding_status == FMD_VPPB_BOUND_LD)
		return fmd_bind_target(vppb->port, vppb->bound_ld_id);
	return NULL;
}

/* Append to @arr the link to Endpoint @id unless @arr has it */
static void append_endpoint_once(json_t *arr, const char *id)
{
	char uri[256];
	size_t i;
	json_t *v;

	snprintf(uri, sizeof(uri), "/redfish/v1/Fabrics/CXL/Endpoints/%s", id);
	json_array_foreach(arr, i, v) {
		if (!strcmp(json_string_value(json_object_get(v, "@odata.id")), uri))
			return;
	}
	json_array_append_new(arr, rf_link("%s", uri));
}

/*
 * The target Endpoints of memory device @dev: the device for an SLD,
 * each LD of an MLD or a Multi-Headed device. Caller holds the topology
 * read lock.
 */
static void append_device_endpoints(json_t *arr, struct fmd_component *dev)
{
	struct fmd_component *by_id[256] = { NULL };
	struct fmd_component *child;

	if (dev->type == FMD_COMPONENT_SLD) {
		append_endpoint_once(arr, dev->id);
		return;
	}
	/* in LD-ID (pool index) order */
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD)
		by_id[fmd_to_ld(child)->ld_id] = child;
	for (size_t i = 0; i < 256; i++) {
		if (by_id[i])
			append_endpoint_once(arr, by_id[i]->id);
	}
}

/*
 * The target Endpoints at the other end of switch port @port: the SLD on
 * it, each LD of the MLD on it, or for a head of a Multi-Headed device the
 * head if it presents an MLD, the pool LDs mapped to it if it presents an
 * SLD, and none if what it presents is not known.
 * Caller holds the topology read lock.
 */
static void append_port_endpoints(json_t *arr, struct fmd_port *port)
{
	struct fmd_component *child;

	fmd_for_each_child(&port->base, child) {
		if (child->type == FMD_COMPONENT_SLD || child->type == FMD_COMPONENT_MLD)
			append_device_endpoints(arr, child);
	}
	if (port->head && port->head->base.parent) {
		if (port->head->presentation == FMD_HEAD_MLD) {
			append_endpoint_once(arr, port->head->base.id);
		} else if (port->head->presentation == FMD_HEAD_SLD) {
			struct fmd_component *by_id[256] = { NULL };

			fmd_for_each_child_type(port->head->base.parent, child,
						FMD_COMPONENT_LD) {
				if (fmd_to_ld(child)->binding_point_id == port->head->head_id)
					by_id[fmd_to_ld(child)->ld_id] = child;
			}
			for (size_t i = 0; i < 256; i++) {
				if (by_id[i])
					append_endpoint_once(arr, by_id[i]->id);
			}
		}
	}
}

/* The pool LDs of a Multi-Headed device mapped to @head, in LD-ID order */
static void append_head_lds(json_t *arr, const struct fmd_head *head, bool endpoints)
{
	struct fmd_component *by_id[256] = { NULL };
	struct fmd_component *child, *mhd = head->base.parent;

	if (!mhd)
		return;
	fmd_for_each_child_type(mhd, child, FMD_COMPONENT_LD) {
		if (fmd_to_ld(child)->binding_point_id == head->head_id)
			by_id[fmd_to_ld(child)->ld_id] = child;
	}
	for (size_t i = 0; i < 256; i++) {
		if (!by_id[i])
			continue;
		if (endpoints)
			append_endpoint_once(arr, by_id[i]->id);
		else
			json_array_append_new(arr,
				rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%zu",
					mhd->id, mhd->id, i));
	}
}

/*
 * The TargetEndpoints of a Connection: the Endpoint of what the binding
 * reaches (an SLD, an LD, or a head that presents an MLD); for a
 * direct-attached MLD each of its LDs, and for a direct-attached head the
 * pool LDs mapped to it. None while Absent. Topology read lock held.
 */
static void append_conn_targets(json_t *arr, const struct fmd_conn_view *v)
{
	const struct fmd_component *t = v->target;

	if (!t || v->state == FMD_CONN_ABSENT)
		return;
	if (v->kind == FMD_CONN_DIRECT) {
		if (t->type == FMD_COMPONENT_HEAD)
			append_head_lds(arr, v->head, true);
		else
			append_device_endpoints(arr, (struct fmd_component *)t);
		return;
	}
	if (is_target_endpoint(t))
		append_endpoint_once(arr, t->id);
}

/* The CXLLogicalDevices a Connection reaches, as for its targets */
static void append_conn_lds(json_t *arr, const struct fmd_conn_view *v)
{
	const struct fmd_component *t = v->target, *child;

	if (!t || v->state == FMD_CONN_ABSENT)
		return;
	if (t->type == FMD_COMPONENT_SLD) {
		json_array_append_new(arr,
			rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/0",
				t->id, t->id));
	} else if (t->type == FMD_COMPONENT_LD && t->parent) {
		json_array_append_new(arr,
			rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%u",
				t->parent->id, t->parent->id, fmd_to_ld((struct fmd_component *)t)->ld_id));
	} else if (t->type == FMD_COMPONENT_MLD && v->kind == FMD_CONN_DIRECT) {
		fmd_for_each_child_type(t, child, FMD_COMPONENT_LD)
			json_array_append_new(arr,
				rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%u",
					t->id, t->id, fmd_to_ld((struct fmd_component *)child)->ld_id));
	} else if (t->type == FMD_COMPONENT_HEAD && v->kind == FMD_CONN_DIRECT) {
		append_head_lds(arr, v->head, false);
	}
}

struct chunk_info_arg {
	json_t *arr;
	const char *access_state;
};

static int chunk_info(const struct fmd_mem_chunk *chunk, void *arg)
{
	struct chunk_info_arg *a = arg;
	char uri[RF_CHUNK_URI_LEN];

	if (chunk_uri(uri, sizeof(uri), chunk))
		return 0;
	/*
	 * Read and Write: Bind vPPB has no field for how a host may use what
	 * is bound (Table 7-35).
	 */
	json_array_append_new(a->arr, json_pack("{s:{s:s}, s:[s, s], s:s}",
		"MemoryChunk", "@odata.id", uri,
		"AccessCapabilities", "Read", "Write",
		"AccessState", a->access_state));
	return 0;
}

/*
 * The AccessState of what a Connection in @state reaches, cxl-fmd's
 * choice among Connection_v1's: Optimized ("in an active and optimized
 * state") once it is bound; Transitioning ("transitioning to a new
 * state") while a bind or an unbind of it is under way, as the binding is
 * then neither made nor gone (Starting, Updating); Unavailable otherwise.
 * An Absent or Unresolved Connection lists no chunk today (conn_json(),
 * fmd_conn_for_each_chunk()): Unavailable is for one that would.
 */
static const char *conn_access_state(enum fmd_conn_state state)
{
	switch (state) {
	case FMD_CONN_ENABLED:
		return "Optimized";
	case FMD_CONN_STARTING:
	case FMD_CONN_UPDATING:
		return "Transitioning";
	default:
		return "Unavailable";
	}
}

/*
 * A Connection, from the view. Its Status is cxl-fmd's choice of meaning
 * for the standard states:
 *  - Starting: a bind of it is under way;
 *  - Updating: an unbind of it is under way;
 *  - Enabled, OK: bound, and what it reaches is known;
 *  - Absent: bound, and no device is on the port. "This function or device
 *    is not currently present or detected" (Resource_v1); it has no target
 *    nor memory to link to;
 *  - Enabled, Warning: bound, and what it reaches is not known; or a
 *    direct-attached Connection that is inferred (direct_origin()).
 * Topology read lock held.
 */
/*
 * Where a direct-attached Connection comes from (cxl-fmd's Oem.Cxl.Origin):
 * "Configured" for a head of a Multi-Headed device whose host the topology
 * names (attached_host_id, which only a mock topology sets), "Inferred"
 * for an SLD or MLD given to the only direct-attached host. An inferred
 * one rests on no evidence of a CXL.mem link: that a device answers on a
 * management interface (MCTP) does not say which root port, if any, it
 * is linked to, and no CXL command reports that. It is Enabled, so that
 * the host has its MemoryDomain, with Health Warning.
 */
static const char *direct_origin(const struct fmd_conn_view *v)
{
	return v->head ? "Configured" : "Inferred";
}

static json_t *conn_json(const struct fmd_conn_view *v)
{
	json_t *targets = json_array(), *chunks = json_array(), *links, *result;
	const char *state = "Enabled", *health = "OK";

	switch (v->state) {
	case FMD_CONN_STARTING:
		state = "Starting";
		break;
	case FMD_CONN_UPDATING:
		state = "Updating";
		break;
	case FMD_CONN_ABSENT:
		state = "Absent";
		break;
	case FMD_CONN_UNRESOLVED:
		health = "Warning";
		break;
	case FMD_CONN_ENABLED:
		if (v->kind == FMD_CONN_DIRECT && !strcmp(direct_origin(v), "Inferred"))
			health = "Warning";
		break;
	}

	append_conn_targets(targets, v);
	if (v->state != FMD_CONN_ABSENT) {
		struct chunk_info_arg a = {
			.arr = chunks,
			.access_state = conn_access_state(v->state),
		};

		fmd_conn_for_each_chunk(v, chunk_info, &a);
	}

	links = json_pack("{s:[{s:s}]}", "InitiatorEndpoints", "@odata.id",
			  rf_uri(0, "/redfish/v1/Fabrics/CXL/Endpoints/%s", v->host->base.id));
	if (json_array_size(targets))
		json_object_set_new(links, "TargetEndpoints", targets);
	else
		json_decref(targets);

	result = json_pack("{s:s, s:s, s:s, s:s, s:s, s:o, s:o}",
		"@odata.id", rf_uri(0, "/redfish/v1/Fabrics/CXL/Connections/%s", v->id),
		"@odata.type", "#Connection.v1_4_0.Connection",
		"Id", v->id,
		"Name", v->kind == FMD_CONN_DIRECT ? "CXL Direct-Attached Connection" :
						     "CXL vPPB Binding",
		"ConnectionType", "Memory",
		"Status", rf_status(state, health),
		"Links", links);
	if (json_array_size(chunks))
		json_object_set_new(result, "MemoryChunkInfo", chunks);
	else
		json_decref(chunks);

	/*
	 * A vPPB binding (cxl-fmd's Oem): Origin "Service" if this run of
	 * cxl-fmd made it, else "Discovered". Advisory only, and lost on
	 * restart: nothing the switch reports says who made a binding
	 * (Table 7-34, Table 7-80). ForceUnbind unbinds it with Simulate
	 * Surprise Hot-Remove.
	 */
	if (v->kind == FMD_CONN_SWITCH) {
		json_object_set_new(result, "Oem", json_pack("{s:{s:s, s:s}}", "Cxl",
			"@odata.type", "#Cxl.v1_0_0.Connection",
			"Origin", v->made_here ? "Service" : "Discovered"));
		json_object_set_new(result, "Actions", json_pack("{s:{s:{s:s}}}",
			"Oem", "#Cxl.ForceUnbind", "target",
			rf_uri(0, "/redfish/v1/Fabrics/CXL/Connections/%s/Actions/Oem/Cxl.ForceUnbind",
			       v->id)));
	} else {
		json_object_set_new(result, "Oem", json_pack("{s:{s:s, s:s}}", "Cxl",
			"@odata.type", "#Cxl.v1_0_0.Connection",
			"Origin", direct_origin(v)));
	}
	return result;
}

/* Links.Connections of an Endpoint: each Connection it is an end of */
struct endpoint_conns {
	const char *id;
	bool host;
	json_t *links;
};

static int endpoint_conn(const struct fmd_conn_view *v, void *arg)
{
	struct endpoint_conns *e = arg;
	char uri[256];
	bool member = false;

	if (e->host) {
		member = !strcmp(v->host->base.id, e->id);
	} else {
		json_t *targets = json_array(), *t;
		size_t i;

		append_conn_targets(targets, v);
		snprintf(uri, sizeof(uri), "/redfish/v1/Fabrics/CXL/Endpoints/%s", e->id);
		json_array_foreach(targets, i, t) {
			if (!strcmp(json_string_value(json_object_get(t, "@odata.id")), uri))
				member = true;
		}
		json_decref(targets);
	}
	if (member)
		json_array_append_new(e->links,
			rf_link("/redfish/v1/Fabrics/CXL/Connections/%s", v->id));
	return 0;
}

/* Set Links.Connections of Endpoint @id, if it has any. Topology read lock held */
static void set_endpoint_connections(struct fmd_ctx *ctx, json_t *result,
				     const char *id, bool host)
{
	struct endpoint_conns e = { .id = id, .host = host, .links = json_array() };
	json_t *links;

	fmd_conn_for_each(ctx, endpoint_conn, &e);
	if (!json_array_size(e.links)) {
		json_decref(e.links);
		return;
	}
	links = json_object_get(result, "Links");
	if (!links) {
		links = json_object();
		json_object_set_new(result, "Links", links);
	}
	json_object_set_new(links, "Connections", e.links);
}

/*
 * Endpoints Collection - /redfish/v1/Fabrics/{fabric_id}/Endpoints
 *
 * The target Endpoints, an SLD, an LD of an MLD or of a Multi-Headed
 * device, a head that presents an MLD, and the hosts (initiators).
 */
json_t *fmd_redfish_endpoints_collection(struct fmd_ctx *ctx, const char *fabric_id)
{
	struct rf_collection coll;
	struct fmd_component *comp;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Fabrics/%s/Endpoints", fabric_id),
			   "#EndpointCollection.EndpointCollection",
			   "Endpoint Collection");

	fmd_topology_rdlock(&ctx->topology);

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SLD)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_LD) {
		if (is_target_endpoint(comp))
			rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);
	}

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_HEAD) {
		if (is_target_endpoint(comp))
			rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);
	}

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_HOST)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);

	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * Endpoint - /redfish/v1/Fabrics/{fabric_id}/Endpoints/{endpoint_id}
 *
 * Target Endpoints have EntityRole=Target, host Endpoints
 * EntityRole=Initiator. Per OCP CMS Figure 7, endpoints link to switch
 * ports via ConnectedPorts.
 */
json_t *fmd_redfish_endpoint(struct fmd_ctx *ctx, const char *fabric_id,
			     const char *endpoint_id)
{
	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_component *comp = fmd_topology_find(&ctx->topology, endpoint_id);
	if (!comp)
		return NULL;

	/* A target that has an Endpoint of its own, or a host */
	if (!is_target_endpoint(comp) && comp->type != FMD_COMPONENT_HOST)
		return NULL;

	char uri[sizeof(RF_ENDPOINTS) + FMD_MAX_ID_LEN];
	if (endpoint_uri(uri, sizeof(uri), endpoint_id))
		return NULL;

	/*
	 * Host endpoints (Initiators). What the FM knows of a host is the CXL
	 * root port its Virtual Hierarchy starts at, "Everything from the CXL
	 * RP down" (CXL r4.0 Table 1-1, VH), not a processor, and cxl-fmd
	 * serves no Processor resource. RootComplex: "The `EntityLink`
	 * property, if present, should be of type `ComputerSystem`"
	 * (Endpoint_v1), which is the host's System. That a CXL host is a
	 * RootComplex is cxl-fmd's choice: DMTF names no EntityType for it.
	 */
	if (comp->type == FMD_COMPONENT_HOST) {
		struct fmd_host *host = fmd_to_host(comp);
		json_t *result = json_pack(
			"{"
			"  s:s,"   /* @odata.id */
			"  s:s,"   /* @odata.type */
			"  s:s,"   /* Id */
			"  s:s,"   /* Name */
			"  s:s,"   /* EndpointProtocol */
			"  s:[{s:s, s:s, s:o}],"  /* ConnectedEntities */
			"  s:o"    /* Status */
			"}",
			"@odata.id", uri,
			"@odata.type", "#Endpoint.v1_8_2.Endpoint",
			"Id", endpoint_id,
			"Name", host->name,
			"EndpointProtocol", "CXL",
			"ConnectedEntities",
				"EntityType", "RootComplex",
				"EntityRole", "Initiator",
				"EntityLink", rf_link("/redfish/v1/Systems/%s", host->base.id),
			"Status", rf_status("Enabled", "OK")
		);

		/*
		 * For switch-attached hosts, add link to the Zone (VCS).
		 * Find the switch by walking topology for components with VCS
		 * that match this host's vcs_id.
		 */
		if (!host->direct_attached) {
			struct fmd_component *sw_comp;
			for (int i = 0; i < FMD_HASH_BUCKETS; i++) {
				for (sw_comp = ctx->topology.components.buckets[i];
				     sw_comp; sw_comp = sw_comp->hash_next) {
					if (sw_comp->type != FMD_COMPONENT_SWITCH)
						continue;
					struct fmd_switch *sw = fmd_to_switch(sw_comp);
					if (!strcmp(sw->base.id, host->switch_id) &&
					    fmd_switch_find_vcs(sw, host->vcs_id)) {
						json_object_set_new(result, "Links",
							rf_links_array_single("Zones",
								"/redfish/v1/Fabrics/CXL/Zones/%s_vcs%d",
								sw->base.id, host->vcs_id));
						goto host_done;
					}
				}
			}
		}
host_done:
		fmd_topology_rdlock(&ctx->topology);
		set_endpoint_connections(ctx, result, endpoint_id, true);
		fmd_topology_rdunlock(&ctx->topology);
		return result;
	}

	/*
	 * Target Endpoints: an SLD, an LD of an MLD or of a Multi-Headed
	 * device, or a head that presents an MLD.
	 */
	struct fmd_component *dev = comp;	/* the device it is on */
	struct cxlmi_cmd_identify_rsp *identify;
	struct fmd_port *port = NULL;		/* the switch port it is reached on */
	const char *name;
	const char *entity_type = "CXLDevice";
	json_t *entity_link = NULL;
	char vendor_str[16];
	char device_str[16];

	fmd_topology_rdlock(&ctx->topology);

	if (comp->type == FMD_COMPONENT_LD || comp->type == FMD_COMPONENT_HEAD)
		dev = comp->parent;
	if (!dev) {
		fmd_topology_rdunlock(&ctx->topology);
		return NULL;
	}

	if (dev->type == FMD_COMPONENT_SLD)
		identify = &fmd_to_sld(dev)->identify;
	else if (dev->type == FMD_COMPONENT_MLD)
		identify = &fmd_to_mld(dev)->identify;
	else
		identify = &fmd_to_mhd(dev)->identify;
	snprintf(vendor_str, sizeof(vendor_str), "0x%04X", identify->vendor_id);
	snprintf(device_str, sizeof(device_str), "0x%04X", identify->device_id);

	if (comp->type == FMD_COMPONENT_SLD) {
		/*
		 * An SLD is one logical device, LD 0 of its CXLLogicalDevices,
		 * and what a host is bound to is that device whole (Bind vPPB,
		 * CXL r4.0 Section 7.6.7.2.2). CXLDevice: "the entity this
		 * endpoint represents is a CXL logical device. The
		 * `EntityLink` property, if present, should be of type
		 * `CXLLogicalDevice`" (Endpoint_v1). That a Type 3 target is a
		 * CXLDevice is cxl-fmd's choice: DMTF names no EntityType for
		 * it.
		 */
		name = "CXL SLD Endpoint";
		entity_link = rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/0",
				      dev->id, dev->id);
	} else if (comp->type == FMD_COMPONENT_LD) {
		/* An LD of an MLD, or a pool LD of a Multi-Headed device */
		struct fmd_ld *ld = fmd_to_ld(comp);

		name = "CXL Logical Device Endpoint";
		entity_link = rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%u",
				      dev->id, dev->id, ld->ld_id);
		if (dev->type == FMD_COMPONENT_MHD) {
			struct fmd_head *head = fmd_ld_head(ld);

			port = head ? head->port : NULL;
		}
	} else {
		/*
		 * A head that presents an MLD: Memory, "the entity this
		 * endpoint represents is a memory device" (Endpoint_v1). Which
		 * pool LD each of its LD-IDs is, is the device's (Section
		 * 2.5.1). No EntityLink: no resource is the head.
		 */
		name = "CXL Multi-Headed Device Head Endpoint";
		entity_type = "Memory";
		port = fmd_to_head(comp)->port;
	}

	if (dev->parent && dev->parent->type == FMD_COMPONENT_PORT)
		port = fmd_to_port(dev->parent);

	json_t *entity = json_pack("{s:s, s:s, s:{s:s, s:s}}",
		"EntityType", entity_type,
		"EntityRole", "Target",
		"EntityPciId", "VendorId", vendor_str, "DeviceId", device_str);
	if (entity_link)
		json_object_set_new(entity, "EntityLink", entity_link);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* EndpointProtocol */
		"  s:[o],"  /* ConnectedEntities */
		"  s:o"    /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#Endpoint.v1_8_2.Endpoint",
		"Id", endpoint_id,
		"Name", name,
		"EndpointProtocol", "CXL",
		"ConnectedEntities", entity,
		"Status", rf_status("Enabled", "OK")
	);

	/*
	 * Per OCP CMS Figure 7, endpoints connect to switch ports: the port
	 * the device is on, or that of the head a pool LD is mapped to, and
	 * Links.Zones, each VCS a vPPB of which is bound to this Endpoint.
	 */
	if (result && port && port->base.parent &&
	    port->base.parent->type == FMD_COMPONENT_SWITCH) {
		struct fmd_switch *sw = fmd_to_switch(port->base.parent);
		json_t *links = json_object();
		json_t *zones = json_array();
		struct fmd_component *sw_comp;

		json_object_set_new(links, "ConnectedPorts",
			json_pack("[o]", rf_link("/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
						 sw->base.id, port->base.id)));

		fmd_for_each_type(&ctx->topology, sw_comp, FMD_COMPONENT_SWITCH) {
			struct fmd_switch *s = fmd_to_switch(sw_comp);

			for (int v = 0; v < s->num_vcs; v++) {
				struct fmd_vcs *vcs = &s->vcs[v];

				for (int p = 0; p < vcs->num_vppbs; p++) {
					if (vppb_target(&vcs->vppbs[p]) != comp)
						continue;
					json_array_append_new(zones,
						rf_link("/redfish/v1/Fabrics/CXL/Zones/%s_vcs%d",
							s->base.id, vcs->vcs_id));
					break; /* each VCS once */
				}
			}
		}

		if (json_array_size(zones) > 0)
			json_object_set_new(links, "Zones", zones);
		else
			json_decref(zones);

		json_object_set_new(result, "Links", links);
	}
	if (result)
		set_endpoint_connections(ctx, result, endpoint_id, false);

	fmd_topology_rdunlock(&ctx->topology);

	return result;
}

/*
 * Connections Collection - /redfish/v1/Fabrics/{fabric_id}/Connections
 *
 * Each vPPB binding of a host's VCS to a memory device, whoever made it,
 * and each direct-attached host and device: see fmd_conn_for_each(). None
 * is stored. A device that no host is bound to has none: a Connection
 * "represents the connection information" between initiators and targets
 * (Connection_v1).
 */
static int conn_member(const struct fmd_conn_view *v, void *arg)
{
	rf_collection_add(arg, "/redfish/v1/Fabrics/CXL/Connections/%s", v->id);
	return 0;
}

json_t *fmd_redfish_connections_collection(struct fmd_ctx *ctx, const char *fabric_id)
{
	struct rf_collection coll;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Fabrics/%s/Connections", fabric_id),
			   "#ConnectionCollection.ConnectionCollection",
			   "Connection Collection");

	fmd_topology_rdlock(&ctx->topology);
	fmd_conn_for_each(ctx, conn_member, &coll);
	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * Connection - /redfish/v1/Fabrics/{fabric_id}/Connections/{connection_id}
 *
 * Computed when it is asked for, from the VCS tables as last read and the
 * binds and unbinds under way: an ID whose vPPB no longer holds what the
 * ID names is not found. GET sends no command, so it is as old as the
 * last time the switch was read (cxl-fmd's choice).
 */
json_t *fmd_redfish_connection(struct fmd_ctx *ctx, const char *fabric_id,
			       const char *connection_id)
{
	struct fmd_conn_view v;
	json_t *result = NULL;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	fmd_topology_rdlock(&ctx->topology);
	if (!fmd_conn_lookup(ctx, connection_id, &v))
		result = conn_json(&v);
	fmd_topology_rdunlock(&ctx->topology);
	return result;
}

/*
 * Zones Collection - /redfish/v1/Fabrics/{fabric_id}/Zones
 *
 * Per DMTF Redfish CXL model, Zones group endpoints that can be accessed
 * together. In CXL, each VCS (Virtual CXL Switch) represents a zone -
 * the devices bound to a VCS are accessible from that VCS's host.
 */
json_t *fmd_redfish_zones_collection(struct fmd_ctx *ctx, const char *fabric_id)
{
	struct rf_collection coll;
	struct fmd_component *comp;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Fabrics/%s/Zones", fabric_id),
			   "#ZoneCollection.ZoneCollection",
			   "Zone Collection");

	fmd_topology_rdlock(&ctx->topology);

	/* Each VCS on each switch is a zone */
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SWITCH) {
		struct fmd_switch *sw = fmd_to_switch(comp);
		for (int i = 0; i < sw->num_vcs; i++)
			rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Zones/%s_vcs%d",
					  sw->base.id, sw->vcs[i].vcs_id);
	}

	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * Zone - /redfish/v1/Fabrics/{fabric_id}/Zones/{zone_id}
 *
 * A Zone represents a VCS and contains links to its host and to each
 * target Endpoint a vPPB of the VCS is bound to (see vppb_target()): an
 * MLD whose LD 0 is bound to VCS 0 and LD 1 to VCS 1 is its LD 0 Endpoint
 * in the one Zone and its LD 1 Endpoint in the other.
 */
json_t *fmd_redfish_zone(struct fmd_ctx *ctx, const char *fabric_id,
			 const char *zone_id)
{
	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	/* Parse zone_id: expected format "{switch_id}_vcs{N}" */
	char switch_id[FMD_MAX_ID_LEN];
	int vcs_num;
	char *vcs_part = strstr(zone_id, "_vcs");
	if (!vcs_part)
		return NULL;

	size_t switch_len = vcs_part - zone_id;
	if (switch_len >= FMD_MAX_ID_LEN)
		return NULL;

	strncpy(switch_id, zone_id, switch_len);
	switch_id[switch_len] = '\0';
	vcs_num = safe_parse_int(vcs_part + 4);
	if (vcs_num < 0)
		return NULL;

	/* Find the switch */
	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, switch_id);
	if (!sw)
		return NULL;

	/* Find the VCS */
	struct fmd_vcs *vcs = NULL;
	for (int i = 0; i < sw->num_vcs; i++) {
		if (sw->vcs[i].vcs_id == vcs_num) {
			vcs = &sw->vcs[i];
			break;
		}
	}
	if (!vcs)
		return NULL;

	char uri[256];
	snprintf(uri, sizeof(uri), "/redfish/v1/Fabrics/CXL/Zones/%s", zone_id);

	/* Build the endpoints array from bound vPPBs and hosts */
	json_t *endpoints = json_array();
	struct fmd_component *child;

	fmd_topology_rdlock(&ctx->topology);

	/* Add host endpoints connected to this VCS */
	struct fmd_fabric *fabric = ctx->topology.root;
	if (fabric) {
		fmd_for_each_child(&fabric->base, child) {
			if (child->type == FMD_COMPONENT_HOST) {
				struct fmd_host *host = fmd_to_host(child);
				/* the host on this VCS of this switch */
				if (!host->direct_attached &&
				    host->vcs_id == vcs_num &&
				    strcmp(host->switch_id, switch_id) == 0) {
					json_array_append_new(endpoints,
						rf_link("/redfish/v1/Fabrics/CXL/Endpoints/%s",
							host->base.id));
				}
			}
		}
	}

	/* Add the target endpoints of bound vPPBs, each once */
	for (int i = 0; i < vcs->num_vppbs; i++) {
		struct fmd_component *target = vppb_target(&vcs->vppbs[i]);

		if (target)
			append_endpoint_once(endpoints, target->id);
	}

	fmd_topology_rdunlock(&ctx->topology);

	/* Build Zone resource */
	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* ZoneType */
		"  s:o,"   /* Status */
		"  s:{"    /* Links */
		"    s:o"  /* Endpoints */
		"  }"
		"}",
		"@odata.id", uri,
		"@odata.type", "#Zone.v1_6_2.Zone",
		"Id", zone_id,
		"Name", "CXL Virtual Switch Zone",
		"ZoneType", "ZoneOfEndpoints",
		"Status", rf_status("Enabled", "OK"),
		"Links",
			"Endpoints", endpoints
	);

	return result;
}

/*
 * Switches Collection - /redfish/v1/Fabrics/{fabric_id}/Switches
 */
json_t *fmd_redfish_switches_collection(struct fmd_ctx *ctx, const char *fabric_id)
{
	struct rf_collection coll;
	struct fmd_component *child;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_fabric *fabric = ctx->topology.root;
	if (!fabric)
		return NULL;

	rf_collection_init(&coll, "/redfish/v1/Fabrics/CXL/Switches",
			   "#SwitchCollection.SwitchCollection",
			   "CXL Switch Collection");

	fmd_topology_rdlock(&ctx->topology);
	fmd_for_each_child_type(&fabric->base, child, FMD_COMPONENT_SWITCH) {
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Switches/%s",
				  child->id);
	}
	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * Switch - /redfish/v1/Fabrics/{fabric_id}/Switches/{switch_id}
 */
json_t *fmd_redfish_switch(struct fmd_ctx *ctx, const char *fabric_id, const char *switch_id)
{
	char uri[256];
	char ports_uri[256];

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, switch_id);
	if (!sw)
		return NULL;

	snprintf(uri, sizeof(uri), "/redfish/v1/Fabrics/CXL/Switches/%s", switch_id);
	snprintf(ports_uri, sizeof(ports_uri), "/redfish/v1/Fabrics/CXL/Switches/%s/Ports",
		 switch_id);

	/*
	 * No Manufacturer: it "shall contain the name of the organization
	 * responsible for producing the switch" (Switch_v1), and only the PCI
	 * Vendor ID is known.
	 */
	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* SwitchType */
		"  s:{s:s},"  /* Ports */
		"  s:o"    /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#Switch.v1_10_0.Switch",
		"Id", switch_id,
		"Name", "CXL Switch",
		"SwitchType", "CXL",
		"Ports", "@odata.id", ports_uri,
		"Status", rf_status("Enabled", "OK")
	);

	/*
	 * DSP0288 Table 2: Identify Switch Device mappings
	 * - Number of VCSs -> CXL/MaxVCSSupported
	 * - Total Number of vPPBs -> CXL/TotalNumbervPPBs
	 * - Total Number of Bound vPPBs -> CXL/NumberOfBoundvPPBs
	 * - Number of HDM Decoders available per USP -> CXL/HDMDecoders
	 *
	 * DSP8010's Switch has no CXL/MaxVCSSupported or CXL/HDMDecoders.
	 * They are served as CXL/MaxVCSsSupported and CXL/TotalHDMDecoders
	 * ("The total number of HDM (Host Device Memory) decoders available
	 * per upstream port", Switch_v1), which is cxl-fmd's reading.
	 */
	int total_vppbs = sw->sw_identify.num_total_vppb;
	int bound_vppbs = sw->sw_identify.num_active_vppb;

	/*
	 * Number of Bound vPPBs is the "total number of vPPBs, across all
	 * VCSs, that are bound". Identify Switch Device said how many were
	 * when the switch was discovered; the VCSs say how many are now.
	 */
	if (sw->num_vcs) {
		bound_vppbs = 0;
		for (int v = 0; v < sw->num_vcs; v++) {
			for (int b = 0; b < sw->vcs[v].num_vppbs; b++) {
				if (sw->vcs[v].vppbs[b].binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT ||
				    sw->vcs[v].vppbs[b].binding_status == FMD_VPPB_BOUND_LD ||
				    sw->vcs[v].vppbs[b].binding_status == FMD_VPPB_BOUND_PBR)
					bound_vppbs++;
			}
		}
	}
	int hdm_decoders = sw->sw_identify.num_hdm_decoder_per_usp;

	/* Build VCSs collection URI */
	char vcs_uri[256];
	snprintf(vcs_uri, sizeof(vcs_uri),
		 "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs", switch_id);

	/*
	 * Check if switch is PBR capable (DSP0288 Table 2: Get Physical Port State)
	 * PBR Capable if any port supports PBR mode (bit 4 of port_cxl_ver_bitmask)
	 */
	int pbr_capable = 0;
	struct fmd_component *port_child;
	fmd_for_each_child(&sw->base, port_child) {
		if (port_child->type == FMD_COMPONENT_PORT) {
			struct fmd_port *port = fmd_to_port(port_child);
			if (port->port_cxl_ver_bitmask & 0x10) {
				pbr_capable = 1;
				break;
			}
		}
	}

	/* Add CXL-specific properties */
	json_object_set_new(result, "CXL",
		json_pack("{s:i, s:i, s:i, s:i, s:b, s:{s:s}}",
			  "MaxVCSsSupported", sw->max_vcs,
			  "TotalNumbervPPBs", total_vppbs,
			  "NumberOfBoundvPPBs", bound_vppbs,
			  "TotalHDMDecoders", hdm_decoders,
			  "PBRCapable", pbr_capable,
			  "VirtualCXLSwitches", "@odata.id", vcs_uri));

	return result;
}

/*
 * Ports Collection - /redfish/v1/Fabrics/{fabric_id}/Switches/{switch_id}/Ports
 */
json_t *fmd_redfish_ports_collection(struct fmd_ctx *ctx, const char *fabric_id,
				      const char *switch_id)
{
	struct rf_collection coll;
	struct fmd_component *child;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, switch_id);
	if (!sw)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Fabrics/CXL/Switches/%s/Ports", switch_id),
			   "#PortCollection.PortCollection",
			   "Port Collection");

	fmd_topology_rdlock(&ctx->topology);
	fmd_for_each_child_type(&sw->base, child, FMD_COMPONENT_PORT) {
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
				  switch_id, child->id);
	}
	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * Port - /redfish/v1/Fabrics/{fabric_id}/Switches/{switch_id}/Ports/{port_id}
 */
json_t *fmd_redfish_port(struct fmd_ctx *ctx, const char *fabric_id,
			  const char *switch_id, const char *port_id)
{
	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	/* Find port by its full ID */
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, port_id);
	if (!comp || comp->type != FMD_COMPONENT_PORT)
		return NULL;

	struct fmd_port *port = fmd_to_port(comp);

	/* Verify this port belongs to the specified switch */
	if (!comp->parent || comp->parent->type != FMD_COMPONENT_SWITCH ||
	    strcmp(comp->parent->id, switch_id) != 0)
		return NULL;

	/* Map port state to Redfish */
	const char *link_state = rf_link_state_str(port->link_status);
	const char *link_status = "LinkDown";
	if (port->link_status == FMD_LINK_UP)
		link_status = "LinkUp";
	else if (port->link_status == FMD_LINK_TRAINING)
		link_status = "Training";

	/* Map connected device type to Port.CXL.ConnectedDeviceType enum */
	const char *conn_dev_type_str = "None";
	switch (port->conn_dev_type) {
	case FMD_CONN_PCIE:  conn_dev_type_str = "PCIeDevice"; break;
	case FMD_CONN_TYPE1: conn_dev_type_str = "Type1"; break;
	case FMD_CONN_TYPE2: conn_dev_type_str = "Type2"; break;
	case FMD_CONN_SLD:   conn_dev_type_str = "Type3SLD"; break;
	case FMD_CONN_MLD:   conn_dev_type_str = "Type3MLD"; break;
	case FMD_CONN_PBR:   conn_dev_type_str = "PBRComponent"; break;
	default: break;
	}

	/* Format PortId as string per schema */
	char port_id_str[16];
	snprintf(port_id_str, sizeof(port_id_str), "%d", port->port_number);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* PortId */
		"  s:s,"   /* PortProtocol */
		"  s:o,"   /* Status */
		"  s:s,"   /* LinkStatus */
		"  s:i,"   /* Width */
		"  s:i"    /* ActiveWidth */
		"}",
		"@odata.id", rf_uri(0, "/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
				    switch_id, port_id),
		"@odata.type", "#Port.v1_17_0.Port",
		"Id", port_id,
		"Name", "CXL Switch Port",
		"PortId", port_id_str,
		"PortProtocol", "CXL",
		"Status", rf_status(link_state, "OK"),
		"LinkStatus", link_status,
		"Width", port->max_link_width,
		"ActiveWidth", port->negotiated_link_width
	);

	/*
	 * PortType: a USP "connects to a host device", a DSP "to a target
	 * device", a Fabric Port "to another switch". A port in any other
	 * state has no direction to report. DSP0288 maps no CXL field to
	 * PortType: deriving it from Current Port Configuration State is
	 * cxl-fmd's.
	 */
	const char *port_type = NULL;
	switch (port->state) {
	case FMD_PORT_USP:    port_type = "UpstreamPort"; break;
	case FMD_PORT_DSP:    port_type = "DownstreamPort"; break;
	case FMD_PORT_FABRIC: port_type = "InterswitchPort"; break;
	default: break;
	}
	if (port_type)
		json_object_set_new(result, "PortType", json_string(port_type));

	/*
	 * Speeds, as DSP0288 maps them from Get Physical Port State: Max
	 * Link Speed to MaxSpeedGbps, Current Link Speed to CurrentSpeedGbps
	 * and the Supported Link Speeds Vector to CapableLinkSpeedGbps. Each
	 * "matches" the PCIe field (Table 7-21). The PCIe encoding is 1 for
	 * 2.5 GT/s up to 6 for 64 GT/s and 7 for 128 GT/s, the rate CXL 4.0
	 * runs at (Section 1.4.1); that 7 is 128 GT/s is from PCIe 7.0, not
	 * the CXL specification. Bit n of the vector is encoding n + 1, and
	 * Table 7-21 has only Bits[5:0] of it, up to 64 GT/s: Bits[7:6] are
	 * Reserved. A speed that is not known is left out.
	 */
	static const double pcie_gts[] = { 0, 2.5, 5, 8, 16, 32, 64, 128 };
	const int nspeeds = sizeof(pcie_gts) / sizeof(pcie_gts[0]);
	const int vector_bits = 6;

	if (port->max_link_speed > 0 && port->max_link_speed < nspeeds)
		json_object_set_new(result, "MaxSpeedGbps",
			json_real(pcie_gts[port->max_link_speed]));
	if (port->link_status != FMD_LINK_UP)
		json_object_set_new(result, "CurrentSpeedGbps", json_integer(0));
	else if (port->current_link_speed > 0 && port->current_link_speed < nspeeds)
		json_object_set_new(result, "CurrentSpeedGbps",
			json_real(pcie_gts[port->current_link_speed]));

	json_t *capable_speeds = json_array();
	for (int bit = 0; bit < vector_bits; bit++) {
		if (port->supported_link_speeds & (1 << bit))
			json_array_append_new(capable_speeds,
					      json_real(pcie_gts[bit + 1]));
	}

	json_object_set_new(result, "LinkConfiguration",
		json_pack("[{s:o}]", "CapableLinkSpeedGbps", capable_speeds));

	/* Map port state to CXL.CurrentPortConfigurationState enum */
	const char *port_config_state = "Disabled";
	switch (port->state) {
	case FMD_PORT_DISABLED:           port_config_state = "Disabled"; break;
	case FMD_PORT_BIND_IN_PROGRESS:   port_config_state = "BindInProgress"; break;
	case FMD_PORT_UNBIND_IN_PROGRESS: port_config_state = "UnbindInProgress"; break;
	case FMD_PORT_DSP:                port_config_state = "DSP"; break;
	case FMD_PORT_USP:                port_config_state = "USP"; break;
	case FMD_PORT_FABRIC:             port_config_state = "FabricLink"; break;
	case FMD_PORT_RESERVED:           port_config_state = "Reserved"; break;
	}

	/* Map conn_dev_cxl_ver to ConnectedDeviceMode enum */
	const char *conn_dev_mode = "Disconnected";
	switch (port->conn_dev_cxl_ver) {
	case 1: conn_dev_mode = "RCD"; break;
	case 2: conn_dev_mode = "CXL68BFlitAndVH"; break;
	case 3: conn_dev_mode = "Standard256BFlit"; break;
	case 4: conn_dev_mode = "CXLLatencyOptimized256BFlit"; break;
	case 5: conn_dev_mode = "PBR"; break;
	default: break;
	}

	/* Build SupportedCXLModes array from port_cxl_ver_bitmask */
	json_t *supported_modes = json_array();
	if (port->port_cxl_ver_bitmask & 0x01)
		json_array_append_new(supported_modes, json_string("RCD"));
	if (port->port_cxl_ver_bitmask & 0x02)
		json_array_append_new(supported_modes, json_string("CXL68BFlitAndVH"));
	if (port->port_cxl_ver_bitmask & 0x04)
		json_array_append_new(supported_modes, json_string("Standard256BFlit"));
	if (port->port_cxl_ver_bitmask & 0x08)
		json_array_append_new(supported_modes, json_string("CXLLatencyOptimized256BFlit"));
	if (port->port_cxl_ver_bitmask & 0x10)
		json_array_append_new(supported_modes, json_string("PBR"));

	/* Build CXL extension */
	json_t *cxl_obj = json_pack("{s:i, s:s}",
		"MaxLogicalDeviceCount", port->supported_ld_count,
		"CurrentPortConfigurationState", port_config_state);

	/*
	 * Get Physical Port State reserves Connected Device Mode unless the
	 * port is a DSP, and Connected Device Type unless it is a DSP or a
	 * Fabric Port; Supported CXL Modes is undefined when it is 00h, and
	 * the property "shall not contain the value Disconnected". What the
	 * switch does not define is not reported.
	 */
	if (port->state == FMD_PORT_DSP || port->state == FMD_PORT_FABRIC)
		json_object_set_new(cxl_obj, "ConnectedDeviceType",
				    json_string(conn_dev_type_str));
	if (port->state == FMD_PORT_DSP)
		json_object_set_new(cxl_obj, "ConnectedDeviceMode",
				    json_string(conn_dev_mode));
	if (json_array_size(supported_modes) > 0)
		json_object_set_new(cxl_obj, "SupportedCXLModes", supported_modes);
	else
		json_decref(supported_modes);

	/*
	 * The target Endpoints at the other end of the link: the SLD, each
	 * LD of the MLD, or for a head of a Multi-Headed device the pool LD
	 * mapped to it, or the head itself if it presents an MLD.
	 */
	json_t *associated = json_array();

	fmd_topology_rdlock(&ctx->topology);
	append_port_endpoints(associated, port);
	fmd_topology_rdunlock(&ctx->topology);
	if (json_array_size(associated) > 0)
		json_object_set_new(result, "Links",
			json_pack("{s:o}", "AssociatedEndpoints", associated));
	else
		json_decref(associated);

	struct fmd_component *dev_child;
	fmd_for_each_child(&port->base, dev_child) {
		if (dev_child->type == FMD_COMPONENT_SLD ||
		    dev_child->type == FMD_COMPONENT_MLD) {

			/*
			 * DSP0288 Table 2, Get LD Info: QoS Telemetry
			 * Capabilities[0], Egress Port Congestion Supported, ->
			 * Port CXL/QoSTelemetryCapabilities/
			 * EgressPortBackpressureSupported; Comment 1: "For an
			 * MLD, implementations shall represent Egress Port
			 * Congestion Supported in the Port resource."
			 */
			if (dev_child->type == FMD_COMPONENT_MLD) {
				struct fmd_mld *mld = fmd_to_mld(dev_child);
				json_object_set_new(cxl_obj, "QoSTelemetryCapabilities",
					json_pack("{s:b}",
						  "EgressPortBackpressureSupported",
						  (mld->qos_telemetry_capability & 0x01) ? 1 : 0));

				/*
				 * DSP0288 Table 2: Get QoS Control mappings
				 * Congestion properties for MLD ports
				 */
				json_object_set_new(cxl_obj, "Congestion",
					json_pack("{s:b, s:i, s:i, s:i, s:i, s:i}",
						  "CongestionTelemetryEnabled",
						  mld->qos_control.enabled ? 1 : 0,
						  "EgressModeratePercentage",
						  (int)mld->qos_control.egress_moderate_pct,
						  "EgressSeverePercentage",
						  (int)mld->qos_control.egress_severe_pct,
						  "BackpressureSampleInterval",
						  (int)mld->qos_control.backpressure_sample_interval,
						  "MaxSustainedRequestCmpBias",
						  (int)mld->qos_control.reqcmp_basis,
						  "CompletionCollectionInterval",
						  (int)mld->qos_control.completion_collection_interval));
			}
			break;
		}
	}

	json_object_set_new(result, "CXL", cxl_obj);

	/* Add link to Metrics subordinate resource */
	json_object_set_new(result, "Metrics",
		rf_nav("/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s/Metrics",
		       switch_id, port_id));

	/*
	 * DSP0288 Table 2: Physical Port Control (Opcode 5102h)
	 * Port Opcode 02h: Reset PPB -> Port.ResetPPB action
	 *
	 * Physical Port Control "is used by the FM to control unbound ports
	 * and MLD ports" (CXL r4.0 Section 7.6.7.1.3). Offering it only on a
	 * DSP is cxl-fmd's choice: a USP is a host's link, and is bound to
	 * vPPB 0 of its VCS. Whether a DSP is unbound or has an MLD is left to
	 * the switch to check.
	 */
	if (port->state == FMD_PORT_DSP) {
		char reset_ppb_uri[256];

		snprintf(reset_ppb_uri, sizeof(reset_ppb_uri),
			 "/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s/Actions/Port.ResetPPB",
			 switch_id, port_id);
		json_object_set_new(result, "Actions",
			json_pack("{s:{s:s}}",
				  "#Port.ResetPPB", "target", reset_ppb_uri));
	}

	return result;
}

/*
 * Port Metrics - /redfish/v1/Fabrics/{fabric_id}/Switches/{switch_id}/Ports/{port_id}/Metrics
 *
 * DSP0288 Table 2: Get QoS Status -> PortMetrics.CXL/BackpressureAveragePercentage
 */
json_t *fmd_redfish_port_metrics(struct fmd_ctx *ctx, const char *fabric_id,
				 const char *switch_id, const char *port_id)
{
	char uri[256];

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	/* Find port by its full ID */
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, port_id);
	if (!comp || comp->type != FMD_COMPONENT_PORT)
		return NULL;

	struct fmd_port *port = fmd_to_port(comp);

	/* Verify this port belongs to the specified switch */
	if (!comp->parent || comp->parent->type != FMD_COMPONENT_SWITCH ||
	    strcmp(comp->parent->id, switch_id) != 0)
		return NULL;

	snprintf(uri, sizeof(uri),
		 "/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s/Metrics",
		 switch_id, port_id);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s"    /* Name */
		"}",
		"@odata.id", uri,
		"@odata.type", "#PortMetrics.v1_4_0.PortMetrics",
		"Id", "Metrics",
		"Name", "Port Metrics"
	);

	/*
	 * DSP0288 Table 2, Get QoS Status: Backpressure Average Percentage ->
	 * PortMetrics CXL/BackpressureAveragePercentage. The row has no
	 * comment; it is served for a port with an MLD on it.
	 */
	struct fmd_component *dev_child;
	fmd_for_each_child(&port->base, dev_child) {
		if (dev_child->type == FMD_COMPONENT_MLD) {
			struct fmd_mld *mld = fmd_to_mld(dev_child);
			if (mld->qos_status.backpressure_avg_valid) {
				json_object_set_new(result, "CXL",
					json_pack("{s:i}",
						  "BackpressureAveragePercentage",
						  (int)mld->qos_status.backpressure_avg_pct));
			}
			break;
		}
	}

	return result;
}

/*
 * VCS Collection - /redfish/v1/Fabrics/{fabric_id}/Switches/{switch_id}/VCSs
 */
json_t *fmd_redfish_vcs_collection(struct fmd_ctx *ctx, const char *fabric_id,
				   const char *switch_id)
{
	struct rf_collection coll;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, switch_id);
	if (!sw)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs", switch_id),
			   "#VirtualCXLSwitchCollection.VirtualCXLSwitchCollection",
			   "Virtual CXL Switch Collection");

	for (int i = 0; i < sw->num_vcs; i++)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs/%d",
				  switch_id, sw->vcs[i].vcs_id);

	return rf_collection_finish(&coll);
}

/*
 * VCS - /redfish/v1/Fabrics/{fabric_id}/Switches/{switch_id}/VCSs/{vcs_id}
 */
json_t *fmd_redfish_vcs(struct fmd_ctx *ctx, const char *fabric_id,
			const char *switch_id, const char *vcs_id)
{
	char uri[256];

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, switch_id);
	if (!sw)
		return NULL;

	/* Find the VCS by ID */
	int vcs_idx = parse_prefixed_id(vcs_id, "_vcs");
	if (vcs_idx < 0)
		return NULL;
	struct fmd_vcs *vcs = NULL;
	for (int i = 0; i < sw->num_vcs; i++) {
		if (sw->vcs[i].vcs_id == vcs_idx) {
			vcs = &sw->vcs[i];
			break;
		}
	}
	if (!vcs)
		return NULL;

	snprintf(uri, sizeof(uri), "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs/%s",
		 switch_id, vcs_id);

	char vppbs_uri[256];
	snprintf(vppbs_uri, sizeof(vppbs_uri),
		 "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs/%s/VPPBs",
		 switch_id, vcs_id);

	/* Map VCS state */
	const char *state_str = "Disabled";
	if (vcs->state == 1)
		state_str = "Enabled";

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:o,"   /* Status */
		"  s:s,"   /* VCSId */
		"  s:i,"   /* HDMDecoders */
		"  s:{s:s}"  /* VPPBs */
		"}",
		"@odata.id", uri,
		"@odata.type", "#VirtualCXLSwitch.v1_0_0.VirtualCXLSwitch",
		"Id", vcs_id,
		"Name", "Virtual CXL Switch",
		"Status", rf_status(state_str, "OK"),
		"VCSId", vcs_id,
		"HDMDecoders", vcs->hdm_decoders,
		"VPPBs", "@odata.id", vppbs_uri
	);

	/* Add Links to upstream port */
	json_t *links = rf_links_single("Port",
			"/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s_usp%d",
			switch_id, switch_id, vcs->upstream_port_id);

	/*
	 * Endpoints, "the endpoints for this Virtual CXL Switch"
	 * (VirtualCXLSwitch_v1): its host, and each target Endpoint a vPPB
	 * of it is bound to, each once, as in its Zone.
	 */
	json_t *endpoints = json_array();
	struct fmd_component *child;

	fmd_topology_rdlock(&ctx->topology);
	if (ctx->topology.root) {
		fmd_for_each_child_type(&ctx->topology.root->base, child,
					FMD_COMPONENT_HOST) {
			struct fmd_host *host = fmd_to_host(child);

			if (!host->direct_attached && host->vcs_id == vcs->vcs_id &&
			    !strcmp(host->switch_id, sw->base.id))
				append_endpoint_once(endpoints, child->id);
		}
	}
	for (int i = 0; i < vcs->num_vppbs; i++) {
		struct fmd_component *target = vppb_target(&vcs->vppbs[i]);

		if (target)
			append_endpoint_once(endpoints, target->id);
	}
	fmd_topology_rdunlock(&ctx->topology);

	if (json_array_size(endpoints) > 0)
		json_object_set_new(links, "Endpoints", endpoints);
	else
		json_decref(endpoints);
	json_object_set_new(result, "Links", links);

	return result;
}

/*
 * vPPB Collection - /redfish/v1/Fabrics/{fabric_id}/Switches/{switch_id}/VCSs/{vcs_id}/VPPBs
 */
json_t *fmd_redfish_vppb_collection(struct fmd_ctx *ctx, const char *fabric_id,
				    const char *switch_id, const char *vcs_id)
{
	struct rf_collection coll;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, switch_id);
	if (!sw)
		return NULL;

	/* Find the VCS */
	int vcs_idx = safe_parse_int(vcs_id);
	if (vcs_idx < 0)
		return NULL;
	struct fmd_vcs *vcs = NULL;
	for (int i = 0; i < sw->num_vcs; i++) {
		if (sw->vcs[i].vcs_id == vcs_idx) {
			vcs = &sw->vcs[i];
			break;
		}
	}
	if (!vcs)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs/%s/VPPBs",
				  switch_id, vcs_id),
			   "#VirtualPCI2PCIBridgeCollection.VirtualPCI2PCIBridgeCollection",
			   "Virtual PCI-to-PCI Bridge Collection");

	for (int i = 0; i < vcs->num_vppbs; i++)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs/%s/VPPBs/%d",
				  switch_id, vcs_id, vcs->vppbs[i].vppb_id);

	return rf_collection_finish(&coll);
}

/*
 * vPPB - /redfish/v1/Fabrics/{fabric_id}/Switches/{switch_id}/VCSs/{vcs_id}/VPPBs/{vppb_id}
 */
json_t *fmd_redfish_vppb(struct fmd_ctx *ctx, const char *fabric_id,
			 const char *switch_id, const char *vcs_id, const char *vppb_id)
{
	char uri[256];

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_switch *sw = fmd_topology_find_switch(&ctx->topology, switch_id);
	if (!sw)
		return NULL;

	/* Find the VCS */
	int vcs_idx = safe_parse_int(vcs_id);
	if (vcs_idx < 0)
		return NULL;
	struct fmd_vcs *vcs = NULL;
	for (int i = 0; i < sw->num_vcs; i++) {
		if (sw->vcs[i].vcs_id == vcs_idx) {
			vcs = &sw->vcs[i];
			break;
		}
	}
	if (!vcs)
		return NULL;

	/* Find the vPPB */
	int vppb_idx = parse_prefixed_id(vppb_id, "_vppb");
	if (vppb_idx < 0)
		return NULL;
	struct fmd_vppb *vppb = NULL;
	for (int i = 0; i < vcs->num_vppbs; i++) {
		if (vcs->vppbs[i].vppb_id == vppb_idx) {
			vppb = &vcs->vppbs[i];
			break;
		}
	}
	if (!vppb)
		return NULL;

	snprintf(uri, sizeof(uri),
		 "/redfish/v1/Fabrics/CXL/Switches/%s/VCSs/%s/VPPBs/%s",
		 switch_id, vcs_id, vppb_id);

	/* Map binding status to Redfish enum */
	const char *binding_status_str = "Unbound";
	switch (vppb->binding_status) {
	case FMD_VPPB_UNBOUND:            binding_status_str = "Unbound"; break;
	case FMD_VPPB_BUSY:               binding_status_str = "Busy"; break;
	case FMD_VPPB_BOUND_PHYSICAL_PORT: binding_status_str = "BoundPhysicalPort"; break;
	case FMD_VPPB_BOUND_LD:           binding_status_str = "BoundLD"; break;
	case FMD_VPPB_BOUND_PBR:          binding_status_str = "BoundPID"; break;
	}

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:o,"   /* Status */
		"  s:s,"   /* VPPBId */
		"  s:s"    /* BindingStatus */
		"}",
		"@odata.id", uri,
		"@odata.type", "#VirtualPCI2PCIBridge.v1_0_0.VirtualPCI2PCIBridge",
		"Id", vppb_id,
		"Name", "Virtual PCI-to-PCI Bridge",
		"Status", rf_status("Enabled", "OK"),
		"VPPBId", vppb_id,
		"BindingStatus", binding_status_str
	);

	/* Add bound port ID if applicable */
	if (vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT ||
	    vppb->binding_status == FMD_VPPB_BOUND_LD) {
		json_object_set_new(result, "BoundPortId",
				    json_integer(vppb->bound_port_id));
	}

	/* Add bound LD ID if applicable */
	if (vppb->binding_status == FMD_VPPB_BOUND_LD) {
		json_object_set_new(result, "BoundLDId",
				    json_integer(vppb->bound_ld_id));

		/*
		 * GCXLID - globally unique CXL logical device identifier
		 * Format: XX-XX-XX-XX-XX-XX-XX-XX:XXXX
		 * (PCIe Device Serial Number : Bound LD ID), "where the
		 * most significant octet is first" (Resource schema,
		 * DurableNameFormat GCXLID).
		 */
		if (vppb->ld && vppb->ld->base.parent) {
			struct fmd_component *parent = vppb->ld->base.parent;
			uint64_t serial = 0;
			if (parent->type == FMD_COMPONENT_MLD) {
				struct fmd_mld *mld = fmd_to_mld(parent);
				serial = mld->identify.serial_num;
			}
			if (serial != 0) {
				char gcxlid[32];
				snprintf(gcxlid, sizeof(gcxlid),
					 "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%04X",
					 (unsigned)((serial >> 56) & 0xFF),
					 (unsigned)((serial >> 48) & 0xFF),
					 (unsigned)((serial >> 40) & 0xFF),
					 (unsigned)((serial >> 32) & 0xFF),
					 (unsigned)((serial >> 24) & 0xFF),
					 (unsigned)((serial >> 16) & 0xFF),
					 (unsigned)((serial >> 8) & 0xFF),
					 (unsigned)(serial & 0xFF),
					 vppb->bound_ld_id);
				json_object_set_new(result, "GCXLID",
						    json_string(gcxlid));
			}
		}
	}

	/* Add bound PBR ID if applicable */
	if (vppb->binding_status == FMD_VPPB_BOUND_PBR) {
		json_object_set_new(result, "BoundPBRId",
				    json_integer(vppb->bound_pbr_id));
	}

	/* Add Links to bound port */
	if (vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT ||
	    vppb->binding_status == FMD_VPPB_BOUND_LD) {
		struct fmd_component *target;
		json_t *links;

		if (vppb->port) {
			/* Use resolved port's actual ID */
			links = rf_links_single("Port",
				"/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
				switch_id, vppb->port->base.id);
		} else {
			/* Fallback: assume DSP if port not resolved */
			links = rf_links_single("Port",
				"/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s_dsp%d",
				switch_id, switch_id, vppb->bound_port_id);
		}

		/*
		 * Endpoints, "the endpoints associated with this vPPB"
		 * (VirtualPCI2PCIBridge_v1): the target Endpoint it is bound
		 * to (see vppb_target()).
		 */
		fmd_topology_rdlock(&ctx->topology);
		target = vppb_target(vppb);
		if (target)
			json_object_set_new(links, "Endpoints",
				json_pack("[o]", rf_link("/redfish/v1/Fabrics/CXL/Endpoints/%s",
							 target->id)));
		fmd_topology_rdunlock(&ctx->topology);

		json_object_set_new(result, "Links", links);
	}

	/*
	 * No actions: Bind vPPB and Unbind vPPB are reached through
	 * Connections (POST and DELETE), which is cxl-fmd's mapping.
	 */
	return result;
}


/*
 * Chassis Collection - /redfish/v1/Chassis
 *
 * Per OCP CMS Figure 7:
 * - Each Switch is represented as a Chassis (ChassisType: Module)
 * - Each SLD and MLD is represented as a Chassis (ChassisType: Component)
 */
json_t *fmd_redfish_chassis_collection(struct fmd_ctx *ctx)
{
	struct rf_collection coll;
	struct fmd_component *comp;

	rf_collection_init(&coll, "/redfish/v1/Chassis",
			   "#ChassisCollection.ChassisCollection",
			   "Chassis Collection");

	fmd_topology_rdlock(&ctx->topology);

	/* Switches, SLDs, MLDs, and MHDs are all Chassis */
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SWITCH)
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s", comp->id);

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SLD)
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s", comp->id);

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_MLD)
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s", comp->id);

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_MHD)
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s", comp->id);

	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * The Conditions of @r: "The Conditions property within the Status
 * property is used to show why the Health property contains a value other
 * than OK" (DSP0288 1.4.0 Table 1, Comment 3, of "Get Health Info, Health
 * Status, Many, Status"; Section 3: implementations "shall adhere to the
 * numbered comments"). One for each Health Status bit set, and for each
 * Media Status read that makes Health other than OK (02h to 09h, and a
 * reserved one: see fmd_rollup_status()); none for 00h, or 01h, Not
 * Ready, which leaves Health OK. With @corrected, for a Memory, one for
 * each of Additional Status Bit[4] and Bit[5] set: "The Conditions
 * property within the Status property of the Memory resource shall
 * contain messages to indicate either the Corrected Volatile Error Count
 * or Corrected Persistent Error Count flags are set" (Comment 4). Those
 * leave Health as the Health Status bits and Media Status make it:
 * DSP0288 maps only Health Status to Health, and a condition "may affect
 * the Health" (Resource_v1 Conditions), cxl-fmd's choice being that they
 * do not. Of several LDs, the Conditions are those of all of them, with
 * no OriginOfCondition (cxl-fmd's choice: Resource_v1 has them for "this
 * or a related resource"). NULL when there is none.
 */
static json_t *rollup_conditions(const struct fmd_status_rollup *r, bool corrected)
{
	json_t *conds = json_array();

	for (int bit = 0; bit < 4; bit++) {
		if (r->health_bits & (1u << bit))
			json_array_append_new(conds, fmd_redfish_condition(
				FMD_COND_MAINTENANCE_NEEDED + bit, NULL));
	}
	for (int media = 0x02; media <= 0x09; media++) {
		if (r->media_seen & (1u << media))
			json_array_append_new(conds, fmd_redfish_condition(
				FMD_COND_WRITE_PERSISTENCY_LOST + (media - 0x02), NULL));
	}
	if (r->media_reserved) {
		char val[8];

		snprintf(val, sizeof(val), "%02Xh", r->media_reserved);
		json_array_append_new(conds, fmd_redfish_condition(FMD_COND_MEDIA_RESERVED, val));
	}
	if (corrected && (r->additional & 0x10))
		json_array_append_new(conds, fmd_redfish_condition(FMD_COND_CORRECTED_VOLATILE, NULL));
	if (corrected && (r->additional & 0x20))
		json_array_append_new(conds, fmd_redfish_condition(FMD_COND_CORRECTED_PERSISTENT, NULL));

	if (!json_array_size(conds)) {
		json_decref(conds);
		return NULL;
	}
	return conds;
}

/*
 * A Status from @r (fmd_rollup_status()): State, and Health if @health
 * and HealthRollup if @rollup, each only when it is known. Health "shall
 * represent the health state of the resource without considering its
 * dependent resources", HealthRollup "the resource and its dependent
 * resources" (Resource_v1). Neither is required: one that is not known is
 * left out, not reported OK. Conditions as rollup_conditions() makes them,
 * those of Additional Status with @corrected (a Memory).
 */
static json_t *rf_rollup_status(const struct fmd_status_rollup *r, bool health,
				bool rollup, bool corrected)
{
	enum rf_health h;
	enum rf_state st;
	bool known = fmd_rollup_status(r, &h, &st);
	json_t *status = json_pack("{s:s}", "State", rf_state_to_str(st));
	json_t *conds = rollup_conditions(r, corrected);

	if (known && health)
		json_object_set_new(status, "Health", json_string(rf_health_to_str(h)));
	if (known && rollup)
		json_object_set_new(status, "HealthRollup", json_string(rf_health_to_str(h)));
	if (conds)
		json_object_set_new(status, "Conditions", conds);
	return status;
}

/*
 * The Status of a chunk, @chunk (NULL for a switch's pool chunk), from
 * @r. A staged chunk (struct fmd_mem_chunk) is StandbyOffline, "a
 * resource is temporarily unavailable but may become available through
 * an external action" (Resource_v1): the device's next reset (cxl-fmd's
 * choice of state).
 */
static json_t *chunk_status(const struct fmd_status_rollup *r,
			    const struct fmd_mem_chunk *chunk)
{
	json_t *status = rf_rollup_status(r, true, false, false);

	if (chunk && chunk->staged)
		json_object_set_new(status, "State", json_string("StandbyOffline"));
	return status;
}

/*
 * Oem/Cxl of a staged chunk of @sld: the partition it waits for, and the
 * resets the device's CEL gives for Set Partition Info (4101h). "If
 * cleared, the change in partitioning shall become the "next"
 * configuration, to become active on the next reset (as specified in the
 * command effects)" (CXL r4.0 Table 8-311), and the Command Effect
 * (Table 8-254) has "Configuration Change after Cold Reset" (Bit[0]),
 * "after Conventional Reset" (Bit[10]) and "after CXL Reset" (Bit[11]),
 * the last two valid only with "CEL[11:10] Valid" (Bit[9]). Each listed
 * is a reset the device says the change takes effect after; whether one
 * of them is enough or all are needed, Table 8-254 does not say.
 * ActivationResets is empty when the CEL lists 4101h with none of them,
 * and absent when the CEL was not read or does not list 4101h. The
 * names are cxl-fmd's (Cxl_v1.xml). Review F15 claim 4.
 */
static json_t *staged_chunk_oem(const struct fmd_sld *sld,
				const struct fmd_mem_chunk *chunk)
{
	json_t *pending = json_pack("{s:I, s:b}", "NextVolatileCapacityBytes",
				    (json_int_t)(chunk->staged_vmem * CXL_CAPACITY_UNIT),
				    "Removal", chunk->removing);

	if (sld->set_partition_effect.known) {
		uint16_t e = sld->set_partition_effect.effect;
		json_t *resets = json_array();

		if (e & FMD_CEL_CONFIG_AFTER_COLD_RESET)
			json_array_append_new(resets, json_string("ColdReset"));
		if ((e & FMD_CEL_RESETS_VALID) && (e & FMD_CEL_CONFIG_AFTER_CONV_RESET))
			json_array_append_new(resets, json_string("ConventionalReset"));
		if ((e & FMD_CEL_RESETS_VALID) && (e & FMD_CEL_CONFIG_AFTER_CXL_RESET))
			json_array_append_new(resets, json_string("CXLReset"));
		json_object_set_new(pending, "ActivationResets", resets);
	}
	return json_pack("{s:{s:s, s:o}}", "Cxl",
			 "@odata.type", "#Cxl.v1_0_0.MemoryChunk",
			 "PendingPartition", pending);
}

/* @str as a JSON string, or null if it is NULL */
static json_t *json_string_or_null(const char *str)
{
	return str ? json_string(str) : json_null();
}

/* Add the LDs of @dev, an MLD or a Multi-Headed device, to @r */
static void rollup_lds(struct fmd_status_rollup *r, const struct fmd_component *dev)
{
	const struct fmd_component *child;

	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		const struct fmd_ld *ld = fmd_to_ld((struct fmd_component *)child);

		fmd_rollup_add(r, &ld->health, ld->health_valid);
	}
}

/* Add a memory device, every LD of it if it has them, to @r */
static void rollup_memdev(struct fmd_status_rollup *r, const struct fmd_component *dev)
{
	if (dev->type == FMD_COMPONENT_SLD) {
		const struct fmd_sld *sld = fmd_to_sld((struct fmd_component *)dev);

		fmd_rollup_add(r, &sld->health, sld->health_valid);
	} else {
		rollup_lds(r, dev);
	}
}

/*
 * Chassis - /redfish/v1/Chassis/{id}
 *
 * Per OCP CMS Figure 7:
 * - Switches are Chassis with ChassisType "Module"
 *   - MemoryDomains shows aggregate capacity of all attached devices
 *   - Links.Contains lists all device Chassis attached to the switch
 * - SLDs/MLDs are Chassis with ChassisType "Component"
 *   - Links.ContainedBy points to parent switch Chassis (if switch-attached)
 */
json_t *fmd_redfish_chassis(struct fmd_ctx *ctx, const char *id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, id);
	if (!comp)
		return NULL;

	char chassis_uri[256];
	char domains_uri[256];
	char serial_str[32];
	char description[256];
	const char *name;
	const char *chassis_type;
	struct fmd_status_rollup r = { 0 };

	snprintf(chassis_uri, sizeof(chassis_uri), "/redfish/v1/Chassis/%s", id);
	snprintf(domains_uri, sizeof(domains_uri), "/redfish/v1/Chassis/%s/MemoryDomains", id);

	json_t *result = NULL;

	if (comp->type == FMD_COMPONENT_SWITCH) {
		/*
		 * Switch Chassis - ChassisType "Module" per OCP CMS Figure 7
		 * Represents the physical switch with aggregate memory pool.
		 */
		struct fmd_switch *sw = fmd_to_switch(comp);
		name = "CXL Switch";
		chassis_type = "Module";
		snprintf(serial_str, sizeof(serial_str), "0x%016llX",
			 (unsigned long long)sw->identify.serial_num);

		/* Count attached devices and aggregate capacity */
		int device_count = 0;
		uint64_t total_capacity_mb = 0;
		json_t *contains = json_array();

		fmd_topology_rdlock(&ctx->topology);
		struct fmd_component *port_child;
		fmd_for_each_child(&sw->base, port_child) {
			if (port_child->type != FMD_COMPONENT_PORT)
				continue;

			struct fmd_component *dev_child;
			fmd_for_each_child(port_child, dev_child) {
				if (!fmd_is_memdev(dev_child))
					continue;

				device_count++;

				/* Build Contains link */
				json_array_append_new(contains,
					rf_link("/redfish/v1/Chassis/%s", dev_child->id));

				/* Aggregate capacity and health */
				rollup_memdev(&r, dev_child);
				if (dev_child->type == FMD_COMPONENT_SLD) {
					struct fmd_sld *sld = fmd_to_sld(dev_child);
					total_capacity_mb += sld->memory_size / (1024ULL * 1024);
				} else if (dev_child->type == FMD_COMPONENT_MLD) {
					struct fmd_mld *mld = fmd_to_mld(dev_child);
					/* Sum LD capacities for MLD */
					struct fmd_component *ld_child;
					fmd_for_each_child(&mld->base, ld_child) {
						if (ld_child->type == FMD_COMPONENT_LD) {
							struct fmd_ld *ld = fmd_to_ld(ld_child);
							total_capacity_mb += ld->memory_size / (1024ULL * 1024);
						}
					}
				}
			}
		}
		fmd_topology_rdunlock(&ctx->topology);

		snprintf(description, sizeof(description),
			 "CXL Switch with %d attached device(s), %llu MB aggregate capacity",
			 device_count, (unsigned long long)total_capacity_mb);

		/*
		 * The switch reads no health of its own: "It is prohibited for
		 * switches to support any commands from the Memory Device
		 * Command Set" (CXL r4.0 Table 8-308, note 1), Get Health Info
		 * among them. It is Enabled, with no Health; what its devices
		 * report is its HealthRollup (cxl-fmd's choice).
		 */
		json_t *status = rf_rollup_status(&r, false, true, false);

		json_object_set_new(status, "State", json_string("Enabled"));

		result = json_pack(
			"{"
			"  s:s,"   /* @odata.id */
			"  s:s,"   /* @odata.type */
			"  s:s,"   /* Id */
			"  s:s,"   /* Name */
			"  s:s,"   /* Description */
			"  s:s,"   /* ChassisType */
			"  s:s,"   /* SerialNumber */
			"  s:o,"   /* Status */
			"  s:{s:s}"   /* MemoryDomains */
			"}",
			"@odata.id", chassis_uri,
			"@odata.type", "#Chassis.v1_23_0.Chassis",
			"Id", id,
			"Name", name,
			"Description", description,
			"ChassisType", chassis_type,
			"SerialNumber", serial_str,
			"Status", status,
			"MemoryDomains", "@odata.id", domains_uri
		);

		/* the switch's CXL event records: see src/event_log.c */
		json_object_set_new(result, "LogServices", json_pack("{s:s}", "@odata.id",
			rf_uri(0, "/redfish/v1/Chassis/%s/LogServices", id)));

		/* Add Links.Contains with all device chassis */
		if (json_array_size(contains) > 0) {
			json_object_set_new(result, "Links",
				json_pack("{s:o}", "Contains", contains));
		} else {
			json_decref(contains);
		}

		return result;
	}

	/* Device Chassis (SLD or MLD) */
	char pcie_uri[256];
	char memory_uri[256];
	snprintf(pcie_uri, sizeof(pcie_uri), "/redfish/v1/Chassis/%s/PCIeDevices", id);
	snprintf(memory_uri, sizeof(memory_uri), "/redfish/v1/Chassis/%s/Memory", id);

	if (comp->type == FMD_COMPONENT_SLD) {
		struct fmd_sld *sld = fmd_to_sld(comp);
		name = "CXL SLD Memory Device";
		chassis_type = "Component";
		snprintf(serial_str, sizeof(serial_str), "0x%016llX",
			 (unsigned long long)sld->identify.serial_num);
		snprintf(description, sizeof(description),
			 "CXL Type 3 Single Logical Device (%llu MB)",
			 (unsigned long long)(sld->memory_size / (1024ULL * 1024)));
	} else if (comp->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(comp);
		name = "CXL MLD Memory Device";
		chassis_type = "Component";
		snprintf(serial_str, sizeof(serial_str), "0x%016llX",
			 (unsigned long long)mld->identify.serial_num);
		snprintf(description, sizeof(description),
			 "CXL Type 3 Multi-Logical Device with %d LDs",
			 mld->ld_count);
	} else if (comp->type == FMD_COMPONENT_MHD) {
		struct fmd_mhd *mhd = fmd_to_mhd(comp);
		name = "CXL MHD Memory Device";
		chassis_type = "Component";
		snprintf(serial_str, sizeof(serial_str), "0x%016llX",
			 (unsigned long long)mhd->identify.serial_num);
		snprintf(description, sizeof(description),
			 "CXL Type 3 Multi-Headed Device with %d heads, %d LDs",
			 mhd->num_heads, mhd->total_lds);
	} else {
		return NULL;
	}

	/*
	 * An SLD's Get Health Info, or that of each LD of an MLD or a
	 * Multi-Headed device, which is all the health the device reports:
	 * its Health, and its HealthRollup, its PCIeDevice and Memory being
	 * views of the same (cxl-fmd's choice). The same mapping as theirs.
	 */
	fmd_topology_rdlock(&ctx->topology);
	rollup_memdev(&r, comp);
	fmd_topology_rdunlock(&ctx->topology);

	/*
	 * No Manufacturer or Model: they are names ("the name of the
	 * organization responsible for producing the chassis", "the name by
	 * which the manufacturer generally refers to the chassis", Chassis_v1),
	 * and only the PCI Vendor and Device IDs are known. Those are on the
	 * device's Endpoint, as EntityPciId.
	 */
	result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* Description */
		"  s:s,"   /* ChassisType */
		"  s:s,"   /* SerialNumber */
		"  s:o,"   /* Status */
		"  s:{s:s},"  /* PCIeDevices */
		"  s:{s:s},"  /* Memory */
		"  s:{s:s}"   /* MemoryDomains */
		"}",
		"@odata.id", chassis_uri,
		"@odata.type", "#Chassis.v1_23_0.Chassis",
		"Id", id,
		"Name", name,
		"Description", description,
		"ChassisType", chassis_type,
		"SerialNumber", serial_str,
		"Status", rf_rollup_status(&r, true, true, false),
		"PCIeDevices", "@odata.id", pcie_uri,
		"Memory", "@odata.id", memory_uri,
		"MemoryDomains", "@odata.id", domains_uri
	);

	/*
	 * Add Links.ContainedBy if this device is attached to a switch.
	 * Walk up: device -> port -> switch
	 */
	if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT) {
		struct fmd_component *sw = comp->parent->parent;
		if (sw && sw->type == FMD_COMPONENT_SWITCH) {
			json_object_set_new(result, "Links",
				rf_links_single("ContainedBy",
					"/redfish/v1/Chassis/%s", sw->id));
		}
	}

	return result;
}

/*
 * PCIeDevices Collection - /redfish/v1/Chassis/{id}/PCIeDevices
 */
json_t *fmd_redfish_pcie_devices(struct fmd_ctx *ctx, const char *chassis_id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp))
		return NULL;

	json_t *members = json_array();

	/* For direct-attached devices, each chassis has exactly one PCIe device */
	json_array_append_new(members,
		rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s", chassis_id, chassis_id));

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:i,"
		"  s:o"
		"}",
		"@odata.id", rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices", chassis_id),
		"@odata.type", "#PCIeDeviceCollection.PCIeDeviceCollection",
		"Name", "PCIe Device Collection",
		"Members@odata.count", 1,
		"Members", members
	);
}

/*
 * PCIeDevice - /redfish/v1/Chassis/{id}/PCIeDevices/{dev}
 */
json_t *fmd_redfish_pcie_device(struct fmd_ctx *ctx,
				const char *chassis_id,
				const char *dev_id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp) || strcmp(chassis_id, dev_id) != 0)
		return NULL;

	char uri[256];
	char ld_uri[256];
	char fw_str[17];
	struct fmd_status_rollup r = { 0 };
	uint64_t serial = 0;

	snprintf(uri, sizeof(uri), "/redfish/v1/Chassis/%s/PCIeDevices/%s",
		 chassis_id, dev_id);
	snprintf(ld_uri, sizeof(ld_uri),
		 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices",
		 chassis_id, dev_id);

	/* Get device-specific info and aggregate health */
	const char *dev_name;
	if (comp->type == FMD_COMPONENT_SLD) {
		struct fmd_sld *sld = fmd_to_sld(comp);
		snprintf(fw_str, sizeof(fw_str), "%.16s", (char *)sld->memdev.fw_revision);
		serial = sld->identify.serial_num;
		dev_name = "CXL Type 3 SLD";
	} else if (comp->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(comp);
		fw_str[0] = '\0';	/* an LD has a firmware revision, the MLD none */
		serial = mld->identify.serial_num;
		dev_name = "CXL Type 3 MLD";
	} else if (comp->type == FMD_COMPONENT_MHD) {
		struct fmd_mhd *mhd = fmd_to_mhd(comp);
		fw_str[0] = '\0';
		serial = mhd->identify.serial_num;
		dev_name = "CXL Type 3 MHD";
	} else {
		return NULL;
	}

	/* as its Chassis' (fmd_redfish_chassis()) */
	fmd_topology_rdlock(&ctx->topology);
	rollup_memdev(&r, comp);
	fmd_topology_rdunlock(&ctx->topology);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* DeviceType */
		"  s:o,"   /* Status */
		"  s:{s:s}"  /* CXLLogicalDevices */
		"}",
		"@odata.id", uri,
		"@odata.type", "#PCIeDevice.v1_23_0.PCIeDevice",
		"Id", dev_id,
		"Name", dev_name,
		"DeviceType", "SingleFunction",
		"Status", rf_rollup_status(&r, true, false, false),
		"CXLLogicalDevices", "@odata.id", ld_uri
	);

	/* what is not known is not reported: no "N/A", and no component ID */
	if (fw_str[0])
		json_object_set_new(result, "FirmwareVersion", json_string(fw_str));

	/*
	 * SerialNumber is the device's, from Identify (Table 8-216), written
	 * as it is on the Chassis. 0 is no serial number.
	 */
	if (serial) {
		char serial_str[32];

		snprintf(serial_str, sizeof(serial_str), "0x%016llX",
			 (unsigned long long)serial);
		json_object_set_new(result, "SerialNumber", json_string(serial_str));
	}

	/*
	 * PCIeInterface: the link of a device on a switch port is the port's
	 * link, which Get Physical Port State describes: its negotiated
	 * width and current speed are the lanes and the PCIe generation in
	 * use. What the device itself is capable of is not reported by any
	 * command, and neither is the link of a device with no switch.
	 */
	if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT) {
		static const char *const gen[] = {
			NULL, "Gen1", "Gen2", "Gen3", "Gen4", "Gen5", "Gen6",
		};
		struct fmd_port *port = fmd_to_port(comp->parent);

		if (port->link_status == FMD_LINK_UP) {
			json_t *pcie = json_object();

			if (port->current_link_speed > 0 &&
			    port->current_link_speed < sizeof(gen) / sizeof(gen[0]))
				json_object_set_new(pcie, "PCIeType",
					json_string(gen[port->current_link_speed]));
			if (port->negotiated_link_width)
				json_object_set_new(pcie, "LanesInUse",
					json_integer(port->negotiated_link_width));
			if (json_object_size(pcie))
				json_object_set_new(result, "PCIeInterface", pcie);
			else
				json_decref(pcie);
		}
	}

	/* CXL-specific properties */
	int max_ld;
	uint8_t qos_telemetry_caps = 0;
	bool has_qos_control = false;
	bool temp_throughput_reduction_enabled = false;

	if (comp->type == FMD_COMPONENT_SLD) {
		struct fmd_sld *sld = fmd_to_sld(comp);
		max_ld = 1;
		qos_telemetry_caps = sld->memdev.qos_telemetry_caps;
	} else if (comp->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(comp);
		max_ld = mld->max_ld_count ? mld->max_ld_count : mld->ld_count;
		has_qos_control = true;
		temp_throughput_reduction_enabled = mld->qos_control.temp_throughput_reduction_enabled;

		/*
		 * DSP0288 maps these from Get LD Info for an MLD. A mock
		 * topology may give them only on its LDs.
		 */
		qos_telemetry_caps = mld->qos_telemetry_capability;
		struct fmd_component *child;
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mld->base, child) {
			if (qos_telemetry_caps)
				break;
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				qos_telemetry_caps = ld->memdev.qos_telemetry_caps;
				break;
			}
		}
		fmd_topology_rdunlock(&ctx->topology);
	} else if (comp->type == FMD_COMPONENT_MHD) {
		struct fmd_mhd *mhd = fmd_to_mhd(comp);
		max_ld = mhd->max_lds ? mhd->max_lds : mhd->total_lds;
		has_qos_control = true;
		temp_throughput_reduction_enabled = mhd->qos_control.temp_throughput_reduction_enabled;

		/* Get QoS telemetry caps from first LD in pool */
		struct fmd_component *child;
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mhd->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				qos_telemetry_caps = ld->memdev.qos_telemetry_caps;
				break;
			}
		}
		fmd_topology_rdunlock(&ctx->topology);
	} else {
		max_ld = 1;
	}

	/*
	 * DSP0288 Table 2, Comment 1: "For an MLD, implementations shall
	 * represent Egress Port Congestion Supported in the Port resource. For
	 * an SLD, implementations shall represent Egress Port Congestion
	 * Supported in the PCIeDevice resource." Table 1 names the PCIeDevice
	 * property CXLDevice/EgressPortCongestionSupported, which DSP8010 does
	 * not have; the schema's is CXLDevice/EgressPortCongestionSupport. For
	 * an MLD it is Port CXL/QoSTelemetryCapabilities/
	 * EgressPortBackpressureSupported.
	 *
	 * An MLD that is not on a switch port has no Port resource. Serving it
	 * on the PCIeDevice then, as for an SLD, is cxl-fmd's choice: it would
	 * otherwise be served nowhere.
	 *
	 * TemporaryThroughputReductionSupported is always in PCIeDevice.CXLDevice
	 * (DSP0288 Tables 1 and 2).
	 */
	json_t *cxl_device = json_pack("{s:s, s:i, s:b}",
		"DeviceType", rf_cxl_device_type_to_str(CXL_DEVICE_TYPE_TYPE3),
		"MaxNumberLogicalDevices", max_ld,
		"TemporaryThroughputReductionSupported", (qos_telemetry_caps & 0x02) ? 1 : 0);

	if (comp->type == FMD_COMPONENT_SLD ||
	    (comp->type == FMD_COMPONENT_MLD &&
	     !(comp->parent && comp->parent->type == FMD_COMPONENT_PORT))) {
		json_object_set_new(cxl_device, "EgressPortCongestionSupport",
			json_boolean((qos_telemetry_caps & 0x01) ? 1 : 0));
	}

	/*
	 * DSP0288 Table 2: Get QoS Control → TemporaryThroughputReductionEnabled
	 * This is the runtime enable state (vs Supported which is capability)
	 */
	if (has_qos_control) {
		json_object_set_new(cxl_device, "TemporaryThroughputReductionEnabled",
			json_boolean(temp_throughput_reduction_enabled));
	}

	/*
	 * DSP0288 Table 1: Get/Set Timestamp → CXLDevice/Timestamp
	 * Uses abstraction that handles both real hardware and mock mode
	 */
	{
		uint64_t timestamp_ns = fmd_get_device_timestamp(comp);
		if (timestamp_ns > 0) {
			char *ts_str = format_cxl_timestamp(timestamp_ns);
			if (ts_str) {
				json_object_set_new(cxl_device, "Timestamp",
						    json_string(ts_str));
				free(ts_str);
			}
		}
	}

	/*
	 * CXLDevice/DeviceClass (PCIeDevice v1.23): SLD, MLD, MH_SLD or
	 * MH_MLD. A Multi-Headed device is an MH_MLD if one of its heads
	 * presents an MLD, and an MH_SLD only if every head is known to
	 * present an SLD, over the whole LD Map with every head kept: with
	 * any of that not known, the property is left out (cxl-fmd's choice;
	 * the CSDL does not make it required).
	 */
	{
		enum rf_cxl_device_class dev_class = CXL_DEVICE_CLASS_SLD;
		bool known = true;

		if (comp->type == FMD_COMPONENT_MLD) {
			dev_class = CXL_DEVICE_CLASS_MLD;
		} else if (comp->type == FMD_COMPONENT_MHD) {
			const struct fmd_mhd *mhd = fmd_to_mhd(comp);
			struct fmd_component *child;
			int heads = 0;

			dev_class = CXL_DEVICE_CLASS_MH_SLD;
			fmd_topology_rdlock(&ctx->topology);
			known = mhd->ld_map_complete &&
				mhd->num_heads <= FMD_MAX_HEADS;
			fmd_for_each_child_type(comp, child, FMD_COMPONENT_HEAD) {
				enum fmd_head_type p = fmd_to_head(child)->presentation;

				heads++;
				if (p == FMD_HEAD_MLD) {
					dev_class = CXL_DEVICE_CLASS_MH_MLD;
					known = true;
					break;
				}
				if (p != FMD_HEAD_SLD)
					known = false;
			}
			fmd_topology_rdunlock(&ctx->topology);
			if (!heads)
				known = false;
		}
		if (known)
			json_object_set_new(cxl_device, "DeviceClass",
				json_string(rf_cxl_device_class_to_str(dev_class)));
	}

	/*
	 * DSP0288 gap, see docs/DCD-FM-API.md Table 3:
	 * Get DCD Info (5600h) -> CXLDevice/DynamicCapacity, and the
	 * CXLReferenceDCMemory action for DC Add/Remove Reference (5606h/5607h).
	 */
	{
		json_t *dyn = fmd_redfish_dc_capabilities(ctx, comp, chassis_id, dev_id);
		if (dyn) {
			json_object_set_new(cxl_device, "DynamicCapacity", dyn);
			json_object_set_new(result, "Actions",
				fmd_redfish_dc_pcie_actions(chassis_id, dev_id));
			json_object_set_new(result, "Oem",
				fmd_redfish_dc_tags_oem(ctx, comp, chassis_id, dev_id));
		}
	}

	json_object_set_new(result, "CXLDevice", cxl_device);

	/* Add Links.ConnectedPCIePorts for switch-attached devices */
	if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT) {
		struct fmd_port *port = fmd_to_port(comp->parent);
		struct fmd_component *sw = port->base.parent;
		if (sw && sw->type == FMD_COMPONENT_SWITCH) {
			json_object_set_new(result, "Links",
				rf_links_array_single("ConnectedPCIePorts",
					"/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
					sw->id, port->base.id));
		}
	}

	/* A Multi-Headed device is on a port per head */
	if (comp->type == FMD_COMPONENT_MHD) {
		struct fmd_component *child;
		json_t *ports = json_array();

		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child_type(comp, child, FMD_COMPONENT_HEAD) {
			struct fmd_port *port = fmd_to_head(child)->port;

			if (port && port->base.parent)
				json_array_append_new(ports,
					rf_link("/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
						port->base.parent->id, port->base.id));
		}
		fmd_topology_rdunlock(&ctx->topology);

		if (json_array_size(ports))
			json_object_set_new(result, "Links",
				json_pack("{s:o}", "ConnectedPCIePorts", ports));
		else
			json_decref(ports);
	}

	return result;
}

/*
 * CXLLogicalDevices Collection
 *
 * For SLD: returns single LD
 * For MLD: returns all fmd_ld children
 */
json_t *fmd_redfish_cxl_logical_devices(struct fmd_ctx *ctx,
					const char *chassis_id,
					const char *pcie_id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp) || strcmp(chassis_id, pcie_id) != 0)
		return NULL;

	json_t *members = json_array();
	int ld_count = 0;

	fmd_topology_rdlock(&ctx->topology);

	if (comp->type == FMD_COMPONENT_SLD) {
		/* SLD has a single implicit LD (id=0) */
		json_array_append_new(members,
			rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/0",
				chassis_id, pcie_id));
		ld_count = 1;
	} else {
		/* MLD - enumerate fmd_ld children */
		struct fmd_component *child;
		fmd_for_each_child(comp, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				json_array_append_new(members,
					rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%d",
						chassis_id, pcie_id, ld->ld_id));
				ld_count++;
			}
		}
	}

	fmd_topology_rdunlock(&ctx->topology);

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:i,"
		"  s:o"
		"}",
		"@odata.id", rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices",
				    chassis_id, pcie_id),
		"@odata.type", "#CXLLogicalDeviceCollection.CXLLogicalDeviceCollection",
		"Name", "CXL Logical Device Collection",
		"Members@odata.count", ld_count,
		"Members", members
	);
}

/*
 * CXLLogicalDevice - the key CXL resource
 */
json_t *fmd_redfish_cxl_logical_device(struct fmd_ctx *ctx,
				       const char *chassis_id,
				       const char *pcie_id,
				       const char *ld_id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp) || strcmp(chassis_id, pcie_id) != 0)
		return NULL;

	int ld_idx = parse_ld_id(ld_id);
	if (ld_idx < 0)
		return NULL;
	char uri[512];
	char name[64];
	uint64_t mem_size_bytes, dc_bytes;
	struct fmd_status_rollup r = { 0 };
	uint8_t qos_allocated = 0;
	uint8_t qos_limit = 0;
	uint8_t qos_caps = 0;		/* QoS Telemetry Capabilities */
	uint64_t serial = 0;
	bool has_qos = false;		/* an LD of an MLD has bandwidth fractions */
	json_t *semantics;
	char ep_id[FMD_MAX_ID_LEN];	/* its target Endpoint */

	(void)snprintf(uri, sizeof(uri),
		       "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s",
		       chassis_id, pcie_id, ld_id);

	if (comp->type == FMD_COMPONENT_SLD) {
		struct fmd_sld *sld = fmd_to_sld(comp);

		if (ld_idx != 0)
			return NULL;

		/* Total Capacity, in multiples of 256 MB (Table 8-309) */
		if (__builtin_mul_overflow(sld->memdev.total_capacity, 256 * FMD_MIB,
					   &mem_size_bytes))
			mem_size_bytes = UINT64_MAX;
		fmd_rollup_add(&r, &sld->health, sld->health_valid);
		qos_caps = sld->memdev.qos_telemetry_caps;
		snprintf(name, sizeof(name), "CXL Logical Device 0");
		snprintf(ep_id, sizeof(ep_id), "%s", comp->id);
		semantics = build_sld_semantics_array(sld);
	} else if (comp->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(comp);

		/* Find the specific fmd_ld child */
		struct fmd_ld *ld = NULL;
		struct fmd_component *child;

		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mld->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *l = fmd_to_ld(child);
				if (l->ld_id == ld_idx) {
					ld = l;
					break;
				}
			}
		}
		if (ld)
			snprintf(ep_id, sizeof(ep_id), "%s", ld->base.id);
		fmd_topology_rdunlock(&ctx->topology);

		if (!ld)
			return NULL;

		mem_size_bytes = ld->memory_size;
		fmd_rollup_add(&r, &ld->health, ld->health_valid);
		/* Convert CXL format (fraction * 256) back to percentage with rounding */
		qos_allocated = (ld->qos_allocated_bw * 100 + 128) / 256;
		qos_limit = (ld->qos_limit_bw * 100 + 128) / 256;
		has_qos = true;
		qos_caps = ld->memdev.qos_telemetry_caps ?
			   ld->memdev.qos_telemetry_caps : mld->qos_telemetry_capability;
		serial = mld->identify.serial_num;

		snprintf(name, sizeof(name), "CXL Logical Device %d", ld_idx);
		semantics = build_mld_semantics_array(mld);
	} else if (comp->type == FMD_COMPONENT_MHD) {
		struct fmd_mhd *mhd = fmd_to_mhd(comp);

		/* Find the specific fmd_ld child in the pool */
		struct fmd_ld *ld = NULL;
		struct fmd_component *child;

		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mhd->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *l = fmd_to_ld(child);
				if (l->ld_id == ld_idx) {
					ld = l;
					break;
				}
			}
		}
		if (ld)
			snprintf(ep_id, sizeof(ep_id), "%s", ld->base.id);
		fmd_topology_rdunlock(&ctx->topology);

		if (!ld)
			return NULL;

		mem_size_bytes = ld->memory_size;
		fmd_rollup_add(&r, &ld->health, ld->health_valid);
		/* Convert CXL format (fraction * 256) back to percentage with rounding */
		qos_allocated = (ld->qos_allocated_bw * 100 + 128) / 256;
		qos_limit = (ld->qos_limit_bw * 100 + 128) / 256;
		has_qos = true;
		qos_caps = ld->memdev.qos_telemetry_caps ?
			   ld->memdev.qos_telemetry_caps : mhd->qos_telemetry_capability;

		snprintf(name, sizeof(name), "CXL Logical Device %d", ld_idx);
		semantics = build_mhd_semantics_array(mhd);
	} else {
		return NULL;
	}

	/*
	 * MemorySizeMiB "shall equate to the sum of the dynamic capacity
	 * extents and the static capacity assigned to this logical device"
	 * (CXLLogicalDevice_v1.xml): the static capacity above plus the
	 * extents the LD's host holds, summed in bytes and divided once.
	 * That Total Capacity and the LD allocation leave the dynamic
	 * capacity out is cxl-fmd's reading; CXL does not say.
	 */
	dc_bytes = fmd_dc_host_bytes(ctx, comp, ld_idx);
	if (__builtin_add_overflow(mem_size_bytes, dc_bytes, &mem_size_bytes))
		mem_size_bytes = UINT64_MAX;


	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* MemorySizeMiB */
		"  s:o,"   /* SemanticsSupported */
		"  s:o"   /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#CXLLogicalDevice.v1_5_0.CXLLogicalDevice",
		"Id", ld_id,
		"Name", name,
		"MemorySizeMiB", (json_int_t)(mem_size_bytes / FMD_MIB),
		"SemanticsSupported", semantics,
		"Status", rf_rollup_status(&r, true, false, false)
	);

	/*
	 * QoS: the bandwidth fractions of Get QoS Allocated BW and Get QoS
	 * BW Limit (DSP0288 Table 2). Those are commands of an MLD, and
	 * prohibited for an SLD, which has no fraction to report.
	 */
	if (has_qos)
		json_object_set_new(result, "QoS",
			json_pack("{s:I, s:I}",
				  "AllocatedBandwidth", (json_int_t)qos_allocated,
				  "LimitPercent", (json_int_t)qos_limit));

	/*
	 * QoSTelemetryCapabilities are what the device supports: Bit[0] and
	 * Bit[1] of the QoS Telemetry Capabilities of Identify Memory Device
	 * (Table 8-309), or of Get LD Info (Table 7-46) for an MLD. Whether
	 * a mechanism is enabled is a setting, reported elsewhere.
	 *
	 * DSP0288 does not map anything to CXLLogicalDevice
	 * QoSTelemetryCapabilities: it puts Bit[0] on the Port (MLD) or the
	 * PCIeDevice (SLD), and Bit[1] on the PCIeDevice. This copy on the LD
	 * is cxl-fmd's addition.
	 */
	json_object_set_new(result, "QoSTelemetryCapabilities",
		json_pack("{s:b, s:b}",
			  "EgressPortBackpressureSupported", (qos_caps & 0x01) ? 1 : 0,
			  "TemporaryThroughputReductionSupported", (qos_caps & 0x02) ? 1 : 0));

	/*
	 * Identifiers: the durable name of an LD is its GCXLID, the PCIe
	 * serial number of the device, most significant octet first, and
	 * the LD-ID (Resource schema, DurableNameFormat GCXLID). It needs
	 * the serial number of the MLD. A pool LD of a Multi-Headed device
	 * has a pool index, and its LD-ID depends on the head it is mapped
	 * to: no name is made up for it.
	 */
	if (comp->type == FMD_COMPONENT_MLD && serial) {
		char durable_name[64];

		snprintf(durable_name, sizeof(durable_name),
			 "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%04X",
			 (unsigned)((serial >> 56) & 0xFF),
			 (unsigned)((serial >> 48) & 0xFF),
			 (unsigned)((serial >> 40) & 0xFF),
			 (unsigned)((serial >> 32) & 0xFF),
			 (unsigned)((serial >> 24) & 0xFF),
			 (unsigned)((serial >> 16) & 0xFF),
			 (unsigned)((serial >> 8) & 0xFF),
			 (unsigned)(serial & 0xFF), ld_idx);
		json_object_set_new(result, "Identifiers",
			json_pack("[{s:s, s:s}]",
				  "DurableNameFormat", "GCXLID",
				  "DurableName", durable_name));
	}

	/*
	 * Add Links section per DMTF CXLLogicalDevice schema:
	 * - Endpoints: the target Endpoint of this LD, the SLD's for an SLD
	 *   and the LD's own for an LD of an MLD or a Multi-Headed device
	 * - MemoryDomains: memory domain containing this LD
	 * - MemoryChunks: memory chunks for this LD (only if provisioned)
	 */
	char endpoint_uri[256];
	char domain_uri[256];

	snprintf(endpoint_uri, sizeof(endpoint_uri),
		 "/redfish/v1/Fabrics/CXL/Endpoints/%s", ep_id);
	snprintf(domain_uri, sizeof(domain_uri),
		 "/redfish/v1/Chassis/%s/MemoryDomains/%s_domain0",
		 chassis_id, chassis_id);

	/*
	 * Build MemoryChunks array - only include chunks for this specific LD.
	 * For an MLD or a Multi-Headed device, the chunks whose backing_ld it
	 * is. For SLDs, include all chunks (only one LD exists).
	 */
	json_t *chunks_array = json_array();
	fmd_topology_rdlock(&ctx->topology);
	struct fmd_component *domain_child;
	fmd_for_each_child(comp, domain_child) {
		if (domain_child->type == FMD_COMPONENT_DOMAIN) {
			struct fmd_component *chunk_child;
			fmd_for_each_child(domain_child, chunk_child) {
				if (chunk_child->type == FMD_COMPONENT_CHUNK) {
					struct fmd_mem_chunk *chunk = fmd_to_chunk(chunk_child);
					bool belongs_to_ld = false;

					/* For SLD, all chunks belong to the single LD */
					if (comp->type == FMD_COMPONENT_SLD) {
						belongs_to_ld = true;
					} else {
						belongs_to_ld = chunk->backing_ld == ld_idx;
					}

					if (belongs_to_ld) {
						json_array_append_new(chunks_array,
							rf_link("/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s",
								chassis_id, domain_child->id, chunk_child->id));
					}
				}
			}
		}
	}
	fmd_topology_rdunlock(&ctx->topology);

	/* Build Links - only include MemoryChunks if there are any */
	json_t *links = json_pack("{s:[{s:s}], s:[{s:s}]}",
			  "Endpoints", "@odata.id", endpoint_uri,
			  "MemoryDomains", "@odata.id", domain_uri);
	if (json_array_size(chunks_array) > 0) {
		json_object_set_new(links, "MemoryChunks", chunks_array);
	} else {
		json_decref(chunks_array);
	}
	json_object_set_new(result, "Links", links);

	/*
	 * Dynamic capacity (docs/DCD-FM-API.md Table 3): MemoryRegions link,
	 * extent/tag counters and the DynamicCapacityAdd/Release actions.
	 */
	/* its CXL event records: see src/event_log.c */
	json_object_set_new(result, "Log", json_pack("{s:s}", "@odata.id",
		rf_uri(0, "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s/DeviceLog",
		       chassis_id, pcie_id, ld_id)));

	/*
	 * DSP0288 1.4.0 Table 1: the security actions (sec_ld_actions()). A
	 * DCD's actions are added to the same Actions.
	 */
	{
		json_t *actions = json_object();

		if (actions)
			sec_ld_actions(ctx, comp, ld_idx, uri, actions);
		if (actions && json_object_size(actions))
			json_object_set_new(result, "Actions", actions);
		else
			json_decref(actions);
	}

	fmd_redfish_dc_logical_device_props(ctx, comp, ld_idx, chassis_id, pcie_id,
					    ld_id, result);

	return result;
}


/*
 * MemoryDomains Collection
 *
 * Per OCP CMS Figure 7:
 * - Switch chassis has aggregate MemoryDomains (total pool)
 * - Device chassis has individual MemoryDomains
 */
json_t *fmd_redfish_memory_domains(struct fmd_ctx *ctx, const char *chassis_id)
{
	struct rf_collection coll;
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	struct fmd_component *child;

	if (!comp)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Chassis/%s/MemoryDomains", chassis_id),
			   "#MemoryDomainCollection.MemoryDomainCollection",
			   "Memory Domain Collection");

	fmd_topology_rdlock(&ctx->topology);

	if (comp->type == FMD_COMPONENT_SWITCH) {
		/* Switch chassis: single aggregate pool domain */
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s/MemoryDomains/%s_pool",
				  chassis_id, chassis_id);
	} else if (fmd_is_memdev(comp)) {
		/* Device chassis: enumerate domain children */
		fmd_for_each_child_type(comp, child, FMD_COMPONENT_DOMAIN) {
			rf_collection_add(&coll, "/redfish/v1/Chassis/%s/MemoryDomains/%s",
					  chassis_id, child->id);
		}
	} else {
		fmd_topology_rdunlock(&ctx->topology);
		json_decref(coll.members);
		return NULL;
	}

	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * Memory Collection - /redfish/v1/Chassis/{id}/Memory
 *
 * Per DSP0288, Memory resources provide access to MemoryMetrics.
 * Only available for device chassis (SLD/MLD), not switch chassis.
 */
json_t *fmd_redfish_memory_collection(struct fmd_ctx *ctx, const char *chassis_id)
{
	struct rf_collection coll;
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	struct fmd_component *child;

	if (!comp || !fmd_is_memdev(comp))
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Chassis/%s/Memory", chassis_id),
			   "#MemoryCollection.MemoryCollection",
			   "Memory Collection");

	if (comp->type == FMD_COMPONENT_SLD) {
		/* SLD has one Memory resource */
		rf_collection_add(&coll, "/redfish/v1/Chassis/%s/Memory/%s",
				  chassis_id, chassis_id);
	} else if (comp->type == FMD_COMPONENT_MLD) {
		/* MLD has one Memory resource per LD */
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child_type(comp, child, FMD_COMPONENT_LD) {
			struct fmd_ld *ld = fmd_to_ld(child);
			rf_collection_add(&coll, "/redfish/v1/Chassis/%s/Memory/%s_ld%d",
					  chassis_id, chassis_id, ld->ld_id);
		}
		fmd_topology_rdunlock(&ctx->topology);
	} else if (comp->type == FMD_COMPONENT_MHD) {
		/* MHD has one Memory resource per LD in pool */
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child_type(comp, child, FMD_COMPONENT_LD) {
			struct fmd_ld *ld = fmd_to_ld(child);
			rf_collection_add(&coll, "/redfish/v1/Chassis/%s/Memory/%s_ld%d",
					  chassis_id, chassis_id, ld->ld_id);
		}
		fmd_topology_rdunlock(&ctx->topology);
	}

	return rf_collection_finish(&coll);
}

/*
 * Memory Resource - /redfish/v1/Chassis/{id}/Memory/{mem_id}
 *
 * Per DSP0288, provides link to MemoryMetrics for health data.
 * For SLD: mem_id = chassis_id
 * For MLD: mem_id = chassis_id_ld{n}
 */
json_t *fmd_redfish_memory(struct fmd_ctx *ctx,
			   const char *chassis_id,
			   const char *mem_id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp))
		return NULL;

	char uri[256];
	char metrics_uri[256];
	char name[128];
	uint64_t capacity_mib = 0;
	uint64_t volatile_limit_mib = 0;
	uint64_t staged_volatile_mib = 0;
	uint64_t staged_persistent_mib = 0;
	struct fmd_status_rollup r = { 0 };
	const char *mem_type = "DRAM";
	const char *mem_dev_type = "Logical";
	int ld_id = 0;  /* For MLD tunneling in Get Security State */

	snprintf(uri, sizeof(uri),
		 "/redfish/v1/Chassis/%s/Memory/%s", chassis_id, mem_id);
	snprintf(metrics_uri, sizeof(metrics_uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/MemoryMetrics", chassis_id, mem_id);

	if (comp->type == FMD_COMPONENT_SLD) {
		/* SLD: mem_id should match chassis_id */
		if (strcmp(mem_id, chassis_id) != 0)
			return NULL;

		struct fmd_sld *sld = fmd_to_sld(comp);
		capacity_mib = sld->memory_size / (1024ULL * 1024);
		fmd_rollup_add(&r, &sld->health, sld->health_valid);
		snprintf(name, sizeof(name), "CXL Memory Device");

		/* Get Partition Info (4100h) - values in 256MB CXL units */
		volatile_limit_mib = sld->partition.active_vmem * 256;
		staged_volatile_mib = sld->partition.next_vmem * 256;
		staged_persistent_mib = sld->partition.next_pmem * 256;
	} else if (comp->type == FMD_COMPONENT_MLD) {
		/* MLD: mem_id should be chassis_id_ld{n} */
		char prefix[128];
		snprintf(prefix, sizeof(prefix), "%s_ld", chassis_id);

		if (strncmp(mem_id, prefix, strlen(prefix)) != 0)
			return NULL;

		ld_id = safe_parse_int(mem_id + strlen(prefix));
		if (ld_id < 0)
			return NULL;

		struct fmd_mld *mld = fmd_to_mld(comp);
		struct fmd_ld *target_ld = NULL;
		struct fmd_component *child;

		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mld->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				if (ld->ld_id == ld_id) {
					target_ld = ld;
					break;
				}
			}
		}
		fmd_topology_rdunlock(&ctx->topology);

		if (!target_ld)
			return NULL;

		capacity_mib = target_ld->memory_size / (1024ULL * 1024);
		fmd_rollup_add(&r, &target_ld->health, target_ld->health_valid);
		snprintf(name, sizeof(name), "CXL Logical Device %d", ld_id);

		/* Get Partition Info (4100h) - values in 256MB CXL units */
		volatile_limit_mib = target_ld->partition.active_vmem * 256;
		staged_volatile_mib = target_ld->partition.next_vmem * 256;
		staged_persistent_mib = target_ld->partition.next_pmem * 256;
	} else if (comp->type == FMD_COMPONENT_MHD) {
		/* MHD: mem_id should be chassis_id_ld{n} */
		char prefix[128];
		snprintf(prefix, sizeof(prefix), "%s_ld", chassis_id);

		if (strncmp(mem_id, prefix, strlen(prefix)) != 0)
			return NULL;

		ld_id = safe_parse_int(mem_id + strlen(prefix));
		if (ld_id < 0)
			return NULL;

		struct fmd_mhd *mhd = fmd_to_mhd(comp);
		struct fmd_ld *target_ld = NULL;
		struct fmd_component *child;

		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mhd->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				if (ld->ld_id == ld_id) {
					target_ld = ld;
					break;
				}
			}
		}
		fmd_topology_rdunlock(&ctx->topology);

		if (!target_ld)
			return NULL;

		capacity_mib = target_ld->memory_size / (1024ULL * 1024);
		fmd_rollup_add(&r, &target_ld->health, target_ld->health_valid);
		snprintf(name, sizeof(name), "CXL Logical Device %d", ld_id);

		/* Get Partition Info (4100h) - values in 256MB CXL units */
		volatile_limit_mib = target_ld->partition.active_vmem * 256;
		staged_volatile_mib = target_ld->partition.next_vmem * 256;
		staged_persistent_mib = target_ld->partition.next_pmem * 256;
	} else {
		return NULL;
	}


	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* CapacityMiB */
		"  s:s,"   /* MemoryType */
		"  s:s,"   /* MemoryDeviceType */
		"  s:o,"   /* Status */
		"  s:{s:s}"  /* Metrics */
		"}",
		"@odata.id", uri,
		"@odata.type", "#Memory.v1_17_0.Memory",
		"Id", mem_id,
		"Name", name,
		"CapacityMiB", (json_int_t)capacity_mib,
		"MemoryType", mem_type,
		"MemoryDeviceType", mem_dev_type,
		"Status", rf_rollup_status(&r, true, false, true),
		"Metrics", "@odata.id", metrics_uri
	);

	/*
	 * DSP0288 Table 1: Get Partition Info / Set Partition Info mappings
	 * - VolatileSizeLimitMiB at root level (active volatile capacity)
	 * - CXL.StagedVolatileSizeMiB (next volatile capacity)
	 * - CXL.StagedNonVolatileSizeMiB (next persistent capacity)
	 * Only add if non-zero (device supports partitioning)
	 */
	if (volatile_limit_mib > 0)
		json_object_set_new(result, "VolatileSizeLimitMiB",
				    json_integer((json_int_t)volatile_limit_mib));

	/* CXL complex type for staged partition values */
	/*
	 * "If both this field and the Next Persistent Capacity field are 0,
	 * there is no pending change to the partitioning" (Table 8-310).
	 * When either is not, both say what the partitioning will be, and a
	 * 0 among them is a capacity of 0.
	 */
	if (staged_volatile_mib > 0 || staged_persistent_mib > 0) {
		json_t *cxl = json_object();
		json_object_set_new(cxl, "StagedVolatileSizeMiB",
				    json_integer((json_int_t)staged_volatile_mib));
		json_object_set_new(cxl, "StagedNonVolatileSizeMiB",
				    json_integer((json_int_t)staged_persistent_mib));
		json_object_set_new(result, "CXL", cxl);
	}

	/*
	 * DSP0288 Table 1: Get Security State -> Memory.SecurityStates
	 */
	uint32_t sec_state = fmd_get_security_state(comp, ld_id);
	json_object_set_new(result, "SecurityStates",
		json_pack("{s:b, s:b}",
			  "UserPassphraseAttemptCountReached", (sec_state >> 4) & 1,
			  "MasterPassphraseAttemptCountReached", (sec_state >> 5) & 1));

	/*
	 * DSP0288 Table 1: Scan Media -> the ScanMedia action of Memory. The
	 * security actions on Memory are those DSP0288 1.2.0 Table 1 mapped
	 * (SetPassphrase, SetMasterPassphrase, DisablePassphrase,
	 * DisableMasterPassphrase, FreezeSecurityState, SecureEraseUnit),
	 * now deprecated aliases of the CXLLogicalDevice actions DSP0288
	 * 1.3.0 and 1.4.0 map them to (sec_memory_actions()). An action whose
	 * command may not be sent to the device
	 * (fmd_mctp_restricted_allowed()), or that its CEL does not list, is
	 * left out: its POST is refused.
	 */
	json_t *acts = json_object();

	if (acts && fmd_mctp_restricted_allowed(ctx, comp, 0x4304))
		json_object_set_new(acts, "#Memory.ScanMedia", json_pack("{s:s}", "target",
			rf_uri(0, "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.ScanMedia",
			       chassis_id, mem_id)));
	if (acts)
		sec_memory_actions(ctx, chassis_id, mem_id, acts);
	if (acts && json_object_size(acts))
		json_object_set_new(result, "Actions", acts);
	else
		json_decref(acts);

	return result;
}

/*
 * MemoryDomain
 *
 * Per OCP CMS Memory Fabric Orchestration Architecture:
 * - Switch Chassis has aggregate pool domain (total capacity)
 * - Device Chassis: MLD maps to MemoryDomain
 * - Status reflects aggregate health of all LDs
 */
json_t *fmd_redfish_memory_domain(struct fmd_ctx *ctx,
				  const char *chassis_id,
				  const char *domain_id)
{
	struct fmd_component *chassis = fmd_topology_find(&ctx->topology, chassis_id);
	if (!chassis)
		return NULL;

	char uri[256];
	char chunks_uri[256];
	uint64_t capacity_mib = 0;
	uint64_t chunk_increment_mib = 0;
	struct fmd_status_rollup r = { 0 };
	const char *domain_name;

	snprintf(uri, sizeof(uri), "/redfish/v1/Chassis/%s/MemoryDomains/%s",
		 chassis_id, domain_id);
	snprintf(chunks_uri, sizeof(chunks_uri),
		 "/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks",
		 chassis_id, domain_id);

	if (chassis->type == FMD_COMPONENT_SWITCH) {
		/*
		 * Switch chassis aggregate pool domain.
		 * Domain ID should be {switch_id}_pool
		 */
		char expected_id[128];
		snprintf(expected_id, sizeof(expected_id), "%s_pool", chassis_id);
		if (strcmp(domain_id, expected_id) != 0)
			return NULL;

		domain_name = "CXL Switch Aggregate Memory Pool";

		/* Aggregate capacity and health from all attached devices */
		fmd_topology_rdlock(&ctx->topology);
		struct fmd_component *port_child;
		fmd_for_each_child(chassis, port_child) {
			if (port_child->type != FMD_COMPONENT_PORT)
				continue;

			struct fmd_component *dev_child;
			fmd_for_each_child(port_child, dev_child) {
				if (!fmd_is_memdev(dev_child))
					continue;

				rollup_memdev(&r, dev_child);
				if (dev_child->type == FMD_COMPONENT_SLD) {
					struct fmd_sld *sld = fmd_to_sld(dev_child);
					capacity_mib += sld->memory_size / (1024ULL * 1024);
				} else if (dev_child->type == FMD_COMPONENT_MLD) {
					struct fmd_mld *mld = fmd_to_mld(dev_child);
					struct fmd_component *ld_child;
					fmd_for_each_child(&mld->base, ld_child) {
						if (ld_child->type == FMD_COMPONENT_LD) {
							struct fmd_ld *ld = fmd_to_ld(ld_child);
							capacity_mib += ld->memory_size / (1024ULL * 1024);
						}
					}
				}
			}
		}
		fmd_topology_rdunlock(&ctx->topology);
	} else if (fmd_is_memdev(chassis)) {
		/* Device chassis: look up specific domain */
		struct fmd_mem_domain *dom = fmd_topology_find_domain(&ctx->topology, domain_id);
		if (!dom)
			return NULL;

		/* Verify the domain belongs to this chassis */
		if (!dom->base.parent || strcmp(dom->base.parent->id, chassis_id) != 0)
			return NULL;

		domain_name = "CXL Memory Domain";
		capacity_mib = dom->capacity / (1024ULL * 1024);

		/* Memory granularity from Get LD Allocations (DSP0288 Table 2) */
		if (chassis->type == FMD_COMPONENT_MLD)
			chunk_increment_mib = fmd_to_mld(chassis)->memory_granularity * 256;

		/* health from the device, as its Chassis' */
		fmd_topology_rdlock(&ctx->topology);
		rollup_memdev(&r, chassis);
		fmd_topology_rdunlock(&ctx->topology);
	} else {
		return NULL;
	}

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* MemorySizeMiB */
		"  s:{s:s},"  /* MemoryChunks */
		"  s:o"   /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#MemoryDomain.v1_5_1.MemoryDomain",
		"Id", domain_id,
		"Name", domain_name,
		"MemorySizeMiB", (json_int_t)capacity_mib,
		"MemoryChunks", "@odata.id", chunks_uri,
		"Status", rf_rollup_status(&r, true, false, false)
	);

	/* Add MemoryChunkIncrementMiB for MLDs (DSP0288 Table 2: Get LD Allocations) */
	if (chunk_increment_mib > 0)
		json_object_set_new(result, "MemoryChunkIncrementMiB",
				    json_integer((json_int_t)chunk_increment_mib));

	return result;
}

/*
 * MemoryMetrics - /redfish/v1/Chassis/{id}/Memory/{mem_id}/MemoryMetrics
 *
 * From Get Health Info (4200h), Table 8-315:
 * - HealthData.PredictedMediaLifeLeftPercent from Life Used. DSP0288
 *   Table 1 maps Life Used to CapacityUtilizationPercent, which the schema
 *   defines as "the memory capacity utilization as a percentage", not
 *   media life; serving 100 - Life Used as the media life left is
 *   cxl-fmd's choice. Life Used "Returns FFh if not implemented", and any
 *   value past 100 leaves the property out.
 * - DirtyShutdownCount from dirty_shutdown_count
 * - CorrectedVolatileErrorCount from corrected_volatile_error_count
 * - CorrectedPersistentErrorCount from corrected_persistent_error_count
 */
json_t *fmd_redfish_memory_metrics(struct fmd_ctx *ctx,
				   const char *chassis_id,
				   const char *mem_id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp))
		return NULL;

	char uri[256];
	uint8_t life_used = 0;
	uint32_t dirty_shutdown_count = 0;
	uint32_t corrected_volatile = 0;
	uint32_t corrected_persistent = 0;

	snprintf(uri, sizeof(uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/MemoryMetrics",
		 chassis_id, mem_id);

	if (comp->type == FMD_COMPONENT_SLD) {
		/* SLD: mem_id should match chassis_id */
		if (strcmp(mem_id, chassis_id) != 0)
			return NULL;

		struct fmd_sld *sld = fmd_to_sld(comp);
		life_used = sld->health.life_used;
		dirty_shutdown_count = sld->health.dirty_shutdown_count;
		corrected_volatile = sld->health.corrected_volatile_error_count;
		corrected_persistent = sld->health.corrected_persistent_error_count;
	} else if (comp->type == FMD_COMPONENT_MLD) {
		/* MLD: mem_id should be chassis_id_ld{n} */
		char prefix[128];
		int ld_id = -1;
		snprintf(prefix, sizeof(prefix), "%s_ld", chassis_id);

		if (strncmp(mem_id, prefix, strlen(prefix)) != 0)
			return NULL;

		ld_id = safe_parse_int(mem_id + strlen(prefix));
		if (ld_id < 0)
			return NULL;

		struct fmd_mld *mld = fmd_to_mld(comp);
		struct fmd_ld *target_ld = NULL;
		struct fmd_component *child;

		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mld->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				if (ld->ld_id == ld_id) {
					target_ld = ld;
					break;
				}
			}
		}
		fmd_topology_rdunlock(&ctx->topology);

		if (!target_ld)
			return NULL;

		life_used = target_ld->health.life_used;
		dirty_shutdown_count = target_ld->health.dirty_shutdown_count;
		corrected_volatile = target_ld->health.corrected_volatile_error_count;
		corrected_persistent = target_ld->health.corrected_persistent_error_count;
	} else if (comp->type == FMD_COMPONENT_MHD) {
		/* MHD: mem_id should be chassis_id_ld{n} */
		char prefix[128];
		int ld_id = -1;
		snprintf(prefix, sizeof(prefix), "%s_ld", chassis_id);

		if (strncmp(mem_id, prefix, strlen(prefix)) != 0)
			return NULL;

		ld_id = safe_parse_int(mem_id + strlen(prefix));
		if (ld_id < 0)
			return NULL;

		struct fmd_mhd *mhd = fmd_to_mhd(comp);
		struct fmd_ld *target_ld = NULL;
		struct fmd_component *child;

		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mhd->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				if (ld->ld_id == ld_id) {
					target_ld = ld;
					break;
				}
			}
		}
		fmd_topology_rdunlock(&ctx->topology);

		if (!target_ld)
			return NULL;

		life_used = target_ld->health.life_used;
		dirty_shutdown_count = target_ld->health.dirty_shutdown_count;
		corrected_volatile = target_ld->health.corrected_volatile_error_count;
		corrected_persistent = target_ld->health.corrected_persistent_error_count;
	} else {
		return NULL;
	}

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* DirtyShutdownCount */
		"  s:I,"   /* CorrectedVolatileErrorCount */
		"  s:I"    /* CorrectedPersistentErrorCount */
		"}",
		"@odata.id", uri,
		"@odata.type", "#MemoryMetrics.v1_6_0.MemoryMetrics",
		"Id", "MemoryMetrics",
		"Name", "Memory Metrics",
		"DirtyShutdownCount", (json_int_t)dirty_shutdown_count,
		"CorrectedVolatileErrorCount", (json_int_t)corrected_volatile,
		"CorrectedPersistentErrorCount", (json_int_t)corrected_persistent
	);

	/*
	 * Life Used is "a percentage value (0 to 100)" and "Returns FFh if
	 * not implemented" (Table 8-315): only a percentage gives the life
	 * left. Anything else is not a value to report.
	 */
	if (result && life_used <= 100)
		json_object_set_new(result, "HealthData",
			json_pack("{s:i}", "PredictedMediaLifeLeftPercent",
				  100 - life_used));

	return result;
}

/*
 * MemoryChunks Collection
 *
 * Per OCP CMS Figure 7:
 * - Switch chassis pool has chunks per attached device (or per-LD for MLDs)
 * - Device chassis has individual chunks per LD
 */
json_t *fmd_redfish_memory_chunks(struct fmd_ctx *ctx,
				  const char *chassis_id,
				  const char *domain_id)
{
	struct rf_collection coll;
	struct fmd_component *chassis = fmd_topology_find(&ctx->topology, chassis_id);
	struct fmd_component *port_child, *dev_child, *child;

	if (!chassis)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks",
				  chassis_id, domain_id),
			   "#MemoryChunksCollection.MemoryChunksCollection",
			   "Memory Chunks Collection");

	fmd_topology_rdlock(&ctx->topology);

	if (chassis->type == FMD_COMPONENT_SWITCH) {
		/* Switch chassis: enumerate chunks from all attached devices */
		char expected_id[128];
		snprintf(expected_id, sizeof(expected_id), "%s_pool", chassis_id);
		if (strcmp(domain_id, expected_id) != 0) {
			fmd_topology_rdunlock(&ctx->topology);
			json_decref(coll.members);
			return NULL;
		}

		fmd_for_each_child_type(chassis, port_child, FMD_COMPONENT_PORT) {
			fmd_for_each_child(port_child, dev_child) {
				if (dev_child->type == FMD_COMPONENT_SLD) {
					rf_collection_add(&coll,
						"/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s_chunk",
						chassis_id, domain_id, dev_child->id);
				} else if (dev_child->type == FMD_COMPONENT_MLD) {
					fmd_for_each_child_type(dev_child, child, FMD_COMPONENT_LD) {
						struct fmd_ld *ld = fmd_to_ld(child);
						rf_collection_add(&coll,
							"/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s_ld%d_chunk",
							chassis_id, domain_id, dev_child->id, ld->ld_id);
					}
				}
			}
		}
	} else if (fmd_is_memdev(chassis)) {
		/* Device chassis: enumerate chunk children of the domain */
		struct fmd_mem_domain *dom = fmd_topology_find_domain(&ctx->topology, domain_id);
		if (!dom || !dom->base.parent || strcmp(dom->base.parent->id, chassis_id) != 0) {
			fmd_topology_rdunlock(&ctx->topology);
			json_decref(coll.members);
			return NULL;
		}

		fmd_for_each_child_type(&dom->base, child, FMD_COMPONENT_CHUNK) {
			rf_collection_add(&coll,
				"/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s",
				chassis_id, domain_id, child->id);
		}
	} else {
		fmd_topology_rdunlock(&ctx->topology);
		json_decref(coll.members);
		return NULL;
	}

	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * MemoryChunk
 *
 * Per OCP CMS Memory Fabric Orchestration Architecture:
 * - Switch chassis pool: chunks reference device CXLLogicalDevices
 * - Device chassis: chunks reference device's own CXLLogicalDevices
 */
json_t *fmd_redfish_memory_chunk(struct fmd_ctx *ctx,
				 const char *chassis_id,
				 const char *domain_id,
				 const char *chunk_id)
{
	struct fmd_component *chassis = fmd_topology_find(&ctx->topology, chassis_id);
	if (!chassis)
		return NULL;

	char uri[256];
	snprintf(uri, sizeof(uri),
		 "/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s",
		 chassis_id, domain_id, chunk_id);

	uint64_t size_mib = 0;
	uint64_t offset_mib = 0;
	bool offset_known = false;
	json_t *range_type = NULL;
	struct fmd_status_rollup r = { 0 };
	const char *chunk_name = "CXL Memory Chunk";
	char ld_link_uri[512] = {0};
	const struct fmd_mem_chunk *found = NULL;	/* a device's chunk */

	if (chassis->type == FMD_COMPONENT_SWITCH) {
		/*
		 * Switch chassis aggregate pool chunk.
		 * Chunk ID format: {device_id}_chunk or {device_id}_ld{N}_chunk
		 */
		char expected_domain[128];
		snprintf(expected_domain, sizeof(expected_domain), "%s_pool", chassis_id);
		if (strcmp(domain_id, expected_domain) != 0)
			return NULL;

		/* Parse device ID and LD index from chunk_id */
		char device_id[FMD_MAX_ID_LEN] = {0};
		int ld_idx = -1;  /* -1 means SLD (single chunk) */

		const char *ld_marker = strstr(chunk_id, "_ld");
		if (ld_marker) {
			/* MLD chunk: {device}_ld{N}_chunk */
			size_t dev_len = ld_marker - chunk_id;
			if (dev_len >= sizeof(device_id))
				return NULL;
			memcpy(device_id, chunk_id, dev_len);
			device_id[dev_len] = '\0';
			ld_idx = safe_parse_int(ld_marker + 3);
			if (ld_idx < 0)
				return NULL;
		} else {
			/* SLD chunk: {device}_chunk */
			const char *chunk_suffix = strstr(chunk_id, "_chunk");
			if (!chunk_suffix)
				return NULL;
			size_t dev_len = chunk_suffix - chunk_id;
			if (dev_len >= sizeof(device_id))
				return NULL;
			memcpy(device_id, chunk_id, dev_len);
			device_id[dev_len] = '\0';
			ld_idx = 0;
		}

		/* Find the device */
		struct fmd_component *device = fmd_topology_find(&ctx->topology, device_id);
		if (!device || !fmd_is_memdev(device))
			return NULL;

		fmd_topology_rdlock(&ctx->topology);

		if (device->type == FMD_COMPONENT_SLD) {
			struct fmd_sld *sld = fmd_to_sld(device);
			size_mib = sld->memory_size / (1024ULL * 1024);
			offset_known = true;	/* the whole device, from 0 */
			fmd_rollup_add(&r, &sld->health, sld->health_valid);
			range_type = json_string_or_null(fmd_range_type(&sld->memdev,
									&sld->partition));

			/* Link to device's CXLLogicalDevice */
			snprintf(ld_link_uri, sizeof(ld_link_uri),
				 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/0",
				 device_id, device_id);
		} else if (device->type == FMD_COMPONENT_MLD) {
			struct fmd_mld *mld = fmd_to_mld(device);
			struct fmd_component *child;

			/* Now find the target LD for other properties */
			fmd_for_each_child(&mld->base, child) {
				if (child->type == FMD_COMPONENT_LD) {
					struct fmd_ld *ld = fmd_to_ld(child);
					if (ld->ld_id == ld_idx) {
						size_mib = ld->memory_size / (1024ULL * 1024);
						fmd_rollup_add(&r, &ld->health, ld->health_valid);
						range_type = json_string_or_null(
							fmd_range_type(&ld->memdev, &ld->partition));

						/* Link to device's CXLLogicalDevice */
						snprintf(ld_link_uri, sizeof(ld_link_uri),
							 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%d",
							 device_id, device_id, ld_idx);
						break;
					}
				}
			}
		}

		fmd_topology_rdunlock(&ctx->topology);

		if (size_mib == 0) {
			json_decref(range_type);
			return NULL;
		}

		chunk_name = "CXL Pool Memory Chunk";

	} else if (fmd_is_memdev(chassis)) {
		/* Device chassis: look up the actual chunk */
		struct fmd_mem_chunk *chunk = fmd_topology_find_chunk(&ctx->topology, chunk_id);
		if (!chunk)
			return NULL;

		/* Verify ownership: chunk -> domain -> chassis */
		if (!chunk->base.parent ||
		    chunk->base.parent->type != FMD_COMPONENT_DOMAIN ||
		    strcmp(chunk->base.parent->id, domain_id) != 0)
			return NULL;

		struct fmd_mem_domain *dom = fmd_to_domain(chunk->base.parent);
		if (!dom->base.parent || strcmp(dom->base.parent->id, chassis_id) != 0)
			return NULL;

		found = chunk;
		size_mib = chunk->size / (1024ULL * 1024);
		offset_mib = chunk->base_address / (1024ULL * 1024);
		offset_known = chassis->type == FMD_COMPONENT_SLD;
		/* an SLD's chunk is of its LD 0, any other of its backing LD */
		int ld_idx = chassis->type == FMD_COMPONENT_SLD ? 0 : chunk->backing_ld;

		/* the type it was made with; its LD's health, an MHD's too */
		range_type = fmd_chunk_range_type(chunk);
		fmd_topology_rdlock(&ctx->topology);
		if (chassis->type == FMD_COMPONENT_SLD) {
			struct fmd_sld *sld = fmd_to_sld(chassis);

			fmd_rollup_add(&r, &sld->health, sld->health_valid);
		} else {
			const struct fmd_ld *cl = fmd_chunk_ld(chassis, chunk);

			if (cl)
				fmd_rollup_add(&r, &cl->health, cl->health_valid);
		}
		fmd_topology_rdunlock(&ctx->topology);

		/* Link to this chassis's CXLLogicalDevice, if it has one */
		if (ld_idx >= 0)
			snprintf(ld_link_uri, sizeof(ld_link_uri),
				 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%d",
				 chassis_id, chassis_id, ld_idx);
	} else {
		return NULL;
	}


	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* MemoryChunkSizeMiB */
		"  s:o,"   /* AddressRangeType */
		"  s:b,"   /* IsMirrorEnabled */
		"  s:o"   /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#MemoryChunks.v1_6_2.MemoryChunks",
		"Id", chunk_id,
		"Name", chunk_name,
		"MemoryChunkSizeMiB", (json_int_t)size_mib,
		"AddressRangeType", range_type ? range_type : json_null(),
		"IsMirrorEnabled", false,
		"Status", chunk_status(&r, found)
	);

	/*
	 * AddressRangeOffsetMiB is where the chunk lies in the device's
	 * address range. The chunk of an SLD is given one. The chunk of an
	 * LD is that LD's allocation, and nothing says where in the device
	 * an allocation is: Get LD Allocations gives sizes only. Adding up
	 * the sizes of the LDs before it would be a guess.
	 */
	if (offset_known)
		json_object_set_new(result, "AddressRangeOffsetMiB",
				    json_integer((json_int_t)offset_mib));

	/* what a staged chunk waits for */
	if (chassis->type == FMD_COMPONENT_SLD && found && found->staged)
		json_object_set_new(result, "Oem",
				    staged_chunk_oem(fmd_to_sld(chassis), found));

	/* Add Links.CXLLogicalDevices back-reference */
	if (ld_link_uri[0] != '\0') {
		json_object_set_new(result, "Links",
			rf_links_array_single("CXLLogicalDevices",
				"%s", ld_link_uri));
	}

	return result;
}


/*
 * A chunk a binding exposes is not changed under the host (cxl-fmd's
 * choice: the specification does not say whether Set LD Allocations or
 * Set Partition Info may be used on an LD or an SLD that is bound). The
 * Connection that exposes @chunk is a vPPB binding, not Absent, whose
 * chunks (fmd_conn_for_each_chunk()) would include it: one of the device,
 * of every chunk's LD for an SLD, or of the chunk's LD. The chunk need not
 * be there yet: a POST is refused before anything is made or sent. A
 * direct-attached Connection is none: it has no binding to undo, cannot
 * be deleted, and would keep the device's memory from ever being changed.
 * The exception is a pool LD of a Multi-Headed device, which any
 * Connection of its head holds (see ld_connected()).
 */
struct chunk_user {
	const struct fmd_component *dev;
	const struct fmd_ld *ld;	/* none for an SLD, or a chunk of no LD */
	const struct fmd_head *head;	/* of the chunk's pool LD, if any */
	char id[FMD_CONN_ID_LEN];
};

static int chunk_user_conn(const struct fmd_conn_view *v, void *arg)
{
	struct chunk_user *u = arg;

	if (v->state == FMD_CONN_ABSENT)
		return 0;
	/* fmd_conn_for_each_chunk(): a head on a switch reaches no chunk */
	if (!(u->head && v->head == u->head) &&
	    (v->kind != FMD_CONN_SWITCH || !v->target || v->device != u->dev ||
	     v->target->type == FMD_COMPONENT_HEAD || (v->ld && v->ld != u->ld)))
		return 0;
	snprintf(u->id, sizeof(u->id), "%s", v->id);
	return 1;
}

/*
 * Whether a Connection exposes the chunks of @ld of @dev (NULL: of an SLD,
 * or of no LD), logged as refusing @what if one does.
 *
 * A pool LD of a Multi-Headed device is exposed by any Connection of the
 * head the LD Map maps it to (Table 7-61), direct-attached ones included,
 * and Unresolved ones: which LD-ID of a head that presents an MLD a pool
 * LD is, "The MH-MLD maps ... to the device-wide LD index in the MH-MLD's
 * LD pool" (CXL r4.0 Section 2.5.1) by no rule the FM is told, so every
 * pool LD of a head that has a host or a binding is taken to be in use
 * (cxl-fmd's choice).
 */
static bool ld_connected(struct fmd_ctx *ctx, const struct fmd_component *dev,
			 const struct fmd_ld *ld, const char *what)
{
	struct chunk_user u = { .dev = dev, .ld = ld };
	int rc;

	fmd_topology_rdlock(&ctx->topology);
	if (dev->type == FMD_COMPONENT_MHD && ld)
		u.head = fmd_ld_head(ld);
	rc = fmd_conn_for_each(ctx, chunk_user_conn, &u);
	fmd_topology_rdunlock(&ctx->topology);
	if (rc == 1)
		fmd_warn(ctx, "%s is exposed by Connection %s: refused", what, u.id);
	return rc == 1;
}

/*
 * What a fresh read of the switches that can expose a chunk shows
 * (binding_coverage()).
 */
enum chunk_cover {
	COVER_NONE = 0,		/* read whole, and no vPPB exposes it */
	COVER_BOUND,		/* a vPPB exposes it, or may: refused (409) */
	COVER_UNREAD,		/* a VCS could not be read: try again (503) */
	COVER_INCOMPLETE,	/* not all that could expose it is seen (409) */
};

#define COVER_MAX_PORTS (FMD_MAX_HEADS + FMD_MAX_ROUTES)

/* a switch port that can expose the chunk, and what of it does */
struct cover_port {
	struct fmd_switch *sw;
	uint8_t port;
	/* an LD-ID only a 03h binding of that LD exposes; FMD_LD_NONE: any */
	uint16_t ld_id;
	/* the head on it is not known: any binding of the port is incomplete */
	bool unplaced;
};

static int cover_add(struct cover_port *p, int n, struct fmd_switch *sw,
		     uint8_t port, uint16_t ld_id, bool unplaced)
{
	for (int i = 0; i < n; i++) {
		if (p[i].sw == sw && p[i].port == port) {
			/* the stricter of the two */
			if (ld_id == FMD_LD_NONE)
				p[i].ld_id = FMD_LD_NONE;
			p[i].unplaced &= unplaced;
			return n;
		}
	}
	if (n == COVER_MAX_PORTS)
		return n;
	p[n] = (struct cover_port){ .sw = sw, .port = port, .ld_id = ld_id,
				    .unplaced = unplaced };
	return n + 1;
}

/* the switch whose endpoint @ep is, NULL if none */
static struct fmd_switch *switch_on_ep(struct fmd_ctx *ctx,
				       const struct cxlmi_endpoint *ep)
{
	struct fmd_component *comp;

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SWITCH) {
		if (ep && comp->ep == ep)
			return fmd_to_switch(comp);
	}
	return NULL;
}

/*
 * The switch ports that can expose the chunks of @ld of @dev (NULL: of an
 * SLD, or of no LD), into @p: the port an SLD or an MLD is on; for a pool
 * LD of a Multi-Headed device, the port its head is placed on, and every
 * switch port the device was found through with no head placed on it.
 * *@incomplete is set when what a head of the device presents is not
 * known (FMD_HEAD_UNKNOWN): which head a binding reaches cannot be told.
 * Caller holds the topology read lock.
 */
static int cover_ports(struct fmd_ctx *ctx, const struct fmd_component *dev,
		       const struct fmd_ld *ld, struct cover_port *p,
		       bool *incomplete)
{
	struct fmd_component *child;
	int n = 0;

	*incomplete = false;
	if (dev->type == FMD_COMPONENT_SLD || dev->type == FMD_COMPONENT_MLD) {
		const struct fmd_port *port;

		if (!dev->parent || dev->parent->type != FMD_COMPONENT_PORT ||
		    !dev->parent->parent ||
		    dev->parent->parent->type != FMD_COMPONENT_SWITCH)
			return 0;	/* direct-attached: not gated */
		port = fmd_to_port(dev->parent);
		return cover_add(p, n, fmd_to_switch(dev->parent->parent),
				 port->port_number,
				 dev->type == FMD_COMPONENT_MLD && ld ?
				 ld->ld_id : FMD_LD_NONE, false);
	}
	if (dev->type != FMD_COMPONENT_MHD)
		return 0;

	{
		const struct fmd_mhd *mhd = fmd_to_mhd(dev);
		const struct fmd_head *head = ld ? fmd_ld_head(ld) : NULL;

		fmd_for_each_child_type(dev, child, FMD_COMPONENT_HEAD) {
			const struct fmd_head *h = fmd_to_head(child);

			if (h->presentation == FMD_HEAD_UNKNOWN)
				*incomplete = true;
			/*
			 * The LD's head: any binding of its port (a head
			 * that presents an MLD maps its LD-IDs to pool LDs
			 * by no rule the FM is told, Section 2.5.1)
			 */
			if (h == head && h->port && h->port->base.parent)
				n = cover_add(p, n, fmd_to_switch(h->port->base.parent),
					      h->port->port_number, FMD_LD_NONE,
					      false);
		}
		/* a switch port the device was found through, no head there */
		for (int i = 0; i < mhd->nroutes; i++) {
			const struct fmd_route *r = &mhd->routes[i];
			struct fmd_switch *sw;
			struct fmd_port *port;

			if (!r->tunnel.level)
				continue;
			sw = switch_on_ep(ctx, r->ep);
			port = sw ? fmd_topology_find_port(&ctx->topology,
							   sw->base.id,
							   r->tunnel.port) : NULL;
			if (!port || port->head)
				continue;
			n = cover_add(p, n, sw, r->tunnel.port, FMD_LD_NONE, true);
		}
	}
	return n;
}

/*
 * Whether a binding of a switch can expose the chunks of @ld of @dev, as
 * the switch is now: each switch with a port that can expose them
 * (cover_ports()) is read again (fmd_vcs_refresh()), and every vPPB of
 * every VCS read is looked at, whether or not its VCS has a host and so a
 * Connection. A vPPB exposes them when it is bound to the port (02h), or
 * to the port and the LD (03h; any LD of a port whose LD is not named, or
 * that a head of a Multi-Headed device is on). One that is Busy (01h), or
 * bound to a PBR ID (04h), names no port: it may, so it is taken to
 * (cxl-fmd's choice). A binding read now is COVER_BOUND, whatever else is
 * so; a switch not read whole, or a head that cannot be told, is
 * COVER_INCOMPLETE; and a VCS that could not be read (whose table, as last
 * read, is not looked at) COVER_UNREAD. Logged as refusing @what. Called
 * with fabric_lock held exclusively and the topology lock not held.
 */
static enum chunk_cover binding_coverage(struct fmd_ctx *ctx,
					 const struct fmd_component *dev,
					 const struct fmd_ld *ld,
					 const char *what)
{
	struct cover_port p[COVER_MAX_PORTS];
	enum chunk_cover cover = COVER_NONE;
	bool incomplete, partial = false;
	int n;

	fmd_topology_rdlock(&ctx->topology);
	n = cover_ports(ctx, dev, ld, p, &incomplete);
	fmd_topology_rdunlock(&ctx->topology);
	if (incomplete) {
		fmd_warn(ctx, "%s: what a head of %s presents is not known, so "
			 "which head a binding reaches cannot be told: refused",
			 what, dev->id);
		return COVER_INCOMPLETE;
	}

	/* each switch once, outside the topology lock, which it takes */
	for (int i = 0; i < n; i++) {
		bool seen = false;

		for (int j = 0; j < i; j++)
			seen |= p[j].sw == p[i].sw;
		if (!seen)
			fmd_vcs_refresh(ctx, p[i].sw);
	}

	fmd_topology_rdlock(&ctx->topology);
	for (int i = 0; i < n && cover != COVER_BOUND; i++) {
		struct fmd_switch *sw = p[i].sw;

		if (sw->vcs_unread && cover == COVER_NONE)
			cover = COVER_UNREAD;
		if (sw->vcs_incomplete)
			cover = COVER_INCOMPLETE, partial = true;
		for (int v = 0; v < sw->num_vcs && cover != COVER_BOUND; v++) {
			const struct fmd_vcs *vcs = &sw->vcs[v];

			/* what it held before says nothing of it now */
			if (vcs->stale) {
				if (cover == COVER_NONE)
					cover = COVER_UNREAD;
				continue;
			}
			if (vcs->truncated)
				cover = COVER_INCOMPLETE, partial = true;
			for (int b = 0; b < vcs->num_vppbs; b++) {
				const struct fmd_vppb *vppb = &vcs->vppbs[b];
				bool hit;

				switch (vppb->binding_status) {
				case FMD_VPPB_BUSY:
				case FMD_VPPB_BOUND_PBR:
					hit = true;
					break;
				case FMD_VPPB_BOUND_PHYSICAL_PORT:
					hit = vppb->bound_port_id == p[i].port;
					break;
				case FMD_VPPB_BOUND_LD:
					hit = vppb->bound_port_id == p[i].port &&
					      (p[i].ld_id == FMD_LD_NONE ||
					       vppb->bound_ld_id == p[i].ld_id);
					break;
				default:
					hit = false;
				}
				if (!hit)
					continue;
				fmd_warn(ctx, "%s: vPPB %d of VCS %d of %s reads %d "
					 "(port %d, LD %d)%s: refused", what,
					 vppb->vppb_id, vcs->vcs_id, sw->base.id,
					 vppb->binding_status, vppb->bound_port_id,
					 vppb->bound_ld_id, p[i].unplaced ?
					 ", on a port whose head is not known" : "");
				cover = p[i].unplaced &&
					vppb->binding_status != FMD_VPPB_BUSY ?
					COVER_INCOMPLETE : COVER_BOUND;
				break;
			}
		}
	}
	fmd_topology_rdunlock(&ctx->topology);

	if (cover == COVER_UNREAD)
		fmd_warn(ctx, "%s: a VCS of a switch that can expose it could not "
			 "be read: refused, nothing sent", what);
	else if (cover == COVER_INCOMPLETE && partial)
		fmd_warn(ctx, "%s: a switch that can expose it is not read whole "
			 "(more than %d VCSs, or a VCS of more than %d vPPBs): "
			 "refused, nothing sent", what, FMD_MAX_VCS, FMD_MAX_VPPB);
	return cover;
}

/* Whether a Connection exposes @chunk: see ld_connected() */
static bool chunk_connected(struct fmd_ctx *ctx, const struct fmd_mem_chunk *chunk)
{
	const struct fmd_component *dev = chunk->base.parent ?
					  chunk->base.parent->parent : NULL;
	char what[FMD_MAX_ID_LEN + 8];

	if (!dev)
		return false;
	snprintf(what, sizeof(what), "chunk %s", chunk->base.id);
	return ld_connected(ctx, dev, fmd_chunk_ld(dev, chunk), what);
}

/*
 * The URIs an LD of a memory device is served at: its Memory resource
 * (fmd_redfish_memory_collection()) and its CXLLogicalDevice. @ld is NULL
 * for the one LD of an SLD. Each returns 0, or -ENAMETOOLONG if @len is
 * too short for it.
 */
static int ld_memory_uri(char *buf, size_t len, const struct fmd_component *dev,
			 const struct fmd_ld *ld)
{
	int n = ld ? snprintf(buf, len, "/redfish/v1/Chassis/%s/Memory/%s_ld%u",
			      dev->id, dev->id, ld->ld_id)
		   : snprintf(buf, len, "/redfish/v1/Chassis/%s/Memory/%s",
			      dev->id, dev->id);

	return n >= 0 && (size_t)n < len ? 0 : -ENAMETOOLONG;
}

static int ld_device_uri(char *buf, size_t len, const struct fmd_component *dev,
			 const struct fmd_ld *ld)
{
	int n = snprintf(buf, len,
			 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%u",
			 dev->id, dev->id, ld ? ld->ld_id : 0);

	return n >= 0 && (size_t)n < len ? 0 : -ENAMETOOLONG;
}

#define RF_LD_URI_LEN (sizeof("/redfish/v1/Chassis//PCIeDevices//CXLLogicalDevices/") + \
		       2 * FMD_MAX_ID_LEN + 8)

/*
 * The LD of @dev that @uri names: the URI of its Memory resource if
 * @memory, else of its CXLLogicalDevice, exactly as they are served.
 * Returns its LD-ID (0 for an SLD), or -1 if @uri is no such URI.
 */
static int resolve_ld(struct fmd_ctx *ctx, const struct fmd_component *dev,
		      const char *uri, bool memory)
{
	char canon[RF_LD_URI_LEN];
	struct fmd_component *child;
	int found = -1;

	if (dev->type == FMD_COMPONENT_SLD) {
		if (memory ? ld_memory_uri(canon, sizeof(canon), dev, NULL)
			   : ld_device_uri(canon, sizeof(canon), dev, NULL))
			return -1;
		return strcmp(canon, uri) ? -1 : 0;
	}

	fmd_topology_rdlock(&ctx->topology);
	fmd_for_each_child_type(dev, child, FMD_COMPONENT_LD) {
		const struct fmd_ld *ld = fmd_to_ld(child);

		if (memory ? ld_memory_uri(canon, sizeof(canon), dev, ld)
			   : ld_device_uri(canon, sizeof(canon), dev, ld))
			continue;
		if (!strcmp(canon, uri)) {
			found = ld->ld_id;
			break;
		}
	}
	fmd_topology_rdunlock(&ctx->topology);
	return found;
}

/* The @odata.id of link @v, NULL if @v is not an object with one */
static const char *link_odata_id(const json_t *v)
{
	return json_is_object(v) ?
	       json_string_value(json_object_get(v, "@odata.id")) : NULL;
}

/*
 * A chunk POST, checked before anything is created or sent: its size and
 * offset in bytes, its type, and the LD whose allocation it grows.
 */
struct chunk_plan {
	uint64_t size;
	uint64_t offset;
	bool offset_given;
	char type[16];
	int ld;				/* LD-ID or pool index, -1 for none */
};

/*
 * Parse @root into @plan for a chunk of @dev. Returns 0, or the code
 * fmd_redfish_post_memory_chunk() answers with (-2, -6 or -8 to -12).
 *
 * The LD is named as the URI of its Memory resource, InterleaveSets[0].
 * Memory, or of its CXLLogicalDevice, Links.CXLLogicalDevices[0], or
 * both if they name the same LD. Set LD Allocations (5402h) gives an LD
 * "Range 1 Allocation Multiplier" and "Range 2 Allocation Multiplier"
 * (CXL r4.0 Table 7-49), nothing else: a chunk grows one LD, so one
 * InterleaveSets entry at most, whose SizeMiB is the chunk's, and whose
 * OffsetMiB, RegionId and MemoryLevel are the defaults (0, none and 1:
 * cxl-fmd's), which is all an allocation can be.
 */
static int parse_chunk_plan(struct fmd_ctx *ctx, const struct fmd_component *dev,
			    json_t *root, struct chunk_plan *plan)
{
	json_t *type_j, *offset_j, *sets_j, *set = NULL, *size_j, *links_j, *lds_j;
	const char *mem_uri = NULL, *ld_uri = NULL;
	uint64_t set_size = 0, mib_bytes;
	bool set_size_given = false;
	int ld = -1;

	memset(plan, 0, sizeof(*plan));
	plan->ld = -1;

	/*
	 * AddressRangeType (optional): PMEM or Volatile. Any other value,
	 * Block included, which cxl-fmd cannot make, is a bad request rather
	 * than a chunk of another type than the one asked for. Left empty if
	 * not given: chunk_plan_type() gives it the device's.
	 */
	type_j = json_object_get(root, "AddressRangeType");
	if (type_j) {
		const char *type_str = json_string_value(type_j);

		if (!type_str || (strcmp(type_str, "PMEM") && strcmp(type_str, "Volatile")))
			return -8;
		snprintf(plan->type, sizeof(plan->type), "%s", type_str);
	}

	/* AddressRangeOffsetMiB (optional): auto-assigned if omitted */
	offset_j = json_object_get(root, "AddressRangeOffsetMiB");
	if (offset_j) {
		if (rf_mib_to_bytes(offset_j, true, &plan->offset))
			return -9;
		plan->offset_given = true;
	}

	/* InterleaveSets (optional): one entry at most */
	sets_j = json_object_get(root, "InterleaveSets");
	if (sets_j) {
		if (!json_is_array(sets_j) || json_array_size(sets_j) > 1)
			return -11;
		set = json_array_get(sets_j, 0);
		if (set && !json_is_object(set))
			return -11;
	}
	if (set) {
		json_t *mem_j = json_object_get(set, "Memory");
		json_t *v;

		if (mem_j) {
			mem_uri = link_odata_id(mem_j);
			if (!mem_uri)
				return -11;
		}
		v = json_object_get(set, "OffsetMiB");
		if (v) {
			if (rf_mib_to_bytes(v, true, &mib_bytes))
				return -10;
			if (mib_bytes)
				return -12;
		}
		v = json_object_get(set, "SizeMiB");
		if (v) {
			if (rf_mib_to_bytes(v, true, &set_size))
				return -10;
			set_size_given = true;
		}
		v = json_object_get(set, "MemoryLevel");
		if (v && (!json_is_integer(v) || json_integer_value(v) != 1))
			return -12;
		v = json_object_get(set, "RegionId");
		if (v && !json_is_null(v) &&
		    (!json_is_string(v) || json_string_length(v)))
			return -12;
	}

	/*
	 * MemoryChunkSizeMiB, > 0. If it is not given, InterleaveSets[0].
	 * SizeMiB is used instead; if both are, they are the same.
	 */
	size_j = json_object_get(root, "MemoryChunkSizeMiB");
	if (size_j) {
		if (rf_mib_to_bytes(size_j, false, &plan->size))
			return -10;
		if (set_size_given && set_size != plan->size)
			return -12;
	} else if (set_size) {
		plan->size = set_size;
	} else {
		return -2;
	}

	/* Links.CXLLogicalDevices (optional): one entry at most */
	links_j = json_object_get(root, "Links");
	lds_j = json_is_object(links_j) ? json_object_get(links_j, "CXLLogicalDevices") : NULL;
	if (lds_j) {
		if (!json_is_array(lds_j) || json_array_size(lds_j) > 1)
			return -11;
		if (json_array_size(lds_j)) {
			ld_uri = link_odata_id(json_array_get(lds_j, 0));
			if (!ld_uri)
				return -11;
		}
	}

	if (mem_uri) {
		ld = resolve_ld(ctx, dev, mem_uri, true);
		if (ld < 0)
			return -11;
	}
	if (ld_uri) {
		int n = resolve_ld(ctx, dev, ld_uri, false);

		if (n < 0 || (mem_uri && n != ld))
			return -11;
		ld = n;
	}

	if (dev->type == FMD_COMPONENT_SLD)
		return 0;		/* named or not, its one LD */
	if (ld < 0)
		return -11;		/* an MLD's or MHD's chunk grows an LD */
	plan->ld = ld;

	/*
	 * The LD's size is a multiple of the Memory Granularity (Table
	 * 7-48), as Get LD Info or Get LD Allocations last gave it (0 if
	 * not known). Set LD Allocations itself is refused before it is
	 * sent if Get LD Allocations then gives another one, or a Range 2
	 * the size is short of (set_ld_allocation()).
	 */
	{
		const struct fmd_ld *l = fmd_chunk_ld(dev, &(struct fmd_mem_chunk){
						      .backing_ld = ld });
		uint64_t gran = (uint64_t)(dev->type == FMD_COMPONENT_MLD ?
				 fmd_to_mld((struct fmd_component *)dev)->memory_granularity :
				 fmd_to_mhd((struct fmd_component *)dev)->memory_granularity) *
				CXL_CAPACITY_UNIT;
		uint64_t total;

		if (!l || __builtin_add_overflow(l->memory_size, plan->size, &total) ||
		    (gran && total % gran))
			return -6;
	}
	return 0;
}

/*
 * Whether a request that sends @opcode to memory device @dev is refused
 * under note 4 of Table 8-308 (fmd_mctp_restricted_allowed()). An SLD on a
 * switch port is sent nothing on hardware, and is refused for that as it
 * was; a mock's is answered as any mock device's.
 */
static bool mctp_refused(struct fmd_ctx *ctx, const struct fmd_component *dev,
			 uint16_t opcode)
{
	if (likely(!fmd_is_mock(ctx)) && fmd_sld_behind_switch(dev))
		return false;
	return !fmd_mctp_restricted_allowed(ctx, dev, opcode);
}

/*
 * Whether a chunk of @type of @dev changes a partition, with Set Partition
 * Info (4101h): a Volatile chunk of an SLD that has partitionable capacity
 * (fmd_partitionable()). Without any the command "shall fail with an
 * Unsupported error" (CXL r4.0 Section 8.2.10.9.2.2), and a Volatile
 * chunk, of the Volatile Only capacity, only divides cxl-fmd's record of
 * the domain, as a PMEM chunk does: nothing is sent for it, and it is not
 * refused while the SLD is bound (cxl-fmd's choice).
 */
static bool chunk_repartitions(const struct fmd_component *dev, const char *type)
{
	return dev->type == FMD_COMPONENT_SLD && !strcmp(type, "Volatile") &&
	       fmd_partitionable(&fmd_to_sld(dev)->memdev);
}

/*
 * The AddressRangeType of the chunk of @plan, on @dev and its LD @ld (NULL
 * for an SLD). One asked for is taken if the device or LD can have memory
 * of that type (fmd_range_type_possible()), or if nothing is known of its
 * memory (an LD that did not answer Identify Memory Device). One not asked
 * for is the type all of its memory is (fmd_range_type()): on an SLD, whose
 * Volatile chunks change the partition, there has to be one; the chunk of
 * an LD, which grows its allocation whatever the type, is left with none
 * known (null) if its memory is of both types or not known. Returns 0, -15
 * if an SLD's chunk has no type, or -16 if the type asked for cannot be.
 * cxl-fmd's rules: a chunk is not given a type its memory does not have, as
 * it was given PMEM whatever the device.
 */
static int chunk_plan_type(const struct fmd_component *dev, const struct fmd_ld *ld,
			   struct chunk_plan *plan)
{
	const struct cxlmi_cmd_memdev_identify_rsp *id;
	const struct cxlmi_cmd_memdev_get_partition_info_rsp *part;
	const char *type;

	if (dev->type == FMD_COMPONENT_SLD) {
		id = &fmd_to_sld((struct fmd_component *)dev)->memdev;
		part = &fmd_to_sld((struct fmd_component *)dev)->partition;
	} else if (ld) {
		id = &ld->memdev;
		part = &ld->partition;
	} else {
		return -15;
	}
	if (plan->type[0]) {
		if (dev->type != FMD_COMPONENT_SLD && !id->total_capacity)
			return 0;	/* nothing known of the LD's memory */
		return fmd_range_type_possible(id, part, plan->type) ? 0 : -16;
	}
	type = fmd_range_type(id, part);
	if (!type)
		return dev->type == FMD_COMPONENT_SLD ? -15 : 0;
	snprintf(plan->type, sizeof(plan->type), "%s", type);
	return 0;
}

/*
 * POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks
 *
 * Create a new memory chunk in the specified domain.
 *
 * Request body:
 * {
 *   "MemoryChunkSizeMiB": 4096,        // > 0; or InterleaveSets[0].SizeMiB
 *   "AddressRangeType": "PMEM",        // "PMEM" or "Volatile"; optional if
 *                                      // all of the memory is of one type
 *   "AddressRangeOffsetMiB": 0,        // optional, auto-assigned if omitted
 *   "InterleaveSets": [{"Memory": {"@odata.id": <the LD's Memory>}}],
 *   "Links": {"CXLLogicalDevices": [{"@odata.id": <the LD>}]}
 * }
 *
 * A chunk of an MLD or a Multi-Headed device grows the allocation of one
 * of its LDs, which the request names (see parse_chunk_plan()); one of an
 * SLD is of its one LD.
 *
 * Sizes and offsets in MiB are integers from 0 to RF_MIB_MAX. A chunk lies
 * within its domain: an offset given is at most MemorySizeMiB less the
 * size, and one assigned is the lowest offset where the chunk fits.
 *
 * Returns:
 *   0 = success, *response contains the new chunk JSON
 *  -1 = chassis/domain not found
 *  -2 = invalid request body
 *  -3 = insufficient capacity
 *  -4 = address overlap
 *  -5 = CCI command failed
 *  -6 = the device cannot be given that size (allocation granularity,
 *       partition alignment, or no partitionable capacity)
 *  -7 = the chunk would change an LD or an SLD a Connection binds, or a
 *       vPPB as the switch reads now binds, or is Busy (binding_coverage())
 *  -8 = AddressRangeType is not PMEM or Volatile
 *  -9 = AddressRangeOffsetMiB is not an integer of RF_MIB_MAX or less, or
 *       the chunk would not lie within the domain at it
 * -10 = a size or offset in MiB is not an integer of RF_MIB_MAX or less,
 *       or MemoryChunkSizeMiB is 0
 * -11 = the request names no LD of the device (an MLD's or a Multi-Headed
 *       device's chunk needs one), names one by anything but its URI, or
 *       names several
 * -12 = an InterleaveSets entry asks for what an allocation cannot be: an
 *       OffsetMiB, RegionId or MemoryLevel, or a SizeMiB that is not
 *       MemoryChunkSizeMiB
 * -13 = cxl-fmd knows no route to the LD Pool CCI of the Multi-Headed
 *       device (fmd_mhd_pool_route()): nothing was sent
 * -14 = Set LD Allocations had an outcome that could not be told, and the
 *       LD could not be read back (fmd_ld_alloc_set()): the chunk is kept,
 *       and no chunk of the device is changed until the LD is read
 * -15 = no AddressRangeType, and the SLD's memory is of both types, or of
 *       none known (chunk_plan_type())
 * -16 = an AddressRangeType the device or LD has no memory of, and cannot
 *       be partitioned to
 * -17 = a VCS of a switch that can expose the LD or SLD could not be read
 *       just now (binding_coverage())
 * -18 = a switch that can expose the LD or SLD is not read whole, or which
 *       head of its Multi-Headed device a binding reaches cannot be told
 *       (binding_coverage())
 * -19 = Set Partition Info had an outcome that could not be told, and the
 *       partition could not be read back (fmd_memdev_set_partition()): the
 *       chunk is kept, staged, and no chunk that changes the partition is
 *       created or deleted until it is read (-5 while it cannot be)
 * FMD_MCTP_RESTRICTED = a chunk that changes the partition of an SLD that
 *       Set Partition Info may not be sent to (mctp_refused())
 * Every one of -2, -6, -8 to -12, -15 to -18 and FMD_MCTP_RESTRICTED is
 * answered before anything is changed or sent, as is -7.
 */
int fmd_redfish_post_memory_chunk(struct fmd_ctx *ctx,
				  const char *chassis_id,
				  const char *domain_id,
				  const char *body,
				  size_t body_size,
				  json_t **response)
{
	struct fmd_component *chassis;
	struct fmd_mem_domain *domain = NULL;
	struct fmd_mem_chunk *chunk;
	struct fmd_ld *ld = NULL;	/* the LD an MLD's or MHD's chunk grows */
	struct chunk_plan plan;
	json_t *root;
	json_error_t error;
	char uri[RF_CHUNK_URI_LEN], ld_uri[RF_LD_URI_LEN];
	int rc;

	*response = NULL;

	/* Find chassis */
	chassis = fmd_topology_find(&ctx->topology, chassis_id);
	if (!chassis || !fmd_is_memdev(chassis))
		return -1;

	/* Find domain (use hash table lookup like GET handler) */
	domain = fmd_topology_find_domain(&ctx->topology, domain_id);
	if (!domain)
		return -1;

	/* Verify domain belongs to this chassis */
	if (!domain->base.parent || strcmp(domain->base.parent->id, chassis_id) != 0)
		return -1;

	/* Parse request body */
	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -2;
	rc = parse_chunk_plan(ctx, chassis, root, &plan);
	json_decref(root);
	if (rc)
		return rc;
	if (chassis->type != FMD_COMPONENT_SLD)
		ld = (struct fmd_ld *)fmd_chunk_ld(chassis, &(struct fmd_mem_chunk){
							  .backing_ld = plan.ld });
	rc = chunk_plan_type(chassis, ld, &plan);
	if (rc)
		return rc;

	/*
	 * A chunk of an LD a vPPB has bound changes the LD's allocation
	 * (5402h), and a Volatile one of a bound SLD with partitionable
	 * capacity its partition (4101h, chunk_repartitions()): refused
	 * while the binding stands, as a DELETE is. Checked on a mock too,
	 * so that it answers as a device would, and before anything is
	 * sent.
	 */
	if (chassis->type != FMD_COMPONENT_SLD || chunk_repartitions(chassis, plan.type)) {
		switch (binding_coverage(ctx, chassis, ld, "a new chunk")) {
		case COVER_BOUND:
			return -7;
		case COVER_UNREAD:
			return -17;
		case COVER_INCOMPLETE:
			return -18;
		case COVER_NONE:
			break;
		}
		if (ld_connected(ctx, chassis, ld, "a new chunk"))
			return -7;
	}

	/* Set Partition Info, where note 4 of Table 8-308 lets it go */
	if (chunk_repartitions(chassis, plan.type) && mctp_refused(ctx, chassis, 0x4101))
		return FMD_MCTP_RESTRICTED;

	/*
	 * A partition not known (a 4101h whose outcome could not be told) is
	 * read before it is changed again: nothing is sent while it cannot be
	 */
	if (chassis->type == FMD_COMPONENT_SLD && chunk_repartitions(chassis, plan.type) &&
	    fmd_to_sld(chassis)->partition_unknown &&
	    fmd_sld_partition_refresh(ctx, fmd_to_sld(chassis)))
		return -5;

	/*
	 * The LD's allocation as it is now, which others than cxl-fmd may
	 * have changed (fmd_ld_alloc_refresh_ld()): the domain is made what
	 * it says before the chunk is checked against what is left, and its
	 * Range 2 is what Set LD Allocations keeps.
	 */
	if (ld) {
		uint64_t total, gran;

		rc = fmd_ld_alloc_refresh_ld(ctx, chassis, ld);
		if (rc == -EOPNOTSUPP)
			return -13;	/* no route to the LD Pool CCI: nothing sent */
		if (rc)
			return -5;
		gran = (uint64_t)(chassis->type == FMD_COMPONENT_MLD ?
				  fmd_to_mld(chassis)->memory_granularity :
				  fmd_to_mhd(chassis)->memory_granularity) * CXL_CAPACITY_UNIT;
		/* 0 if not known, as in parse_chunk_plan() */
		if (__builtin_add_overflow(ld->memory_size, plan.size, &total) ||
		    (gran && total % gran))
			return -6;
	}

	/*
	 * A chunk given an offset lies within its domain: AddressRangeOffsetMiB
	 * is where it is in the domain's MemorySizeMiB, from 0 to MemorySizeMiB
	 * less its size (cxl-fmd's coordinate: nothing in CXL places an
	 * allocation at an offset).
	 */
	if (plan.offset_given && (plan.offset > domain->capacity ||
				  plan.size > domain->capacity - plan.offset))
		return -9;

	/* Check capacity */
	if (plan.size > domain->remaining)
		return -3;

	/* Create chunk in domain */
	chunk = fmd_chunk_create_in_domain(&ctx->topology, domain, plan.size,
					   plan.offset_given ? plan.offset : FMD_CHUNK_OFFSET_AUTO,
					   plan.type, plan.ld);
	if (!chunk)
		return -4;

	/*
	 * Issue CCI command for hardware mode.
	 * For MLD: resize LD allocation via Set LD Allocations (5402h)
	 * For SLD: adjust partition via Set Partition Info (4101h)
	 * In mock mode the LD-allocation calls change the mock's LDs, as a
	 * device's would be (fmd_ld_alloc_set() sends nothing then); an
	 * SLD's partition is not changed.
	 */
	{
		uint64_t old = ld ? ld->memory_size : 0;

		rc = 0;
		if (chassis->type == FMD_COMPONENT_MLD ||
		    chassis->type == FMD_COMPONENT_MHD) {
			/* the LD grows by the chunk (checked above) */
			rc = fmd_ld_alloc_set(ctx, chassis, ld, old + chunk->size);
			if (rc == -EREMOTEIO) {
				/*
				 * Not known whether the LD grew: the chunk
				 * stays, counted as allocated, until the LD
				 * is read (fmd_ld_alloc_set()).
				 */
				ld->memory_size = old + chunk->size;
				return -14;
			}
			if (!rc && ld->memory_size != old + chunk->size) {
				/*
				 * The LD has what the device says, not what
				 * was asked: as it was, the chunk goes; any
				 * other size, the domain is made what the LD
				 * has (fmd_ld_domain_sync()), the new chunk
				 * first. Either is a failure.
				 */
				fmd_warn(ctx, "%s: LD %u has %" PRIu64 " MB, not the %"
					 PRIu64 " MB asked for", chassis->id, ld->ld_id,
					 ld->memory_size >> 20, (old + chunk->size) >> 20);
				if (ld->memory_size == old)
					fmd_chunk_remove(&ctx->topology, chunk);
				else
					fmd_ld_domain_sync(ctx, chassis);
				return -5;
			}
		} else if (chassis->type == FMD_COMPONENT_SLD) {
			/*
			 * Only volatile chunks of partitionable capacity change
			 * the partition (chunk_repartitions()): the total
			 * volatile capacity, as staged or else active, grows by
			 * the chunk. Staged for the next reset, not at once.
			 */
			if (chunk_repartitions(chassis, plan.type) && likely(!fmd_is_mock(ctx))) {
				struct fmd_sld *sld = fmd_to_sld(chassis);
				uint64_t new_volatile;

				if (__builtin_add_overflow(CXL_TO_BYTES(
						fmd_partition_next_volatile(&sld->partition)),
						chunk->size, &new_volatile))
					rc = -EINVAL;
				else
					rc = fmd_sld_set_partition(ctx, sld, new_volatile, false);
				/*
				 * The chunk is of the next configuration, not of
				 * the active one, until a read-back shows it
				 * active (fmd_sld_partition_refresh()). One whose
				 * 4101h had an outcome not known is kept so too:
				 * the read that makes the partition known again
				 * judges it as any staged chunk.
				 */
				if (!rc || rc == -EREMOTEIO) {
					chunk->staged = true;
					chunk->staged_vmem = new_volatile / CXL_CAPACITY_UNIT;
				}
				if (rc == -EREMOTEIO)
					return -19;
			}
		}

		if (rc < 0) {
			/* Rollback topology change on CCI failure */
			fmd_chunk_remove(&ctx->topology, chunk);
			/* no route to the LD Pool CCI known: nothing was sent */
			if (rc == -EOPNOTSUPP && chassis->type == FMD_COMPONENT_MHD)
				return -13;
			/* a size the device cannot be given */
			if (rc == -EINVAL || rc == -EOPNOTSUPP)
				return -6;
			return -5;  /* CCI command failed */
		}
	}

	/* Build response JSON */
	if (chunk_uri(uri, sizeof(uri), chunk))
		uri[0] = '\0';

	*response = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* MemoryChunkSizeMiB */
		"  s:I,"   /* AddressRangeOffsetMiB */
		"  s:o"    /* AddressRangeType */
		"}",
		"@odata.id", uri,
		"@odata.type", "#MemoryChunks.v1_6_2.MemoryChunks",
		"Id", chunk->base.id,
		"Name", "CXL Memory Chunk",
		"MemoryChunkSizeMiB", (json_int_t)(chunk->size / (1024ULL * 1024)),
		"AddressRangeOffsetMiB", (json_int_t)(chunk->base_address / (1024ULL * 1024)),
		"AddressRangeType", fmd_chunk_range_type(chunk)
	);
	if (chunk->staged) {
		struct fmd_status_rollup r = { 0 };

		fmd_rollup_add(&r, &fmd_to_sld(chassis)->health,
			       fmd_to_sld(chassis)->health_valid);
		json_object_set_new(*response, "Status", chunk_status(&r, chunk));
		json_object_set_new(*response, "Oem",
				    staged_chunk_oem(fmd_to_sld(chassis), chunk));
	}

	/*
	 * The LD the chunk grows, for an MLD or MHD (a pool index for an
	 * MHD), as its one InterleaveSets entry and Links.CXLLogicalDevices,
	 * built from the chunk's backing_ld.
	 */
	{
		const struct fmd_ld *cl = fmd_chunk_ld(chassis, chunk);

		if (cl && !ld_memory_uri(uri, sizeof(uri), chassis, cl) &&
		    !ld_device_uri(ld_uri, sizeof(ld_uri), chassis, cl)) {
			json_object_set_new(*response, "InterleaveSets",
				json_pack("[{s:{s:s}, s:i, s:i, s:I}]",
					  "Memory", "@odata.id", uri,
					  "MemoryLevel", 1,
					  "OffsetMiB", 0,
					  "SizeMiB", (json_int_t)(chunk->size >> 20)));
			json_object_set_new(*response, "Links",
				rf_links_array_single("CXLLogicalDevices", "%s", ld_uri));
		}
	}

	return 0;
}


/*
 * Give back the allocation of @chunk, of an MLD or a Multi-Headed device
 * @dev: its LD's allocation less chunk->size (Set LD Allocations). "The
 * allocations provided in the response reflect the state of the LD
 * allocations after the command is processed" (CXL r4.0 Section
 * 7.6.7.4.3), and the LD is sized from them: what the domain gets back is
 * what the LD lost. A chunk of which the LD gave back less than all is
 * kept, as large as what it did not, and -3 returned.
 *
 * Returns 0 for the chunk to be removed, or the code of
 * fmd_redfish_delete_memory_chunk().
 */
static int chunk_give_back(struct fmd_ctx *ctx, struct fmd_component *dev,
			   struct fmd_mem_chunk *chunk)
{
	struct fmd_ld *ld = (struct fmd_ld *)fmd_chunk_ld(dev, chunk);
	struct fmd_mem_domain *domain = fmd_to_domain(chunk->base.parent);
	uint64_t before, freed;
	int rc;

	if (!ld)
		return -6;
	before = ld->memory_size;
	if (chunk->size > before) {
		fmd_warn(ctx, "%s: LD %u has %" PRIu64 " MB allocated, less than "
			 "chunk %s: refused", dev->id, ld->ld_id, before >> 20,
			 chunk->base.id);
		return -7;
	}
	rc = fmd_ld_alloc_set(ctx, dev, ld, before - chunk->size);
	if (rc == -EOPNOTSUPP && dev->type == FMD_COMPONENT_MHD)
		return -6;		/* no LD Pool CCI known: nothing sent */
	if (rc == -EINVAL || rc == -EOPNOTSUPP)
		return -4;
	if (rc == -EREMOTEIO)
		return -8;		/* the chunk stays until the LD is read */
	if (rc < 0)
		return -3;

	if (ld->memory_size == before - chunk->size)
		return 0;
	if (ld->memory_size < before - chunk->size || ld->memory_size > before) {
		/* more given back than asked, or the LD grew: as it is */
		fmd_warn(ctx, "%s: LD %u has %" PRIu64 " MB, not the %" PRIu64
			 " MB asked for", dev->id, ld->ld_id, ld->memory_size >> 20,
			 (before - chunk->size) >> 20);
		fmd_ld_domain_sync(ctx, dev);
		return -3;
	}
	freed = before - ld->memory_size;
	fmd_warn(ctx, "%s: LD %u gave back %" PRIu64 " MB of the %" PRIu64
		 " MB of chunk %s", dev->id, ld->ld_id, freed >> 20,
		 chunk->size >> 20, chunk->base.id);
	fmd_topology_wrlock(&ctx->topology);
	chunk->size -= freed;
	domain->remaining += freed;
	fmd_topology_wrunlock(&ctx->topology);
	return -3;
}

/*
 * DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}
 *
 * Remove a memory chunk from the domain. A chunk of an MLD or of a
 * Multi-Headed device gives back its size, chunk->size, from the
 * allocation of its backing LD (see chunk_give_back()).
 *
 * Returns:
 *   0 = success
 *  -1 = chunk not found
 *  -2 = chunk not in specified domain
 *  -3 = CCI command failed
 *  -4 = the device cannot be given the partition left
 *  -5 = a Connection exposes the chunk (see chunk_connected())
 *  -6 = cxl-fmd cannot give back the chunk's allocation: it has no LD, its
 *       device is not one whose allocation it changes, or no LD Pool CCI
 *       of its Multi-Headed device is known; nothing was sent
 *  -7 = the chunk's LD has less allocated than the chunk; nothing was sent
 *  -8 = as -14 of fmd_redfish_post_memory_chunk(): the chunk is kept
 * -11 = as -19 of fmd_redfish_post_memory_chunk(): the chunk is kept as it
 *       was, until a read of the partition shows whether it was deleted
 *  -9 = a VCS of a switch that can expose the chunk could not be read just
 *       now (binding_coverage()); nothing was sent
 * -10 = a switch that can expose the chunk is not read whole, or which
 *       head of its Multi-Headed device a binding reaches cannot be told
 *       (binding_coverage()); nothing was sent
 *  -5 is also a vPPB that exposes the chunk as the switch reads now, or
 *       is Busy, whether or not its VCS has a host; nothing was sent
 *  FMD_MCTP_RESTRICTED = the chunk changes the partition of an SLD that
 *       Set Partition Info may not be sent to (mctp_refused()); nothing
 *       was sent or changed
 */
int fmd_redfish_delete_memory_chunk(struct fmd_ctx *ctx,
				    const char *chassis_id,
				    const char *domain_id,
				    const char *chunk_id)
{
	struct fmd_mem_chunk *chunk;

	/* Find the chunk */
	chunk = fmd_topology_find_chunk(&ctx->topology, chunk_id);
	if (!chunk)
		return -1;

	/* Verify it belongs to the specified domain */
	if (!chunk->base.parent ||
	    chunk->base.parent->type != FMD_COMPONENT_DOMAIN ||
	    strcmp(chunk->base.parent->id, domain_id) != 0)
		return -2;

	/* Verify the domain belongs to the specified chassis */
	struct fmd_component *domain_parent = chunk->base.parent->parent;
	if (!domain_parent || strcmp(domain_parent->id, chassis_id) != 0)
		return -2;

	/*
	 * Deleted already: its decrease is staged, and it goes at the reset
	 * (fmd_sld_chunk_deleted()). Nothing is sent again, which would take
	 * the chunk off the partition twice.
	 */
	if (chunk->removing)
		return 0;

	/*
	 * a chunk a binding exposes stays while it does, as the switches
	 * read now show it (binding_coverage()) and as the Connections do
	 * (chunk_connected())
	 */
	{
		char what[FMD_MAX_ID_LEN + 8];

		snprintf(what, sizeof(what), "chunk %s", chunk->base.id);
		switch (binding_coverage(ctx, domain_parent,
					 fmd_chunk_ld(domain_parent, chunk), what)) {
		case COVER_BOUND:
			return -5;
		case COVER_UNREAD:
			return -9;
		case COVER_INCOMPLETE:
			return -10;
		case COVER_NONE:
			break;
		}
	}
	if (chunk_connected(ctx, chunk))
		return -5;

	/* as for a POST: a chunk that changes the partition */
	if (domain_parent->type == FMD_COMPONENT_SLD &&
	    chunk_repartitions(domain_parent, chunk->address_range_type) &&
	    mctp_refused(ctx, domain_parent, 0x4101))
		return FMD_MCTP_RESTRICTED;

	/*
	 * as for a POST, a partition not known is read first, which may
	 * carry out an earlier DELETE of the chunk or find it gone
	 */
	if (domain_parent->type == FMD_COMPONENT_SLD &&
	    chunk_repartitions(domain_parent, chunk->address_range_type) &&
	    fmd_to_sld(domain_parent)->partition_unknown) {
		if (fmd_sld_partition_refresh(ctx, fmd_to_sld(domain_parent)))
			return -3;
		chunk = fmd_topology_find_chunk(&ctx->topology, chunk_id);
		if (!chunk)
			return -1;
		if (chunk->removing)
			return 0;
	}

	/*
	 * Issue CCI command for hardware mode before removing from topology.
	 * For MLD or MHD: decrease LD allocation via Set LD Allocations (5402h)
	 * For SLD: adjust partition via Set Partition Info (4101h)
	 * In mock mode, as for a POST: the mock's LDs change, not an SLD's
	 * partition.
	 */
	{
		int rc = 0;

		if (domain_parent->type == FMD_COMPONENT_MLD ||
		    domain_parent->type == FMD_COMPONENT_MHD) {
			struct fmd_ld *ld = (struct fmd_ld *)fmd_chunk_ld(domain_parent, chunk);

			/*
			 * The LD's allocation as it is now, as for a POST: the
			 * chunk is what is left of it once the domain is made
			 * what the LD has (fmd_ld_domain_sync()), and gone if
			 * nothing is.
			 */
			if (ld) {
				rc = fmd_ld_alloc_refresh_ld(ctx, domain_parent, ld);
				if (rc == -EOPNOTSUPP)
					return -6;	/* nothing sent */
				if (rc)
					return -3;
				chunk = fmd_topology_find_chunk(&ctx->topology, chunk_id);
				if (!chunk)
					return -1;
			}
			rc = chunk_give_back(ctx, domain_parent, chunk);
			if (rc)
				return rc;
		} else if (domain_parent->type == FMD_COMPONENT_SLD) {
			/* only volatile chunks of partitionable capacity change it */
			if (chunk_repartitions(domain_parent, chunk->address_range_type) &&
			    likely(!fmd_is_mock(ctx))) {
				struct fmd_sld *sld = fmd_to_sld(domain_parent);
				uint64_t cur = CXL_TO_BYTES(
					fmd_partition_next_volatile(&sld->partition));
				uint64_t new_volatile = 0;
				if (cur > chunk->size)
					new_volatile = cur - chunk->size;
				rc = fmd_sld_set_partition(ctx, sld, new_volatile, false);
				/*
				 * The chunks staged after it wait for less; an
				 * active one stays active until the reset, and
				 * is kept as removing until a read shows it.
				 */
				if (!rc && fmd_sld_chunk_deleted(ctx, sld, chunk,
								 new_volatile / CXL_CAPACITY_UNIT))
					return 0;
				/*
				 * Not known whether it was carried out: the
				 * chunk is kept as it was, and the read that
				 * makes the partition known carries the DELETE
				 * out if the device has it
				 * (fmd_sld_partition_refresh()).
				 */
				if (rc == -EREMOTEIO) {
					snprintf(sld->unknown_delete,
						 sizeof(sld->unknown_delete), "%s",
						 chunk->base.id);
					sld->unknown_next = new_volatile / CXL_CAPACITY_UNIT;
					return -11;
				}
			}
		} else {
			/* nothing would be given back: refused, not 204 */
			return -6;
		}

		/* a partition the device cannot be given, as for a POST */
		if (rc == -EINVAL || rc == -EOPNOTSUPP)
			return -4;
		if (rc < 0)
			return -3;  /* CCI command failed */
	}

	/* Remove the chunk */
	return fmd_chunk_remove(&ctx->topology, chunk);
}


/*
 * EDMX Metadata Document - /redfish/v1/$metadata
 */
char *fmd_redfish_metadata(struct fmd_ctx *ctx)
{
	(void)ctx;

	static const char *metadata_xml =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<edmx:Edmx xmlns:edmx=\"http://docs.oasis-open.org/odata/ns/edmx\" Version=\"4.0\">\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/ServiceRoot_v1.xml\">\n"
		"    <edmx:Include Namespace=\"ServiceRoot\"/>\n"
		"    <edmx:Include Namespace=\"ServiceRoot.v1_15_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/Fabric_v1.xml\">\n"
		"    <edmx:Include Namespace=\"Fabric\"/>\n"
		"    <edmx:Include Namespace=\"Fabric.v1_3_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/FabricCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"FabricCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/Switch_v1.xml\">\n"
		"    <edmx:Include Namespace=\"Switch\"/>\n"
		"    <edmx:Include Namespace=\"Switch.v1_10_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/SwitchCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"SwitchCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/Port_v1.xml\">\n"
		"    <edmx:Include Namespace=\"Port\"/>\n"
		"    <edmx:Include Namespace=\"Port.v1_11_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/PortCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"PortCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/Chassis_v1.xml\">\n"
		"    <edmx:Include Namespace=\"Chassis\"/>\n"
		"    <edmx:Include Namespace=\"Chassis.v1_23_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/ChassisCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"ChassisCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/PCIeDevice_v1.xml\">\n"
		"    <edmx:Include Namespace=\"PCIeDevice\"/>\n"
		"    <edmx:Include Namespace=\"PCIeDevice.v1_23_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/PCIeDeviceCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"PCIeDeviceCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/CXLLogicalDevice_v1.xml\">\n"
		"    <edmx:Include Namespace=\"CXLLogicalDevice\"/>\n"
		"    <edmx:Include Namespace=\"CXLLogicalDevice.v1_5_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/CXLLogicalDeviceCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"CXLLogicalDeviceCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryDomain_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryDomain\"/>\n"
		"    <edmx:Include Namespace=\"MemoryDomain.v1_5_1\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/Memory_v1.xml\">\n"
		"    <edmx:Include Namespace=\"Memory\"/>\n"
		"    <edmx:Include Namespace=\"Memory.v1_17_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryMetrics_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryMetrics\"/>\n"
		"    <edmx:Include Namespace=\"MemoryMetrics.v1_6_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryDomainCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryDomainCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryChunksCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryChunksCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryRegion_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryRegion\"/>\n"
		"    <edmx:Include Namespace=\"MemoryRegion.v1_1_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryRegionCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryRegionCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryExtent_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryExtent\"/>\n"
		"    <edmx:Include Namespace=\"MemoryExtent.v1_0_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MemoryExtentCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MemoryExtentCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MessageRegistryFileCollection_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MessageRegistryFileCollection\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"http://redfish.dmtf.org/schemas/v1/MessageRegistryFile_v1.xml\">\n"
		"    <edmx:Include Namespace=\"MessageRegistryFile\"/>\n"
		"    <edmx:Include Namespace=\"MessageRegistryFile.v1_1_5\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:Reference Uri=\"/redfish/v1/Schemas/Cxl_v1.xml\">\n"
		"    <edmx:Include Namespace=\"Cxl\"/>\n"
		"    <edmx:Include Namespace=\"Cxl.v1_0_0\"/>\n"
		"  </edmx:Reference>\n"
		"  <edmx:DataServices>\n"
		"    <Schema xmlns=\"http://docs.oasis-open.org/odata/ns/edm\" Namespace=\"Service\">\n"
		"      <EntityContainer Name=\"Service\" Extends=\"ServiceRoot.v1_15_0.ServiceContainer\"/>\n"
		"    </Schema>\n"
		"  </edmx:DataServices>\n"
		"</edmx:Edmx>\n";

	return strdup(metadata_xml);
}


/*
 * Build semantics array for SLD
 */
static json_t *build_sld_semantics_array(struct fmd_sld *sld)
{
	json_t *arr = json_array();

	/* Type 3 devices always support CXL.io */
	json_array_append_new(arr,
		json_string(rf_cxl_semantic_to_str(CXL_SEMANTIC_CX_LIO)));

	/* CXL.mem if device has memory capacity */
	if (sld->memdev.total_capacity > 0)
		json_array_append_new(arr,
			json_string(rf_cxl_semantic_to_str(CXL_SEMANTIC_CX_LMEM)));

	return arr;
}

/*
 * Build semantics array for MLD
 */
static json_t *build_mld_semantics_array(struct fmd_mld *mld)
{
	json_t *arr = json_array();
	(void)mld;

	/* MLDs support CXL.io */
	json_array_append_new(arr,
		json_string(rf_cxl_semantic_to_str(CXL_SEMANTIC_CX_LIO)));

	/* MLDs support CXL.mem */
	json_array_append_new(arr,
		json_string(rf_cxl_semantic_to_str(CXL_SEMANTIC_CX_LMEM)));

	return arr;
}

static json_t *build_mhd_semantics_array(struct fmd_mhd *mhd)
{
	json_t *arr = json_array();
	(void)mhd;

	/* MHDs support CXL.io */
	json_array_append_new(arr,
		json_string(rf_cxl_semantic_to_str(CXL_SEMANTIC_CX_LIO)));

	/* MHDs support CXL.mem */
	json_array_append_new(arr,
		json_string(rf_cxl_semantic_to_str(CXL_SEMANTIC_CX_LMEM)));

	return arr;
}


/*
 * PATCH PCIeDevice - update writable properties
 *
 * DSP0288 Table 1: Set Timestamp → CXLDevice/Timestamp
 *
 * Returns:
 *   0 on success
 *  -1 on resource not found
 *  -2 on invalid request body
 *  -3 on CCI command failure
 *  -4 if the device cannot be reached: an SLD on a switch port
 */
int fmd_redfish_patch_pcie_device(struct fmd_ctx *ctx,
				  const char *chassis_id,
				  const char *dev_id,
				  const char *body,
				  size_t body_size)
{
	struct fmd_component *comp;
	json_t *root, *cxl_device, *timestamp;
	json_error_t error;

	/* Find the device */
	comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp) || strcmp(chassis_id, dev_id) != 0)
		return -1;

	/* Parse request body with size limit */
	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -2;

	/* Look for CXLDevice.Timestamp */
	cxl_device = json_object_get(root, "CXLDevice");
	if (cxl_device && json_is_object(cxl_device)) {
		timestamp = json_object_get(cxl_device, "Timestamp");
		if (timestamp && json_is_string(timestamp)) {
			const char *ts_str = json_string_value(timestamp);
			uint64_t ts_ns = parse_cxl_timestamp(ts_str);

			if (ts_ns == 0) {
				json_decref(root);
				return -2;  /* Invalid timestamp format */
			}

			int rc = fmd_set_device_timestamp(comp, ts_ns);
			if (rc == -EOPNOTSUPP) {
				json_decref(root);
				return -4;  /* nothing to send it to */
			}
			if (rc != 0) {
				json_decref(root);
				return -3;  /* CCI command failed */
			}
		}
	}

	json_decref(root);
	return 0;
}

/*
 * The memory device of the Memory resource @mem_id of Chassis @chassis_id:
 * the SLD whose ID both are, or the MLD of {chassis_id}_ld{n}. NULL if
 * there is none, or @mem_id is not one of its Memory resources.
 */
static struct fmd_component *memory_device(struct fmd_ctx *ctx,
					   const char *chassis_id,
					   const char *mem_id)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	char prefix[128];

	if (!comp)
		return NULL;
	if (comp->type == FMD_COMPONENT_SLD)
		return strcmp(mem_id, chassis_id) ? NULL : comp;
	if (comp->type != FMD_COMPONENT_MLD)
		return NULL;
	snprintf(prefix, sizeof(prefix), "%s_ld", chassis_id);
	if (strncmp(mem_id, prefix, strlen(prefix)) != 0 ||
	    safe_parse_int(mem_id + strlen(prefix)) < 0)
		return NULL;
	return comp;
}

/*
 * The checks of a Memory action that sends @opcode, before its body is
 * read: -1 if there is no such Memory resource, FMD_MCTP_RESTRICTED if
 * @opcode may not be sent to it (mctp_refused()), else 0.
 */
static int memory_action_check(struct fmd_ctx *ctx, const char *chassis_id,
			       const char *mem_id, uint16_t opcode)
{
	struct fmd_component *dev = memory_device(ctx, chassis_id, mem_id);

	if (!dev)
		return -1;
	if (mctp_refused(ctx, dev, opcode))
		return FMD_MCTP_RESTRICTED;
	return 0;
}

/*
 * PATCH /redfish/v1/Chassis/{id}/Memory/{mem}
 *
 * DSP0288 Table 1: Set Partition Info (Opcode 4101h) mappings
 * - CXL.StagedVolatileSizeMiB -> Next Volatile Capacity
 * - Next Persistent Capacity: DSP0288 names StagedPersistentSizeMiB, which
 *   the Memory schema does not have; cxl-fmd takes StagedNonVolatileSizeMiB
 *   ("Total device non-volatile memory capacity in MiB staged for next
 *   activation") and derives the volatile capacity from it.
 *
 * Both are totals; Set Partition Info takes the partitionable part, which
 * fmd_memdev_set_partition() works out. The change is staged for the next
 * reset (Immediate cleared), and loses the data of the persistent capacity
 * it changes.
 *
 * Returns:
 *   0 on success
 *  -1 on resource not found
 *  -2 on invalid request body, or a size the device cannot be given
 *  -3 on CCI command failure
 *  -4 if the device cannot be reached: an SLD on a switch port
 *  -5 if the device cannot be partitioned (Partition Alignment 0)
 *  FMD_MCTP_RESTRICTED if Set Partition Info may not be sent to it
 *  (mctp_refused()), before the body is read
 */
int fmd_redfish_patch_memory(struct fmd_ctx *ctx,
			     const char *chassis_id,
			     const char *mem_id,
			     const char *body,
			     size_t body_size)
{
	struct fmd_component *comp;
	json_t *root, *cxl, *staged_volatile_j, *staged_nonvolatile_j;
	json_error_t error;
	const struct cxlmi_cmd_memdev_identify_rsp *memdev = NULL;
	struct cxlmi_cmd_memdev_get_partition_info_rsp *partition = NULL;
	uint64_t total, next_volatile;
	int rc;

	comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp))
		return -1;

	/* Identify Memory Device and Get Partition Info of the device or LD */
	if (comp->type == FMD_COMPONENT_SLD) {
		if (strcmp(mem_id, chassis_id) != 0)
			return -1;
		struct fmd_sld *sld = fmd_to_sld(comp);
		memdev = &sld->memdev;
		partition = &sld->partition;
	} else if (comp->type == FMD_COMPONENT_MLD) {
		char prefix[128];
		snprintf(prefix, sizeof(prefix), "%s_ld", chassis_id);
		if (strncmp(mem_id, prefix, strlen(prefix)) != 0)
			return -1;
		int ld_id = safe_parse_int(mem_id + strlen(prefix));
		if (ld_id < 0)
			return -1;

		struct fmd_mld *mld = fmd_to_mld(comp);
		struct fmd_component *child;
		struct fmd_ld *target_ld = NULL;

		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mld->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				if (ld->ld_id == ld_id) {
					target_ld = ld;
					break;
				}
			}
		}
		fmd_topology_rdunlock(&ctx->topology);

		if (!target_ld)
			return -1;

		memdev = &target_ld->memdev;
		partition = &target_ld->partition;
	} else {
		return -1;
	}
	total = memdev->total_capacity;

	if (mctp_refused(ctx, comp, 0x4101))
		return FMD_MCTP_RESTRICTED;

	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -2;

	/* Look for CXL.StagedVolatileSizeMiB and/or CXL.StagedNonVolatileSizeMiB */
	cxl = json_object_get(root, "CXL");
	if (!cxl || !json_is_object(cxl)) {
		json_decref(root);
		return -2;
	}

	staged_volatile_j = json_object_get(cxl, "StagedVolatileSizeMiB");
	staged_nonvolatile_j = json_object_get(cxl, "StagedNonVolatileSizeMiB");

	/*
	 * The total volatile capacity wanted, in 256 MB units. Both
	 * properties are "in multiples of 256 MiB" (Memory_v1): one that is
	 * not is refused, not rounded.
	 */
	json_int_t mib;
	if (staged_volatile_j && json_is_integer(staged_volatile_j)) {
		mib = json_integer_value(staged_volatile_j);
		if (mib < 0 || mib % 256) {
			json_decref(root);
			return -2;
		}
		next_volatile = (uint64_t)mib / 256;
	} else if (staged_nonvolatile_j && json_is_integer(staged_nonvolatile_j)) {
		mib = json_integer_value(staged_nonvolatile_j);
		if (mib < 0 || mib % 256 || (uint64_t)mib / 256 > total) {
			json_decref(root);
			return -2;
		}
		next_volatile = total - (uint64_t)mib / 256;
	} else {
		json_decref(root);
		return -2;
	}
	json_decref(root);

	struct cxlmi_endpoint *ep = NULL;
	struct cxlmi_tunnel_info ti_buf, *ti = CXLMI_NO_TUNNEL;

	/* mock mode: the same checks, local state only */
	if (likely(!fmd_is_mock(ctx))) {
		if (fmd_sld_behind_switch(comp))
			return -4;
		if (memory_endpoint_setup(ctx, chassis_id, mem_id, &ep, &ti_buf,
					  &ti) != 0)
			return -1;
	}

	rc = fmd_memdev_set_partition(ctx, mem_id, ep, ti, memdev, partition,
				      next_volatile, false);
	if (rc == -EINVAL)
		return -2;
	if (rc == -EOPNOTSUPP)
		return -5;
	if (rc == -EREMOTEIO) {
		/* read again on the next --rescan (fmd_sld_partition_refresh()) */
		if (comp->type == FMD_COMPONENT_SLD) {
			fmd_topology_wrlock(&ctx->topology);
			fmd_to_sld(comp)->partition_unknown = true;
			fmd_topology_wrunlock(&ctx->topology);
		}
		return -6;
	}
	return rc ? -3 : 0;
}

/*
 * Memory.ScanMedia action handler
 *
 * DSP0288 Table 1: Scan Media -> Memory ScanMedia action
 * CXL command: Scan Media (Opcode 4304h)
 *
 * Parameters:
 *   PhysicalAddress - Starting DPA as hex string "0x...", 64-byte aligned
 *   Length - Length in bytes, a nonzero multiple of 64
 *   NoEventLog - If true, don't log events (optional, default false)
 *
 * Returns: 0 on success, FMD_MCTP_RESTRICTED if Scan Media may not be sent
 * to the device (mctp_refused()), -1 on another error
 */
int fmd_redfish_action_scan_media(struct fmd_ctx *ctx,
				  const char *chassis_id,
				  const char *mem_id,
				  const char *body,
				  size_t body_size)
{
	struct fmd_component *comp;
	json_t *root, *phys_addr_j, *length_j, *no_event_log_j;
	json_error_t error;
	uint64_t phys_addr;
	uint64_t length;
	uint8_t flags = 0;
	int rc;

	comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp))
		return -1;
	rc = memory_action_check(ctx, chassis_id, mem_id, 0x4304);
	if (rc)
		return rc;

	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -1;

	/* Get required PhysicalAddress parameter */
	phys_addr_j = json_object_get(root, "PhysicalAddress");
	if (!phys_addr_j || !json_is_string(phys_addr_j)) {
		json_decref(root);
		return -1;
	}
	const char *addr_str = json_string_value(phys_addr_j);
	if (strncmp(addr_str, "0x", 2) != 0 && strncmp(addr_str, "0X", 2) != 0) {
		json_decref(root);
		return -1;
	}
	phys_addr = strtoull(addr_str, NULL, 16);

	/* Get required Length parameter */
	length_j = json_object_get(root, "Length");
	if (!length_j || !json_is_integer(length_j)) {
		json_decref(root);
		return -1;
	}
	/*
	 * Length is "in bytes" (Memory_v1 ScanMedia), and Scan Media
	 * Physical Address Length "shall be in units of 64 bytes"; Bits[5:0]
	 * of Scan Media Physical Address are Reserved (CXL r4.0 Table 8-327).
	 * A range that is not in whole 64-byte lines cannot be expressed: it
	 * is refused rather than widened. An empty one is refused too, which
	 * is cxl-fmd's choice: the specification does not say what a Length
	 * of 0 does.
	 */
	if (json_integer_value(length_j) <= 0 ||
	    (json_integer_value(length_j) & 0x3f) || (phys_addr & 0x3f)) {
		json_decref(root);
		return -1;
	}
	length = (uint64_t)json_integer_value(length_j) / 64;

	/* Get optional NoEventLog parameter */
	no_event_log_j = json_object_get(root, "NoEventLog");
	if (no_event_log_j && json_is_true(no_event_log_j))
		flags |= 0x01;  /* Bit 0: No Event Log */

	json_decref(root);

	if (unlikely(fmd_is_mock(ctx)))
		return 0;

	/* For real hardware, issue the CXL Scan Media command (4304h) */
	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info ti_buf, *ti;

	struct cxlmi_cmd_memdev_scan_media_req req = {
		.scan_media_physaddr = phys_addr,
		.scan_media_physaddr_length = length,
		.scan_media_flags = flags,
	};

	if (comp->type == FMD_COMPONENT_SLD) {
		struct fmd_sld *sld = fmd_to_sld(comp);

		if (strcmp(chassis_id, mem_id) != 0)
			return -1;

		ep = sld->base.ep;
		if (!ep || fmd_sld_behind_switch(&sld->base))
			return -1;

		/* SLDs don't need tunneling */
		ti = CXLMI_NO_TUNNEL;

	} else if (comp->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(comp);

		/* Parse LD ID from mem_id (format: {chassis_id}_ld{n}) */
		char prefix[128];
		snprintf(prefix, sizeof(prefix), "%s_ld", chassis_id);
		if (strncmp(mem_id, prefix, strlen(prefix)) != 0)
			return -1;
		int ld_id = safe_parse_int(mem_id + strlen(prefix));
		if (ld_id < 0)
			return -1;

		ep = mld->base.ep;
		if (!ep)
			return -1;

		/* LDs need tunneling based on MLD topology */
		ti = fmd_mld_ld_tunnel(mld, ld_id, &ti_buf);
	} else {
		return -1;
	}

	/* Scan Media runs in the background: "started" is success */
	rc = cxlmi_cmd_memdev_scan_media(ep, ti, &req);
	if (rc != 0 && rc != CXLMI_RET_BACKGROUND)
		return -1;

	return 0;
}

/*
 * Helper to setup endpoint and tunneling from Memory resource mem_id
 * Returns 0 on success with ep and ti populated, -1 on error
 *
 * *ti_out is either CXLMI_NO_TUNNEL or ti_buf, which the caller provides
 * and must keep alive until it is done with the tunnel.
 */
static int memory_endpoint_setup(struct fmd_ctx *ctx,
				   const char *chassis_id,
				   const char *mem_id,
				   struct cxlmi_endpoint **ep_out,
				   struct cxlmi_tunnel_info *ti_buf,
				   struct cxlmi_tunnel_info **ti_out)
{
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp))
		return -1;

	*ti_out = CXLMI_NO_TUNNEL;

	if (comp->type == FMD_COMPONENT_SLD) {
		/* SLD: mem_id should match chassis_id */
		if (strcmp(mem_id, chassis_id) != 0)
			return -1;

		if (unlikely(!comp->ep) || fmd_sld_behind_switch(comp))
			return -1;
		*ep_out = comp->ep;

	} else if (comp->type == FMD_COMPONENT_MLD) {
		/* MLD: mem_id should be chassis_id_ld{n} */
		char prefix[128];
		snprintf(prefix, sizeof(prefix), "%s_ld", chassis_id);
		if (strncmp(mem_id, prefix, strlen(prefix)) != 0)
			return -1;
		int ld_id = safe_parse_int(mem_id + strlen(prefix));
		if (ld_id < 0)
			return -1;

		struct fmd_mld *mld = fmd_to_mld(comp);
		if (!mld->base.ep)
			return -1;
		*ep_out = mld->base.ep;

		*ti_out = fmd_mld_ld_tunnel(mld, ld_id, ti_buf);
	} else {
		return -1;
	}

	return 0;
}

/*
 * The Persistent Memory Data-at-rest Security commands (CXL r4.0 Section
 * 8.2.10.9.6) that DSP0288 1.4.0 Table 1 maps to actions of
 * CXLLogicalDevice: Set Passphrase, Disable Passphrase, Freeze Security
 * State, Passphrase Secure Erase and Unlock, to the SetPassphrase,
 * DisablePassphrase, FreezeSecurityState, PassphraseSecureErase and
 * Unlock actions. (Get Security State stays on Memory, SecurityStates.)
 */
enum sec_op {
	SEC_SET_PASSPHRASE,
	SEC_DISABLE_PASSPHRASE,
	SEC_UNLOCK,
	SEC_FREEZE,
	SEC_SECURE_ERASE,
};

static const struct sec_action {
	const char *name;	/* CXLLogicalDevice.{name} */
	uint16_t opcode;
	bool type;		/* takes PassphraseType and Passphrase */
	bool new_passphrase;	/* and NewPassphrase */
} sec_actions[] = {
	[SEC_SET_PASSPHRASE] = { "SetPassphrase", 0x4501, true, true },
	[SEC_DISABLE_PASSPHRASE] = { "DisablePassphrase", 0x4502, true, false },
	[SEC_UNLOCK] = { "Unlock", 0x4503, true, false },
	[SEC_FREEZE] = { "FreezeSecurityState", 0x4504, false, false },
	[SEC_SECURE_ERASE] = { "PassphraseSecureErase", 0x4505, true, false },
};

#define SEC_ACTIONS (sizeof(sec_actions) / sizeof(sec_actions[0]))

/* A passphrase field: "Current Passphrase", "New Passphrase" (Table 8-336) */
#define SEC_PASSPHRASE_LEN 0x20

/* Passphrase Type (Tables 8-336, 8-337 and 8-339) */
#define SEC_MASTER 0x00
#define SEC_USER   0x01

/* What a request of one of the actions asks for */
struct sec_req {
	enum sec_op op;
	uint8_t type;
	uint8_t passphrase[SEC_PASSPHRASE_LEN];
	uint8_t new_passphrase[SEC_PASSPHRASE_LEN];
};

/*
 * The memory device @dev and LD @ld_idx a security action goes to, and the
 * commands its Command Effects Log lists: an SLD's own (LD 0), or an LD of
 * an MLD's, read through the tunnel (a mock's LD has its MLD's
 * cel_commands). -ENOENT if there is no such LD; -EOPNOTSUPP for the pool
 * LD of a Multi-Headed device, which cxl-fmd sends no Memory Device
 * command (it has no route to one: memory_endpoint_setup()).
 */
static int sec_target(struct fmd_ctx *ctx, struct fmd_component *dev,
		      int ld_idx, uint64_t *cmds)
{
	if (!dev || ld_idx < 0 || ld_idx > UINT8_MAX)
		return -ENOENT;
	if (dev->type == FMD_COMPONENT_SLD) {
		if (ld_idx != 0)
			return -ENOENT;
		*cmds = fmd_to_sld(dev)->supported_cmds;
		return 0;
	}
	if (dev->type == FMD_COMPONENT_MLD) {
		struct fmd_ld *ld = fmd_topology_find_ld(&ctx->topology, dev->id,
							 (uint8_t)ld_idx);

		if (!ld)
			return -ENOENT;
		*cmds = ld->supported_cmds;
		return 0;
	}
	if (dev->type == FMD_COMPONENT_MHD &&
	    fmd_topology_find_ld(&ctx->topology, dev->id, (uint8_t)ld_idx))
		return -EOPNOTSUPP;
	return -ENOENT;
}

/*
 * Whether action @a may be asked of @dev: its command is in @cmds, what
 * the CEL lists (-EOPNOTSUPP if not: one the CEL could not be read for
 * lists none of these, cxl-fmd's choice), and may be sent to it
 * (mctp_refused(): FMD_MCTP_RESTRICTED).
 */
static int sec_allowed(struct fmd_ctx *ctx, const struct fmd_component *dev,
		       uint64_t cmds, const struct sec_action *a, const char **msg)
{
	if (!(cmds & fmd_cmd_bit(a->opcode))) {
		*msg = "The device's Command Effects Log does not list the command";
		return -EOPNOTSUPP;
	}
	if (mctp_refused(ctx, dev, a->opcode))
		return FMD_MCTP_RESTRICTED;
	return 0;
}

/* Whether the action is served on @dev's LD: as sec_allowed() takes it */
static bool sec_served(struct fmd_ctx *ctx, const struct fmd_component *dev,
		       uint64_t cmds, const struct sec_action *a)
{
	const char *msg;

	return sec_allowed(ctx, dev, cmds, a, &msg) == 0 &&
	       fmd_mctp_restricted_allowed(ctx, dev, a->opcode);
}

/*
 * Passphrase @name of @root into @out, zero-padded. A passphrase field is
 * 20h bytes long (Tables 8-336 to 8-339): a longer one is refused rather
 * than cut, and the bytes are the string's UTF-8 as JSON gives them,
 * which is cxl-fmd's choice (CXL gives them no encoding).
 */
static int sec_passphrase(json_t *root, const char *name,
			  uint8_t out[SEC_PASSPHRASE_LEN], const char **msg)
{
	json_t *j = json_object_get(root, name);

	if (!j || !json_is_string(j)) {
		*msg = "A required parameter is missing, or is not a string";
		return -EINVAL;
	}
	if (json_string_length(j) > SEC_PASSPHRASE_LEN) {
		*msg = "A passphrase is at most 32 bytes";
		return -EINVAL;
	}
	memset(out, 0, SEC_PASSPHRASE_LEN);
	memcpy(out, json_string_value(j), json_string_length(j));
	return 0;
}

/*
 * The parameters of @a from @body: PassphraseType, "User" or "Master"
 * (CXLLogicalDevice_v1 PassphraseType), unless @type is fixed (>= 0, the
 * Memory aliases); Passphrase; NewPassphrase. FreezeSecurityState takes
 * none, and its body is not read: "If an action does not have any
 * required parameters, the service shall accept an empty JSON object"
 * (DSP0266 1.25.0 Section 7.11). Unlock "supplies the user passphrase"
 * (Section 8.2.10.9.6.4), and its payload has no Passphrase Type (Table
 * 8-338): Master is refused.
 */
static int sec_parse(const struct sec_action *a, int type, const char *body,
		     size_t body_size, struct sec_req *req, const char **msg)
{
	json_error_t error;
	json_t *root;
	int rc = 0;

	req->op = (enum sec_op)(a - sec_actions);
	if (!a->type)
		return 0;
	root = json_loadb(body, body_size, 0, &error);
	if (!root || !json_is_object(root)) {
		json_decref(root);
		*msg = "The request body is not a JSON object";
		return -EINVAL;
	}
	if (type < 0) {
		json_t *t = json_object_get(root, "PassphraseType");
		const char *v = json_is_string(t) ? json_string_value(t) : NULL;

		if (v && !strcmp(v, "User")) {
			type = SEC_USER;
		} else if (v && !strcmp(v, "Master")) {
			type = SEC_MASTER;
		} else {
			*msg = "PassphraseType must be User or Master";
			rc = -EINVAL;
			goto out;
		}
	}
	if (req->op == SEC_UNLOCK && type != SEC_USER) {
		*msg = "Unlock takes the user passphrase: CXL r4.0 Unlock has no Passphrase Type";
		rc = -EINVAL;
		goto out;
	}
	req->type = (uint8_t)type;
	rc = sec_passphrase(root, "Passphrase", req->passphrase, msg);
	if (!rc && a->new_passphrase)
		rc = sec_passphrase(root, "NewPassphrase", req->new_passphrase, msg);
out:
	json_decref(root);
	return rc;
}

/*
 * Send @req to LD @ld_idx of @dev. What a device answers is mapped by
 * cxl-fmd's choice: Invalid Input, Incorrect Passphrase and Invalid
 * Payload Length -EINVAL (400), Invalid Security State -EBUSY (409),
 * Unsupported and Unsupported Mailbox or CCI -EOPNOTSUPP (501), Retry
 * Required and Busy -EAGAIN (503), any other -EIO (500), as is a command
 * that could not be sent or whose response could not be read. In a mock
 * nothing is sent.
 */
static int sec_send(struct fmd_ctx *ctx, struct fmd_component *dev, int ld_idx,
		    const struct sec_req *req, const char **msg)
{
	struct cxlmi_tunnel_info ti_buf, *ti = CXLMI_NO_TUNNEL;
	struct cxlmi_endpoint *ep = dev->ep;
	int rc;

	if (unlikely(fmd_is_mock(ctx)))
		return 0;
	if (!ep || fmd_sld_behind_switch(dev)) {
		*msg = "The device cannot be reached";
		return -EOPNOTSUPP;
	}
	if (dev->type == FMD_COMPONENT_MLD)
		ti = fmd_mld_ld_tunnel(fmd_to_mld(dev), ld_idx, &ti_buf);

	switch (req->op) {
	case SEC_SET_PASSPHRASE: {
		struct cxlmi_cmd_memdev_set_passphrase_req in = {
			.passphrase_type = req->type,
		};

		memcpy(in.current_passphrase, req->passphrase, SEC_PASSPHRASE_LEN);
		memcpy(in.new_passphrase, req->new_passphrase, SEC_PASSPHRASE_LEN);
		rc = cxlmi_cmd_memdev_set_passphrase(ep, ti, &in);
		break;
	}
	case SEC_DISABLE_PASSPHRASE: {
		struct cxlmi_cmd_memdev_disable_passphrase_req in = {
			.passphrase_type = req->type,
		};

		memcpy(in.passphrase, req->passphrase, SEC_PASSPHRASE_LEN);
		rc = cxlmi_cmd_memdev_disable_passphrase(ep, ti, &in);
		break;
	}
	case SEC_UNLOCK: {
		struct cxlmi_cmd_memdev_unlock_req in;

		memcpy(in.current_passphrase, req->passphrase, SEC_PASSPHRASE_LEN);
		rc = cxlmi_cmd_memdev_unlock(ep, ti, &in);
		break;
	}
	case SEC_FREEZE:
		rc = cxlmi_cmd_memdev_freeze_security_state(ep, ti);
		break;
	case SEC_SECURE_ERASE: {
		struct cxlmi_cmd_memdev_passphrase_secure_erase_req in = {
			.passphrase_type = req->type,
		};

		memcpy(in.passphrase, req->passphrase, SEC_PASSPHRASE_LEN);
		rc = cxlmi_cmd_memdev_passphrase_secure_erase(ep, ti, &in);
		break;
	}
	default:
		return -EINVAL;
	}

	if (rc == CXLMI_RET_SUCCESS)
		return 0;
	if (rc < 0) {
		*msg = "The command could not be sent, or its response could not be read";
		return -EIO;
	}
	*msg = cxlmi_cmd_retcode_tostr(rc);
	switch (rc) {
	case CXLMI_RET_INPUT:
	case CXLMI_RET_PASSPHRASE:
	case CXLMI_RET_PAYLOADLEN:
		return -EINVAL;
	case CXLMI_RET_SECURITY:
		return -EBUSY;
	case CXLMI_RET_UNSUPPORTED:
	case CXLMI_RET_MBUNSUPPORTED:
		return -EOPNOTSUPP;
	case CXLMI_RET_RETRY:
	case CXLMI_RET_BUSY:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

/*
 * The checks and the command of action @a on LD @ld_idx of @dev, in this
 * order: the LD is there (-ENOENT, -EOPNOTSUPP: sec_target()), its CEL
 * lists the command (-EOPNOTSUPP) and it may be sent (FMD_MCTP_RESTRICTED:
 * sec_allowed()), the parameters (-EINVAL: sec_parse()), then what the
 * device answers (sec_send()). Nothing is sent unless all of them pass.
 */
static int sec_action(struct fmd_ctx *ctx, struct fmd_component *dev, int ld_idx,
		      const struct sec_action *a, int type, const char *body,
		      size_t body_size, const char **msg)
{
	struct sec_req req = { 0 };
	uint64_t cmds = 0;
	int rc;

	rc = sec_target(ctx, dev, ld_idx, &cmds);
	if (!rc)
		rc = sec_allowed(ctx, dev, cmds, a, msg);
	if (!rc)
		rc = sec_parse(a, type, body, body_size, &req, msg);
	if (!rc)
		rc = sec_send(ctx, dev, ld_idx, &req, msg);
	return rc;
}

/*
 * The security actions of CXLLogicalDevice @ld_id served for it: those
 * whose command the LD's CEL lists and may be sent (sec_allowed()).
 * Added to @actions, which a DCD's actions share.
 */
static void sec_ld_actions(struct fmd_ctx *ctx, struct fmd_component *dev,
			   int ld_idx, const char *uri, json_t *actions)
{
	uint64_t cmds = 0;

	if (sec_target(ctx, dev, ld_idx, &cmds))
		return;
	for (size_t i = 0; i < SEC_ACTIONS; i++) {
		char key[64];

		if (!sec_served(ctx, dev, cmds, &sec_actions[i]))
			continue;
		snprintf(key, sizeof(key), "#CXLLogicalDevice.%s", sec_actions[i].name);
		json_object_set_new(actions, key, json_pack("{s:s}", "target",
			rf_uri(0, "%s/Actions/CXLLogicalDevice.%s", uri,
			       sec_actions[i].name)));
	}
}

int fmd_redfish_ld_security_action(struct fmd_ctx *ctx, const char *chassis_id,
				   const char *dev_id, const char *ld_id,
				   const char *action, const char *body,
				   size_t body_size, const char **msg)
{
	struct fmd_component *dev = fmd_topology_find(&ctx->topology, chassis_id);

	*msg = NULL;
	if (!dev || strcmp(chassis_id, dev_id) != 0 || !fmd_is_memdev(dev))
		return -ENOENT;
	for (size_t i = 0; i < SEC_ACTIONS; i++) {
		if (!strcmp(action, sec_actions[i].name))
			return sec_action(ctx, dev, parse_ld_id(ld_id), &sec_actions[i],
					  -1, body, body_size, msg);
	}
	return -ENOENT;
}

/*
 * The security actions of Memory, as DSP0288 1.2.0 mapped them, now
 * deprecated aliases of the CXLLogicalDevice actions (cxl-fmd's choice:
 * the Memory schema does not deprecate them). Each is the action of the
 * LD of the Memory resource, with the request it took: SetPassphrase and
 * DisablePassphrase for the user passphrase (01h), SetMasterPassphrase
 * and DisableMasterPassphrase for the master passphrase (00h), each with
 * Passphrase (and NewPassphrase); SecureEraseUnit with PassphraseType and
 * Passphrase; FreezeSecurityState with none. There is no UnlockUnit.
 * Returns as fmd_redfish_ld_security_action().
 */
static int memory_sec_action(struct fmd_ctx *ctx, const char *chassis_id,
			     const char *mem_id, enum sec_op op, int type,
			     const char *body, size_t body_size, const char **msg)
{
	struct fmd_component *dev = memory_device(ctx, chassis_id, mem_id);
	int ld_idx = 0;

	*msg = NULL;
	if (!dev)
		return -ENOENT;
	if (dev->type == FMD_COMPONENT_MLD)
		ld_idx = safe_parse_int(mem_id + strlen(chassis_id) + strlen("_ld"));
	return sec_action(ctx, dev, ld_idx, &sec_actions[op], type, body,
			  body_size, msg);
}

int fmd_redfish_action_set_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
				      const char *mem_id, const char *body,
				      size_t body_size, const char **msg)
{
	return memory_sec_action(ctx, chassis_id, mem_id, SEC_SET_PASSPHRASE,
				 SEC_USER, body, body_size, msg);
}

int fmd_redfish_action_set_master_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
					     const char *mem_id, const char *body,
					     size_t body_size, const char **msg)
{
	return memory_sec_action(ctx, chassis_id, mem_id, SEC_SET_PASSPHRASE,
				 SEC_MASTER, body, body_size, msg);
}

int fmd_redfish_action_disable_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
					  const char *mem_id, const char *body,
					  size_t body_size, const char **msg)
{
	return memory_sec_action(ctx, chassis_id, mem_id, SEC_DISABLE_PASSPHRASE,
				 SEC_USER, body, body_size, msg);
}

int fmd_redfish_action_disable_master_passphrase(struct fmd_ctx *ctx, const char *chassis_id,
						 const char *mem_id, const char *body,
						 size_t body_size, const char **msg)
{
	return memory_sec_action(ctx, chassis_id, mem_id, SEC_DISABLE_PASSPHRASE,
				 SEC_MASTER, body, body_size, msg);
}

int fmd_redfish_action_freeze_security_state(struct fmd_ctx *ctx, const char *chassis_id,
					     const char *mem_id, const char **msg)
{
	return memory_sec_action(ctx, chassis_id, mem_id, SEC_FREEZE, -1, NULL, 0, msg);
}

int fmd_redfish_action_secure_erase_unit(struct fmd_ctx *ctx, const char *chassis_id,
					 const char *mem_id, const char *body,
					 size_t body_size, const char **msg)
{
	return memory_sec_action(ctx, chassis_id, mem_id, SEC_SECURE_ERASE, -1,
				 body, body_size, msg);
}

/* The Memory alias of each security action, and the action it is */
static const struct {
	const char *name;
	enum sec_op op;
} memory_sec_aliases[] = {
	{ "SetPassphrase", SEC_SET_PASSPHRASE },
	{ "SetMasterPassphrase", SEC_SET_PASSPHRASE },
	{ "DisablePassphrase", SEC_DISABLE_PASSPHRASE },
	{ "DisableMasterPassphrase", SEC_DISABLE_PASSPHRASE },
	{ "FreezeSecurityState", SEC_FREEZE },
	{ "SecureEraseUnit", SEC_SECURE_ERASE },
};

/*
 * The deprecated security actions of Memory @mem_id served for it, into
 * @actions: as the CXLLogicalDevice's are (sec_served()).
 */
static void sec_memory_actions(struct fmd_ctx *ctx, const char *chassis_id,
			       const char *mem_id, json_t *actions)
{
	struct fmd_component *dev = memory_device(ctx, chassis_id, mem_id);
	uint64_t cmds = 0;
	int ld_idx = 0;

	if (!dev)
		return;
	if (dev->type == FMD_COMPONENT_MLD)
		ld_idx = safe_parse_int(mem_id + strlen(chassis_id) + strlen("_ld"));
	if (sec_target(ctx, dev, ld_idx, &cmds))
		return;
	for (size_t i = 0; i < sizeof(memory_sec_aliases) / sizeof(memory_sec_aliases[0]); i++) {
		char key[64];

		if (!sec_served(ctx, dev, cmds, &sec_actions[memory_sec_aliases[i].op]))
			continue;
		snprintf(key, sizeof(key), "#Memory.%s", memory_sec_aliases[i].name);
		json_object_set_new(actions, key, json_pack("{s:s}", "target",
			rf_uri(0, "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.%s",
			       chassis_id, mem_id, memory_sec_aliases[i].name)));
	}
}

/*
 * Port.ResetPPB action handler
 *
 * DSP0288 Table 2: Physical Port Control (Opcode 5102h)
 * Port Opcode 02h: Reset PPB -> Port.ResetPPB action
 *
 * This action resets the PCI-to-PCI bridge (PPB) for the specified port.
 *
 * Returns: 0 on success, -2 if the port is not a DSP, -1 on another error
 */
int fmd_redfish_action_reset_ppb(struct fmd_ctx *ctx,
				 const char *switch_id,
				 const char *port_id)
{
	/* Find the switch */
	struct fmd_component *sw_comp = fmd_topology_find(&ctx->topology, switch_id);
	if (!sw_comp || sw_comp->type != FMD_COMPONENT_SWITCH)
		return -1;

	struct fmd_switch *sw = fmd_to_switch(sw_comp);

	/* Find the port */
	struct fmd_component *port_comp = fmd_topology_find(&ctx->topology, port_id);
	if (!port_comp || port_comp->type != FMD_COMPONENT_PORT)
		return -1;

	/* Verify port belongs to this switch */
	if (port_comp->parent != sw_comp)
		return -1;

	struct fmd_port *port = fmd_to_port(port_comp);

	/* offered only on a DSP (see fmd_redfish_port()), in mock mode too */
	if (port->state != FMD_PORT_DSP)
		return -2;

	if (unlikely(fmd_is_mock(ctx)))
		return 0;

	/* Need switch endpoint to send FM-API command */
	if (!sw->base.ep)
		return -1;

	struct cxlmi_cmd_fmapi_phys_port_control_req req = {0};
	req.ppb_id = port->port_number;
	req.port_opcode = 0x02;  /* Reset PPB */

	if (cxlmi_cmd_fmapi_phys_port_control(sw->base.ep,
					      CXLMI_NO_TUNNEL, &req) != 0)
		return -1;

	return 0;
}

/*
 * ============================================================================
 * Connections: POST binds, DELETE unbinds
 * ============================================================================
 *
 * DSP0288 maps no CXL command to a Connection. That a POST of one binds a
 * vPPB (Bind vPPB, 5201h) and a DELETE unbinds it (Unbind vPPB, 5202h)
 * follows the OCP CMS Memory Fabric Orchestration white paper, which is
 * informative, and is cxl-fmd's choice.
 *
 * Both run with fabric_lock held exclusively (server.c), the command sent
 * included: reading the VCSs of the switch, deciding on what they say,
 * choosing a vPPB and recording the bind under way (struct fmd_binding_op)
 * are one step, and the op is on ctx->binding_ops before 5201h is sent.
 * Another request for the same target finds it, and one for another
 * target cannot choose the same vPPB.
 */

/*
 * Links in a request name a resource by the URI the service serves it at,
 * and by nothing else: "This property shall contain an array of links to
 * resources of type `Endpoint`" (Connection_v1, InitiatorEndpoints and
 * TargetEndpoints), and "a link to a resource of type `MemoryChunk`"
 * (MemoryChunk of MemoryChunkInfo). A URI whose last segment is the Id of
 * a component, under any other path, names no such resource. The URIs are
 * built by endpoint_uri() and chunk_uri(), which the GET handlers serve
 * them by, so that what a request is held to cannot drift from what is
 * served.
 */

/* The @odata.id of link object @link, NULL if it has none */
static const char *link_uri(json_t *link)
{
	return json_string_value(json_object_get(link, "@odata.id"));
}

#define POST_MAX_CHUNKS 64

struct post_request {
	struct fmd_host *host;
	struct fmd_component *target;
	struct fmd_mem_chunk *chunks[POST_MAX_CHUNKS];
	int nchunks;
	bool bad_access;	/* AccessCapabilities other than Read and Write */
	bool volume_info;	/* VolumeInfo is in the body */
	bool bad_state;		/* an AccessState other than Optimized */
};

/* The one link of array @arr, NULL unless it has exactly one */
static const char *only_link(json_t *arr)
{
	if (!json_is_array(arr) || json_array_size(arr) != 1)
		return NULL;
	return link_uri(json_array_get(arr, 0));
}

/*
 * The component Endpoint link @uri names: 0 with *comp, NULL if no
 * component has the Id, or -21 if @uri is not an Endpoint's URI. The
 * component is not checked to have an Endpoint: the alias of an MLD or a
 * Multi-Headed device, which have none, is matched by the caller, by its
 * type.
 */
static int resolve_endpoint(struct fmd_ctx *ctx, const char *uri,
			    struct fmd_component **comp)
{
	const size_t n = strlen(RF_ENDPOINTS);
	char canon[sizeof(RF_ENDPOINTS) + FMD_MAX_ID_LEN];
	const char *id;

	if (!uri || strncmp(uri, RF_ENDPOINTS, n))
		return -21;
	id = uri + n;
	if (!*id || strchr(id, '/') || endpoint_uri(canon, sizeof(canon), id) ||
	    strcmp(canon, uri))
		return -21;
	*comp = fmd_topology_find(&ctx->topology, id);
	return 0;
}

/*
 * Whether @p, what follows "/redfish/v1/Chassis/" in a URI, is
 * {a}/MemoryDomains/{b}/MemoryChunks/{c}, each segment not empty
 */
static bool chunk_uri_shape(const char *p)
{
	static const char *const sep[] = { "/MemoryDomains/", "/MemoryChunks/" };

	for (size_t i = 0; i < 2; i++) {
		const char *slash = strchr(p, '/');

		if (!slash || slash == p || strncmp(slash, sep[i], strlen(sep[i])))
			return false;
		p = slash + strlen(sep[i]);
	}
	return *p && !strchr(p, '/');
}

/*
 * The chunk link @uri names: 0 with *chunk, NULL if it is a chunk URI
 * whose Id no chunk has, or -21 if @uri is not a chunk's URI: one under
 * another Chassis or MemoryDomain than the chunk's is not, nor is the
 * same chunk in a host's Systems view.
 */
static int resolve_chunk(struct fmd_ctx *ctx, const char *uri,
			 struct fmd_mem_chunk **chunk)
{
	static const char prefix[] = "/redfish/v1/Chassis/";
	char canon[RF_CHUNK_URI_LEN];
	struct fmd_mem_chunk *c;

	if (!uri || strncmp(uri, prefix, sizeof(prefix) - 1) ||
	    !chunk_uri_shape(uri + sizeof(prefix) - 1))
		return -21;
	c = fmd_topology_find_chunk(&ctx->topology, strrchr(uri, '/') + 1);
	if (c && (chunk_uri(canon, sizeof(canon), c) || strcmp(canon, uri)))
		return -21;
	*chunk = c;
	return 0;
}

/*
 * The body of a Connection POST, its links resolved. Returns 0, or as
 * fmd_redfish_post_connection(): -2 a body that is not as below, -3 no
 * host has the initiator's Id, -4 nothing has the target's, -5 no chunk
 * has a chunk's, -21 a link that is not the URI of its resource, -22
 * EndpointGroups.
 */
static int parse_post(struct fmd_ctx *ctx, const char *body, size_t body_size,
		      struct post_request *req)
{
	json_t *root, *links, *info, *entry;
	struct fmd_component *comp = NULL;
	json_error_t error;
	size_t i;
	int rc = -2;

	memset(req, 0, sizeof(*req));
	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -2;
	/*
	 * VolumeInfo has no OData.Permissions annotation in Connection_v1,
	 * where ConnectionType's is Read, so it is not one of the
	 * "service-controlled properties" a create "may ignore" (DSP0266
	 * Section 7.9). Of the Volume of each entry, "The endpoints
	 * referenced by the `InitiatorEndpoints` or `InitiatorEndpointGroups`
	 * properties shall be given access to this volume" (Connection_v1):
	 * cxl-fmd serves no Volume, and a binding gives memory, so it cannot
	 * carry that out. Present at all, even empty, it is refused rather
	 * than ignored (cxl-fmd's choice).
	 */
	req->volume_info = json_object_get(root, "VolumeInfo") != NULL;
	links = json_object_get(root, "Links");
	if (!json_is_object(links))
		goto out;

	/*
	 * InitiatorEndpoints "shall not be present if
	 * `InitiatorEndpointGroups` is present", TargetEndpoints "if
	 * `TargetEndpointGroups` is present" (Connection_v1), and cxl-fmd
	 * serves no EndpointGroup: a body with either names nothing it can
	 * bind (cxl-fmd's choice to refuse rather than ignore it).
	 */
	if (json_object_get(links, "InitiatorEndpointGroups") ||
	    json_object_get(links, "TargetEndpointGroups")) {
		rc = -22;
		goto out;
	}

	/* one initiator, the host, and one target */
	{
		const char *init = only_link(json_object_get(links, "InitiatorEndpoints"));
		const char *target = only_link(json_object_get(links, "TargetEndpoints"));

		if (!init || !target)
			goto out;
		rc = resolve_endpoint(ctx, init, &comp);
		if (rc)
			goto out;
		if (!comp || comp->type != FMD_COMPONENT_HOST) {
			rc = -3;
			goto out;
		}
		req->host = fmd_to_host(comp);
		rc = resolve_endpoint(ctx, target, &req->target);
		if (rc)
			goto out;
		if (!req->target) {
			rc = -4;
			goto out;
		}
	}

	rc = -2;
	info = json_object_get(root, "MemoryChunkInfo");
	if (info && !json_is_array(info))
		goto out;
	json_array_foreach(info, i, entry) {
		json_t *access = json_object_get(entry, "AccessCapabilities");
		json_t *state = json_object_get(entry, "AccessState");
		struct fmd_mem_chunk *chunk = NULL;
		const char *uri = link_uri(json_object_get(entry, "MemoryChunk"));

		if (!uri || req->nchunks == POST_MAX_CHUNKS) {
			rc = -2;
			goto out;
		}
		rc = resolve_chunk(ctx, uri, &chunk);
		if (rc)
			goto out;
		if (!chunk) {
			rc = -5;
			goto out;
		}
		req->chunks[req->nchunks++] = chunk;
		rc = -2;

		/* a binding gives the whole of what it reaches, to read and write */
		if (access) {
			unsigned int caps = 0;
			size_t j;
			json_t *cap;

			if (!json_is_array(access))
				goto out;
			json_array_foreach(access, j, cap) {
				const char *c = json_string_value(cap);

				if (c && !strcmp(c, "Read"))
					caps |= FMD_ACCESS_READ;
				else if (c && !strcmp(c, "Write"))
					caps |= FMD_ACCESS_WRITE;
				else
					req->bad_access = true;
			}
			if (caps != (FMD_ACCESS_READ | FMD_ACCESS_WRITE))
				req->bad_access = true;
		}

		/*
		 * AccessState is ReadWrite, one of Optimized, NonOptimized,
		 * Standby, Unavailable and Transitioning (Connection_v1). What
		 * a Connection serves is conn_access_state()'s: Optimized
		 * once it is Enabled, which is where a POST that succeeds
		 * ends (201; 202, then Enabled; 200 of an Enabled one; an
		 * Absent one lists no chunk). Bind vPPB has no field for any
		 * other (Table 7-35), and Transitioning is what the service
		 * says while a bind runs, not something to ask for: Optimized
		 * only (cxl-fmd's choice).
		 */
		if (state && (!json_is_string(state) ||
			      strcmp(json_string_value(state), "Optimized")))
			req->bad_state = true;
	}
	rc = 0;
out:
	json_decref(root);
	return rc;
}

/* The Connection @id, answered as existing (200) */
static int post_existing(struct fmd_ctx *ctx, const char *id, json_t **response)
{
	*response = fmd_redfish_connection(ctx, "CXL", id);
	return *response ? FMD_CONN_EXISTS : -6;
}

/*
 * Bind port @port_id, LD @ld_id (FMD_LD_NONE for the whole port) of @sw to
 * a vPPB of @host's VCS: see fmd_redfish_post_connection().
 */
static int post_bind(struct fmd_ctx *ctx, struct fmd_host *host,
		     struct fmd_switch *sw, uint8_t port_id, uint16_t ld_id,
		     json_t **response)
{
	struct fmd_vcs *vcs, *user_vcs = NULL;
	struct fmd_binding_op *op;
	struct fmd_conn_view v;
	struct fmd_bind_key key;
	struct fmd_task *task;
	char id[FMD_CONN_ID_LEN];
	int vppb_id = -1, rc;

	/*
	 * What the switch has now, every VCS of it: another FM, or a request
	 * through another CCI, may have bound or unbound since it was read.
	 * The host's VCS is the one decided on: nothing is answered from it,
	 * nor bound in it, unless it was just read whole. One that could not
	 * be read may be read on a retry (503); one wider than cxl-fmd reads,
	 * or past the VCSs it reads, will not be (409).
	 */
	fmd_vcs_refresh(ctx, sw);
	switch (fmd_vcs_check(sw, host->vcs_id)) {
	case FMD_VCS_UNREAD:
		return -26;
	case FMD_VCS_TRUNCATED:
		return -27;
	default:
		break;
	}

	/*
	 * The host's VCS has it already, or is being given it: the same
	 * Connection, not another (cxl-fmd's choice; the specification
	 * does not say whether a second bind of a target is an error).
	 */
	fmd_topology_rdlock(&ctx->topology);
	rc = fmd_conn_find_target(ctx, sw, host->vcs_id, port_id, ld_id, &v);
	if (!rc) {
		enum fmd_conn_state state = v.state;
		int task_id = v.op ? atoi(v.op->task_id) : 0;

		snprintf(id, sizeof(id), "%s", v.id);
		fmd_topology_rdunlock(&ctx->topology);

		if (state == FMD_CONN_UPDATING)
			return -16;
		if (state == FMD_CONN_STARTING) {
			task = fmd_task_find(ctx, task_id);
			if (!task)
				return -11;
			*response = fmd_redfish_task(ctx, task->id);
			return FMD_OP_STARTED;
		}
		return post_existing(ctx, id, response);
	}
	fmd_topology_rdunlock(&ctx->topology);

	vcs = fmd_switch_find_vcs(sw, host->vcs_id);
	if (!vcs) {
		fmd_err(ctx, "%s: host %s has no VCS %d", sw->base.id,
			host->base.id, host->vcs_id);
		return -7;
	}

	/*
	 * An SLD port "is bound to only one VCS", and "The Fabric Manager can
	 * bind any unused physical port to the unbound vPPB" (CXL r4.0
	 * Section 7.2.3.1); "LDs within an MLD component can be bound to a
	 * host or unbound" (Section 7.3.4). That an LD, too, is bound to one
	 * vPPB only is cxl-fmd's reading of that model. A port bound whole
	 * while an LD of it is asked for, or the reverse, is in use as well.
	 */
	if (fmd_switch_bind_target_user(sw, port_id, ld_id, &user_vcs)) {
		fmd_warn(ctx, "%s: port %d%s is bound to a vPPB of VCS %d, not "
			 "for host %s", sw->base.id, port_id,
			 ld_id != FMD_LD_NONE ? " (LD)" : "", user_vcs->vcs_id,
			 host->base.id);
		return -9;
	}
	/* or is being bound, or unbound, for another */
	for (op = ctx->binding_ops; op; op = op->next) {
		if (strcmp(op->key.switch_id, sw->base.id) || op->key.port_id != port_id)
			continue;
		if (op->key.ld_id == ld_id || op->key.ld_id == FMD_LD_NONE ||
		    ld_id == FMD_LD_NONE)
			return -9;
	}

	/* the lowest downstream vPPB that is unbound and has nothing under way */
	for (int i = 0; i < vcs->num_vppbs; i++) {
		const struct fmd_vppb *p = &vcs->vppbs[i];

		if (p->vppb_id == 0 || p->binding_status != FMD_VPPB_UNBOUND ||
		    fmd_binding_op_find(ctx, sw->base.id, vcs->vcs_id, p->vppb_id))
			continue;
		if (vppb_id < 0 || p->vppb_id < vppb_id)
			vppb_id = p->vppb_id;
	}
	if (vppb_id < 0) {
		fmd_warn(ctx, "%s: no unbound vPPB in VCS %d", sw->base.id, vcs->vcs_id);
		return -12;
	}

	memset(&key, 0, sizeof(key));
	snprintf(key.switch_id, sizeof(key.switch_id), "%s", sw->base.id);
	key.vcs_id = vcs->vcs_id;
	key.vppb_id = (uint8_t)vppb_id;
	key.port_id = port_id;
	key.ld_id = ld_id;
	if (fmd_conn_id_format(&key, id, sizeof(id)))
		return -6;

	/*
	 * Recorded, and its task made, before the switch is asked: nothing
	 * is allocated once it may have started. Both keep the vPPB as just
	 * read, unbound, against which the outcome is judged.
	 */
	task = fmd_task_alloc(ctx, FMD_TASK_BIND, sw, &key);
	if (!task)
		return -6;
	op = fmd_binding_op_add(ctx, &key, FMD_BIND_OP_BIND, 0, NULL, &task->before);
	if (!op) {
		fmd_task_discard(task);
		return -6;
	}

	rc = fmd_vppb_bind(ctx, sw, key.vcs_id, key.vppb_id, port_id, ld_id);
	if (rc == FMD_OP_STARTED || rc == FMD_OP_UNCONFIRMED) {
		/* answered with the task; the Connection is Starting meanwhile */
		task = fmd_task_activate(ctx, task, 0);
		/*
		 * Success, and the vPPB not read bound after it: what the
		 * switch did is shown by the next read of the VCS that sees
		 * the vPPB settled (fmd_tasks_reconcile()), not taken from
		 * its answer alone (cxl-fmd's choice).
		 */
		if (task && rc == FMD_OP_UNCONFIRMED)
			fmd_task_interrupt(ctx, task, "The switch answered Bind "
					   "vPPB with Success, and the vPPB, read "
					   "after it, does not show the bind; the "
					   "vPPB is read again until it settles");
		*response = task ? fmd_redfish_task(ctx, task->id) : NULL;
		return task ? FMD_OP_STARTED : -6;
	}
	fmd_task_discard(task);
	fmd_binding_op_remove(ctx, op);

	if (rc == 0) {
		fmd_binding_mark_add(ctx, &key);
		fmd_info(ctx, "Connection %s: bound vPPB %d of VCS %d to port %d for %s",
			 id, key.vppb_id, key.vcs_id, port_id, host->base.id);
		*response = fmd_redfish_connection(ctx, "CXL", id);
		return *response ? 0 : -6;
	}

	fmd_err(ctx, "Connection %s: vPPB bind failed: %d", id, rc);
	switch (rc) {
	case -EBUSY:		/* the port or LD is bound to a vPPB */
		return -9;
	case -EAGAIN:		/* Busy, or Retry Required twice */
	case -EALREADY:		/* the vPPB chosen is no longer unbound */
		return -11;
	case -ESTALE:		/* the host's VCS could not be read again */
		return -26;
	case -E2BIG:		/* nor read whole */
		return -27;
	case -EINVAL:		/* an LD-ID the port does not have */
		return -13;
	case -EOPNOTSUPP:	/* Unsupported */
		return -17;
	case -EPROTO:		/* Invalid Input */
		return -18;
	default:
		return -7;
	}
}

struct direct_find {
	const struct fmd_host *host;
	const struct fmd_component *dev;
	const struct fmd_head *head;
	char id[FMD_CONN_ID_LEN];
};

static int direct_match(const struct fmd_conn_view *v, void *arg)
{
	struct direct_find *f = arg;

	if (v->kind != FMD_CONN_DIRECT || v->host != f->host ||
	    v->device != f->dev || v->head != f->head)
		return 0;
	snprintf(f->id, sizeof(f->id), "%s", v->id);
	return 1;
}

/*
 * POST /redfish/v1/Fabrics/{fabricId}/Connections
 *
 * Give a host a memory target: bind it to a vPPB of the host's VCS. The
 * body names one initiator, the host's Endpoint, and one target:
 *  - an SLD's or an LD's Endpoint, with or without MemoryChunkInfo, which
 *    then names chunks of that target;
 *  - accepted for compatibility: Endpoints/{id} with the ID of an MLD or
 *    of a Multi-Headed device, which have no Endpoint of their own (a GET
 *    of it is 404), with MemoryChunkInfo whose chunks are all on one LD,
 *    which is the target. It is matched by the component's type.
 * Every link is the URI the service serves its resource at (see
 * resolve_endpoint() and resolve_chunk()).
 * AccessCapabilities, if given, are Read and Write: a binding exposes the
 * whole of what it reaches, and Bind vPPB has no field for access (Table
 * 7-35). AccessState, if given, is Optimized, and the body has no
 * VolumeInfo: see parse_post().
 *
 * Idempotent: if the host's VCS has the target already, the answer is that
 * Connection (200), whoever made the binding; if it is being bound, the
 * task that follows the bind (202); if it is being unbound, 409.
 *
 * A direct-attached host and device have their Connection already, which
 * is answered with 200; a pair that has none cannot be given one (400).
 *
 * Returns:
 *   0 bound: *response is the Connection (201)
 *   FMD_OP_STARTED: the switch binds in the background, or was already, or
 *     answered Success and the vPPB was not read bound after it (the
 *     task Interrupted until a read settles it): *response is the Task (202)
 *   FMD_CONN_EXISTS: *response is the Connection there already (200)
 *  -1 fabric not found
 *  -2 invalid request body
 *  -3 host/initiator not found
 *  -4 target not found
 *  -5 memory chunk not found
 *  -6 out of memory, or no task could be made
 *  -7 the bind failed
 *  -8 the direct-attached host is not attached to that device or head
 *  -9 the target, or part of it, is bound or being bound for another vPPB
 * -10 the host and the device are not on the same switch, or one is
 *     direct-attached and the other is not
 * -11 the switch is Busy, answered Retry Required twice, or the vPPB
 *     chosen was bound by the time it was asked: try again
 * -12 the host's VCS has no unbound vPPB
 * -13 a chunk names no LD of the MLD, or one its port does not have
 * -14 the target is, or is reached through, a head that presents an MLD
 * -15 a chunk is not on the target
 * -16 the host's Connection to the target is being unbound
 * -17 the switch answered Unsupported
 * -18 the switch answered Invalid Input
 * -19 the chunks of an MLD or a Multi-Headed device are on several LDs
 * -20 AccessCapabilities other than Read and Write
 * -21 a link that is not the URI the service serves its resource at
 * -22 InitiatorEndpointGroups or TargetEndpointGroups
 * -23 a chunk is staged: its partition is active from the device's next
 *     reset (struct fmd_mem_chunk)
 * -24 VolumeInfo, which names volumes, not memory
 * -25 a MemoryChunkInfo AccessState other than Optimized
 * -26 the host's VCS could not be read: try again
 * -27 the host's VCS is not read whole: more vPPBs than FMD_MAX_VPPB, or
 *     past the FMD_MAX_VCS VCSs read
 * -28 the target is a pool LD of a head whose presentation is not known
 *     (FMD_HEAD_UNKNOWN)
 * Nothing is sent, nor changed, for any of -1 to -5 and -8 to -28.
 */
int fmd_redfish_post_connection(struct fmd_ctx *ctx, const char *fabric_id,
				const char *body, size_t body_size,
				json_t **response)
{
	struct post_request req;
	struct fmd_component *comp, *dev;
	const struct fmd_ld *ld = NULL;
	struct fmd_head *head = NULL;
	struct fmd_port *port = NULL;
	struct fmd_host *host;
	bool ld_named;
	int rc;

	*response = NULL;
	if (strcmp(fabric_id, "CXL") != 0)
		return -1;

	rc = parse_post(ctx, body, body_size, &req);
	if (rc)
		return rc;
	if (req.bad_access)
		return -20;
	if (req.volume_info)
		return -24;
	if (req.bad_state)
		return -25;

	host = req.host;
	comp = req.target;

	/*
	 * A head that presents an MLD: its LDs are bound by an LD-ID
	 * relative to the head, which the device maps to a pool LD by no
	 * rule the FM is told (CXL r4.0 Section 2.5.1).
	 */
	if (comp->type == FMD_COMPONENT_HEAD)
		return -14;
	if (comp->type == FMD_COMPONENT_LD && comp->parent &&
	    (comp->parent->type == FMD_COMPONENT_MLD ||
	     comp->parent->type == FMD_COMPONENT_MHD)) {
		ld = fmd_to_ld(comp);
		dev = comp->parent;
	} else if (fmd_is_memdev(comp)) {
		dev = comp;
	} else {
		return -4;
	}
	ld_named = ld != NULL;

	/* each chunk named is the target's; an MLD's are on one LD */
	for (int i = 0; i < req.nchunks; i++) {
		struct fmd_mem_chunk *chunk = req.chunks[i];
		const struct fmd_ld *cld;

		if (!chunk->base.parent || chunk->base.parent->parent != dev) {
			fmd_warn(ctx, "Connection rejected: chunk %s is not %s's",
				 chunk->base.id, dev->id);
			return -15;
		}
		/* not active until the device's next reset */
		if (chunk->staged) {
			fmd_warn(ctx, "Connection rejected: chunk %s is staged",
				 chunk->base.id);
			return -23;
		}
		if (dev->type == FMD_COMPONENT_SLD)
			continue;
		cld = fmd_chunk_ld(dev, chunk);
		if (!cld) {
			fmd_warn(ctx, "Connection rejected: chunk %s names no LD of %s",
				 chunk->base.id, dev->id);
			return -13;
		}
		if (ld && cld != ld)
			return ld_named ? -15 : -19;
		ld = cld;
	}
	/* an MLD or Multi-Headed device is bound per LD: one has to be named */
	if (dev->type != FMD_COMPONENT_SLD && !ld)
		return -13;

	if (dev->type == FMD_COMPONENT_MHD) {
		head = fmd_ld_head(ld);
		/*
		 * Whether its head binds the LD whole or by an LD-ID of its
		 * own depends on what the head presents (Section 2.5), which
		 * is not known: nothing is bound
		 */
		if (head && head->presentation == FMD_HEAD_UNKNOWN)
			return -28;
		port = head ? head->port : NULL;
	} else if (dev->parent && dev->parent->type == FMD_COMPONENT_PORT) {
		port = fmd_to_port(dev->parent);
	}

	if (port && port->base.parent &&
	    port->base.parent->type == FMD_COMPONENT_SWITCH) {
		struct fmd_switch *sw = fmd_to_switch(port->base.parent);

		/*
		 * A host reaches devices through the switch its root port is
		 * on: its VCS is a VCS of that switch.
		 */
		if (host->direct_attached || strcmp(host->switch_id, sw->base.id)) {
			fmd_warn(ctx, "Connection rejected: host %s is not on %s",
				 host->base.id, sw->base.id);
			return -10;
		}
		/*
		 * The port of a head is bound whole when the head presents an
		 * SLD, the one pool LD mapped to it: "A head with one LD
		 * mapped shall present itself as an SLD" (Section 2.5).
		 */
		if (head && (head->presentation == FMD_HEAD_MLD ||
			     fmd_head_whole_ld(head) != ld))
			return -14;

		/* LD ID: "Must be FFFFh for other EP types" (Table 7-35) */
		return post_bind(ctx, host, sw, port->port_number,
				 dev->type == FMD_COMPONENT_MLD ? ld->ld_id : FMD_LD_NONE,
				 response);
	}

	/* direct-attached: the pair has a Connection, or cannot have one */
	{
		struct direct_find f = { .host = host, .dev = dev,
					 .head = dev->type == FMD_COMPONENT_MHD ? head : NULL };

		if (!host->direct_attached || (port && !head))
			return -10;
		if (dev->type == FMD_COMPONENT_MHD && !head)
			return -8;
		fmd_topology_rdlock(&ctx->topology);
		rc = fmd_conn_for_each(ctx, direct_match, &f);
		fmd_topology_rdunlock(&ctx->topology);
		if (rc != 1) {
			fmd_warn(ctx, "Connection rejected: host %s is not attached to %s",
				 host->base.id, head ? head->base.id : dev->id);
			return -8;
		}
		return post_existing(ctx, f.id, response);
	}
}

#define UNBIND_UNCONFIRMED "The switch answered Unbind vPPB with Success, " \
	"and the vPPB, read after it, does not show it unbound; the vPPB is " \
	"read again until it settles"

/*
 * Unbind the vPPB of Connection @connection_id with Unbind Option @option
 * (Table 7-36), whoever bound it, the Connection Absent included. The
 * switch is read first: an ID whose vPPB now holds something else is not
 * found, and nothing is unbound. fmd_vppb_unbind() reads it again just
 * before Unbind vPPB, and sends it only if the vPPB still holds the port
 * and LD the ID names. If the ID's VCS could not be read, at either read,
 * nothing is sent.
 *
 * While an unbind with Simulate Managed Hot-Remove is under way, one with
 * Simulate Surprise Hot-Remove is still sent: "There is no requirement for
 * the FM to force the unbind operation, but it can do so using the
 * 'Simulate Surprise Hot-Remove' unbinding option in the Unbind vPPB
 * command" (CXL r4.0 Section 7.6.6.7). If the switch takes it, the task of
 * the unbind under way follows it to its end; if it is still running the
 * first, it answers Busy, and the op is left as it was.
 *
 * Returns as fmd_redfish_delete_connection().
 */
static int unbind_connection(struct fmd_ctx *ctx, const char *connection_id,
			     enum fmd_unbind_option option, json_t **response)
{
	struct fmd_binding_op *op;
	struct fmd_conn_view v;
	struct fmd_bind_key key;
	struct fmd_switch *sw;
	enum fmd_task_state was;
	struct fmd_task *task;
	enum fmd_conn_kind kind;
	bool busy, lost;
	int rc;

	/*
	 * What the switch has now. A VCS that could not be read is not
	 * looked up as it was last known: the switch may have changed it
	 * since (nothing is sent; try again).
	 */
	if (!fmd_conn_id_parse(connection_id, &key)) {
		sw = fmd_topology_find_switch(&ctx->topology, key.switch_id);
		if (sw) {
			fmd_vcs_refresh(ctx, sw);
			if (fmd_vcs_check(sw, key.vcs_id) == FMD_VCS_UNREAD)
				return -7;
		}
	}

	fmd_topology_rdlock(&ctx->topology);
	rc = fmd_conn_lookup(ctx, connection_id, &v);
	if (!rc) {
		kind = v.kind;
		key = v.key;
		busy = v.op != NULL;
	}
	fmd_topology_rdunlock(&ctx->topology);
	if (rc)
		return -1;

	/* a host's link to a device: no vPPB to unbind (cxl-fmd's choice) */
	if (kind == FMD_CONN_DIRECT)
		return -4;

	sw = fmd_topology_find_switch(&ctx->topology, key.switch_id);
	if (!sw)
		return -1;

	op = busy ? fmd_binding_op_find(ctx, key.switch_id, key.vcs_id, key.vppb_id) : NULL;
	if (busy) {
		/*
		 * The switch is still working on it: wait for that to end,
		 * unless a managed unbind is to be forced.
		 */
		if (!op || op->kind != FMD_BIND_OP_UNBIND ||
		    option != FMD_UNBIND_SURPRISE_HOT_REMOVE ||
		    op->unbind_option == FMD_UNBIND_SURPRISE_HOT_REMOVE)
			return -3;
		task = fmd_task_find(ctx, atoi(op->task_id));
		if (!task)
			return -3;

		fmd_info(ctx, "Connection %s: forcing the unbind of vPPB %d of VCS %d",
			 connection_id, key.vppb_id, key.vcs_id);
		/*
		 * An Interrupted task is followed again from here: the reads
		 * of the VCSs around the command must not end it, and free
		 * its op, under this one.
		 */
		was = task->state;
		lost = op->lost;
		task->state = FMD_TASK_RUNNING;
		op->lost = false;
		rc = fmd_vppb_unbind(ctx, sw, key.vcs_id, key.vppb_id,
				     key.port_id, key.ld_id, option);
		if (rc == 0 || rc == FMD_OP_STARTED || rc == FMD_OP_UNCONFIRMED) {
			/* the task under way sees the vPPB unbound */
			if (rc == FMD_OP_UNCONFIRMED)
				fmd_task_interrupt(ctx, task, UNBIND_UNCONFIRMED);
			op->unbind_option = option;
			task->failed_polls = 0;
			task->unobserved = 0;
			*response = fmd_redfish_task(ctx, task->id);
			return FMD_OP_STARTED;
		}
		task->state = was;
		op->lost = lost;
	} else {
		fmd_info(ctx, "Connection %s: unbinding vPPB %d of VCS %d (option %d)",
			 connection_id, key.vppb_id, key.vcs_id, option);
		/*
		 * its task is made before the switch is asked, as for a bind,
		 * with the vPPB as just read
		 */
		task = fmd_task_alloc(ctx, FMD_TASK_UNBIND, sw, &key);
		if (!task)
			return -2;
		op = fmd_binding_op_add(ctx, &key, FMD_BIND_OP_UNBIND, option,
					NULL, &task->before);
		if (!op) {
			fmd_task_discard(task);
			return -2;
		}

		rc = fmd_vppb_unbind(ctx, sw, key.vcs_id, key.vppb_id,
				     key.port_id, key.ld_id, option);
		if (rc == FMD_OP_STARTED || rc == FMD_OP_UNCONFIRMED) {
			/*
			 * it stands, Updating, until the vPPB is unbound; on
			 * a Success not read back, until a read shows it
			 */
			task = fmd_task_activate(ctx, task, option);
			if (task && rc == FMD_OP_UNCONFIRMED)
				fmd_task_interrupt(ctx, task, UNBIND_UNCONFIRMED);
			*response = task ? fmd_redfish_task(ctx, task->id) : NULL;
			return task ? FMD_OP_STARTED : -2;
		}
		fmd_task_discard(task);
		fmd_binding_op_remove(ctx, op);
	}

	switch (rc) {
	case 0:
		return 0;
	case -EAGAIN:
		return -3;
	case -ESTALE:		/* the last read before 5202h failed */
		return -7;
	case -ENOENT:		/* unbound, or another target, by then */
		return -1;
	case -EOPNOTSUPP:
		return -5;
	case -EPROTO:
		return -6;
	default:
		fmd_err(ctx, "Connection %s: vPPB unbind failed: %d", connection_id, rc);
		return -2;
	}
}

/*
 * DELETE /redfish/v1/Fabrics/{fabricId}/Connections/{connectionId}
 *
 * Unbind the vPPB, whoever bound it, the Connection Absent included, with
 * Unbind Option 1h, Simulate Managed Hot-Remove (Table 7-36). Option 0h,
 * waiting for the link to go down, is never sent: how it applies to an LD
 * the specification does not say. The switch is read first: an ID whose
 * vPPB now holds something else is not found, and nothing is unbound.
 *
 * Returns:
 *   0 unbound (204)
 *   FMD_OP_STARTED: the switch unbinds in the background; *response is the Task
 *  -1 not found, or no longer what the vPPB holds
 *  -2 the unbind failed
 *  -3 a bind or unbind of it is under way, or the switch is busy: try again
 *  -4 a direct-attached Connection: there is nothing to unbind
 *  -5 the switch answered Unsupported
 *  -6 the switch answered Invalid Input
 *  -7 the Connection's VCS could not be read: nothing is sent, try again
 */
int fmd_redfish_delete_connection(struct fmd_ctx *ctx, const char *fabric_id,
				  const char *connection_id, json_t **response)
{
	*response = NULL;
	if (strcmp(fabric_id, "CXL") != 0)
		return -1;
	return unbind_connection(ctx, connection_id, FMD_UNBIND_MANAGED_HOT_REMOVE,
				 response);
}

/*
 * POST /redfish/v1/Fabrics/{fabricId}/Connections/{id}/Actions/Oem/Cxl.ForceUnbind
 *
 * Unbind the vPPB with Unbind Option 2h, Simulate Surprise Hot-Remove
 * (Table 7-36), for a host that does not finish a managed hot-remove: the
 * FM "can do so using the 'Simulate Surprise Hot-Remove' unbinding option"
 * (CXL r4.0 Section 7.6.6.7). The specification gives no timeout for the
 * host's handshake, so when to force is the client's to decide. The action
 * and its name are cxl-fmd's (Connection_v1 has an Oem container for
 * actions). Also taken while a DELETE's unbind is under way: see
 * unbind_connection(). Returns as fmd_redfish_delete_connection(); -4 for a
 * direct-attached Connection is answered 400, the action not being offered.
 */
int fmd_redfish_action_force_unbind(struct fmd_ctx *ctx, const char *fabric_id,
				    const char *connection_id, json_t **response)
{
	*response = NULL;
	if (strcmp(fabric_id, "CXL") != 0)
		return -1;
	return unbind_connection(ctx, connection_id, FMD_UNBIND_SURPRISE_HOT_REMOVE,
				 response);
}
