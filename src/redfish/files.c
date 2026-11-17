/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * CXL Fabric Manager Daemon - cxl-fmd's own registry files
 *
 * "/redfish/v1/Registries/<RegistryFile>: Local copy of a Redfish registry
 * file, where <RegistryFile> is the file name of the local registry file"
 * (DSP0266 1.25.0 Table 4, Redfish-defined URIs; not required). Each is
 * found from /redfish/v1/Registries, a MessageRegistryFileCollection, and
 * its MessageRegistryFile, whose Location[].Uri is that URI. The files are
 * compiled in from schema/oem (scripts/embed_file.py): nothing is read
 * from disk.
 */

#include "handlers_internal.h"

extern const char fmd_file_cxl_health_registry[];
extern const size_t fmd_file_cxl_health_registry_len;

struct served_registry {
	const char *id;			/* the MessageRegistryFile's Id */
	const char *registry;		/* <RegistryName>.<Major>.<Minor> */
	const char *file;		/* <RegistryName>.<Major>.<Minor>.<Errata>.json */
	const char *data;
	const size_t *len;
};

static const struct served_registry registries[] = {
	{ "CxlHealth", "CxlHealth.1.0", "CxlHealth.1.0.0.json",
	  fmd_file_cxl_health_registry, &fmd_file_cxl_health_registry_len },
};

#define N_REGISTRIES (sizeof(registries) / sizeof(registries[0]))

const char *fmd_redfish_registry_file(const char *file, size_t *len)
{
	for (size_t i = 0; i < N_REGISTRIES; i++) {
		if (!strcmp(file, registries[i].file)) {
			*len = *registries[i].len;
			return registries[i].data;
		}
	}
	return NULL;
}

/* GET /redfish/v1/Registries */
json_t *fmd_redfish_registries(struct fmd_ctx *ctx)
{
	struct rf_collection coll;

	(void)ctx;
	rf_collection_init(&coll, "/redfish/v1/Registries",
			   "#MessageRegistryFileCollection.MessageRegistryFileCollection",
			   "Registry File Collection");
	for (size_t i = 0; i < N_REGISTRIES; i++)
		rf_collection_add(&coll, "/redfish/v1/Registries/%s", registries[i].id);
	return rf_collection_finish(&coll);
}

/* GET /redfish/v1/Registries/{id} */
json_t *fmd_redfish_registry(struct fmd_ctx *ctx, const char *id)
{
	(void)ctx;
	for (size_t i = 0; i < N_REGISTRIES; i++) {
		const struct served_registry *r = &registries[i];

		if (strcmp(id, r->id))
			continue;
		return json_pack("{s:s, s:s, s:s, s:s, s:s, s:[s], s:s,"
				 " s:[{s:s, s:s}]}",
			"@odata.id", rf_uri(0, "/redfish/v1/Registries/%s", r->id),
			"@odata.type", "#MessageRegistryFile.v1_1_5.MessageRegistryFile",
			"Id", r->id,
			"Name", "cxl-fmd Message Registry File",
			"Description", "A message registry of cxl-fmd",
			"Languages", "en",
			"Registry", r->registry,
			"Location", "Language", "en",
				"Uri", rf_uri(1, "/redfish/v1/Registries/%s", r->file));
	}
	return NULL;
}
