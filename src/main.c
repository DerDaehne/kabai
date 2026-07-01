/*
 * kb.ai MCP Server
 *
 * Implements the Model Context Protocol (MCP) over STDIO using JSON-RPC 2.0.
 * Exposes Kanban operations as MCP tools backed by a PostgreSQL database.
 *
 * Protocol: https://spec.modelcontextprotocol.io/
 * Supported methods: initialize, notifications/initialized, tools/list, tools/call
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "db/connection.h"
#include "kanban/projects.h"
#include "kanban/tickets.h"
#include "kanban/comments.h"
#include "kanban/board_statuses.h"

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_SERVER_VERSION   "0.5.0"
#define MCP_SERVER_NAME      "kb.ai"

#define DEFAULT_DB_HOST     "localhost"
#define DEFAULT_DB_PORT     "5432"
#define DEFAULT_DB_NAME     "kb_ai"
#define DEFAULT_DB_USER     "postgres"
#define DEFAULT_DB_PASSWORD ""

static DatabaseConnection *global_db    = NULL;
static const char         *g_agent_name = NULL;
static const char         *g_agent_model = NULL;


/* ============================================================================
 * JSON-RPC 2.0 Protocol Helpers
 *
 * id is always borrowed (we duplicate it into the response).
 * result/error objects are always consumed (ownership transferred).
 * ============================================================================ */

static cJSON *jsonrpc_result(cJSON *id, cJSON *result) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON *jsonrpc_error(cJSON *id, int code, const char *message) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    return resp;
}


/* ============================================================================
 * MCP Tool Result Helpers
 *
 * MCP wraps tool output in content blocks. Errors are signalled via isError,
 * not via the JSON-RPC error field (which is reserved for protocol errors).
 * ============================================================================ */

/* Success: serialise data as JSON into a text content block. Consumes data. */
static cJSON *mcp_tool_ok(cJSON *id, cJSON *data) {
    char *text = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);

    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "{}");
    if (text) cJSON_free(text);

    cJSON *content = cJSON_CreateArray();
    cJSON_AddItemToArray(content, item);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", 0);

    return jsonrpc_result(id, result);
}

/* Error: message in a text content block with isError:true. */
static cJSON *mcp_tool_err(cJSON *id, const char *message) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", message ? message : "Unknown error");

    cJSON *content = cJSON_CreateArray();
    cJSON_AddItemToArray(content, item);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", 1);

    return jsonrpc_result(id, result);
}


/* ============================================================================
 * MCP Method: initialize
 * ============================================================================ */

static cJSON *handle_initialize(cJSON *id, cJSON *params) {
    (void)params;

    cJSON *tools_cap = cJSON_CreateObject();

    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", tools_cap);

    cJSON *server_info = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info, "name", MCP_SERVER_NAME);
    cJSON_AddStringToObject(server_info, "version", MCP_SERVER_VERSION);
    if (g_agent_name)  cJSON_AddStringToObject(server_info, "agentName",  g_agent_name);
    if (g_agent_model) cJSON_AddStringToObject(server_info, "agentModel", g_agent_model);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON_AddItemToObject(result, "serverInfo", server_info);

    return jsonrpc_result(id, result);
}


/* ============================================================================
 * MCP Tools: Board Statuses & Workflow
 * ============================================================================ */

