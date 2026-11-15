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
static int memory_endpoint_setup(struct fmd_ctx *ctx, const char *chassis_id,
				 const char *mem_id, struct cxlmi_endpoint **ep_out,
				 struct cxlmi_tunnel_info *ti_buf,
				 struct cxlmi_tunnel_info **ti_out);

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
 * System MemoryDomains Collection - /redfish/v1/Systems/{systemId}/MemoryDomains
 *
 * Returns memory domains visible to this host via active Connections.
 * Each connection to a device creates a virtual domain from the host's view.
 */
json_t *fmd_redfish_system_memory_domains(struct fmd_ctx *ctx, const char *system_id)
{
	struct fmd_host *host;
	struct fmd_connection *conn;
	json_t *members;
	int count = 0;
	char odata_id[128];

	host = fmd_topology_find_host(&ctx->topology, system_id);
	if (!host)
		return NULL;

	snprintf(odata_id, sizeof(odata_id),
		 "/redfish/v1/Systems/%s/MemoryDomains", system_id);

	members = json_array();

	/*
	 * Find all connections for this host. One whose device has been
	 * removed, and that stands while its vPPB is bound, gives the host no
	 * memory: its chunk went with the device.
	 */
	for (conn = ctx->connections; conn; conn = conn->next) {
		if (strcmp(conn->initiator_id, system_id) == 0 &&
		    fmd_topology_find_chunk(&ctx->topology, conn->chunk_id)) {
			json_array_append_new(members,
				rf_link("/redfish/v1/Systems/%s/MemoryDomains/%s",
					system_id, conn->id));
			count++;
		}
	}

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:i,"
		"  s:o"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#MemoryDomainCollection.MemoryDomainCollection",
		"Name", "Host Memory Domains",
		"Members@odata.count", count,
		"Members", members
	);
}

/*
 * System MemoryDomain - /redfish/v1/Systems/{systemId}/MemoryDomains/{domainId}
 *
 * Returns a memory domain visible to the host (derived from a Connection).
 */
json_t *fmd_redfish_system_memory_domain(struct fmd_ctx *ctx,
					 const char *system_id,
					 const char *domain_id)
{
	struct fmd_host *host;
	struct fmd_connection *conn;
	struct fmd_mem_chunk *chunk;
	char odata_id[128];
	char chunks_id[128];

	host = fmd_topology_find_host(&ctx->topology, system_id);
	if (!host)
		return NULL;

	/* domain_id is the connection ID */
	conn = fmd_connection_find(ctx, domain_id);
	if (!conn || strcmp(conn->initiator_id, system_id) != 0)
		return NULL;

	/* Find the connected chunk to get size info */
	chunk = fmd_topology_find_chunk(&ctx->topology, conn->chunk_id);
	if (!chunk)
		return NULL;

	snprintf(odata_id, sizeof(odata_id),
		 "/redfish/v1/Systems/%s/MemoryDomains/%s", system_id, domain_id);
	snprintf(chunks_id, sizeof(chunks_id),
		 "/redfish/v1/Systems/%s/MemoryDomains/%s/MemoryChunks",
		 system_id, domain_id);

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:I,"
		"  s:{s:s}"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#MemoryDomain.v1_5_1.MemoryDomain",
		"Id", domain_id,
		"Name", conn->target_id,
		"MemorySizeMiB", (int64_t)(chunk->size / (1024 * 1024)),
		"MemoryChunks", "@odata.id", chunks_id
	);
}

/*
 * System MemoryChunks Collection - /redfish/v1/Systems/{systemId}/MemoryDomains/{domainId}/MemoryChunks
 */
json_t *fmd_redfish_system_memory_chunks(struct fmd_ctx *ctx,
					 const char *system_id,
					 const char *domain_id)
{
	struct fmd_host *host;
	struct fmd_connection *conn;
	json_t *members;
	char odata_id[128];

	host = fmd_topology_find_host(&ctx->topology, system_id);
	if (!host)
		return NULL;

	conn = fmd_connection_find(ctx, domain_id);
	if (!conn || strcmp(conn->initiator_id, system_id) != 0)
		return NULL;

	snprintf(odata_id, sizeof(odata_id),
		 "/redfish/v1/Systems/%s/MemoryDomains/%s/MemoryChunks",
		 system_id, domain_id);

	/* Single chunk from the connection */
	members = json_array();
	json_array_append_new(members,
		rf_link("/redfish/v1/Systems/%s/MemoryDomains/%s/MemoryChunks/%s",
			system_id, domain_id, conn->chunk_id));

	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:i,"
		"  s:o"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#MemoryChunksCollection.MemoryChunksCollection",
		"Name", "Host Memory Chunks",
		"Members@odata.count", 1,
		"Members", members
	);
}

/*
 * System MemoryChunk - /redfish/v1/Systems/{systemId}/MemoryDomains/{domainId}/MemoryChunks/{chunkId}
 */
json_t *fmd_redfish_system_memory_chunk(struct fmd_ctx *ctx,
					const char *system_id,
					const char *domain_id,
					const char *chunk_id)
{
	struct fmd_host *host;
	struct fmd_connection *conn;
	struct fmd_mem_chunk *chunk;
	char odata_id[192];

	host = fmd_topology_find_host(&ctx->topology, system_id);
	if (!host)
		return NULL;

	conn = fmd_connection_find(ctx, domain_id);
	if (!conn || strcmp(conn->initiator_id, system_id) != 0)
		return NULL;

	/* Verify chunk matches */
	if (strcmp(conn->chunk_id, chunk_id) != 0)
		return NULL;

	chunk = fmd_topology_find_chunk(&ctx->topology, chunk_id);
	if (!chunk)
		return NULL;

	snprintf(odata_id, sizeof(odata_id),
		 "/redfish/v1/Systems/%s/MemoryDomains/%s/MemoryChunks/%s",
		 system_id, domain_id, chunk_id);

	/*
	 * How the host may use it is the Connection's: AccessCapabilities of
	 * its MemoryChunkInfo. MemoryChunks has no property for it.
	 */
	return json_pack(
		"{"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:s,"
		"  s:I,"
		"  s:s"
		"}",
		"@odata.id", odata_id,
		"@odata.type", "#MemoryChunks.v1_6_1.MemoryChunks",
		"Id", chunk_id,
		"Name", chunk_id,
		"MemoryChunkSizeMiB", (int64_t)(chunk->size / (1024 * 1024)),
		"AddressRangeType", chunk->address_range_type
	);
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

static void task_time(time_t t, char *buf, size_t len)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* TaskService - /redfish/v1/TaskService */
json_t *fmd_redfish_task_service(struct fmd_ctx *ctx)
{
	char now[32];

	(void)ctx;
	task_time(time(NULL), now, sizeof(now));

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

/* Task - /redfish/v1/TaskService/Tasks/{id} */
json_t *fmd_redfish_task(struct fmd_ctx *ctx, int id)
{
	struct fmd_task *task = fmd_task_find(ctx, id);
	char uri[64], monitor[64], start[32], conn_uri[256];
	json_t *result;

	if (!task)
		return NULL;

	snprintf(uri, sizeof(uri), "/redfish/v1/TaskService/Tasks/%d", id);
	snprintf(monitor, sizeof(monitor), "/redfish/v1/TaskService/TaskMonitors/%d", id);
	task_time(task->start, start, sizeof(start));
	task_connection_uri(task, conn_uri, sizeof(conn_uri));

	result = json_pack("{s:s, s:s, s:s, s:s, s:s, s:i, s:s, s:s, s:{s:s, s:s}}",
		"@odata.id", uri,
		"@odata.type", "#Task.v1_7_4.Task",
		"Id", uri + strlen("/redfish/v1/TaskService/Tasks/"),
		"Name", task->kind == FMD_TASK_BIND ? "Bind vPPB" : "Unbind vPPB",
		"TaskState", task->state == FMD_TASK_RUNNING ? "Running" :
			     task->state == FMD_TASK_COMPLETED ? "Completed" : "Exception",
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
	if (task->state != FMD_TASK_RUNNING) {
		bool ok = task->state == FMD_TASK_COMPLETED;
		char end[32];

		task_time(task->end, end, sizeof(end));
		json_object_set_new(result, "EndTime", json_string(end));
		json_object_set_new(result, "TaskStatus", json_string(ok ? "OK" : "Critical"));
		json_object_set_new(result, "Messages",
			json_pack("[{s:s, s:s}]",
				  "MessageId", ok ? "Base.1.0.Success" : "Base.1.0.GeneralError",
				  "Message", task->message));
		if (ok && task->kind == FMD_TASK_BIND)
			json_object_set_new(result, "Links",
				json_pack("{s:[{s:s}]}", "CreatedResources",
					  "@odata.id", conn_uri));
	}

	return result;
}

/*
 * Task monitor - /redfish/v1/TaskService/TaskMonitors/{id}
 *
 * Returns the HTTP status to answer with, and for all but 204 a body in
 * @response. @location is the monitor while the task runs, and the
 * connection once a bind has made it.
 */
int fmd_redfish_task_monitor(struct fmd_ctx *ctx, int id, json_t **response,
			     char *location, size_t len)
{
	struct fmd_task *task = fmd_task_find(ctx, id);

	*response = NULL;
	location[0] = '\0';
	if (!task)
		return 404;

	if (task->state == FMD_TASK_RUNNING) {
		snprintf(location, len, "/redfish/v1/TaskService/TaskMonitors/%d", id);
		*response = fmd_redfish_task(ctx, id);
		return 202;
	}

	if (task->state == FMD_TASK_EXCEPTION) {
		*response = json_pack("{s:{s:s, s:s}}", "error",
				      "code", "Base.1.0.GeneralError",
				      "message", task->message);
		return 500;
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
 * Endpoints Collection - /redfish/v1/Fabrics/{fabric_id}/Endpoints
 *
 * Each SLD/MLD/MHD is represented as an Endpoint in the fabric.
 * Hosts are also endpoints (initiators).
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

	/* Add SLDs, MLDs, MHDs, and Hosts as endpoints */
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SLD)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_MLD)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_MHD)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);

	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_HOST)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);

	fmd_topology_rdunlock(&ctx->topology);

	return rf_collection_finish(&coll);
}

/*
 * Endpoint - /redfish/v1/Fabrics/{fabric_id}/Endpoints/{endpoint_id}
 *
 * SLD/MLD endpoints have EntityRole=Target (memory targets).
 * Host endpoints have EntityRole=Initiator (CPUs issuing memory requests).
 * Per OCP CMS Figure 7, endpoints link to switch ports via ConnectedPorts.
 */
