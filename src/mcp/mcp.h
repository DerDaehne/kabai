/*
 * MCP tool framework: registry, dispatch, JSON-RPC and tool-result helpers.
 *
 * A tool is ONE registry entry {name, description, inputSchema, handler}.
 * tools/list and tools/call dispatch are derived from the registry; modules
 * register their tools via <module>_register_tools(McpRegistry*).
 */

#ifndef MCP_MCP_H
#define MCP_MCP_H

#include <cjson/cJSON.h>
#include "db/connection.h"

typedef struct McpContext {
    DatabaseConnection *db;
    const char         *agent_name;   /* KB_AI_AGENT_NAME, may be NULL */
    const char         *agent_model;  /* KB_AI_AGENT_MODEL, may be NULL */
} McpContext;

typedef cJSON *(*McpToolHandler)(McpContext *ctx, cJSON *id, cJSON *params);

typedef struct McpRegistry McpRegistry;

McpRegistry *mcp_registry_new(void);
void         mcp_registry_free(McpRegistry *r);

/* input_schema is consumed (ownership moves to the registry). An empty
 * "required" array in the schema is stripped on registration. */
void mcp_registry_add(McpRegistry *r, const char *name, const char *description,
                      cJSON *input_schema, McpToolHandler handler);

/* Returns the JSON-RPC response, or NULL if name is not registered
 * (caller may fall back to another dispatch path). */
cJSON *mcp_registry_dispatch(McpRegistry *r, McpContext *ctx, cJSON *id,
                             const char *name, cJSON *params);

/* Appends a duplicated {name, description, inputSchema} descriptor per
 * registered tool (in registration order) to tools_array. */
void mcp_registry_list_tools(McpRegistry *r, cJSON *tools_array);

/* Duplicated descriptor for one tool, or NULL if not registered. */
cJSON *mcp_registry_tool_descriptor(McpRegistry *r, const char *name);

/* JSON-RPC 2.0 helpers. id is borrowed; result is consumed. */
cJSON *jsonrpc_result(cJSON *id, cJSON *result);
cJSON *jsonrpc_error(cJSON *id, int code, const char *message);

/* MCP tool results: content blocks with isError flag (JSON-RPC error is
 * reserved for protocol errors). mcp_tool_ok consumes data. */
cJSON *mcp_tool_ok(cJSON *id, cJSON *data);
cJSON *mcp_tool_err(cJSON *id, const char *message);

typedef struct McpServerInfo {
    const char *name;
    const char *version;
    const char *protocol_version;
} McpServerInfo;

/* Reads JSON-RPC requests line-by-line from stdin until EOF and answers
 * initialize, notifications/initialized, tools/list and tools/call from
 * the registry. */
void mcp_run_stdio_loop(McpRegistry *r, McpContext *ctx, const McpServerInfo *info);

#endif /* MCP_MCP_H */