static cJSON *tool_list_board_statuses(cJSON *id, cJSON *params) {
    cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    if (!cJSON_IsNumber(proj_j))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    BoardStatus **statuses = board_status_list_by_project(global_db, (int)proj_j->valueint);
    cJSON *arr = cJSON_CreateArray();
    if (!statuses) return mcp_tool_ok(id, arr);

    for (int i = 0; statuses[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", statuses[i]->id);
        cJSON_AddNumberToObject(o, "project_id", statuses[i]->project_id);
        cJSON_AddStringToObject(o, "name", statuses[i]->name);
        cJSON_AddStringToObject(o, "display_name", statuses[i]->display_name);
        cJSON_AddNumberToObject(o, "position", statuses[i]->position);
        if (statuses[i]->agent_role_instruction)
            cJSON_AddStringToObject(o, "agent_role_instruction",
                                    statuses[i]->agent_role_instruction);
        if (statuses[i]->special_type)
            cJSON_AddStringToObject(o, "special_type", statuses[i]->special_type);
        cJSON_AddItemToArray(arr, o);
    }
    board_status_free_array(statuses);
    return mcp_tool_ok(id, arr);
}

static cJSON *tool_create_board_status(cJSON *id, cJSON *params) {
    cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    cJSON *name_j = cJSON_GetObjectItemCaseSensitive(params, "name");
    cJSON *disp_j = cJSON_GetObjectItemCaseSensitive(params, "display_name");
    cJSON *pos_j  = cJSON_GetObjectItemCaseSensitive(params, "position");
    cJSON *ari_j  = cJSON_GetObjectItemCaseSensitive(params, "agent_role_instruction");

    if (!cJSON_IsNumber(proj_j) || !cJSON_IsString(name_j) ||
        !cJSON_IsString(disp_j) || !cJSON_IsNumber(pos_j))
        return mcp_tool_err(id,
            "Missing required parameters: project_id, name, display_name, position");

    BoardStatus *bs = board_status_create(
        global_db,
        (int)proj_j->valueint,
        name_j->valuestring,
        disp_j->valuestring,
        (int)pos_j->valueint,
        cJSON_IsString(ari_j) ? ari_j->valuestring : NULL,
        NULL  /* special_type not exposed via MCP — managed internally */
    );
    if (!bs) return mcp_tool_err(id, "Failed to create board status");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", bs->id);
    cJSON_AddNumberToObject(r, "project_id", bs->project_id);
    cJSON_AddStringToObject(r, "name", bs->name);
    cJSON_AddStringToObject(r, "display_name", bs->display_name);
    cJSON_AddNumberToObject(r, "position", bs->position);
    if (bs->agent_role_instruction)
        cJSON_AddStringToObject(r, "agent_role_instruction", bs->agent_role_instruction);
    board_status_free(bs);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_create_status_transition(cJSON *id, cJSON *params) {
    cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    cJSON *from_j = cJSON_GetObjectItemCaseSensitive(params, "from_status_id");
    cJSON *to_j   = cJSON_GetObjectItemCaseSensitive(params, "to_status_id");

    if (!cJSON_IsNumber(proj_j) || !cJSON_IsNumber(from_j) || !cJSON_IsNumber(to_j))
        return mcp_tool_err(id,
            "Missing required parameters: project_id, from_status_id, to_status_id");

    if (!status_transition_create(global_db, (int)proj_j->valueint,
                                  (int)from_j->valueint, (int)to_j->valueint))
        return mcp_tool_err(id, "Failed to create status transition");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_list_status_transitions(cJSON *id, cJSON *params) {
    cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    if (!cJSON_IsNumber(proj_j))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    StatusTransition **trans = status_transition_list_by_project(
        global_db, (int)proj_j->valueint);

    cJSON *arr = cJSON_CreateArray();
    if (!trans) return mcp_tool_ok(id, arr);

    for (int i = 0; trans[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "from_status_id", trans[i]->from_status_id);
        cJSON_AddNumberToObject(o, "to_status_id",   trans[i]->to_status_id);
        cJSON_AddItemToArray(arr, o);
    }
    status_transition_free_array(trans);
    return mcp_tool_ok(id, arr);
}


/* ============================================================================
 * MCP Method: tools/list
 *
 * Each tool descriptor includes a JSON Schema for its inputSchema, enabling
 * MCP clients to validate and present arguments without a round-trip.
 * ============================================================================ */

static cJSON *prop_str(const char *description) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

static cJSON *prop_num(const char *description) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "number");
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

static cJSON *make_schema(cJSON *properties, const char *const *required) {
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON_AddItemToObject(s, "properties", properties);
    if (required && required[0]) {
        cJSON *req_arr = cJSON_CreateArray();
        for (int i = 0; required[i]; i++)
            cJSON_AddItemToArray(req_arr, cJSON_CreateString(required[i]));
        cJSON_AddItemToObject(s, "required", req_arr);
    }
    return s;
}

static cJSON *make_tool(const char *name, const char *description, cJSON *schema) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", description);
    cJSON_AddItemToObject(t, "inputSchema", schema);
    return t;
}

static cJSON *handle_tools_list(cJSON *id) {
    cJSON *tools = cJSON_CreateArray();
    cJSON *props;
    const char *req[8];

    /* ---- Projects ---- */

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "slug",        prop_str("Short unique identifier (e.g. 'robot-game')"));
    cJSON_AddItemToObject(props, "name",        prop_str("Human-readable display name"));
    cJSON_AddItemToObject(props, "description", prop_str("Optional project description"));
    req[0] = "slug"; req[1] = "name"; req[2] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_create_project",
        "Create a new project/board", make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToArray(tools, make_tool("kb.ai_list_projects",
        "List all projects", make_schema(props, NULL)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "project_id", prop_num("Numeric project ID"));
    req[0] = "project_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_get_project",
        "Get project details", make_schema(props, req)));

    /* ---- Tickets ---- */

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "project_id",  prop_num("ID of the project this ticket belongs to"));
    cJSON_AddItemToObject(props, "status_id",   prop_num("ID of the initial board column/status"));
    cJSON_AddItemToObject(props, "title",        prop_str("Ticket title"));
    cJSON_AddItemToObject(props, "description",  prop_str("Optional detailed description"));
    cJSON_AddItemToObject(props, "type",         prop_str("'ticket' (default) or 'epic'"));
    req[0] = "project_id"; req[1] = "status_id"; req[2] = "title"; req[3] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_create_ticket",
        "Create a new ticket or epic in a project. "
        "Use type='epic' for high-level goals that group child tickets via link_tickets(parent_of).",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "project_id", prop_num("ID of the project to list tickets from"));
    cJSON_AddItemToObject(props, "status_id",  prop_num("Filter by status column (optional)"));
    cJSON_AddItemToObject(props, "type",       prop_str("Filter by type: 'ticket' or 'epic' (optional)"));
    cJSON_AddItemToObject(props, "limit",      prop_num("Max tickets to return (optional, default unlimited)"));
    cJSON_AddItemToObject(props, "offset",     prop_num("Tickets to skip for pagination (optional, default 0)"));
    {
        cJSON *bp = cJSON_CreateObject();
        cJSON_AddStringToObject(bp, "type", "boolean");
        cJSON_AddStringToObject(bp, "description",
            "If true, omit description fields — returns only id/status_id/title/assignee/timestamps. "
            "Use for an overview when descriptions are not needed.");
        cJSON_AddItemToObject(props, "summary", bp);
    }
    req[0] = "project_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_list_tickets",
        "List tickets in a project. Supports status filter, pagination (limit/offset), "
        "and summary mode (omits description). Use summary:true + status_id for cheap overview calls.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "project_id", prop_num("ID of the project to search in"));
    cJSON_AddItemToObject(props, "query",      prop_str("Search string matched case-insensitively against title and description"));
    req[0] = "project_id"; req[1] = "query"; req[2] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_search_tickets",
        "Search tickets by title/description substring (ILIKE). Returns up to 50 matches. "
        "Use before create_ticket to detect duplicates.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id", prop_num("Numeric ticket ID"));
    req[0] = "ticket_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_get_ticket",
        "Get basic ticket information including timestamps", make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id", prop_num("Numeric ticket ID"));
    {
        cJSON *bp = cJSON_CreateObject();
        cJSON_AddStringToObject(bp, "type", "boolean");
        cJSON_AddStringToObject(bp, "description",
            "Include agent_role_instruction in response (default true). "
            "Pass false after the first call to avoid repeating the same instruction for tickets in the same column.");
        cJSON_AddItemToObject(props, "include_role_instruction", bp);
    }
    req[0] = "ticket_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_get_ticket_detailed",
        "Get ticket with all tasks (acceptance criteria), work log, and timestamps. "
        "Use this before starting work. Pass include_role_instruction:false on subsequent "
        "calls within the same session to avoid redundant context.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id",     prop_num("Numeric ticket ID"));
    cJSON_AddItemToObject(props, "new_status_id", prop_num(
        "Target column/status ID. Must be an allowed transition per workflow graph."));
    req[0] = "ticket_id"; req[1] = "new_status_id"; req[2] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_move_ticket",
        "Move a ticket to a new column. Rejected by the database if the transition "
        "is not in the workflow graph or if acceptance criteria are unmet.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    {
        cJSON *arr_schema = cJSON_CreateObject();
        cJSON_AddStringToObject(arr_schema, "type", "array");
        cJSON *items = cJSON_CreateObject();
        cJSON_AddStringToObject(items, "type", "number");
        cJSON_AddItemToObject(arr_schema, "items", items);
        cJSON_AddStringToObject(arr_schema, "description", "Array of ticket IDs to move");
        cJSON_AddItemToObject(props, "ticket_ids", arr_schema);
    }
    cJSON_AddItemToObject(props, "new_status_id", prop_num("Target status ID for all tickets"));
    req[0] = "ticket_ids"; req[1] = "new_status_id"; req[2] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_move_tickets",
        "Batch move multiple tickets to the same new status in one call. "
        "Returns per-ticket success/error breakdown.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id", prop_num("Numeric ticket ID"));
    cJSON_AddItemToObject(props, "assignee",  prop_str(
        "Agent or user identifier. If omitted, falls back to KB_AI_AGENT_NAME env var."));
    req[0] = "ticket_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_assign_ticket",
        "Assign a ticket to an agent or user. Uses KB_AI_AGENT_NAME as default assignee "
        "and always writes KB_AI_AGENT_MODEL to the model field. "
        "Both KB_AI_AGENT_NAME and KB_AI_AGENT_MODEL must be set in the MCP server environment.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id",   prop_num("Numeric ticket ID"));
    cJSON_AddItemToObject(props, "title",        prop_str("New title (optional)"));
    cJSON_AddItemToObject(props, "description",  prop_str("New description, or null to clear (optional)"));
    req[0] = "ticket_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_update_ticket",
        "Edit a ticket's title and/or description", make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id", prop_num("Numeric ticket ID"));
    cJSON_AddItemToObject(props, "reason",    prop_str(
        "Required reason for deletion (e.g. 'duplicate of #42', 'created by mistake')"));
    req[0] = "ticket_id"; req[1] = "reason"; req[2] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_delete_ticket",
        "Permanently delete a ticket (cascades tasks, comments, documents). "
        "Requires a non-empty reason. Use merge_into comment on the surviving ticket before deleting duplicates.",
        make_schema(props, req)));

    /* ---- Relations ---- */

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "from_ticket_id", prop_num("Source ticket ID"));
    cJSON_AddItemToObject(props, "to_ticket_id",   prop_num("Target ticket ID"));
    cJSON_AddItemToObject(props, "relation_type",  prop_str(
        "One of: parent_of (epic→child), blocks (from blocks to), "
        "duplicate_of (from is duplicate of to), relates_to (generic)"));
    req[0] = "from_ticket_id"; req[1] = "to_ticket_id"; req[2] = "relation_type"; req[3] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_link_tickets",
        "Create a directed relation between two tickets. "
        "Use parent_of to link an epic to its child tickets. "
        "Relations are visible in get_ticket_detailed as the 'relations' array.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "from_ticket_id", prop_num("Source ticket ID"));
    cJSON_AddItemToObject(props, "to_ticket_id",   prop_num("Target ticket ID"));
    cJSON_AddItemToObject(props, "relation_type",  prop_str(
        "The relation to remove (must match exactly what was created)"));
    req[0] = "from_ticket_id"; req[1] = "to_ticket_id"; req[2] = "relation_type"; req[3] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_unlink_tickets",
        "Remove a directed relation between two tickets",
        make_schema(props, req)));

    /* ---- Tasks ---- */

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id", prop_num("Numeric ticket ID"));
    cJSON_AddItemToObject(props, "title",      prop_str("Task / acceptance criterion description"));
    req[0] = "ticket_id"; req[1] = "title"; req[2] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_add_task",
        "Add an acceptance criterion task to a ticket. "
        "All tasks must be completed before the ticket can be moved to 'done'.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "task_id", prop_num("Numeric task ID (from get_ticket_detailed)"));
    req[0] = "task_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_complete_task",
        "Mark an acceptance criterion task as completed", make_schema(props, req)));

    /* ---- Comments / Work Log ---- */

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id", prop_num("Numeric ticket ID"));
    cJSON_AddItemToObject(props, "author",    prop_str("Author identifier (e.g. 'claude-sonnet-4-6')"));
    cJSON_AddItemToObject(props, "text",      prop_str("Comment / work log entry text"));
    req[0] = "ticket_id"; req[1] = "author"; req[2] = "text"; req[3] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_add_comment",
        "Add a work log entry or comment to a ticket. "
        "Use this to document progress and hand-off notes.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "ticket_id", prop_num("Numeric ticket ID"));
    req[0] = "ticket_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_list_comments",
        "List all work log entries / comments for a ticket", make_schema(props, req)));

    /* ---- Board Statuses & Workflow ---- */

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "project_id", prop_num("Numeric project ID"));
    req[0] = "project_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_list_board_statuses",
        "List all columns (board statuses) of a project including their "
        "agent_role_instruction. Call this first to discover status IDs and "
        "agent personas before creating tickets or setting up transitions.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "project_id",            prop_num("Numeric project ID"));
    cJSON_AddItemToObject(props, "name",                  prop_str("Machine name, e.g. 'in_progress'"));
    cJSON_AddItemToObject(props, "display_name",          prop_str("Human-readable label, e.g. 'In Arbeit'"));
    cJSON_AddItemToObject(props, "position",              prop_num("Column order (0-based)"));
    cJSON_AddItemToObject(props, "agent_role_instruction",
        prop_str("Dynamic persona prompt injected when an agent picks up a ticket in this column (optional)"));
    req[0] = "project_id"; req[1] = "name"; req[2] = "display_name"; req[3] = "position"; req[4] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_create_board_status",
        "Create a new board column/status for a project", make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "project_id",    prop_num("Numeric project ID"));
    cJSON_AddItemToObject(props, "from_status_id", prop_num("Source column ID"));
    cJSON_AddItemToObject(props, "to_status_id",   prop_num("Target column ID"));
    req[0] = "project_id"; req[1] = "from_status_id"; req[2] = "to_status_id"; req[3] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_create_status_transition",
        "Define an allowed workflow transition between two columns. "
        "The database will reject moves not defined here.",
        make_schema(props, req)));

    props = cJSON_CreateObject();
    cJSON_AddItemToObject(props, "project_id", prop_num("Numeric project ID"));
    req[0] = "project_id"; req[1] = NULL;
    cJSON_AddItemToArray(tools, make_tool("kb.ai_list_status_transitions",
        "List all allowed workflow transitions for a project",
        make_schema(props, req)));

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", tools);
    return jsonrpc_result(id, result);
}


