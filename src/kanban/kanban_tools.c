#include "kanban/kanban_tools.h"
#include "mcp/schema.h"
#include "kanban/projects.h"

/* ============================================================================
 * Projects
 * ============================================================================ */

static cJSON *tool_get_project(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id;
    if (!param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    Project *p = project_get_by_id(ctx->db, project_id);
    if (!p) return mcp_tool_err(id, "Project not found");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", p->id);
    cJSON_AddStringToObject(r, "slug", p->slug);
    cJSON_AddStringToObject(r, "name", p->name);
    if (p->description) cJSON_AddStringToObject(r, "description", p->description);
    project_free(p);
    return mcp_tool_ok(id, r);
}


void kanban_register_tools(McpRegistry *r) {
    cJSON *s;

    s = schema_new();
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kb.ai_get_project", "Get project details", s, tool_get_project);
}
