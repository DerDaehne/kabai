# MCP-Framework-Design (Ticket #336, Epic #335)

Status: beschlossen · 2026-07-03 · Autor: claude-fable-5

## Motivation

main.c (1271 Zeilen) vereint pro Tool drei Stellen, die synchron gehalten werden
müssen: Handler-Funktion, `make_tool`-Eintrag mit JSON-Schema in
`handle_tools_list`, Dispatch-Zweig in `dispatch_tool`. Mit dem kbai-docs-Modul
(Projekt "kbai-docs", Epic #329) kommen ~10 weitere Tools. Das Framework
zentralisiert die Boilerplate, sodass ein Tool genau EIN Registry-Eintrag ist.

## Entscheidungen

### 1. Registry-Struktur

Dynamisch wachsendes Array im `McpRegistry`; Module registrieren zur Laufzeit
(Bootstrap-Phase), kein statisches Array — sonst müsste main.c wieder alle
Tools kennen.

```c
typedef struct McpContext {
    DatabaseConnection *db;
    const char         *agent_name;   /* KB_AI_AGENT_NAME,  darf NULL sein */
    const char         *agent_model;  /* KB_AI_AGENT_MODEL, darf NULL sein */
} McpContext;

typedef cJSON *(*McpToolHandler)(McpContext *ctx, cJSON *id, cJSON *params);

/* schema wird konsumiert (Ownership an Registry); tools/list dupliziert. */
void mcp_registry_add(McpRegistry *r, const char *name, const char *description,
                      cJSON *input_schema, McpToolHandler handler);
```

**Handler-Signatur mit Kontext-Struct** statt der bisherigen Globals
(`global_db`, `g_agent_name`, `g_agent_model`): Die Globals wandern in einen
`McpContext`, den main.c einmal befüllt und der Dispatch an jeden Handler
durchreicht. Verworfen: Signatur `(id, params)` beibehalten und Globals ins
Framework verschieben — funktioniert, zementiert aber den globalen Zustand und
macht Handler untestbar ohne Prozess-Setup.

`tools/list` und `tools/call`-Dispatch werden generisch aus der Registry
erzeugt (Iteration bzw. Name-Lookup, linear — bei <100 Tools irrelevant).

### 2. Schema-Builder

Das bisherige Muster (props-Objekt + `req[]`-Array + `make_schema`) wird durch
Helper ersetzt, die required direkt am Schema pflegen — eine Stelle statt zwei:

```c
cJSON *schema_new(void);  /* {type:"object", properties:{}, required:[]} */
void schema_num (cJSON *s, const char *name, const char *desc, bool required);
void schema_str (cJSON *s, const char *name, const char *desc, bool required);
void schema_bool(cJSON *s, const char *name, const char *desc, bool required);
void schema_num_array(cJSON *s, const char *name, const char *desc, bool required);
```

Ein leeres `required`-Array wird bei Finalisierung entfernt (identisches
Verhalten zu heute: Schemas ohne Pflichtfelder haben kein required-Feld).
Verworfen: deklarative Tabellen (Structs mit Feldbeschreibungen) — mehr
Konzept-Overhead als Nutzen bei dieser Toolzahl.

### 3. Param-Extraktion

Getter ohne eingebaute Fehlerproduktion; die Fehlermeldungen bleiben explizit
im Handler. Grund: Die bestehenden Meldungen sind teils kombiniert ("Missing
required parameters: project_id, status_id, title") und müssen für #338
byte-identisch bleiben — ein Auto-Fehler-Mechanismus würde sie verändern.

```c
bool        param_num (cJSON *params, const char *name, int *out);   /* false wenn fehlt/kein Number */
const char *param_str (cJSON *params, const char *name);             /* NULL wenn fehlt/kein String */
bool        param_bool(cJSON *params, const char *name, bool dflt);
bool        param_is_null(cJSON *params, const char *name);          /* explizites JSON-null (update_ticket description) */
```

Typischer Handler danach:

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

### 4. Fehler-Konventionen

`mcp_tool_ok` / `mcp_tool_err` (und die jsonrpc_*-Helper) ziehen unverändert
ins Framework um und werden exportiert. Semantik bleibt: Tool-Fehler über
`isError:true`-Content-Block, JSON-RPC-error nur für Protokollfehler.
Die inhaltliche Verbesserung der Fehlertexte (SQLSTATE-Mapping statt
"Failed to ...") ist bewusst NICHT Teil des Frameworks — eigenes Ticket #345,
nach der Migration, damit das Mapping zentral an einer Stelle entsteht.

### 5. Modul-Schnittstelle

```c
void kanban_register_tools(McpRegistry *r);   /* src/kanban/kanban_tools.c */
void docs_register_tools(McpRegistry *r);     /* src/docs/docs_tools.c (Epic #329) */
```

Konvention: `<modul>_register_tools`, definiert im Modul-Verzeichnis. Die
Tool-Handler wandern von main.c in `src/kanban/kanban_tools.c` (MCP-Adapter des
Moduls); die Fachlogik (`tickets.c`, `projects.c`, ...) bleibt unberührt.
main.c schrumpft auf: cJSON-Hooks, db_init → McpContext, Registry + Module
registrieren, stdio-Loop.

## Datei-Layout

```
src/mcp/mcp.h        öffentliche API: McpContext, McpRegistry, Handler-Typ,
                     mcp_registry_*, mcp_tool_ok/err, mcp_run_stdio_loop
src/mcp/mcp.c        Registry, generisches tools/list + Dispatch, JSON-RPC-
                     Helpers, initialize-Handler, stdio-Loop
src/mcp/schema.h/.c  schema_* Builder + param_* Getter
src/kanban/kanban_tools.c  alle bestehenden Tool-Handler + Registrierung
```

## Verhaltensgarantie (für #338)

Tool-Namen, Beschreibungen, Schemas, Fehlermeldungen und Erfolgs-Payloads
bleiben byte-identisch. Verifikation: `tools/list`-Dump vor/nach der Migration
diffen; Stichproben-Calls pro Tool-Kategorie.

## Bootstrapping-Hinweis

Dieses Doc ist eine Übergangslösung als Markdown im Repo. Sobald kbai-docs
läuft (Projekt "kbai-docs", #333), wird es als Notes in den Zettelkasten
importiert; danach entstehen kbai-Design-Docs direkt dort.
