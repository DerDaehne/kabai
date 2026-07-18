#include <stdio.h>
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

/* Builds the {"type":"text","text":...} block shared by mcp_tool_ok and
 * mcp_tool_ok_with_image. Consumes data. */
static cJSON *text_content_block(cJSON *data) {
    char *text = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);

    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "{}");
    if (text) cJSON_free(text);
    return item;
}

/* Success: serialise data as JSON into a text content block. Consumes data. */
cJSON *mcp_tool_ok(cJSON *id, cJSON *data) {
    cJSON *content = cJSON_CreateArray();
    cJSON_AddItemToArray(content, text_content_block(data));

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", 0);

    return jsonrpc_result(id, result);
}

/* Success with a text block (metadata) followed by an image content block
 * (MCP image content: {"type":"image","data":<base64>,"mimeType":...}).
 * Consumes data; base64_data/mime_type are borrowed. */
cJSON *mcp_tool_ok_with_image(cJSON *id, cJSON *data, const char *base64_data,
                              const char *mime_type) {
    cJSON *content = cJSON_CreateArray();
    cJSON_AddItemToArray(content, text_content_block(data));

    cJSON *image = cJSON_CreateObject();
    cJSON_AddStringToObject(image, "type", "image");
    cJSON_AddStringToObject(image, "data", base64_data ? base64_data : "");
    cJSON_AddStringToObject(image, "mimeType", mime_type ? mime_type : "application/octet-stream");
    cJSON_AddItemToArray(content, image);

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


/* ============================================================================
 * MCP Server: request handling + STDIO loop (JSON-RPC 2.0)
 * ============================================================================ */

static cJSON *handle_initialize(McpContext *ctx, const McpServerInfo *info, cJSON *id) {
    cJSON *tools_cap = cJSON_CreateObject();

    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", tools_cap);

    cJSON *server_info = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info, "name", info->name);
    cJSON_AddStringToObject(server_info, "version", info->version);
    if (ctx->agent_name)  cJSON_AddStringToObject(server_info, "agentName",  ctx->agent_name);
    if (ctx->agent_model) cJSON_AddStringToObject(server_info, "agentModel", ctx->agent_model);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", info->protocol_version);
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON_AddItemToObject(result, "serverInfo", server_info);

    return jsonrpc_result(id, result);
}

static cJSON *handle_tools_list(McpRegistry *r, cJSON *id) {
    cJSON *tools = cJSON_CreateArray();
    mcp_registry_list_tools(r, tools);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", tools);
    return jsonrpc_result(id, result);
}

/* Returns the response, or NULL for notifications. */
static cJSON *handle_request(McpRegistry *r, McpContext *ctx,
                             const McpServerInfo *info, const char *json_str) {
    cJSON *req = cJSON_Parse(json_str);
    if (!req)
        return jsonrpc_error(NULL, -32700, "Parse error");

    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");  /* may be NULL for notifications */

    cJSON *method_j = cJSON_GetObjectItemCaseSensitive(req, "method");
    if (!method_j || !cJSON_IsString(method_j)) {
        /* id_j lives inside req — build the response before freeing the tree */
        cJSON *err = jsonrpc_error(id_j, -32600, "Invalid Request: missing method");
        cJSON_Delete(req);
        return err;
    }
    const char *method = method_j->valuestring;
    cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");

    cJSON *resp = NULL;

    if (strcmp(method, "initialize") == 0) {
        resp = handle_initialize(ctx, info, id_j);

    } else if (strcmp(method, "notifications/initialized") == 0) {
        /* Notification — no response required */
        cJSON_Delete(req);
        return NULL;

    } else if (strcmp(method, "tools/list") == 0) {
        resp = handle_tools_list(r, id_j);

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
                resp = mcp_registry_dispatch(r, ctx, id_j, name_j->valuestring, tool_params);
                if (!resp) resp = mcp_tool_err(id_j, "Unknown tool");
                cJSON_Delete(tool_params);
            }
        }

    } else {
        resp = jsonrpc_error(id_j, -32601, "Method not found");
    }

    cJSON_Delete(req);
    return resp;
}

static void send_json(cJSON *json) {
    char *str = cJSON_PrintUnformatted(json);
    if (str) {
        puts(str);
        fflush(stdout);
        cJSON_free(str);
    }
    cJSON_Delete(json);
}

#ifdef _WIN32
/* getline() is POSIX-only and not provided by the MinGW/MSVCRT runtime */
#include <sys/types.h>
static ssize_t kabai_getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) return -1;
    if (!*lineptr || *n == 0) {
        *n = 256;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }
    size_t len = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (len + 2 > *n) {
            char *grown = realloc(*lineptr, *n * 2);
            if (!grown) return -1;
            *lineptr = grown;
            *n *= 2;
        }
        (*lineptr)[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0) return -1;
    (*lineptr)[len] = '\0';
    return (ssize_t)len;
}
#define getline kabai_getline
#endif

void mcp_run_stdio_loop(McpRegistry *r, McpContext *ctx, const McpServerInfo *info) {
    /* getline() grows the buffer automatically — no truncation risk */
    char   *line     = NULL;
    size_t  line_cap = 0;
    ssize_t n;

    while ((n = getline(&line, &line_cap, stdin)) != -1) {
        /* Strip trailing newline */
        if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
        if (n > 0 && line[n - 1] == '\r') line[--n] = '\0';
        if (n == 0) continue;

        cJSON *resp = handle_request(r, ctx, info, line);
        if (resp) send_json(resp);
    }

    free(line);
}