json_t *fmd_redfish_endpoint(struct fmd_ctx *ctx, const char *fabric_id,
			     const char *endpoint_id)
{
	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	struct fmd_component *comp = fmd_topology_find(&ctx->topology, endpoint_id);
	if (!comp)
		return NULL;

	/* Must be a memory device (SLD/MLD) or a host */
	if (!fmd_is_memdev(comp) && comp->type != FMD_COMPONENT_HOST)
		return NULL;

	char uri[256];
	snprintf(uri, sizeof(uri), "/redfish/v1/Fabrics/CXL/Endpoints/%s", endpoint_id);

	/*
	 * Host endpoints (Initiators) - represent CPUs that issue CXL.mem requests
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
			"  s:[{s:s, s:s}],"  /* ConnectedEntities */
			"  s:o"    /* Status */
			"}",
			"@odata.id", uri,
			"@odata.type", "#Endpoint.v1_8_2.Endpoint",
			"Id", endpoint_id,
			"Name", host->name,
			"EndpointProtocol", "CXL",
			"ConnectedEntities",
				"EntityType", "Processor",
				"EntityRole", "Initiator",
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
		return result;
	}

	/*
	 * Memory device endpoints (Targets) - SLD/MLD that respond to requests
	 */
	char vendor_str[16];
	char device_str[16];
	const char *name;

	/* Get PCI IDs from device */
	if (comp->type == FMD_COMPONENT_SLD) {
		struct fmd_sld *sld = fmd_to_sld(comp);
		snprintf(vendor_str, sizeof(vendor_str), "0x%04X", sld->identify.vendor_id);
		snprintf(device_str, sizeof(device_str), "0x%04X", sld->identify.device_id);
		name = "CXL SLD Endpoint";
	} else if (comp->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(comp);
		snprintf(vendor_str, sizeof(vendor_str), "0x%04X", mld->identify.vendor_id);
		snprintf(device_str, sizeof(device_str), "0x%04X", mld->identify.device_id);
		name = "CXL MLD Endpoint";
	} else if (comp->type == FMD_COMPONENT_MHD) {
		struct fmd_mhd *mhd = fmd_to_mhd(comp);
		snprintf(vendor_str, sizeof(vendor_str), "0x%04X", mhd->identify.vendor_id);
		snprintf(device_str, sizeof(device_str), "0x%04X", mhd->identify.device_id);
		name = "CXL MHD Endpoint";
	} else {
		return NULL;
	}

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* EndpointProtocol */
		"  s:[{s:s, s:s, s:{s:s, s:s}}],"  /* ConnectedEntities */
		"  s:o"    /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#Endpoint.v1_8_2.Endpoint",
		"Id", endpoint_id,
		"Name", name,
		"EndpointProtocol", "CXL",
		"ConnectedEntities",
			"EntityType", "MemoryChunk",
			"EntityRole", "Target",
			"EntityPciId", "VendorId", vendor_str, "DeviceId", device_str,
		"Status", rf_status("Enabled", "OK")
	);

	/*
	 * Per OCP CMS Figure 7, endpoints connect to switch ports.
	 * If this device is attached to a switch port, add Links.ConnectedPorts
	 * and Links.Zones (the VCS(s) this endpoint is bound to).
	 */
	if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT) {
		struct fmd_port *port = fmd_to_port(comp->parent);
		struct fmd_component *sw_comp = comp->parent->parent;

		if (sw_comp && sw_comp->type == FMD_COMPONENT_SWITCH) {
			struct fmd_switch *sw = fmd_to_switch(sw_comp);
			json_t *links = json_object();
			json_t *connected_ports = json_array();
			json_t *zones = json_array();

			/* Add connected port */
			json_array_append_new(connected_ports,
				rf_link("/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
					sw->base.id, port->base.id));

			/* Find VCSs this endpoint is bound to */
			for (int v = 0; v < sw->num_vcs; v++) {
				struct fmd_vcs *vcs = &sw->vcs[v];
				for (int p = 0; p < vcs->num_vppbs; p++) {
					struct fmd_vppb *vppb = &vcs->vppbs[p];
					if (vppb->port == port &&
					    (vppb->binding_status == FMD_VPPB_BOUND_PHYSICAL_PORT ||
					     vppb->binding_status == FMD_VPPB_BOUND_LD)) {
						json_array_append_new(zones,
							rf_link("/redfish/v1/Fabrics/CXL/Zones/%s_vcs%d",
								sw->base.id, vcs->vcs_id));
						break; /* Only add each VCS once */
					}
				}
			}

			json_object_set_new(links, "ConnectedPorts", connected_ports);
			if (json_array_size(zones) > 0)
				json_object_set_new(links, "Zones", zones);
			else
				json_decref(zones);

			json_object_set_new(result, "Links", links);
		}
	}

	return result;
}

/*
 * Connections Collection - /redfish/v1/Fabrics/{fabric_id}/Connections
 *
 * Per OCP CMS Figure 7, Connections represent bindings between endpoints.
 * Each switch-attached device has a connection to its parent port.
 */
json_t *fmd_redfish_connections_collection(struct fmd_ctx *ctx, const char *fabric_id)
{
	struct rf_collection coll;
	struct fmd_component *comp;
	struct fmd_connection *conn;

	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	rf_collection_init(&coll,
			   rf_uri(0, "/redfish/v1/Fabrics/%s/Connections", fabric_id),
			   "#ConnectionCollection.ConnectionCollection",
			   "Connection Collection");

	fmd_topology_rdlock(&ctx->topology);

	/* Switch-attached devices get implicit connections */
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_SLD) {
		if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT)
			rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Connections/%s", comp->id);
	}
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_MLD) {
		if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT)
			rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Connections/%s", comp->id);
	}
	fmd_for_each_type(&ctx->topology, comp, FMD_COMPONENT_MHD) {
		if (comp->parent && comp->parent->type == FMD_COMPONENT_PORT)
			rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Connections/%s", comp->id);
	}

	fmd_topology_rdunlock(&ctx->topology);

	/* Dynamic connections (host-to-device bindings) */
	for (conn = ctx->connections; conn; conn = conn->next)
		rf_collection_add(&coll, "/redfish/v1/Fabrics/CXL/Connections/%s", conn->id);

	return rf_collection_finish(&coll);
}

/*
 * Connection - /redfish/v1/Fabrics/{fabric_id}/Connections/{connection_id}
 *
 * Per OCP CMS Figure 7, a Connection binds a device endpoint to the fabric.
 * For switch-attached devices, this represents the binding to the switch port.
 * For dynamic connections (POST-created), this represents host-to-device binding.
 */
json_t *fmd_redfish_connection(struct fmd_ctx *ctx, const char *fabric_id,
			       const char *connection_id)
{
	if (strcmp(fabric_id, "CXL") != 0)
		return NULL;

	/* First check for dynamic connection */
	struct fmd_connection *conn = fmd_connection_find(ctx, connection_id);
	if (conn) {
		char uri[256];
		char init_uri[256];
		char target_uri[256];
		char chunk_uri[256];
		json_t *access_caps;

		snprintf(uri, sizeof(uri),
			 "/redfish/v1/Fabrics/CXL/Connections/%s", conn->id);
		snprintf(init_uri, sizeof(init_uri),
			 "/redfish/v1/Fabrics/CXL/Endpoints/%s", conn->initiator_id);
		snprintf(target_uri, sizeof(target_uri),
			 "/redfish/v1/Fabrics/CXL/Endpoints/%s", conn->target_id);

		/* Find chunk to get its full path */
		struct fmd_mem_chunk *chunk = fmd_topology_find_chunk(&ctx->topology, conn->chunk_id);
		if (chunk && chunk->base.parent && chunk->base.parent->parent) {
			snprintf(chunk_uri, sizeof(chunk_uri),
				 "/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s",
				 chunk->base.parent->parent->id,
				 chunk->base.parent->id,
				 chunk->base.id);
		} else {
			snprintf(chunk_uri, sizeof(chunk_uri),
				 "/redfish/v1/Fabrics/CXL/MemoryChunks/%s", conn->chunk_id);
		}

		/* Build access capabilities array */
		access_caps = json_array();
		if (conn->access_caps & FMD_ACCESS_READ)
			json_array_append_new(access_caps, json_string("Read"));
		if (conn->access_caps & FMD_ACCESS_WRITE)
			json_array_append_new(access_caps, json_string("Write"));

		json_t *result = json_pack(
			"{"
			"  s:s,"   /* @odata.id */
			"  s:s,"   /* @odata.type */
			"  s:s,"   /* Id */
			"  s:s,"   /* Name */
			"  s:s,"   /* ConnectionType */
			"  s:o,"   /* Status */
			"  s:{"    /* Links */
			"    s:[{s:s}],"  /* InitiatorEndpoints */
			"    s:[{s:s}]"   /* TargetEndpoints */
			"  },"
			"  s:[{s:{s:s}, s:o}]"  /* MemoryChunkInfo */
			"}",
			"@odata.id", uri,
			"@odata.type", "#Connection.v1_4_0.Connection",
			"Id", conn->id,
			"Name", "CXL Memory Connection",
			"ConnectionType", "Memory",
			"Status", rf_status(conn->pending == FMD_CONN_BINDING ? "Starting" :
				    conn->pending == FMD_CONN_UNBINDING ? "Updating" :
				    "Enabled", "OK"),
			"Links",
				"InitiatorEndpoints", "@odata.id", init_uri,
				"TargetEndpoints", "@odata.id", target_uri,
			"MemoryChunkInfo",
				"MemoryChunk", "@odata.id", chunk_uri,
				"AccessCapabilities", access_caps
		);

		/*
		 * Its device has been removed from the switch port, and the
		 * vPPB is still bound to the port: Absent, "This function or
		 * device is not currently present or detected" (Resource_v1).
		 * Deleting it unbinds the vPPB; the device and its memory are
		 * not there to link to.
		 */
		if (result && !fmd_topology_find(&ctx->topology, conn->target_id)) {
			json_object_set_new(result, "Status", rf_status("Absent", "OK"));
			json_object_del(json_object_get(result, "Links"),
					"TargetEndpoints");
			json_object_del(result, "MemoryChunkInfo");
		}
		return result;
	}

	/* Connection ID matches the device ID (implicit switch-attached) */
	struct fmd_component *comp = fmd_topology_find(&ctx->topology, connection_id);
	if (!comp || !fmd_is_memdev(comp))
		return NULL;

	/* Only switch-attached devices have connections */
	if (!comp->parent || comp->parent->type != FMD_COMPONENT_PORT)
		return NULL;

	struct fmd_port *port = fmd_to_port(comp->parent);
	struct fmd_component *sw = comp->parent->parent;
	if (!sw || sw->type != FMD_COMPONENT_SWITCH)
		return NULL;

	char uri[256];
	char endpoint_uri[256];
	char port_uri[256];

	snprintf(uri, sizeof(uri),
		 "/redfish/v1/Fabrics/CXL/Connections/%s", connection_id);
	snprintf(endpoint_uri, sizeof(endpoint_uri),
		 "/redfish/v1/Fabrics/CXL/Endpoints/%s", comp->id);
	snprintf(port_uri, sizeof(port_uri),
		 "/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
		 sw->id, port->base.id);

	(void)port_uri;  /* Reserved for future use */

	const char *name = (comp->type == FMD_COMPONENT_SLD) ?
		"CXL SLD Connection" : "CXL MLD Connection";

	/*
	 * Build Connection with:
	 * - ConnectionType: Memory (CXL memory device)
	 * - Links.TargetEndpoints: the device endpoint
	 * - MemoryChunkInfo could be added for detailed memory access info
	 */
	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* ConnectionType */
		"  s:o,"   /* Status */
		"  s:{"    /* Links */
		"    s:[{s:s}]"  /* TargetEndpoints */
		"  }"
		"}",
		"@odata.id", uri,
		"@odata.type", "#Connection.v1_4_0.Connection",
		"Id", connection_id,
		"Name", name,
		"ConnectionType", "Memory",
		"Status", rf_status("Enabled", "OK"),
		"Links",
			"TargetEndpoints", "@odata.id", endpoint_uri
	);

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
 * A Zone represents a VCS and contains links to all endpoints (devices)
 * bound to that VCS via vPPBs.
 */
/*
 * The device a bound vPPB reaches: the device on its port, or the
 * Multi-Headed device one of whose heads is. NULL if it is not bound.
 */
