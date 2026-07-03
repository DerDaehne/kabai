/*
 * JSON-Schema builders and tool-parameter getters for MCP tools.
 *
 * schema_* helpers build an inputSchema object and track "required" in one
 * place. param_* getters only report presence/type — error messages stay in
 * the handlers so existing wording is preserved verbatim.
 */

#ifndef MCP_SCHEMA_H
#define MCP_SCHEMA_H

#include <stdbool.h>
#include <cjson/cJSON.h>

/* {type:"object", properties:{}, required:[]} — an empty required array is
 * stripped when the schema is registered (see mcp_registry_add). */
cJSON *schema_new(void);

void schema_num (cJSON *s, const char *name, const char *desc, bool required);
void schema_str (cJSON *s, const char *name, const char *desc, bool required);
void schema_bool(cJSON *s, const char *name, const char *desc, bool required);
void schema_num_array(cJSON *s, const char *name, const char *desc, bool required);

/* false / NULL when the parameter is absent or has the wrong type. */
bool        param_num (cJSON *params, const char *name, int *out);
const char *param_str (cJSON *params, const char *name);
bool        param_bool(cJSON *params, const char *name, bool dflt);
/* Present with explicit JSON null (e.g. update_ticket description:null). */
bool        param_is_null(cJSON *params, const char *name);
/* Present at all, regardless of type. */
bool        param_present(cJSON *params, const char *name);

#endif /* MCP_SCHEMA_H */
