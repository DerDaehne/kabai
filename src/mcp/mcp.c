#include <stdlib.h>
#include <string.h>
#include "mcp/mcp.h"

typedef struct {
    char           *name;
    cJSON          *descriptor;   /* {name, description, inputSchema} */
    McpToolHandler  handler;
} McpToolEntry;

struct McpRegistry {
    McpToolEntry *entries;
    int           count;
    int           capacity;
};

McpRegistry *mcp_registry_new(void) {
    McpRegistry *r = calloc(1, sizeof(McpRegistry));
    return r;
}

void mcp_registry_free(McpRegistry *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        free(r->entries[i].name);
        cJSON_Delete(r->entries[i].descriptor);
    }
    free(r->entries);
    free(r);
}

void mcp_registry_add(McpRegistry *r, const char *name, const char *description,
                      cJSON *input_schema, McpToolHandler handler) {
    if (r->count == r->capacity) {
        int cap = r->capacity ? r->capacity * 2 : 32;
        McpToolEntry *grown = realloc(r->entries, (size_t)cap * sizeof(McpToolEntry));
        if (!grown) return;
        r->entries  = grown;
        r->capacity = cap;
    }

    /* Empty required array carries no information — match hand-built schemas
     * that omit the field entirely. */
    cJSON *req = cJSON_GetObjectItemCaseSensitive(input_schema, "required");
    if (req && cJSON_GetArraySize(req) == 0)
        cJSON_DeleteItemFromObjectCaseSensitive(input_schema, "required");

    cJSON *desc = cJSON_CreateObject();
    cJSON_AddStringToObject(desc, "name", name);
    cJSON_AddStringToObject(desc, "description", description);
    cJSON_AddItemToObject(desc, "inputSchema", input_schema);

    McpToolEntry *e = &r->entries[r->count++];
    e->name       = strdup(name);
    e->descriptor = desc;
    e->handler    = handler;
}

static McpToolEntry *registry_find(McpRegistry *r, const char *name) {
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->entries[i].name, name) == 0)
            return &r->entries[i];
    return NULL;
}

cJSON *mcp_registry_dispatch(McpRegistry *r, McpContext *ctx, cJSON *id,
                             const char *name, cJSON *params) {
    McpToolEntry *e = registry_find(r, name);
    if (!e) return NULL;
    return e->handler(ctx, id, params);
}

void mcp_registry_list_tools(McpRegistry *r, cJSON *tools_array) {
    for (int i = 0; i < r->count; i++)
        cJSON_AddItemToArray(tools_array, cJSON_Duplicate(r->entries[i].descriptor, 1));
}

cJSON *mcp_registry_tool_descriptor(McpRegistry *r, const char *name) {
    McpToolEntry *e = registry_find(r, name);
    return e ? cJSON_Duplicate(e->descriptor, 1) : NULL;
}


/* ============================================================================
 * JSON-RPC 2.0 Protocol Helpers
 *
 * id is always borrowed (we duplicate it into the response).
 * result/error objects are always consumed (ownership transferred).
 * ============================================================================ */

cJSON *jsonrpc_result(cJSON *id, cJSON *result) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

cJSON *jsonrpc_error(cJSON *id, int code, const char *message) {
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
cJSON *mcp_tool_ok(cJSON *id, cJSON *data) {
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
cJSON *mcp_tool_err(cJSON *id, const char *message) {
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