static struct fmd_component *zone_vppb_device(const struct fmd_vppb *vppb)
{
	if (vppb->binding_status != FMD_VPPB_BOUND_PHYSICAL_PORT &&
	    vppb->binding_status != FMD_VPPB_BOUND_LD)
		return NULL;
	if (vppb->port && vppb->port->base.first_child)
		return vppb->port->base.first_child;
	if (vppb->port && vppb->port->head)
		return vppb->port->head->base.parent;
	return NULL;
}

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

	/* Add device endpoints from bound vPPBs */
	for (int i = 0; i < vcs->num_vppbs; i++) {
		struct fmd_component *dev = zone_vppb_device(&vcs->vppbs[i]);
		bool listed = false;

		if (!dev || !fmd_is_memdev(dev))
			continue;

		/*
		 * Once: an MLD with two LDs bound to this VCS is one Endpoint,
		 * reached through two vPPBs.
		 */
		for (int j = 0; j < i && !listed; j++)
			listed = (zone_vppb_device(&vcs->vppbs[j]) == dev);
		if (!listed)
			json_array_append_new(endpoints,
				rf_link("/redfish/v1/Fabrics/CXL/Endpoints/%s", dev->id));
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

	/* A head of a Multi-Headed device: the endpoint is the device's */
	if (port->head && port->head->base.parent)
		json_object_set_new(result, "Links",
			rf_links_array_single("AssociatedEndpoints",
				"/redfish/v1/Fabrics/CXL/Endpoints/%s",
				port->head->base.parent->id));

	/* Add link to connected device's Endpoint resource */
	struct fmd_component *dev_child;
	fmd_for_each_child(&port->base, dev_child) {
		if (dev_child->type == FMD_COMPONENT_SLD ||
		    dev_child->type == FMD_COMPONENT_MLD) {
			json_object_set_new(result, "Links",
				rf_links_array_single("AssociatedEndpoints",
					"/redfish/v1/Fabrics/CXL/Endpoints/%s",
					dev_child->id));

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
	json_object_set_new(result, "Links",
		rf_links_single("Port",
			"/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s_usp%d",
			switch_id, switch_id, vcs->upstream_port_id));

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
		if (vppb->port) {
			/* Use resolved port's actual ID */
			json_object_set_new(result, "Links",
				rf_links_single("Port",
					"/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s",
					switch_id, vppb->port->base.id));
		} else {
			/* Fallback: assume DSP if port not resolved */
			json_object_set_new(result, "Links",
				rf_links_single("Port",
					"/redfish/v1/Fabrics/CXL/Switches/%s/Ports/%s_dsp%d",
					switch_id, switch_id, vppb->bound_port_id));
		}
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
	uint8_t worst_health = 0;
	uint8_t worst_media = 0;

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
				if (dev_child->type == FMD_COMPONENT_SLD) {
					struct fmd_sld *sld = fmd_to_sld(dev_child);
					total_capacity_mb += sld->memory_size / (1024ULL * 1024);
					worst_health |= sld->health.health_status;
					worst_media = fmd_worse_media_status(worst_media,
									     sld->health.media_status);
				} else if (dev_child->type == FMD_COMPONENT_MLD) {
					struct fmd_mld *mld = fmd_to_mld(dev_child);
					/* Sum LD capacities for MLD */
					struct fmd_component *ld_child;
					fmd_for_each_child(&mld->base, ld_child) {
						if (ld_child->type == FMD_COMPONENT_LD) {
							struct fmd_ld *ld = fmd_to_ld(ld_child);
							total_capacity_mb += ld->memory_size / (1024ULL * 1024);
							worst_health |= ld->health.health_status;
							worst_media = fmd_worse_media_status(worst_media,
											     ld->health.media_status);
						}
					}
				}
			}
		}
		fmd_topology_rdunlock(&ctx->topology);

		snprintf(description, sizeof(description),
			 "CXL Switch with %d attached device(s), %llu MB aggregate capacity",
			 device_count, (unsigned long long)total_capacity_mb);

		/* Map health status to Redfish */
		const char *health_str = "OK";
		const char *state_str = "Enabled";
		if (worst_health >= 3 || worst_media >= 3) {
			health_str = "Critical";
			state_str = "UnavailableOffline";
		} else if (worst_health >= 1 || worst_media >= 1) {
			health_str = "Warning";
		}

		result = json_pack(
			"{"
			"  s:s,"   /* @odata.id */
			"  s:s,"   /* @odata.type */
			"  s:s,"   /* Id */
			"  s:s,"   /* Name */
			"  s:s,"   /* Description */
			"  s:s,"   /* ChassisType */
			"  s:s,"   /* SerialNumber */
			"  s:{s:s, s:s},"  /* Status */
			"  s:{s:s}"   /* MemoryDomains */
			"}",
			"@odata.id", chassis_uri,
			"@odata.type", "#Chassis.v1_23_0.Chassis",
			"Id", id,
			"Name", name,
			"Description", description,
			"ChassisType", chassis_type,
			"SerialNumber", serial_str,
			"Status", "State", state_str, "Health", health_str,
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
		worst_health = sld->health.health_status;
		worst_media = sld->health.media_status;
	} else if (comp->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(comp);
		name = "CXL MLD Memory Device";
		chassis_type = "Component";
		snprintf(serial_str, sizeof(serial_str), "0x%016llX",
			 (unsigned long long)mld->identify.serial_num);
		snprintf(description, sizeof(description),
			 "CXL Type 3 Multi-Logical Device with %d LDs",
			 mld->ld_count);

		/* Aggregate health from all LDs */
		struct fmd_component *child;
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mld->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				/* Health Status is a bitmask, Media Status is not a scale */
				worst_health |= ld->health.health_status;
				worst_media = fmd_worse_media_status(worst_media,
								     ld->health.media_status);
			}
		}
		fmd_topology_rdunlock(&ctx->topology);
	} else if (comp->type == FMD_COMPONENT_MHD) {
		struct fmd_mhd *mhd = fmd_to_mhd(comp);
		name = "CXL MHD Memory Device";
		chassis_type = "Component";
		snprintf(serial_str, sizeof(serial_str), "0x%016llX",
			 (unsigned long long)mhd->identify.serial_num);
		snprintf(description, sizeof(description),
			 "CXL Type 3 Multi-Headed Device with %d heads, %d LDs",
			 mhd->num_heads, mhd->total_lds);

		/* Aggregate health from all LDs in pool */
		struct fmd_component *child;
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mhd->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				/* Health Status is a bitmask, Media Status is not a scale */
				worst_health |= ld->health.health_status;
				worst_media = fmd_worse_media_status(worst_media,
								     ld->health.media_status);
			}
		}
		fmd_topology_rdunlock(&ctx->topology);
	} else {
		return NULL;
	}

	/* Map health status to Redfish */
	const char *health_str = "OK";
	const char *state_str = "Enabled";

	if (worst_health >= 3 || worst_media >= 3) {
		health_str = "Critical";
		state_str = "UnavailableOffline";
	} else if (worst_health >= 1 || worst_media >= 1) {
		health_str = "Warning";
	}

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
		"  s:{s:s, s:s},"  /* Status */
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
		"Status", "State", state_str, "Health", health_str,
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
	uint8_t worst_health = 0;
	uint8_t worst_media = 0;
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
		worst_health = sld->health.health_status;
		worst_media = sld->health.media_status;
		serial = sld->identify.serial_num;
		dev_name = "CXL Type 3 SLD";
	} else if (comp->type == FMD_COMPONENT_MLD) {
		struct fmd_mld *mld = fmd_to_mld(comp);
		fw_str[0] = '\0';	/* an LD has a firmware revision, the MLD none */
		serial = mld->identify.serial_num;
		dev_name = "CXL Type 3 MLD";

		/* Aggregate health from all LDs */
		struct fmd_component *child;
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mld->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				/* Health Status is a bitmask, Media Status is not a scale */
				worst_health |= ld->health.health_status;
				worst_media = fmd_worse_media_status(worst_media,
								     ld->health.media_status);
			}
		}
		fmd_topology_rdunlock(&ctx->topology);
	} else if (comp->type == FMD_COMPONENT_MHD) {
		struct fmd_mhd *mhd = fmd_to_mhd(comp);
		fw_str[0] = '\0';
		serial = mhd->identify.serial_num;
		dev_name = "CXL Type 3 MHD";

		/* Aggregate health from all LDs in pool */
		struct fmd_component *child;
		fmd_topology_rdlock(&ctx->topology);
		fmd_for_each_child(&mhd->base, child) {
			if (child->type == FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);
				/* Health Status is a bitmask, Media Status is not a scale */
				worst_health |= ld->health.health_status;
				worst_media = fmd_worse_media_status(worst_media,
								     ld->health.media_status);
			}
		}
		fmd_topology_rdunlock(&ctx->topology);
	} else {
		return NULL;
	}

	enum rf_health health = fmd_map_health_status(worst_health);
	enum rf_state state = fmd_map_media_status(worst_media);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:s,"   /* DeviceType */
		"  s:{s:s, s:s},"  /* Status */
		"  s:{s:s}"  /* CXLLogicalDevices */
		"}",
		"@odata.id", uri,
		"@odata.type", "#PCIeDevice.v1_23_0.PCIeDevice",
		"Id", dev_id,
		"Name", dev_name,
		"DeviceType", "SingleFunction",
		"Status", "State", rf_state_to_str(state),
		         "Health", rf_health_to_str(health),
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

	/* CXLDevice/DeviceClass (PCIeDevice v1.23): SLD, MLD, MH_SLD or MH_MLD */
	{
		enum rf_cxl_device_class dev_class = CXL_DEVICE_CLASS_SLD;

		if (comp->type == FMD_COMPONENT_MLD) {
			dev_class = CXL_DEVICE_CLASS_MLD;
		} else if (comp->type == FMD_COMPONENT_MHD) {
			struct fmd_component *child;

			dev_class = CXL_DEVICE_CLASS_MH_SLD;
			fmd_topology_rdlock(&ctx->topology);
			fmd_for_each_child_type(comp, child, FMD_COMPONENT_HEAD) {
				if (fmd_to_head(child)->presentation == FMD_HEAD_MLD) {
					dev_class = CXL_DEVICE_CLASS_MH_MLD;
					break;
				}
			}
			fmd_topology_rdunlock(&ctx->topology);
		}
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
	uint64_t mem_size_mib;
	uint8_t health_status;
	uint8_t media_status;
	uint8_t qos_allocated = 0;
	uint8_t qos_limit = 0;
	uint8_t qos_caps = 0;		/* QoS Telemetry Capabilities */
	uint64_t serial = 0;
	bool has_qos = false;		/* an LD of an MLD has bandwidth fractions */
	json_t *semantics;

	(void)snprintf(uri, sizeof(uri),
		       "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%s",
		       chassis_id, pcie_id, ld_id);

	if (comp->type == FMD_COMPONENT_SLD) {
		struct fmd_sld *sld = fmd_to_sld(comp);

		if (ld_idx != 0)
			return NULL;

		mem_size_mib = sld->memdev.total_capacity * 256;
		health_status = sld->health.health_status;
		media_status = sld->health.media_status;
		qos_caps = sld->memdev.qos_telemetry_caps;
		snprintf(name, sizeof(name), "CXL Logical Device 0");
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
		fmd_topology_rdunlock(&ctx->topology);

		if (!ld)
			return NULL;

		mem_size_mib = ld->memory_size / (1024ULL * 1024);
		health_status = ld->health.health_status;
		media_status = ld->health.media_status;
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
		fmd_topology_rdunlock(&ctx->topology);

		if (!ld)
			return NULL;

		mem_size_mib = ld->memory_size / (1024ULL * 1024);
		health_status = ld->health.health_status;
		media_status = ld->health.media_status;
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

	enum rf_health health = fmd_map_health_status(health_status);
	enum rf_state state = fmd_map_media_status(media_status);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* MemorySizeMiB */
		"  s:o,"   /* SemanticsSupported */
		"  s:{s:s, s:s}"   /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#CXLLogicalDevice.v1_5_0.CXLLogicalDevice",
		"Id", ld_id,
		"Name", name,
		"MemorySizeMiB", (json_int_t)mem_size_mib,
		"SemanticsSupported", semantics,
		"Status", "State", rf_state_to_str(state),
		         "Health", rf_health_to_str(health)
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
	 * - Endpoints: fabric endpoint for this device
	 * - MemoryDomains: memory domain containing this LD
	 * - MemoryChunks: memory chunks for this LD (only if provisioned)
	 */
	char endpoint_uri[256];
	char domain_uri[256];

	snprintf(endpoint_uri, sizeof(endpoint_uri),
		 "/redfish/v1/Fabrics/CXL/Endpoints/%s",
		 chassis_id);
	snprintf(domain_uri, sizeof(domain_uri),
		 "/redfish/v1/Chassis/%s/MemoryDomains/%s_domain0",
		 chassis_id, chassis_id);

	/*
	 * Build MemoryChunks array - only include chunks for this specific LD.
	 * For MLDs, filter by checking if chunk's interleave_sets reference this LD.
	 * For SLDs, include all chunks (only one LD exists).
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
						/* For MLD, check interleave_sets for LD reference */
						for (int i = 0; i < chunk->interleave_set_count; i++) {
							if (strcmp(chunk->interleave_sets[i].memory_id, ld_id) == 0) {
								belongs_to_ld = true;
								break;
							}
						}
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
	uint8_t health_status = 0;
	uint8_t media_status = 0;
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
		health_status = sld->health.health_status;
		media_status = sld->health.media_status;
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
		health_status = target_ld->health.health_status;
		media_status = target_ld->health.media_status;
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
		health_status = target_ld->health.health_status;
		media_status = target_ld->health.media_status;
		snprintf(name, sizeof(name), "CXL Logical Device %d", ld_id);

		/* Get Partition Info (4100h) - values in 256MB CXL units */
		volatile_limit_mib = target_ld->partition.active_vmem * 256;
		staged_volatile_mib = target_ld->partition.next_vmem * 256;
		staged_persistent_mib = target_ld->partition.next_pmem * 256;
	} else {
		return NULL;
	}

	enum rf_health health = fmd_map_health_status(health_status);
	enum rf_state state = fmd_map_media_status(media_status);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* CapacityMiB */
		"  s:s,"   /* MemoryType */
		"  s:s,"   /* MemoryDeviceType */
		"  s:{s:s, s:s},"  /* Status */
		"  s:{s:s}"  /* Metrics */
		"}",
		"@odata.id", uri,
		"@odata.type", "#Memory.v1_17_0.Memory",
		"Id", mem_id,
		"Name", name,
		"CapacityMiB", (json_int_t)capacity_mib,
		"MemoryType", mem_type,
		"MemoryDeviceType", mem_dev_type,
		"Status", "State", rf_state_to_str(state),
		         "Health", rf_health_to_str(health),
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
	 * Security actions on Memory, as DSP0288 1.2.0 Table 1 mapped them
	 * (SetPassphrase, SetMasterPassphrase, DisablePassphrase,
	 * DisableMasterPassphrase, FreezeSecurityState, SecureEraseUnit).
	 * DSP0288 1.3.0 and 1.4.0 map Set Passphrase, Disable Passphrase,
	 * Freeze Security State and Passphrase Secure Erase to actions of
	 * CXLLogicalDevice instead, which cxl-fmd does not serve (a known
	 * gap). Scan Media is on Memory in both.
	 */
	char scan_uri[256], set_pass_uri[256], set_master_uri[256];
	char disable_pass_uri[256], disable_master_uri[256], freeze_uri[256];

	snprintf(scan_uri, sizeof(scan_uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.ScanMedia",
		 chassis_id, mem_id);
	snprintf(set_pass_uri, sizeof(set_pass_uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.SetPassphrase",
		 chassis_id, mem_id);
	snprintf(set_master_uri, sizeof(set_master_uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.SetMasterPassphrase",
		 chassis_id, mem_id);
	snprintf(disable_pass_uri, sizeof(disable_pass_uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.DisablePassphrase",
		 chassis_id, mem_id);
	snprintf(disable_master_uri, sizeof(disable_master_uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.DisableMasterPassphrase",
		 chassis_id, mem_id);
	snprintf(freeze_uri, sizeof(freeze_uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.FreezeSecurityState",
		 chassis_id, mem_id);

	char secure_erase_uri[256];
	snprintf(secure_erase_uri, sizeof(secure_erase_uri),
		 "/redfish/v1/Chassis/%s/Memory/%s/Actions/Memory.SecureEraseUnit",
		 chassis_id, mem_id);

	json_object_set_new(result, "Actions",
		json_pack("{s:{s:s}, s:{s:s}, s:{s:s}, s:{s:s}, s:{s:s}, s:{s:s}, s:{s:s}}",
			  "#Memory.ScanMedia", "target", scan_uri,
			  "#Memory.SetPassphrase", "target", set_pass_uri,
			  "#Memory.SetMasterPassphrase", "target", set_master_uri,
			  "#Memory.DisablePassphrase", "target", disable_pass_uri,
			  "#Memory.DisableMasterPassphrase", "target", disable_master_uri,
			  "#Memory.FreezeSecurityState", "target", freeze_uri,
			  "#Memory.SecureEraseUnit", "target", secure_erase_uri));

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
	uint8_t worst_health = 0;
	uint8_t worst_media = 0;
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

				if (dev_child->type == FMD_COMPONENT_SLD) {
					struct fmd_sld *sld = fmd_to_sld(dev_child);
					capacity_mib += sld->memory_size / (1024ULL * 1024);
					worst_health |= sld->health.health_status;
					worst_media = fmd_worse_media_status(worst_media,
									     sld->health.media_status);
				} else if (dev_child->type == FMD_COMPONENT_MLD) {
					struct fmd_mld *mld = fmd_to_mld(dev_child);
					struct fmd_component *ld_child;
					fmd_for_each_child(&mld->base, ld_child) {
						if (ld_child->type == FMD_COMPONENT_LD) {
							struct fmd_ld *ld = fmd_to_ld(ld_child);
							capacity_mib += ld->memory_size / (1024ULL * 1024);
							worst_health |= ld->health.health_status;
							worst_media = fmd_worse_media_status(worst_media,
											     ld->health.media_status);
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

		/* Get health and granularity from parent device */
		if (chassis->type == FMD_COMPONENT_SLD) {
			struct fmd_sld *sld = fmd_to_sld(chassis);
			worst_health = sld->health.health_status;
			worst_media = sld->health.media_status;
		} else if (chassis->type == FMD_COMPONENT_MLD) {
			struct fmd_mld *mld = fmd_to_mld(chassis);
			struct fmd_component *child;

			/* Memory granularity from Get LD Allocations (DSP0288 Table 2) */
			chunk_increment_mib = mld->memory_granularity * 256;

			fmd_topology_rdlock(&ctx->topology);
			fmd_for_each_child(&mld->base, child) {
				if (child->type == FMD_COMPONENT_LD) {
					struct fmd_ld *ld = fmd_to_ld(child);
					worst_health |= ld->health.health_status;
					worst_media = fmd_worse_media_status(worst_media,
									     ld->health.media_status);
				}
			}
			fmd_topology_rdunlock(&ctx->topology);
		} else if (chassis->type == FMD_COMPONENT_MHD) {
			struct fmd_component *child;

			/* the LDs of the pool, as for the LDs of an MLD */
			fmd_topology_rdlock(&ctx->topology);
			fmd_for_each_child_type(chassis, child, FMD_COMPONENT_LD) {
				struct fmd_ld *ld = fmd_to_ld(child);

				worst_health |= ld->health.health_status;
				worst_media = fmd_worse_media_status(worst_media,
								     ld->health.media_status);
			}
			fmd_topology_rdunlock(&ctx->topology);
		}
	} else {
		return NULL;
	}

	enum rf_health health = fmd_map_health_status(worst_health);
	enum rf_state state = fmd_map_media_status(worst_media);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* MemorySizeMiB */
		"  s:{s:s},"  /* MemoryChunks */
		"  s:{s:s, s:s}"  /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#MemoryDomain.v1_5_1.MemoryDomain",
		"Id", domain_id,
		"Name", domain_name,
		"MemorySizeMiB", (json_int_t)capacity_mib,
		"MemoryChunks", "@odata.id", chunks_uri,
		"Status", "State", rf_state_to_str(state),
		         "Health", rf_health_to_str(health)
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
	const char *address_range_type = "PMEM";
	uint8_t health_status = 0;
	uint8_t media_status = 0;
	const char *chunk_name = "CXL Memory Chunk";
	char ld_link_uri[512] = {0};

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
			health_status = sld->health.health_status;
			media_status = sld->health.media_status;

			if (sld->partition.active_vmem > 0 && sld->partition.active_pmem == 0)
				address_range_type = "Volatile";

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
						health_status = ld->health.health_status;
						media_status = ld->health.media_status;

						if (ld->memdev.volatile_capacity > 0 &&
						    ld->memdev.persistent_capacity == 0)
							address_range_type = "Volatile";

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

		if (size_mib == 0)
			return NULL;

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

		size_mib = chunk->size / (1024ULL * 1024);
		offset_mib = chunk->base_address / (1024ULL * 1024);
		offset_known = chassis->type == FMD_COMPONENT_SLD;
		int ld_idx = 0;

		/* Extract LD index from chunk_id */
		const char *ld_marker = strstr(chunk_id, "_ld");
		if (ld_marker) {
			ld_idx = safe_parse_int(ld_marker + 3);
			if (ld_idx < 0)
				return NULL;
		}

		if (chassis->type == FMD_COMPONENT_SLD) {
			struct fmd_sld *sld = fmd_to_sld(chassis);
			health_status = sld->health.health_status;
			media_status = sld->health.media_status;

			if (sld->partition.active_vmem > 0 && sld->partition.active_pmem == 0)
				address_range_type = "Volatile";
		} else if (chassis->type == FMD_COMPONENT_MLD) {
			struct fmd_mld *mld = fmd_to_mld(chassis);
			struct fmd_component *child;

			fmd_topology_rdlock(&ctx->topology);
			fmd_for_each_child(&mld->base, child) {
				if (child->type == FMD_COMPONENT_LD) {
					struct fmd_ld *ld = fmd_to_ld(child);
					if (ld->ld_id == ld_idx) {
						health_status = ld->health.health_status;
						media_status = ld->health.media_status;

						if (ld->memdev.volatile_capacity > 0 &&
						    ld->memdev.persistent_capacity == 0)
							address_range_type = "Volatile";
						break;
					}
				}
			}
			fmd_topology_rdunlock(&ctx->topology);
		}

		/* Link to this chassis's CXLLogicalDevice */
		snprintf(ld_link_uri, sizeof(ld_link_uri),
			 "/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%d",
			 chassis_id, chassis_id, ld_idx);
	} else {
		return NULL;
	}

	enum rf_health health = fmd_map_health_status(health_status);
	enum rf_state state = fmd_map_media_status(media_status);

	json_t *result = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* MemoryChunkSizeMiB */
		"  s:s,"   /* AddressRangeType */
		"  s:b,"   /* IsMirrorEnabled */
		"  s:{s:s, s:s}"  /* Status */
		"}",
		"@odata.id", uri,
		"@odata.type", "#MemoryChunks.v1_6_2.MemoryChunks",
		"Id", chunk_id,
		"Name", chunk_name,
		"MemoryChunkSizeMiB", (json_int_t)size_mib,
		"AddressRangeType", address_range_type,
		"IsMirrorEnabled", false,
		"Status", "State", rf_state_to_str(state),
		         "Health", rf_health_to_str(health)
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

	/* Add Links.CXLLogicalDevices back-reference */
	if (ld_link_uri[0] != '\0') {
		json_object_set_new(result, "Links",
			rf_links_array_single("CXLLogicalDevices",
				"%s", ld_link_uri));
	}

	return result;
}


/*
 * POST /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks
 *
 * Create a new memory chunk in the specified domain.
 *
 * Request body:
 * {
 *   "MemoryChunkSizeMiB": 4096,        // required
 *   "AddressRangeType": "PMEM",        // optional, default "PMEM"
 *   "AddressRangeOffsetMiB": 0,        // optional, auto-assigned if omitted
 *   "InterleaveSets": [...]            // optional
 * }
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
	json_t *root = NULL;
	json_error_t error;
	uint64_t size_mib, size_bytes;
	uint64_t offset_mib = UINT64_MAX;  /* auto-assign by default */
	uint64_t offset_bytes;
	char addr_type[16] = "PMEM";
	struct fmd_interleave_set interleave_sets[FMD_MAX_INTERLEAVE];
	int interleave_count = 0;
	char uri[512];

	*response = NULL;

	/* Find chassis */
	chassis = fmd_topology_find(&ctx->topology, chassis_id);
	if (!chassis)
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

	/* Extract AddressRangeType (optional) */
	json_t *type_j = json_object_get(root, "AddressRangeType");
	if (type_j && json_is_string(type_j)) {
		const char *type_str = json_string_value(type_j);
		if (strcmp(type_str, "PMEM") == 0 || strcmp(type_str, "Volatile") == 0)
			strncpy(addr_type, type_str, sizeof(addr_type) - 1);
	}

	/* Extract AddressRangeOffsetMiB (optional) */
	json_t *offset_j = json_object_get(root, "AddressRangeOffsetMiB");
	if (offset_j && json_is_integer(offset_j)) {
		offset_mib = (uint64_t)json_integer_value(offset_j);
	}
	offset_bytes = (offset_mib == UINT64_MAX) ? UINT64_MAX : (offset_mib * 1024ULL * 1024);

	/* Extract InterleaveSets (optional) */
	json_t *interleave_j = json_object_get(root, "InterleaveSets");
	if (interleave_j && json_is_array(interleave_j)) {
		size_t idx;
		json_t *set;

		json_array_foreach(interleave_j, idx, set) {
			if (interleave_count >= FMD_MAX_INTERLEAVE)
				break;

			struct fmd_interleave_set *is = &interleave_sets[interleave_count];
			memset(is, 0, sizeof(*is));

			/* Memory reference */
			json_t *mem_j = json_object_get(set, "Memory");
			if (mem_j && json_is_object(mem_j)) {
				json_t *odata_j = json_object_get(mem_j, "@odata.id");
				if (odata_j && json_is_string(odata_j)) {
					strncpy(is->memory_id, json_string_value(odata_j),
						sizeof(is->memory_id) - 1);
				}
			}

			/* MemoryLevel */
			json_t *level_j = json_object_get(set, "MemoryLevel");
			is->memory_level = level_j && json_is_integer(level_j)
				? (int)json_integer_value(level_j) : 1;

			/* OffsetMiB */
			json_t *set_offset_j = json_object_get(set, "OffsetMiB");
			is->offset_mib = set_offset_j && json_is_integer(set_offset_j)
				? (uint64_t)json_integer_value(set_offset_j) : 0;

			/* RegionId */
			json_t *region_j = json_object_get(set, "RegionId");
			if (region_j && json_is_string(region_j)) {
				strncpy(is->region_id, json_string_value(region_j),
					sizeof(is->region_id) - 1);
			}

			/* SizeMiB - size contributed by this interleave set entry */
			json_t *size_mib_j = json_object_get(set, "SizeMiB");
			is->size_mib = size_mib_j && json_is_integer(size_mib_j)
				? (uint64_t)json_integer_value(size_mib_j) : 0;

			interleave_count++;
		}
	}

	/*
	 * Extract MemoryChunkSizeMiB.
	 * If InterleaveSets is provided with SizeMiB, use that instead.
	 */
	json_t *size_j = json_object_get(root, "MemoryChunkSizeMiB");
	if (size_j && json_is_integer(size_j)) {
		size_mib = (uint64_t)json_integer_value(size_j);
	} else if (interleave_count > 0 && interleave_sets[0].size_mib > 0) {
		/* Derive size from InterleaveSets[0].SizeMiB */
		size_mib = interleave_sets[0].size_mib;
	} else {
		/* Neither MemoryChunkSizeMiB nor InterleaveSets.SizeMiB provided */
		json_decref(root);
		return -2;
	}
	size_bytes = size_mib * 1024ULL * 1024;

	json_decref(root);

	/* Check capacity */
	if (size_bytes > domain->remaining)
		return -3;

	/* Create chunk in domain */
	chunk = fmd_chunk_create_in_domain(&ctx->topology, domain,
					   size_bytes, offset_bytes, addr_type,
					   interleave_count > 0 ? interleave_sets : NULL,
					   interleave_count);
	if (!chunk)
		return -4;

	/*
	 * Issue CCI command for hardware mode.
	 * For MLD: resize LD allocation via Set LD Allocations (5402h)
	 * For SLD: adjust partition via Set Partition Info (4101h)
	 */
	if (likely(!fmd_is_mock(ctx))) {
		int rc = 0;

		if (chassis->type == FMD_COMPONENT_MLD) {
			struct fmd_mld *mld = fmd_to_mld(chassis);
			int ld_id = -1;

			/*
			 * Get LD ID from InterleaveSets if provided.
			 * This is the preferred method as it explicitly identifies the target LD.
			 */
			if (chunk->interleave_set_count > 0 &&
			    chunk->interleave_sets[0].memory_id[0] != '\0') {
				ld_id = validate_interleave_set_memory(ctx, chassis_id,
								       chunk->interleave_sets[0].memory_id);
			}

			/* Fallback: parse LD ID from chunk ID (format: {mld_id}_ld{N}_chunk) */
			if (ld_id < 0) {
				const char *ld_str = strstr(chunk->base.id, "_ld");
				if (ld_str)
					ld_id = safe_parse_int(ld_str + 3);
			}

			if (ld_id >= 0) {
				/* Find current LD size and add the new chunk size */
				struct fmd_ld *ld = fmd_topology_find_ld(&ctx->topology,
									 chassis_id, ld_id);
				uint64_t new_size = ld ? ld->memory_size + size_bytes : size_bytes;

				rc = fmd_mld_set_ld_allocation(ctx, mld, ld_id, new_size);
			}
		} else if (chassis->type == FMD_COMPONENT_SLD) {
			/*
			 * Only volatile chunks change the partition: the total
			 * volatile capacity, as staged or else active, grows by
			 * the chunk. Staged for the next reset, not at once.
			 */
			if (strcmp(addr_type, "Volatile") == 0) {
				struct fmd_sld *sld = fmd_to_sld(chassis);
				uint64_t new_volatile = CXL_TO_BYTES(
					fmd_partition_next_volatile(&sld->partition)) +
					size_bytes;
				rc = fmd_sld_set_partition(ctx, sld, new_volatile, false);
			}
		} else if (chassis->type == FMD_COMPONENT_MHD) {
			struct fmd_mhd *mhd = fmd_to_mhd(chassis);
			int pool_ld_id = -1;

			/*
			 * Get pool LD ID from InterleaveSets.
			 * MHD uses pool-wide LD indices, not head-local IDs.
			 */
			if (chunk->interleave_set_count > 0 &&
			    chunk->interleave_sets[0].memory_id[0] != '\0') {
				pool_ld_id = validate_interleave_set_memory(ctx, chassis_id,
									   chunk->interleave_sets[0].memory_id);
			}

			/* Fallback: parse LD ID from chunk ID */
			if (pool_ld_id < 0) {
				const char *ld_str = strstr(chunk->base.id, "_ld");
				if (ld_str)
					pool_ld_id = safe_parse_int(ld_str + 3);
			}

			if (pool_ld_id >= 0) {
				/* Find current LD size and add the new chunk size */
				struct fmd_ld *ld = fmd_topology_find_ld(&ctx->topology,
									 chassis_id, pool_ld_id);
				uint64_t new_size = ld ? ld->memory_size + size_bytes : size_bytes;

				rc = fmd_mhd_set_ld_allocation(ctx, mhd, pool_ld_id, new_size);
			}
		}

		if (rc < 0) {
			/* Rollback topology change on CCI failure */
			fmd_chunk_remove(&ctx->topology, chunk);
			/* a size the device cannot be given */
			if (rc == -EINVAL || rc == -EOPNOTSUPP)
				return -6;
			return -5;  /* CCI command failed */
		}
	}

	/* Build response JSON */
	snprintf(uri, sizeof(uri),
		 "/redfish/v1/Chassis/%s/MemoryDomains/%s/MemoryChunks/%s",
		 chassis_id, domain_id, chunk->base.id);

	*response = json_pack(
		"{"
		"  s:s,"   /* @odata.id */
		"  s:s,"   /* @odata.type */
		"  s:s,"   /* Id */
		"  s:s,"   /* Name */
		"  s:I,"   /* MemoryChunkSizeMiB */
		"  s:I,"   /* AddressRangeOffsetMiB */
		"  s:s"    /* AddressRangeType */
		"}",
		"@odata.id", uri,
		"@odata.type", "#MemoryChunks.v1_6_2.MemoryChunks",
		"Id", chunk->base.id,
		"Name", "CXL Memory Chunk",
		"MemoryChunkSizeMiB", (json_int_t)(chunk->size / (1024ULL * 1024)),
		"AddressRangeOffsetMiB", (json_int_t)(chunk->base_address / (1024ULL * 1024)),
		"AddressRangeType", chunk->address_range_type
	);

	/* Add InterleaveSets if present */
	if (chunk->interleave_set_count > 0) {
		json_t *sets = json_array();
		for (int i = 0; i < chunk->interleave_set_count; i++) {
			struct fmd_interleave_set *is = &chunk->interleave_sets[i];
			json_t *set = json_pack(
				"{s:{s:s}, s:i, s:I, s:s, s:I}",
				"Memory", "@odata.id", is->memory_id,
				"MemoryLevel", is->memory_level,
				"OffsetMiB", (json_int_t)is->offset_mib,
				"RegionId", is->region_id,
				"SizeMiB", (json_int_t)is->size_mib
			);
			json_array_append_new(sets, set);
		}
		json_object_set_new(*response, "InterleaveSets", sets);

		/*
		 * Add Links.CXLLogicalDevices for MLD/MHD chunks.
		 * Derived from InterleaveSets[0].Memory reference.
		 * For MHD, ld_id is the pool index (device-wide).
		 */
		if (chassis->type == FMD_COMPONENT_MLD ||
		    chassis->type == FMD_COMPONENT_MHD) {
			int ld_id = parse_ld_id_from_memory_path(chunk->interleave_sets[0].memory_id);
			if (ld_id >= 0) {
				json_t *links = json_object();
				json_t *cxl_lds = json_array();
				json_array_append_new(cxl_lds,
					rf_link("/redfish/v1/Chassis/%s/PCIeDevices/%s/CXLLogicalDevices/%d",
						chassis_id, chassis_id, ld_id));
				json_object_set_new(links, "CXLLogicalDevices", cxl_lds);
				json_object_set_new(links, "CXLLogicalDevices@odata.count", json_integer(1));
				json_object_set_new(*response, "Links", links);
			}
		}
	}

	return 0;
}


/*
 * DELETE /redfish/v1/Chassis/{id}/MemoryDomains/{dom}/MemoryChunks/{chunk}
 *
 * Remove a memory chunk from the domain.
 *
 * Returns:
 *   0 = success
 *  -1 = chunk not found
 *  -2 = chunk not in specified domain
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
	 * Issue CCI command for hardware mode before removing from topology.
	 * For MLD: decrease LD allocation via Set LD Allocations (5402h)
	 * For SLD: adjust partition via Set Partition Info (4101h)
	 */
	if (likely(!fmd_is_mock(ctx))) {
		int rc = 0;

		if (domain_parent->type == FMD_COMPONENT_MLD) {
			struct fmd_mld *mld = fmd_to_mld(domain_parent);
			int ld_id = -1;
			uint64_t size_to_remove = chunk->size;

			/*
			 * Get LD ID from stored InterleaveSets if available.
			 * Also use stored size_mib if present.
			 */
			if (chunk->interleave_set_count > 0 &&
			    chunk->interleave_sets[0].memory_id[0] != '\0') {
				ld_id = parse_ld_id_from_memory_path(chunk->interleave_sets[0].memory_id);
				if (chunk->interleave_sets[0].size_mib > 0)
					size_to_remove = chunk->interleave_sets[0].size_mib * 1024ULL * 1024;
			}

			/* Fallback: parse LD ID from chunk ID */
			if (ld_id < 0) {
				const char *ld_str = strstr(chunk_id, "_ld");
				if (ld_str)
					ld_id = safe_parse_int(ld_str + 3);
			}

			if (ld_id >= 0) {
				/* Find current LD size and subtract the chunk size */
				struct fmd_ld *ld = fmd_topology_find_ld(&ctx->topology,
									 chassis_id, ld_id);
				uint64_t new_size = 0;
				if (ld && ld->memory_size > size_to_remove)
					new_size = ld->memory_size - size_to_remove;

				rc = fmd_mld_set_ld_allocation(ctx, mld, ld_id, new_size);
			}
		} else if (domain_parent->type == FMD_COMPONENT_SLD) {
			/* Only volatile chunks change the partition */
			if (strcmp(chunk->address_range_type, "Volatile") == 0) {
				struct fmd_sld *sld = fmd_to_sld(domain_parent);
				uint64_t cur = CXL_TO_BYTES(
					fmd_partition_next_volatile(&sld->partition));
				uint64_t new_volatile = 0;
				if (cur > chunk->size)
					new_volatile = cur - chunk->size;
				rc = fmd_sld_set_partition(ctx, sld, new_volatile, false);
			}
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
 * Returns: 0 on success, -1 on error
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

	comp = fmd_topology_find(&ctx->topology, chassis_id);
	if (!comp || !fmd_is_memdev(comp))
		return -1;

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
	int rc = cxlmi_cmd_memdev_scan_media(ep, ti, &req);
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
 * Memory.SetPassphrase action handler (User passphrase)
 *
 * DSP0288 Table 1: Set Passphrase -> Memory.SetPassphrase action
 * CXL command: Set Passphrase (Opcode 4501h) with type=01h (User)
 *
 * Parameters:
 *   Passphrase - current passphrase string
 *   NewPassphrase - new passphrase string
 *
 * Returns: 0 on success, -1 on error
 */
int fmd_redfish_action_set_passphrase(struct fmd_ctx *ctx,
				      const char *chassis_id,
				      const char *mem_id,
				      const char *body,
				      size_t body_size)
{
	json_t *root, *current_j, *new_j;
	json_error_t error;

	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -1;

	/* Get required Passphrase (current) parameter */
	current_j = json_object_get(root, "Passphrase");
	if (!current_j || !json_is_string(current_j)) {
		json_decref(root);
		return -1;
	}
	const char *current_str = json_string_value(current_j);

	/* Get required NewPassphrase parameter */
	new_j = json_object_get(root, "NewPassphrase");
	if (!new_j || !json_is_string(new_j)) {
		json_decref(root);
		return -1;
	}
	const char *new_str = json_string_value(new_j);

	/*
	 * Build CXL request - passphrases are 32 bytes, zero-padded.
	 * The strings belong to the JSON document: copy them before it is freed.
	 */
	struct cxlmi_cmd_memdev_set_passphrase_req req = {0};
	req.passphrase_type = 1;  /* 01h = User */
	strncpy((char *)req.current_passphrase, current_str,
		sizeof(req.current_passphrase));
	strncpy((char *)req.new_passphrase, new_str,
		sizeof(req.new_passphrase));

	json_decref(root);

	if (unlikely(fmd_is_mock(ctx)))
		return 0;

	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info ti_buf, *ti;
	if (memory_endpoint_setup(ctx, chassis_id, mem_id, &ep, &ti_buf, &ti) != 0)
		return -1;

	if (cxlmi_cmd_memdev_set_passphrase(ep, ti, &req) != 0)
		return -1;

	return 0;
}

/*
 * Memory.SetMasterPassphrase action handler (Master passphrase)
 *
 * DSP0288 Table 1: Set Passphrase -> Memory.SetMasterPassphrase action
 * CXL command: Set Passphrase (Opcode 4501h) with type=00h (Master)
 *
 * Parameters:
 *   Passphrase - current passphrase string
 *   NewPassphrase - new passphrase string
 *
 * Returns: 0 on success, -1 on error
 */
int fmd_redfish_action_set_master_passphrase(struct fmd_ctx *ctx,
					     const char *chassis_id,
					     const char *mem_id,
					     const char *body,
					     size_t body_size)
{
	json_t *root, *current_j, *new_j;
	json_error_t error;

	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -1;

	/* Get required Passphrase (current) parameter */
	current_j = json_object_get(root, "Passphrase");
	if (!current_j || !json_is_string(current_j)) {
		json_decref(root);
		return -1;
	}
	const char *current_str = json_string_value(current_j);

	/* Get required NewPassphrase parameter */
	new_j = json_object_get(root, "NewPassphrase");
	if (!new_j || !json_is_string(new_j)) {
		json_decref(root);
		return -1;
	}
	const char *new_str = json_string_value(new_j);

	/*
	 * Build CXL request - passphrases are 32 bytes, zero-padded.
	 * The strings belong to the JSON document: copy them before it is freed.
	 */
	struct cxlmi_cmd_memdev_set_passphrase_req req = {0};
	req.passphrase_type = 0;  /* 00h = Master */
	strncpy((char *)req.current_passphrase, current_str,
		sizeof(req.current_passphrase));
	strncpy((char *)req.new_passphrase, new_str,
		sizeof(req.new_passphrase));

	json_decref(root);

	if (unlikely(fmd_is_mock(ctx)))
		return 0;

	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info ti_buf, *ti;
	if (memory_endpoint_setup(ctx, chassis_id, mem_id, &ep, &ti_buf, &ti) != 0)
		return -1;

	if (cxlmi_cmd_memdev_set_passphrase(ep, ti, &req) != 0)
		return -1;

	return 0;
}

/*
 * Memory.DisablePassphrase action handler (User passphrase)
 *
 * DSP0288 Table 1: Disable Passphrase -> Memory.DisablePassphrase action
 * CXL command: Disable Passphrase (Opcode 4502h) with type=01h (User)
 *
 * Parameters:
 *   Passphrase - current passphrase string
 *
 * Returns: 0 on success, -1 on error
 */
int fmd_redfish_action_disable_passphrase(struct fmd_ctx *ctx,
					  const char *chassis_id,
					  const char *mem_id,
					  const char *body,
					  size_t body_size)
{
	json_t *root, *pass_j;
	json_error_t error;

	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -1;

	/* Get required Passphrase parameter */
	pass_j = json_object_get(root, "Passphrase");
	if (!pass_j || !json_is_string(pass_j)) {
		json_decref(root);
		return -1;
	}
	const char *pass_str = json_string_value(pass_j);

	/*
	 * Build CXL request - passphrase is 32 bytes, zero-padded.
	 * The strings belong to the JSON document: copy them before it is freed.
	 */
	struct cxlmi_cmd_memdev_disable_passphrase_req req = {0};
	req.passphrase_type = 1;  /* 01h = User */
	strncpy((char *)req.passphrase, pass_str, sizeof(req.passphrase));

	json_decref(root);

	if (unlikely(fmd_is_mock(ctx)))
		return 0;

	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info ti_buf, *ti;
	if (memory_endpoint_setup(ctx, chassis_id, mem_id, &ep, &ti_buf, &ti) != 0)
		return -1;

	if (cxlmi_cmd_memdev_disable_passphrase(ep, ti, &req) != 0)
		return -1;

	return 0;
}

/*
 * Memory.DisableMasterPassphrase action handler (Master passphrase)
 *
 * DSP0288 Table 1: Disable Passphrase -> Memory.DisableMasterPassphrase action
 * CXL command: Disable Passphrase (Opcode 4502h) with type=00h (Master)
 *
 * Parameters:
 *   Passphrase - current passphrase string
 *
 * Returns: 0 on success, -1 on error
 */
int fmd_redfish_action_disable_master_passphrase(struct fmd_ctx *ctx,
						 const char *chassis_id,
						 const char *mem_id,
						 const char *body,
						 size_t body_size)
{
	json_t *root, *pass_j;
	json_error_t error;

	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -1;

	/* Get required Passphrase parameter */
	pass_j = json_object_get(root, "Passphrase");
	if (!pass_j || !json_is_string(pass_j)) {
		json_decref(root);
		return -1;
	}
	const char *pass_str = json_string_value(pass_j);

	/*
	 * Build CXL request - passphrase is 32 bytes, zero-padded.
	 * The strings belong to the JSON document: copy them before it is freed.
	 */
	struct cxlmi_cmd_memdev_disable_passphrase_req req = {0};
	req.passphrase_type = 0;  /* 00h = Master */
	strncpy((char *)req.passphrase, pass_str, sizeof(req.passphrase));

	json_decref(root);

	if (unlikely(fmd_is_mock(ctx)))
		return 0;

	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info ti_buf, *ti;
	if (memory_endpoint_setup(ctx, chassis_id, mem_id, &ep, &ti_buf, &ti) != 0)
		return -1;

	if (cxlmi_cmd_memdev_disable_passphrase(ep, ti, &req) != 0)
		return -1;

	return 0;
}

/*
 * Memory.FreezeSecurityState action handler
 *
 * DSP0288 Table 1: Freeze Security State -> Memory.FreezeSecurityState action
 * CXL command: Freeze Security State (Opcode 4504h)
 *
 * No parameters required.
 *
 * Returns: 0 on success, -1 on error
 */
int fmd_redfish_action_freeze_security_state(struct fmd_ctx *ctx,
					     const char *chassis_id,
					     const char *mem_id)
{
	if (unlikely(fmd_is_mock(ctx)))
		return 0;

	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info ti_buf, *ti;
	if (memory_endpoint_setup(ctx, chassis_id, mem_id, &ep, &ti_buf, &ti) != 0)
		return -1;

	if (cxlmi_cmd_memdev_freeze_security_state(ep, ti) != 0)
		return -1;

	return 0;
}

/*
 * Memory.SecureEraseUnit action handler
 *
 * DSP0288 Table 1: Passphrase Secure Erase -> Memory.SecureEraseUnit action
 * CXL command: Passphrase Secure Erase (Opcode 4505h)
 *
 * Parameters:
 *   PassphraseType - "User" or "Master"
 *   Passphrase - current passphrase string
 *   TODO: RegionId - not currently mapped to CXL command
 *
 * Returns: 0 on success, -1 on error
 */
int fmd_redfish_action_secure_erase_unit(struct fmd_ctx *ctx,
					 const char *chassis_id,
					 const char *mem_id,
					 const char *body,
					 size_t body_size)
{
	json_t *root, *type_j, *pass_j;
	json_error_t error;

	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -1;

	/* Get required PassphraseType parameter */
	type_j = json_object_get(root, "PassphraseType");
	if (!type_j || !json_is_string(type_j)) {
		json_decref(root);
		return -1;
	}
	const char *type_str = json_string_value(type_j);
	uint8_t passphrase_type;
	if (strcmp(type_str, "Master") == 0)
		passphrase_type = 0;  /* 00h = Master */
	else if (strcmp(type_str, "User") == 0)
		passphrase_type = 1;  /* 01h = User */
	else {
		json_decref(root);
		return -1;
	}

	/* Get required Passphrase parameter */
	pass_j = json_object_get(root, "Passphrase");
	if (!pass_j || !json_is_string(pass_j)) {
		json_decref(root);
		return -1;
	}
	const char *pass_str = json_string_value(pass_j);

	/* TODO: RegionId parameter - not currently mapped to CXL command */

	/*
	 * Build CXL request - passphrase is 32 bytes, zero-padded.
	 * The strings belong to the JSON document: copy them before it is freed.
	 */
	struct cxlmi_cmd_memdev_passphrase_secure_erase_req req = {0};
	req.passphrase_type = passphrase_type;
	strncpy((char *)req.passphrase, pass_str, sizeof(req.passphrase));

	json_decref(root);

	if (unlikely(fmd_is_mock(ctx)))
		return 0;

	struct cxlmi_endpoint *ep;
	struct cxlmi_tunnel_info ti_buf, *ti;
	if (memory_endpoint_setup(ctx, chassis_id, mem_id, &ep, &ti_buf, &ti) != 0)
		return -1;

	if (cxlmi_cmd_memdev_passphrase_secure_erase(ep, ti, &req) != 0)
		return -1;

	return 0;
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

static uint16_t extract_ld_id_from_chunk(struct fmd_mem_chunk *chunk);

/*
 * The switch port that the target of a connection is reached through, NULL
 * if it is not on a switch. An SLD or an MLD is on a port. A Multi-Headed
 * device has a port per head (CXL r4.0 Section 2.5): the one that matters
 * is that of the head the chunk's LD is mapped to.
 */
static struct fmd_port *connection_port(struct fmd_ctx *ctx,
					struct fmd_component *target,
					struct fmd_mem_chunk *chunk)
{
	struct fmd_component *child;
	struct fmd_ld *ld = NULL;
	uint16_t ld_id;

	if (target->type != FMD_COMPONENT_MHD)
		return (target->parent && target->parent->type == FMD_COMPONENT_PORT) ?
		       fmd_to_port(target->parent) : NULL;

	if (!chunk)
		return NULL;
	ld_id = extract_ld_id_from_chunk(chunk);
	if (ld_id != 0xFFFF)
		ld = fmd_topology_find_ld(&ctx->topology, target->id, ld_id);
	if (!ld || ld->binding_point_id == 0xFF)
		return NULL;	/* mapped to no head */

	fmd_for_each_child_type(target, child, FMD_COMPONENT_HEAD) {
		if (fmd_to_head(child)->head_id == ld->binding_point_id)
			return fmd_to_head(child)->port;
	}
	return NULL;
}

/*
 * Helper: Find an unbound vPPB in a VCS
 * Returns vPPB ID (1+, skipping USP at 0) or -1 if none available
 */
static int find_available_vppb(struct fmd_vcs *vcs)
{
	/* Skip vPPB 0 which is the USP */
	for (int i = 0; i < vcs->num_vppbs; i++) {
		if (vcs->vppbs[i].vppb_id != 0 &&
		    vcs->vppbs[i].binding_status == FMD_VPPB_UNBOUND)
			return vcs->vppbs[i].vppb_id;
	}
	return -1;
}

/*
 * Helper: Extract LD ID from chunk's interleave set memory reference
 * Returns LD ID or 0xFFFF if not found (for SLD)
 */
static uint16_t extract_ld_id_from_chunk(struct fmd_mem_chunk *chunk)
{
	if (chunk->interleave_set_count > 0 &&
	    chunk->interleave_sets[0].memory_id[0] != '\0') {
		/*
		 * memory_id format: "{device}_ld{N}" for MLD
		 * Extract the LD number from the suffix
		 */
		const char *ld_str = strstr(chunk->interleave_sets[0].memory_id, "_ld");
		if (ld_str) {
			int ld_id = safe_parse_int(ld_str + 3);
			if (ld_id >= 0)
				return (uint16_t)ld_id;
		}
	}
	return 0xFFFF;  /* SLD or no interleave info */
}

/*
 * POST /redfish/v1/Fabrics/{fabricId}/Connections
 *
 * Creates a connection binding CXL memory to a host system.
 * For switch-attached hosts, this also performs vPPB binding.
 *
 * Returns:
 *   0 on success (response contains created Connection)
 *  -1 fabric not found
 *  -2 invalid request body
 *  -3 host/initiator not found
 *  -4 target device not found
 *  -5 memory chunk not found
 *  -6 connection allocation failed
 *  -7 vPPB bind failed (the switch refused it, or another error)
 *  -8 cross-head access denied (MHD: host cannot access this LD)
 *  -9 the port or LD is bound to a vPPB of another host's VCS
 * -10 the host and the device are not on the same switch
 * -11 the switch is Busy, answered Retry Required twice, or the vPPB
 *     chosen was bound by the time it was asked: try again
 * -12 the host's VCS has no unbound vPPB
 * -13 the chunk names no LD of the MLD, or one its port does not have
 * FMD_OP_STARTED: the switch binds in the background; *response is the Task
 */
int fmd_redfish_post_connection(struct fmd_ctx *ctx, const char *fabric_id,
				const char *body, size_t body_size,
				json_t **response)
{
	json_t *root;
	json_error_t error;
	char initiator_id[FMD_MAX_ID_LEN] = {0};
	char target_id[FMD_MAX_ID_LEN] = {0};
	char chunk_id[FMD_MAX_ID_LEN] = {0};
	uint8_t access_caps = FMD_ACCESS_READ | FMD_ACCESS_WRITE;

	if (strcmp(fabric_id, "CXL") != 0)
		return -1;

	root = json_loadb(body, body_size, 0, &error);
	if (!root)
		return -2;

	/* Extract InitiatorEndpoints */
	json_t *links = json_object_get(root, "Links");
	if (!links) {
		json_decref(root);
		return -2;
	}

	json_t *init_eps = json_object_get(links, "InitiatorEndpoints");
	if (init_eps && json_is_array(init_eps) && json_array_size(init_eps) > 0) {
		json_t *first = json_array_get(init_eps, 0);
		json_t *odata = json_object_get(first, "@odata.id");
		if (odata && json_is_string(odata)) {
			const char *path = json_string_value(odata);
			const char *last_slash = strrchr(path, '/');
			if (last_slash)
				snprintf(initiator_id, sizeof(initiator_id), "%s", last_slash + 1);
		}
	}

	/* Extract TargetEndpoints */
	json_t *target_eps = json_object_get(links, "TargetEndpoints");
	if (target_eps && json_is_array(target_eps) && json_array_size(target_eps) > 0) {
		json_t *first = json_array_get(target_eps, 0);
		json_t *odata = json_object_get(first, "@odata.id");
		if (odata && json_is_string(odata)) {
			const char *path = json_string_value(odata);
			const char *last_slash = strrchr(path, '/');
			if (last_slash)
				snprintf(target_id, sizeof(target_id), "%s", last_slash + 1);
		}
	}

	/* Extract MemoryChunkInfo */
	json_t *chunk_info = json_object_get(root, "MemoryChunkInfo");
	if (chunk_info && json_is_array(chunk_info) && json_array_size(chunk_info) > 0) {
		json_t *first = json_array_get(chunk_info, 0);

		json_t *chunk_obj = json_object_get(first, "MemoryChunk");
		if (chunk_obj) {
			json_t *odata = json_object_get(chunk_obj, "@odata.id");
			if (odata && json_is_string(odata)) {
				const char *path = json_string_value(odata);
				const char *last_slash = strrchr(path, '/');
				if (last_slash)
					snprintf(chunk_id, sizeof(chunk_id), "%s", last_slash + 1);
			}
		}

		/* Parse AccessCapabilities if present */
		json_t *access = json_object_get(first, "AccessCapabilities");
		if (access && json_is_array(access)) {
			access_caps = 0;
			size_t i;
			json_t *val;
			json_array_foreach(access, i, val) {
				if (json_is_string(val)) {
					const char *cap = json_string_value(val);
					if (strcmp(cap, "Read") == 0)
						access_caps |= FMD_ACCESS_READ;
					else if (strcmp(cap, "Write") == 0)
						access_caps |= FMD_ACCESS_WRITE;
				}
			}
		}
	}

	json_decref(root);

	if (initiator_id[0] == '\0' || target_id[0] == '\0' || chunk_id[0] == '\0')
		return -2;

	/* Validate initiator (host) exists */
	struct fmd_host *host = fmd_topology_find_host(&ctx->topology, initiator_id);
	if (!host)
		return -3;

	/* Validate target (device) exists */
	struct fmd_component *target = fmd_topology_find(&ctx->topology, target_id);
	if (!target || !fmd_is_memdev(target))
		return -4;

	/* Validate chunk exists */
	struct fmd_mem_chunk *chunk = fmd_topology_find_chunk(&ctx->topology, chunk_id);
	if (!chunk)
		return -5;

	/*
	 * For MHD (multi-headed devices), validate cross-head access:
	 * Host can only access pool LDs assigned to its head. When that
	 * head is on a switch port, the host is whichever VCS the port is
	 * bound to, which is decided below.
	 */
	if (target->type == FMD_COMPONENT_MHD &&
	    !connection_port(ctx, target, chunk)) {
		struct fmd_mhd *mhd = fmd_to_mhd(target);
		struct fmd_head *head = fmd_mhd_find_host_head(mhd, initiator_id);

		if (!head) {
			fmd_warn(ctx, "Connection rejected: host %s not attached to any head of %s",
				 initiator_id, target_id);
			return -8;
		}

		uint16_t ld_id = extract_ld_id_from_chunk(chunk);
		if (ld_id != 0xFFFF) {
			struct fmd_ld *ld = fmd_topology_find_ld(&ctx->topology,
								target_id, ld_id);
			if (ld && !fmd_mhd_ld_accessible_from_head(ld, head)) {
				fmd_warn(ctx, "Connection rejected: host %s (head %d) "
					 "cannot access pool LD %d (head %d)",
					 initiator_id, head->head_id,
					 ld_id, ld->binding_point_id);
				return -8;
			}
		}
	}

	/*
	 * An LD of an MLD on a switch port is bound by its LD-ID (Table
	 * 7-35), which is taken from the chunk: a chunk that names no LD
	 * cannot be bound.
	 */
	if (!host->direct_attached && target->type == FMD_COMPONENT_MLD &&
	    connection_port(ctx, target, chunk) &&
	    extract_ld_id_from_chunk(chunk) == 0xFFFF) {
		fmd_warn(ctx, "Connection rejected: chunk %s names no LD of %s",
			 chunk_id, target_id);
		return -13;
	}

	/* Create the connection */
	struct fmd_connection *conn = fmd_connection_create(ctx, initiator_id,
							    target_id, chunk_id,
							    access_caps);
	if (!conn)
		return -6;

	/*
	 * For switch-attached hosts, perform vPPB binding.
	 * This binds the device's port (and LD for MLD) to a vPPB in the host's VCS.
	 */
	if (!host->direct_attached) {
		struct fmd_switch *sw = NULL;
		struct fmd_port *port = NULL;
		uint8_t port_id = 0;
		uint16_t ld_id = 0xFFFF;
		int vppb_id;
		int rc;

		/*
		 * Find the switch and port for the target device: the port
		 * the device is on, or for a Multi-Headed device the port of
		 * the head that the chunk's LD is mapped to.
		 */
		port = connection_port(ctx, target, chunk);
		if (port) {
			port_id = port->port_number;

			if (port->base.parent &&
			    port->base.parent->type == FMD_COMPONENT_SWITCH) {
				sw = fmd_to_switch(port->base.parent);
			}
		}

		if (!sw) {
			fmd_warn(ctx, "Connection %s: host is switch-attached but "
				 "device %s has no switch parent",
				 conn->id, target_id);
			/* Continue without binding - may be direct-attached device */
			goto done;
		}

		/*
		 * A host reaches devices through the switch its root port is
		 * on: its VCS is a VCS of that switch. The device's switch
		 * has a VCS with the same ID, which is another host's.
		 */
		if (strcmp(host->switch_id, sw->base.id) != 0) {
			fmd_warn(ctx, "Connection %s: host %s is on switch %s, "
				 "device %s on switch %s", conn->id, initiator_id,
				 host->switch_id[0] ? host->switch_id : "(none)",
				 target_id, sw->base.id);
			fmd_connection_remove(ctx, conn);
			return -10;
		}

		/* decide on what the switch has now */
		fmd_vcs_refresh(ctx, sw);

		/* The host's VCS, by its VCS ID */
		struct fmd_vcs *vcs = fmd_switch_find_vcs(sw, host->vcs_id);
		struct fmd_vcs *user_vcs = NULL;
		struct fmd_vppb *user;

		if (!vcs) {
			fmd_err(ctx, "Connection %s: host VCS %d invalid for switch %s",
				conn->id, host->vcs_id, sw->base.id);
			fmd_connection_remove(ctx, conn);
			return -7;
		}

		/*
		 * For an MLD, the LD-ID from the chunk's interleave set. The
		 * port of a head of a Multi-Headed device is bound as a whole,
		 * with LD ID FFFFh: that is cxl-fmd's assumption, that the
		 * head presents an SLD. A head may present an MLD instead, and
		 * then its LDs are bound by an LD-ID "from 0 to n-1 relative
		 * to that head" (Section 2.5.1); no command cxl-fmd sends maps
		 * a pool LD to it, and fmd_vppb_bind() refuses FFFFh on an MLD
		 * port.
		 */
		if (target->type == FMD_COMPONENT_MLD)
			ld_id = extract_ld_id_from_chunk(chunk);

		/*
		 * An SLD port "is bound to only one VCS", and "The Fabric
		 * Manager can bind any unused physical port to the unbound
		 * vPPB" (CXL r4.0 Section 7.2.3.1); "LDs within an MLD
		 * component can be bound to a host or unbound" (Section
		 * 7.3.4). That an LD, too, is bound to one vPPB only is
		 * cxl-fmd's reading of that model. If the host's own VCS has it already there is
		 * nothing to bind, and nothing for the connection to unbind
		 * when it goes. If another VCS has it, it is not this host's
		 * to have.
		 */
		user = fmd_switch_bind_target_user(sw, port_id, ld_id, &user_vcs);
		if (user && user_vcs != vcs) {
			fmd_warn(ctx, "Connection %s: port %d%s is bound to VCS %d, "
				 "not to VCS %d of host %s",
				 conn->id, port_id, ld_id != 0xFFFF ? " (LD)" : "",
				 user_vcs->vcs_id, vcs->vcs_id, initiator_id);
			fmd_connection_remove(ctx, conn);
			return -9;
		}
		if (user) {
			conn->vcs_id = vcs->vcs_id;
			conn->vppb_id = user->vppb_id;
			conn->bound = false;
			fmd_info(ctx, "Connection %s: port %d%s is already bound "
				 "to vPPB %d in VCS %d",
				 conn->id, port_id, ld_id != 0xFFFF ? " (LD)" : "",
				 user->vppb_id, vcs->vcs_id);
			goto done;
		}

		/* Find an available vPPB in the host's VCS */
		vppb_id = find_available_vppb(vcs);
		if (vppb_id < 0) {
			fmd_warn(ctx, "Connection %s: no unbound vPPB in VCS %d",
				 conn->id, host->vcs_id);
			fmd_connection_remove(ctx, conn);
			return -12;
		}

		/* Perform the vPPB bind */
		rc = fmd_vppb_bind(ctx, sw, host->vcs_id, (uint8_t)vppb_id,
				   port_id, ld_id);
		if (rc < 0) {
			fmd_err(ctx, "Connection %s: vPPB bind failed: %d",
				conn->id, rc);
			fmd_connection_remove(ctx, conn);
			switch (rc) {
			case -EBUSY:	/* the port or LD is bound to a vPPB */
				return -9;
			case -EAGAIN:	/* Busy, or Retry Required twice */
			case -EALREADY:	/* the vPPB chosen is no longer unbound */
				return -11;
			case -EINVAL:	/* an LD-ID the port does not have */
				return -13;
			default:
				return -7;
			}
		}

		/* Record binding info in connection */
		conn->vcs_id = host->vcs_id;
		conn->vppb_id = (uint8_t)vppb_id;

		/*
		 * The switch has started and not finished: the connection is
		 * not made until it has. The request is answered with the
		 * task that follows the bind.
		 */
		if (rc == FMD_OP_STARTED) {
			struct fmd_task *task;

			conn->bound = false;
			conn->pending = FMD_CONN_BINDING;
			task = fmd_task_start(ctx, FMD_TASK_BIND, sw, conn->vcs_id,
					      conn->vppb_id, conn->id);
			if (!task) {
				fmd_connection_remove(ctx, conn);
				return -6;
			}
			*response = fmd_redfish_task(ctx, task->id);
			return FMD_OP_STARTED;
		}

		conn->bound = true;

		fmd_info(ctx, "Connection %s: bound vPPB %d in VCS %d to port %d%s",
			 conn->id, vppb_id, host->vcs_id, port_id,
			 ld_id != 0xFFFF ? " (MLD)" : "");
	}

	/*
	 * For direct-attached MHD, validate that the host's head can access
	 * the chunk's pool LD. The host connects to a specific head, and
	 * only LDs mapped to that head are accessible.
	 */
	if (host->direct_attached && target->type == FMD_COMPONENT_MHD) {
		struct fmd_mhd *mhd = fmd_to_mhd(target);
		struct fmd_head *head = fmd_mhd_find_host_head(mhd, initiator_id);
		uint16_t pool_ld_id = extract_ld_id_from_chunk(chunk);

		if (!head) {
			fmd_err(ctx, "Connection %s: host %s not attached to any head of MHD %s",
				conn->id, initiator_id, target_id);
			fmd_connection_remove(ctx, conn);
			return -8;
		}

		if (pool_ld_id != 0xFFFF) {
			/* Find the pool LD and validate accessibility */
			struct fmd_ld *ld = fmd_topology_find_ld(&ctx->topology,
								target_id, pool_ld_id);
			if (!ld) {
				fmd_err(ctx, "Connection %s: pool LD %d not found in MHD %s",
					conn->id, pool_ld_id, target_id);
				fmd_connection_remove(ctx, conn);
				return -8;
			}

			if (!fmd_mhd_ld_accessible_from_head(ld, head)) {
				fmd_err(ctx, "Connection %s: pool LD %d not accessible from "
					"head %d (host %s)",
					conn->id, pool_ld_id, head->head_id, initiator_id);
				fmd_connection_remove(ctx, conn);
				return -8;
			}

			fmd_info(ctx, "Connection %s: MHD pool LD %d accessible via head %d "
				 "(visible as LD %d)",
				 conn->id, pool_ld_id, head->head_id, ld->visible_ld_id);
		}
	}

done:
	fmd_info(ctx, "Created connection %s: %s -> %s (chunk: %s)",
		 conn->id, initiator_id, target_id, chunk_id);

	*response = fmd_redfish_connection(ctx, fabric_id, conn->id);
	return 0;
}


/*
 * DELETE /redfish/v1/Fabrics/{fabricId}/Connections/{connectionId}
 *
 * Removes a connection and unbinds the vPPB if bound.
 *
 * Returns:
 *   0 on success
 *  -1 connection not found
 *  -2 vPPB unbind failed
 */
int fmd_redfish_delete_connection(struct fmd_ctx *ctx, const char *fabric_id,
				  const char *connection_id, json_t **response)
{
	*response = NULL;

	if (strcmp(fabric_id, "CXL") != 0)
		return -1;

	struct fmd_connection *conn = fmd_connection_find(ctx, connection_id);
	if (!conn)
		return -1;

	/* the switch is still working on it: wait for that to end */
	if (conn->pending != FMD_CONN_SETTLED)
		return -3;

	fmd_info(ctx, "Deleting connection %s: %s -> %s",
		 conn->id, conn->initiator_id, conn->target_id);

	/*
	 * If the connection has an active vPPB binding, unbind it first.
	 * We need to find the switch to issue the unbind command.
	 */
	if (conn->bound) {
		struct fmd_host *host = fmd_topology_find_host(&ctx->topology,
							       conn->initiator_id);
		struct fmd_component *target = fmd_topology_find(&ctx->topology,
								 conn->target_id);
		struct fmd_switch *sw = NULL;

		struct fmd_port *port = (host && target) ?
			connection_port(ctx, target,
				fmd_topology_find_chunk(&ctx->topology, conn->chunk_id)) : NULL;

		if (port && port->base.parent &&
		    port->base.parent->type == FMD_COMPONENT_SWITCH)
			sw = fmd_to_switch(port->base.parent);

		/*
		 * Its device has been removed, and the vPPB is still bound
		 * to the port it was on: the host's switch is where to
		 * unbind it.
		 */
		if (!target && host && host->switch_id[0])
			sw = fmd_topology_find_switch(&ctx->topology, host->switch_id);

		if (sw) {
			int rc;

			fmd_info(ctx, "Connection %s: unbinding vPPB %d in VCS %d",
				 conn->id, conn->vppb_id, conn->vcs_id);

			rc = fmd_vppb_unbind(ctx, sw, conn->vcs_id,
					     conn->vppb_id,
					     FMD_UNBIND_MANAGED_HOT_REMOVE);
			if (rc == -EAGAIN)
				return -3;
			if (rc < 0 && rc != -ENOENT) {
				fmd_err(ctx, "Connection %s: vPPB unbind failed: %d",
					conn->id, rc);
				return -2;
			}

			/*
			 * Started and not finished: the connection
			 * stands until the vPPB is unbound.
			 */
			if (rc == FMD_OP_STARTED) {
				struct fmd_task *task;

				conn->pending = FMD_CONN_UNBINDING;
				task = fmd_task_start(ctx, FMD_TASK_UNBIND, sw,
						      conn->vcs_id, conn->vppb_id,
						      conn->id);
				if (!task)
					return -2;
				*response = fmd_redfish_task(ctx, task->id);
				return FMD_OP_STARTED;
			}
		}
	}

	return fmd_connection_remove(ctx, conn);
}