/* ============================================================================
 * MCP Tools: Projects
 * ============================================================================ */

static cJSON *tool_create_project(cJSON *id, cJSON *params) {
    cJSON *slug_j = cJSON_GetObjectItemCaseSensitive(params, "slug");
    cJSON *name_j = cJSON_GetObjectItemCaseSensitive(params, "name");
    cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(params, "description");

    if (!cJSON_IsString(slug_j) || !cJSON_IsString(name_j))
        return mcp_tool_err(id, "Missing required parameters: slug, name");

    Project *p = project_create(global_db, slug_j->valuestring, name_j->valuestring,
                                cJSON_IsString(desc_j) ? desc_j->valuestring : NULL);
    if (!p) return mcp_tool_err(id, "Failed to create project");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", p->id);
    cJSON_AddStringToObject(r, "slug", p->slug);
    cJSON_AddStringToObject(r, "name", p->name);
    if (p->description) cJSON_AddStringToObject(r, "description", p->description);
    project_free(p);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_list_projects(cJSON *id) {
    Project **projects = project_list_all(global_db);
    cJSON *arr = cJSON_CreateArray();
    if (!projects) return mcp_tool_ok(id, arr);

    for (int i = 0; projects[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", projects[i]->id);
        cJSON_AddStringToObject(o, "slug", projects[i]->slug);
        cJSON_AddStringToObject(o, "name", projects[i]->name);
        if (projects[i]->description)
            cJSON_AddStringToObject(o, "description", projects[i]->description);
        cJSON_AddItemToArray(arr, o);
    }
    project_free_array(projects);
    return mcp_tool_ok(id, arr);
}

static cJSON *tool_get_project(cJSON *id, cJSON *params) {
    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    if (!cJSON_IsNumber(id_j))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    Project *p = project_get_by_id(global_db, (int)id_j->valueint);
    if (!p) return mcp_tool_err(id, "Project not found");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", p->id);
    cJSON_AddStringToObject(r, "slug", p->slug);
    cJSON_AddStringToObject(r, "name", p->name);
    if (p->description) cJSON_AddStringToObject(r, "description", p->description);
    project_free(p);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * MCP Tools: Tickets
 * ============================================================================ */

static cJSON *tool_create_ticket(cJSON *id, cJSON *params) {
    cJSON *proj_j  = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    cJSON *stat_j  = cJSON_GetObjectItemCaseSensitive(params, "status_id");
    cJSON *title_j = cJSON_GetObjectItemCaseSensitive(params, "title");
    cJSON *desc_j  = cJSON_GetObjectItemCaseSensitive(params, "description");
    cJSON *type_j  = cJSON_GetObjectItemCaseSensitive(params, "type");

    if (!cJSON_IsNumber(proj_j) || !cJSON_IsNumber(stat_j) || !cJSON_IsString(title_j))
        return mcp_tool_err(id, "Missing required parameters: project_id, status_id, title");

    const char *type_val = cJSON_IsString(type_j) ? type_j->valuestring : NULL;
    if (type_val && strcmp(type_val, "ticket") != 0 && strcmp(type_val, "epic") != 0)
        return mcp_tool_err(id, "Invalid type: must be 'ticket' or 'epic'");

    Ticket *t = ticket_create(global_db, (int)proj_j->valueint, (int)stat_j->valueint,
                              title_j->valuestring,
                              cJSON_IsString(desc_j) ? desc_j->valuestring : NULL,
                              type_val);
    if (!t) return mcp_tool_err(id, "Failed to create ticket");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", t->id);
    cJSON_AddNumberToObject(r, "project_id", t->project_id);
    cJSON_AddNumberToObject(r, "status_id", t->status_id);
    cJSON_AddStringToObject(r, "type", t->type ? t->type : "ticket");
    cJSON_AddStringToObject(r, "title", t->title);
    if (t->description) cJSON_AddStringToObject(r, "description", t->description);
    if (t->assignee)    cJSON_AddStringToObject(r, "assignee", t->assignee);
    ticket_free(t);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_list_tickets(cJSON *id, cJSON *params) {
    cJSON *proj_j    = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    cJSON *status_j  = cJSON_GetObjectItemCaseSensitive(params, "status_id");
    cJSON *type_j    = cJSON_GetObjectItemCaseSensitive(params, "type");
    cJSON *limit_j   = cJSON_GetObjectItemCaseSensitive(params, "limit");
    cJSON *offset_j  = cJSON_GetObjectItemCaseSensitive(params, "offset");
    cJSON *summary_j = cJSON_GetObjectItemCaseSensitive(params, "summary");

    if (!cJSON_IsNumber(proj_j))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    int project_id = (int)proj_j->valueint;
    if (project_id <= 0)
        return mcp_tool_err(id, "Invalid project_id: must be a positive integer");

    int         status_id   = cJSON_IsNumber(status_j) ? (int)status_j->valueint : 0;
    const char *type_filter = cJSON_IsString(type_j)   ? type_j->valuestring     : NULL;
    int         limit       = cJSON_IsNumber(limit_j)  ? (int)limit_j->valueint  : 0;
    int         offset      = cJSON_IsNumber(offset_j) ? (int)offset_j->valueint : 0;
    int         summary     = cJSON_IsTrue(summary_j);

    Ticket **tickets = ticket_list_filtered(global_db, project_id, status_id, type_filter, limit, offset);
    cJSON *arr = cJSON_CreateArray();
    if (!tickets) return mcp_tool_ok(id, arr);

    for (int i = 0; tickets[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", tickets[i]->id);
        cJSON_AddNumberToObject(o, "project_id", tickets[i]->project_id);
        cJSON_AddNumberToObject(o, "status_id", tickets[i]->status_id);
        cJSON_AddStringToObject(o, "type", tickets[i]->type ? tickets[i]->type : "ticket");
        cJSON_AddStringToObject(o, "title", tickets[i]->title);
        if (!summary && tickets[i]->description)
            cJSON_AddStringToObject(o, "description", tickets[i]->description);
        if (tickets[i]->assignee)
            cJSON_AddStringToObject(o, "assignee", tickets[i]->assignee);
        if (tickets[i]->created_at)
            cJSON_AddStringToObject(o, "created_at", tickets[i]->created_at);
        if (tickets[i]->updated_at)
            cJSON_AddStringToObject(o, "updated_at", tickets[i]->updated_at);
        cJSON_AddItemToArray(arr, o);
    }
    ticket_free_array(tickets);
    return mcp_tool_ok(id, arr);
}

static cJSON *tool_get_ticket(cJSON *id, cJSON *params) {
    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    if (!cJSON_IsNumber(id_j))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    Ticket *t = ticket_get_by_id(global_db, (int)id_j->valueint);
    if (!t) return mcp_tool_err(id, "Ticket not found");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", t->id);
    cJSON_AddNumberToObject(r, "project_id", t->project_id);
    cJSON_AddNumberToObject(r, "status_id", t->status_id);
    if (t->status_name) cJSON_AddStringToObject(r, "status_name", t->status_name);
    cJSON_AddStringToObject(r, "type", t->type ? t->type : "ticket");
    cJSON_AddStringToObject(r, "title", t->title);
    if (t->description) cJSON_AddStringToObject(r, "description", t->description);
    if (t->assignee)    cJSON_AddStringToObject(r, "assignee", t->assignee);
    if (t->model)       cJSON_AddStringToObject(r, "model", t->model);
    if (t->created_at)  cJSON_AddStringToObject(r, "created_at", t->created_at);
    if (t->updated_at)  cJSON_AddStringToObject(r, "updated_at", t->updated_at);
    if (t->agent_role_instruction)
        cJSON_AddStringToObject(r, "agent_role_instruction", t->agent_role_instruction);
    ticket_free(t);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_get_ticket_detailed(cJSON *id, cJSON *params) {
    cJSON *id_j  = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *ari_j = cJSON_GetObjectItemCaseSensitive(params, "include_role_instruction");
    if (!cJSON_IsNumber(id_j))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    /* include_role_instruction defaults to true; pass false to suppress it */
    int include_ari = !cJSON_IsFalse(ari_j);

    TicketDetailed *d = ticket_get_detailed(global_db, (int)id_j->valueint);
    if (!d) return mcp_tool_err(id, "Ticket not found");

    cJSON *ticket_j = cJSON_CreateObject();
    cJSON_AddNumberToObject(ticket_j, "id", d->ticket->id);
    cJSON_AddNumberToObject(ticket_j, "project_id", d->ticket->project_id);
    cJSON_AddNumberToObject(ticket_j, "status_id", d->ticket->status_id);
    if (d->ticket->status_name)
        cJSON_AddStringToObject(ticket_j, "status_name", d->ticket->status_name);
    cJSON_AddStringToObject(ticket_j, "type", d->ticket->type ? d->ticket->type : "ticket");
    if (include_ari && d->ticket->agent_role_instruction)
        cJSON_AddStringToObject(ticket_j, "agent_role_instruction",
                                d->ticket->agent_role_instruction);
    cJSON_AddStringToObject(ticket_j, "title", d->ticket->title);
    if (d->ticket->description)
        cJSON_AddStringToObject(ticket_j, "description", d->ticket->description);
    if (d->ticket->assignee)
        cJSON_AddStringToObject(ticket_j, "assignee", d->ticket->assignee);
    if (d->ticket->model)
        cJSON_AddStringToObject(ticket_j, "model", d->ticket->model);
    if (d->ticket->created_at)
        cJSON_AddStringToObject(ticket_j, "created_at", d->ticket->created_at);
    if (d->ticket->updated_at)
        cJSON_AddStringToObject(ticket_j, "updated_at", d->ticket->updated_at);

    cJSON *tasks_arr = cJSON_CreateArray();
    if (d->tasks) {
        for (int i = 0; d->tasks[i]; i++) {
            cJSON *tj = cJSON_CreateObject();
            cJSON_AddNumberToObject(tj, "id", d->tasks[i]->id);
            cJSON_AddStringToObject(tj, "title", d->tasks[i]->title);
            cJSON_AddBoolToObject(tj, "is_completed", d->tasks[i]->is_completed);
            cJSON_AddItemToArray(tasks_arr, tj);
        }
    }

    cJSON *comments_arr = cJSON_CreateArray();
    if (d->comments) {
        for (int i = 0; d->comments[i]; i++) {
            cJSON *cj = cJSON_CreateObject();
            cJSON_AddNumberToObject(cj, "id", d->comments[i]->id);
            cJSON_AddStringToObject(cj, "author", d->comments[i]->author);
            cJSON_AddStringToObject(cj, "text", d->comments[i]->comment_text);
            if (d->comments[i]->created_at)
                cJSON_AddStringToObject(cj, "created_at", d->comments[i]->created_at);
            cJSON_AddItemToArray(comments_arr, cj);
        }
    }

    cJSON *relations_arr = cJSON_CreateArray();
    if (d->relations) {
        for (int i = 0; d->relations[i]; i++) {
            TicketRelation *rel = d->relations[i];
            cJSON *rj = cJSON_CreateObject();
            cJSON_AddNumberToObject(rj, "id", rel->id);
            cJSON_AddStringToObject(rj, "relation_type", rel->relation_type);
            /* Expose direction relative to this ticket */
            if (rel->from_ticket_id == d->ticket->id) {
                cJSON_AddStringToObject(rj, "direction", "outgoing");
                cJSON_AddNumberToObject(rj, "other_ticket_id", rel->to_ticket_id);
                cJSON_AddStringToObject(rj, "other_ticket_title", rel->to_ticket_title);
            } else {
                cJSON_AddStringToObject(rj, "direction", "incoming");
                cJSON_AddNumberToObject(rj, "other_ticket_id", rel->from_ticket_id);
                cJSON_AddStringToObject(rj, "other_ticket_title", rel->from_ticket_title);
            }
            if (rel->created_at)
                cJSON_AddStringToObject(rj, "created_at", rel->created_at);
            cJSON_AddItemToArray(relations_arr, rj);
        }
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "ticket", ticket_j);
    cJSON_AddItemToObject(r, "tasks", tasks_arr);
    cJSON_AddItemToObject(r, "relations", relations_arr);
    cJSON_AddItemToObject(r, "comments", comments_arr);

    ticket_detailed_free(d);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_move_ticket(cJSON *id, cJSON *params) {
    cJSON *tid_j = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *sid_j = cJSON_GetObjectItemCaseSensitive(params, "new_status_id");

    if (!cJSON_IsNumber(tid_j) || !cJSON_IsNumber(sid_j))
        return mcp_tool_err(id, "Missing required parameters: ticket_id, new_status_id");

    if (!ticket_update_status(global_db, (int)tid_j->valueint, (int)sid_j->valueint)) {
        /* Read error BEFORE any subsequent query clobbers it */
        const char *raw = PQerrorMessage(global_db->conn);
        if (raw && strstr(raw, "Illegaler Kanban-Move"))
            return mcp_tool_err(id, "Invalid ticket transition: check workflow rules");
        if (raw && strstr(raw, "Akzeptanzkriterium"))
            return mcp_tool_err(id, "Cannot close ticket: open acceptance criteria remain");
        return mcp_tool_err(id, "Failed to move ticket");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_assign_ticket(cJSON *id, cJSON *params) {
    cJSON *tid_j = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *ass_j = cJSON_GetObjectItemCaseSensitive(params, "assignee");

    if (!cJSON_IsNumber(tid_j))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    /* Use provided assignee, fall back to KB_AI_AGENT_NAME, then error */
    const char *assignee = cJSON_IsString(ass_j) ? ass_j->valuestring : g_agent_name;
    if (!assignee)
        return mcp_tool_err(id,
            "Missing assignee: provide 'assignee' parameter or set the KB_AI_AGENT_NAME "
            "environment variable in the MCP server config (KB_AI_AGENT_MODEL is also "
            "recommended for tracking which model worked the ticket)");

    if (!ticket_assign(global_db, (int)tid_j->valueint, assignee, g_agent_model))
        return mcp_tool_err(id, "Failed to assign ticket");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_update_ticket(cJSON *id, cJSON *params) {
    cJSON *tid_j   = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *title_j = cJSON_GetObjectItemCaseSensitive(params, "title");
    cJSON *desc_j  = cJSON_GetObjectItemCaseSensitive(params, "description");

    if (!cJSON_IsNumber(tid_j))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    int ticket_id = (int)tid_j->valueint;
    int updated = 0;

    if (cJSON_IsString(title_j))
        updated += ticket_update_title(global_db, ticket_id, title_j->valuestring);

    if (desc_j) {
        const char *new_desc = cJSON_IsNull(desc_j) ? NULL : desc_j->valuestring;
        updated += ticket_update_description(global_db, ticket_id, new_desc);
    }

    if (!updated)
        return mcp_tool_err(id, "No updatable fields provided (title, description)");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "updated_fields", updated);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * MCP Tools: Tasks
 * ============================================================================ */

static cJSON *tool_add_task(cJSON *id, cJSON *params) {
    cJSON *tid_j   = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *title_j = cJSON_GetObjectItemCaseSensitive(params, "title");

    if (!cJSON_IsNumber(tid_j) || !cJSON_IsString(title_j))
        return mcp_tool_err(id, "Missing required parameters: ticket_id, title");

    TicketTask *task = ticket_add_task(global_db, (int)tid_j->valueint, title_j->valuestring);
    if (!task) return mcp_tool_err(id, "Failed to add task");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", task->id);
    cJSON_AddNumberToObject(r, "ticket_id", task->ticket_id);
    cJSON_AddStringToObject(r, "title", task->title);
    cJSON_AddBoolToObject(r, "is_completed", task->is_completed);
    ticket_task_free(task);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_complete_task(cJSON *id, cJSON *params) {
    cJSON *tid_j = cJSON_GetObjectItemCaseSensitive(params, "task_id");
    if (!cJSON_IsNumber(tid_j))
        return mcp_tool_err(id, "Missing required parameter: task_id");

    if (!ticket_complete_task(global_db, (int)tid_j->valueint))
        return mcp_tool_err(id, "Failed to complete task");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * MCP Tools: Comments / Work Log
 * ============================================================================ */

static cJSON *tool_add_comment(cJSON *id, cJSON *params) {
    cJSON *tid_j    = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *author_j = cJSON_GetObjectItemCaseSensitive(params, "author");
    cJSON *text_j   = cJSON_GetObjectItemCaseSensitive(params, "text");

    if (!cJSON_IsNumber(tid_j) || !cJSON_IsString(author_j) || !cJSON_IsString(text_j))
        return mcp_tool_err(id, "Missing required parameters: ticket_id, author, text");

    TicketComment *c = comment_add(global_db, (int)tid_j->valueint,
                                   author_j->valuestring, text_j->valuestring);
    if (!c) return mcp_tool_err(id, "Failed to add comment");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", c->id);
    cJSON_AddNumberToObject(r, "ticket_id", c->ticket_id);
    cJSON_AddStringToObject(r, "author", c->author);
    cJSON_AddStringToObject(r, "text", c->comment_text);
    if (c->created_at) cJSON_AddStringToObject(r, "created_at", c->created_at);
    comment_free(c);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_list_comments(cJSON *id, cJSON *params) {
    cJSON *tid_j = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    if (!cJSON_IsNumber(tid_j))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    TicketComment **comments = comment_list_by_ticket(global_db, (int)tid_j->valueint);
    cJSON *arr = cJSON_CreateArray();
    if (!comments) return mcp_tool_ok(id, arr);

    for (int i = 0; comments[i]; i++) {
        cJSON *cj = cJSON_CreateObject();
        cJSON_AddNumberToObject(cj, "id", comments[i]->id);
        cJSON_AddNumberToObject(cj, "ticket_id", comments[i]->ticket_id);
        cJSON_AddStringToObject(cj, "author", comments[i]->author);
        cJSON_AddStringToObject(cj, "text", comments[i]->comment_text);
        if (comments[i]->created_at)
            cJSON_AddStringToObject(cj, "created_at", comments[i]->created_at);
        cJSON_AddItemToArray(arr, cj);
    }
    comment_free_array(comments);
    return mcp_tool_ok(id, arr);
}


static cJSON *tool_link_tickets(cJSON *id, cJSON *params) {
    cJSON *from_j = cJSON_GetObjectItemCaseSensitive(params, "from_ticket_id");
    cJSON *to_j   = cJSON_GetObjectItemCaseSensitive(params, "to_ticket_id");
    cJSON *rel_j  = cJSON_GetObjectItemCaseSensitive(params, "relation_type");

    if (!cJSON_IsNumber(from_j) || !cJSON_IsNumber(to_j) || !cJSON_IsString(rel_j))
        return mcp_tool_err(id,
            "Missing required parameters: from_ticket_id, to_ticket_id, relation_type");

    const char *rel = rel_j->valuestring;
    if (strcmp(rel, "parent_of")    != 0 &&
        strcmp(rel, "blocks")       != 0 &&
        strcmp(rel, "duplicate_of") != 0 &&
        strcmp(rel, "relates_to")   != 0)
        return mcp_tool_err(id,
            "Invalid relation_type: must be parent_of, blocks, duplicate_of, or relates_to");

    if (!ticket_link(global_db, (int)from_j->valueint, (int)to_j->valueint, rel))
        return mcp_tool_err(id, "Failed to create relation (may already exist or ticket not found)");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_unlink_tickets(cJSON *id, cJSON *params) {
    cJSON *from_j = cJSON_GetObjectItemCaseSensitive(params, "from_ticket_id");
    cJSON *to_j   = cJSON_GetObjectItemCaseSensitive(params, "to_ticket_id");
    cJSON *rel_j  = cJSON_GetObjectItemCaseSensitive(params, "relation_type");

    if (!cJSON_IsNumber(from_j) || !cJSON_IsNumber(to_j) || !cJSON_IsString(rel_j))
        return mcp_tool_err(id,
            "Missing required parameters: from_ticket_id, to_ticket_id, relation_type");

    if (!ticket_unlink(global_db, (int)from_j->valueint, (int)to_j->valueint, rel_j->valuestring))
        return mcp_tool_err(id, "Relation not found or could not be deleted");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_search_tickets(cJSON *id, cJSON *params) {
    cJSON *proj_j  = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    cJSON *query_j = cJSON_GetObjectItemCaseSensitive(params, "query");

    if (!cJSON_IsNumber(proj_j) || !cJSON_IsString(query_j))
        return mcp_tool_err(id, "Missing required parameters: project_id, query");

    Ticket **tickets = ticket_search(global_db, (int)proj_j->valueint, query_j->valuestring);
    cJSON *arr = cJSON_CreateArray();
    if (!tickets) return mcp_tool_ok(id, arr);

    for (int i = 0; tickets[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", tickets[i]->id);
        cJSON_AddNumberToObject(o, "project_id", tickets[i]->project_id);
        cJSON_AddNumberToObject(o, "status_id", tickets[i]->status_id);
        cJSON_AddStringToObject(o, "type", tickets[i]->type ? tickets[i]->type : "ticket");
        cJSON_AddStringToObject(o, "title", tickets[i]->title);
        if (tickets[i]->description)
            cJSON_AddStringToObject(o, "description", tickets[i]->description);
        if (tickets[i]->assignee)
            cJSON_AddStringToObject(o, "assignee", tickets[i]->assignee);
        if (tickets[i]->created_at)
            cJSON_AddStringToObject(o, "created_at", tickets[i]->created_at);
        cJSON_AddItemToArray(arr, o);
    }
    ticket_free_array(tickets);
    return mcp_tool_ok(id, arr);
}

static cJSON *tool_delete_ticket(cJSON *id, cJSON *params) {
    cJSON *tid_j    = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *reason_j = cJSON_GetObjectItemCaseSensitive(params, "reason");

    if (!cJSON_IsNumber(tid_j))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");
    if (!cJSON_IsString(reason_j) || reason_j->valuestring[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: reason (required for audit trail)");

    int ticket_id = (int)tid_j->valueint;

    /* Verify the ticket exists before deleting */
    Ticket *t = ticket_get_by_id(global_db, ticket_id);
    if (!t) return mcp_tool_err(id, "Ticket not found");
    ticket_free(t);

    if (!ticket_delete(global_db, ticket_id))
        return mcp_tool_err(id, "Failed to delete ticket");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "deleted_ticket_id", ticket_id);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_move_tickets(cJSON *id, cJSON *params) {
    cJSON *ids_j   = cJSON_GetObjectItemCaseSensitive(params, "ticket_ids");
    cJSON *sid_j   = cJSON_GetObjectItemCaseSensitive(params, "new_status_id");

    if (!cJSON_IsArray(ids_j) || !cJSON_IsNumber(sid_j))
        return mcp_tool_err(id, "Missing required parameters: ticket_ids (array), new_status_id");

    int new_status_id = (int)sid_j->valueint;
    int total   = cJSON_GetArraySize(ids_j);
    int success = 0;
    int failed  = 0;

    cJSON *results = cJSON_CreateArray();
    for (int i = 0; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(ids_j, i);
        if (!cJSON_IsNumber(item)) { failed++; continue; }

        int ticket_id = (int)item->valueint;
        cJSON *entry  = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "ticket_id", ticket_id);

        if (ticket_update_status(global_db, ticket_id, new_status_id)) {
            cJSON_AddBoolToObject(entry, "success", 1);
            success++;
        } else {
            cJSON_AddBoolToObject(entry, "success", 0);
            const char *raw = PQerrorMessage(global_db->conn);
            if (raw && strstr(raw, "Illegaler Kanban-Move"))
                cJSON_AddStringToObject(entry, "error", "Invalid transition");
            else if (raw && strstr(raw, "Akzeptanzkriterium"))
                cJSON_AddStringToObject(entry, "error", "Open acceptance criteria remain");
            else
                cJSON_AddStringToObject(entry, "error", "Failed");
            failed++;
        }
        cJSON_AddItemToArray(results, entry);
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "total", total);
    cJSON_AddNumberToObject(r, "success", success);
    cJSON_AddNumberToObject(r, "failed", failed);
    cJSON_AddItemToObject(r, "results", results);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Tool Dispatcher
 * ============================================================================ */

static cJSON *dispatch_tool(cJSON *id, const char *name, cJSON *params) {
    if (strcmp(name, "kb.ai_create_project") == 0)     return tool_create_project(id, params);
    if (strcmp(name, "kb.ai_list_projects") == 0)       return tool_list_projects(id);
    if (strcmp(name, "kb.ai_get_project") == 0)         return tool_get_project(id, params);
    if (strcmp(name, "kb.ai_create_ticket") == 0)       return tool_create_ticket(id, params);
    if (strcmp(name, "kb.ai_list_tickets") == 0)        return tool_list_tickets(id, params);
    if (strcmp(name, "kb.ai_search_tickets") == 0)      return tool_search_tickets(id, params);
    if (strcmp(name, "kb.ai_get_ticket") == 0)          return tool_get_ticket(id, params);
    if (strcmp(name, "kb.ai_get_ticket_detailed") == 0) return tool_get_ticket_detailed(id, params);
    if (strcmp(name, "kb.ai_move_ticket") == 0)         return tool_move_ticket(id, params);
    if (strcmp(name, "kb.ai_move_tickets") == 0)        return tool_move_tickets(id, params);
    if (strcmp(name, "kb.ai_assign_ticket") == 0)       return tool_assign_ticket(id, params);
    if (strcmp(name, "kb.ai_update_ticket") == 0)       return tool_update_ticket(id, params);
    if (strcmp(name, "kb.ai_delete_ticket") == 0)       return tool_delete_ticket(id, params);
    if (strcmp(name, "kb.ai_link_tickets") == 0)        return tool_link_tickets(id, params);
    if (strcmp(name, "kb.ai_unlink_tickets") == 0)      return tool_unlink_tickets(id, params);
    if (strcmp(name, "kb.ai_add_task") == 0)            return tool_add_task(id, params);
    if (strcmp(name, "kb.ai_complete_task") == 0)       return tool_complete_task(id, params);
    if (strcmp(name, "kb.ai_add_comment") == 0)              return tool_add_comment(id, params);
    if (strcmp(name, "kb.ai_list_comments") == 0)            return tool_list_comments(id, params);
    if (strcmp(name, "kb.ai_list_board_statuses") == 0)      return tool_list_board_statuses(id, params);
    if (strcmp(name, "kb.ai_create_board_status") == 0)      return tool_create_board_status(id, params);
    if (strcmp(name, "kb.ai_create_status_transition") == 0) return tool_create_status_transition(id, params);
    if (strcmp(name, "kb.ai_list_status_transitions") == 0)  return tool_list_status_transitions(id, params);
    return mcp_tool_err(id, "Unknown tool");
}


/* ============================================================================
 * Request Handler (JSON-RPC 2.0 dispatch)
 * ============================================================================ */

static cJSON *handle_request(const char *json_str) {
    cJSON *req = cJSON_Parse(json_str);
    if (!req)
        return jsonrpc_error(NULL, -32700, "Parse error");

    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");  /* may be NULL for notifications */

    cJSON *method_j = cJSON_GetObjectItemCaseSensitive(req, "method");
    if (!method_j || !cJSON_IsString(method_j)) {
        cJSON_Delete(req);
        return jsonrpc_error(id_j, -32600, "Invalid Request: missing method");
    }
    const char *method = method_j->valuestring;
    cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");

    cJSON *resp = NULL;

    if (strcmp(method, "initialize") == 0) {
        resp = handle_initialize(id_j, params);

    } else if (strcmp(method, "notifications/initialized") == 0) {
        /* Notification — no response required */
        cJSON_Delete(req);
        return NULL;

    } else if (strcmp(method, "tools/list") == 0) {
        resp = handle_tools_list(id_j);

    } else if (strcmp(method, "tools/call") == 0) {
        if (!params) {
            resp = jsonrpc_error(id_j, -32602, "Invalid params: missing params for tools/call");
        } else {
            cJSON *name_j = cJSON_GetObjectItemCaseSensitive(params, "name");
            cJSON *args_j = cJSON_GetObjectItemCaseSensitive(params, "arguments");

            if (!name_j || !cJSON_IsString(name_j)) {
                resp = jsonrpc_error(id_j, -32602, "Invalid params: missing tool name");
            } else {
                cJSON *tool_params = args_j ? cJSON_Duplicate(args_j, 1) : cJSON_CreateObject();
                resp = dispatch_tool(id_j, name_j->valuestring, tool_params);
                cJSON_Delete(tool_params);
            }
        }

    } else {
        resp = jsonrpc_error(id_j, -32601, "Method not found");
    }

    cJSON_Delete(req);
    return resp;
}


/* ============================================================================
 * Output
 * ============================================================================ */

static void send_json(cJSON *json) {
    char *str = cJSON_PrintUnformatted(json);
    if (str) {
        puts(str);
        fflush(stdout);
        cJSON_free(str);
    }
    cJSON_Delete(json);
}


/* ============================================================================
 * Database Init
 * ============================================================================ */

static int db_init(void) {
    const char *host     = getenv("KB_AI_DB_HOST");
    const char *port     = getenv("KB_AI_DB_PORT");
    const char *dbname   = getenv("KB_AI_DB_NAME");
    const char *user     = getenv("KB_AI_DB_USER");
    const char *password = getenv("KB_AI_DB_PASSWORD");

    g_agent_name  = getenv("KB_AI_AGENT_NAME");
    g_agent_model = getenv("KB_AI_AGENT_MODEL");

    global_db = db_connect(
        host     ? host     : DEFAULT_DB_HOST,
        port     ? port     : DEFAULT_DB_PORT,
        dbname   ? dbname   : DEFAULT_DB_NAME,
        user     ? user     : DEFAULT_DB_USER,
        password ? password : DEFAULT_DB_PASSWORD
    );

    if (!global_db) {
        fprintf(stderr, "kb.ai: failed to connect to database\n");
        return 0;
    }

    fprintf(stderr, "kb.ai: connected to %s:%s/%s\n",
            host   ? host   : DEFAULT_DB_HOST,
            port   ? port   : DEFAULT_DB_PORT,
            dbname ? dbname : DEFAULT_DB_NAME);
    return 1;
}


/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    cJSON_Hooks hooks = {malloc, free};
    cJSON_InitHooks(&hooks);

    if (!db_init())
        return EXIT_FAILURE;

    fprintf(stderr, "kb.ai MCP Server %s (MCP protocol %s) ready\n",
            MCP_SERVER_VERSION, MCP_PROTOCOL_VERSION);

    /* getline() grows the buffer automatically — no truncation risk */
    char   *line     = NULL;
    size_t  line_cap = 0;
    ssize_t n;

    while ((n = getline(&line, &line_cap, stdin)) != -1) {
        /* Strip trailing newline */
        if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
        if (n > 0 && line[n - 1] == '\r') line[--n] = '\0';
        if (n == 0) continue;

        cJSON *resp = handle_request(line);
        if (resp) send_json(resp);
    }

    free(line);
    if (global_db) db_disconnect(global_db);
    fprintf(stderr, "kb.ai MCP Server shut down\n");
    return EXIT_SUCCESS;
}
