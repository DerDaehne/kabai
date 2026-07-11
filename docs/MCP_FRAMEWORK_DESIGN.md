# MCP framework design (ticket #336, epic #335)

Status: accepted · 2026-07-03

## Motivation

main.c (1271 lines) combined three places per tool that had to be kept in
sync: handler function, `make_tool` entry with JSON schema in
`handle_tools_list`, dispatch branch in `dispatch_tool`. With the kabai-docs
module (project "kabai-docs", epic #329) adding ~10 more tools, the framework
centralises the boilerplate so one tool is exactly ONE registry entry.

## Decisions

### 1. Registry structure

Dynamically growing array inside `McpRegistry`; modules register at runtime
(bootstrap phase), no static array — otherwise main.c would again have to
know every tool.

```c
typedef struct McpContext {
    DatabaseConnection *db;
    const char         *agent_name;   /* KABAI_AGENT_NAME,  may be NULL */
    const char         *agent_model;  /* KABAI_AGENT_MODEL, may be NULL */
} McpContext;

typedef cJSON *(*McpToolHandler)(McpContext *ctx, cJSON *id, cJSON *params);

/* schema is consumed (ownership moves to the registry); tools/list duplicates. */
void mcp_registry_add(McpRegistry *r, const char *name, const char *description,
                      cJSON *input_schema, McpToolHandler handler);
```

**Handler signature with a context struct** instead of the previous globals
(`global_db`, `g_agent_name`, `g_agent_model`): the globals move into an
`McpContext` that main.c fills once and the dispatcher passes to every
handler. Rejected: keeping the `(id, params)` signature with globals inside
the framework — works, but cements global state and makes handlers
untestable without process setup.

`tools/list` and the `tools/call` dispatch are generated from the registry
(iteration resp. name lookup, linear — irrelevant below ~100 tools).

### 2. Schema builders

The previous pattern (props object + `req[]` array + `make_schema`) is
replaced by helpers that maintain `required` directly on the schema — one
place instead of two:

```c
cJSON *schema_new(void);  /* {type:"object", properties:{}, required:[]} */
void schema_num (cJSON *s, const char *name, const char *desc, bool required);
void schema_str (cJSON *s, const char *name, const char *desc, bool required);
void schema_bool(cJSON *s, const char *name, const char *desc, bool required);
void schema_num_array(cJSON *s, const char *name, const char *desc, bool required);
```

An empty `required` array is removed on registration (identical behaviour
to before: schemas without required fields carry no required key).
Rejected: declarative tables (structs describing fields) — more conceptual
overhead than value at this tool count.

### 3. Parameter extraction

Getters without built-in error production; error messages stay explicit in
the handler. Reason: the existing messages are partly combined ("Missing
required parameters: project_id, status_id, title") and had to remain
byte-identical for the #338 migration — an auto-error mechanism would have
changed them.

```c
bool        param_num (cJSON *params, const char *name, int *out);   /* false if absent/not a number */
const char *param_str (cJSON *params, const char *name);             /* NULL if absent/not a string */
bool        param_bool(cJSON *params, const char *name, bool dflt);
bool        param_is_null(cJSON *params, const char *name);          /* explicit JSON null (update_ticket description) */
```

Typical handler afterwards:

```c
static cJSON *tool_get_project(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id;
    if (!param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    Project *p = project_get_by_id(ctx->db, project_id);
    if (!p) return mcp_tool_err(id, "Project not found");
    ...
}
```

### 4. Error conventions

`mcp_tool_ok` / `mcp_tool_err` (and the jsonrpc_* helpers) move into the
framework unchanged and are exported. Semantics stay: tool errors via
`isError:true` content block, JSON-RPC error only for protocol errors.
Improving the error texts themselves (SQLSTATE mapping instead of
"Failed to ...") is deliberately NOT part of the framework — separate
ticket #345, after the migration, so the mapping is built centrally once.

### 5. Module interface

```c
void kanban_register_tools(McpRegistry *r);   /* src/kanban/kanban_tools.c */
void docs_register_tools(McpRegistry *r);     /* src/docs/docs_tools.c (epic #329) */
```

Convention: `<module>_register_tools`, defined in the module directory. The
tool handlers move from main.c into `src/kanban/kanban_tools.c` (the
module's MCP adapter); the domain logic (`tickets.c`, `projects.c`, ...)
stays untouched. main.c shrinks to: cJSON hooks, db_init → McpContext,
registry + module registration, stdio loop.

## File layout

```
src/mcp/mcp.h        public API: McpContext, McpRegistry, handler type,
                     mcp_registry_*, mcp_tool_ok/err, mcp_run_stdio_loop
src/mcp/mcp.c        registry, generic tools/list + dispatch, JSON-RPC
                     helpers, initialize handler, stdio loop
src/mcp/schema.h/.c  schema_* builders + param_* getters
src/kanban/kanban_tools.c  all existing tool handlers + registration
```

## Behaviour guarantee (for #338)

Tool names, descriptions, schemas, error messages and success payloads stay
byte-identical. Verification: diff the `tools/list` dump before/after the
migration; spot-check calls per tool category.

## Bootstrapping note

This doc is a transitional markdown file in the repo. Once kabai-docs is
live (project "kabai-docs", #333), it gets imported as notes into the
zettelkasten; kabai design docs are written there from then on.
