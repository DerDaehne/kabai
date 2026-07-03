#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "docs/docs_tools.h"
#include "mcp/schema.h"
#include "db/transaction.h"

/* ============================================================================
 * Helpers
 *
 * Unlike src/kanban/, this module builds cJSON directly from PGresult —
 * there is no separate struct layer, because the rows are only ever
 * marshalled into tool responses.
 * ============================================================================ */

/* Map constraint violations to actionable tool errors instead of raw
 * Postgres text (acceptance criterion of kbai-docs #332). */
static const char *docs_db_error(PGresult *res) {
    const char *state = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
    const char *constraint = res ? PQresultErrorField(res, PG_DIAG_CONSTRAINT_NAME) : NULL;

    if (state && strcmp(state, "23505") == 0) {  /* unique_violation */
        if (constraint && strcmp(constraint, "notes_slug_key") == 0)
            return "Slug already exists: pick a different slug (slugs are global and permanent)";
        return "Already exists (duplicate link or assignment)";
    }
    if (state && strcmp(state, "23514") == 0) {  /* check_violation */
        if (constraint && strstr(constraint, "kind"))
            return "Invalid kind: must be note, adr, or hub";
        if (constraint && strstr(constraint, "link_type"))
            return "Invalid link_type: must be references, contains, supersedes, or contradicts";
        if (constraint && strstr(constraint, "relation"))
            return "Invalid relation: must be documents, created_by, verified_by, or references";
        if (constraint && strstr(constraint, "self_linked"))
            return "A note cannot link to itself";
        return "Constraint violation";
    }
    if (state && strcmp(state, "23503") == 0)    /* foreign_key_violation */
        return "Referenced note, ticket, or project does not exist";
    return NULL;
}

/* Serialise a JSON string array into a Postgres text[] literal.
 * Tags are lowercased (ASCII) per ADR D4. Returns malloc'd string or NULL
 * if arr is not a pure string array. */
static char *tags_to_pg_array(cJSON *arr) {
    size_t cap = 2;
    cJSON *el;
    cJSON_ArrayForEach(el, arr) {
        if (!cJSON_IsString(el)) return NULL;
        cap += strlen(el->valuestring) * 2 + 4;
    }

    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    char *p = buf;
    *p++ = '{';
    int first = 1;
    cJSON_ArrayForEach(el, arr) {
        if (!first) *p++ = ',';
        first = 0;
        *p++ = '"';
        for (const char *c = el->valuestring; *c; c++) {
            if (*c == '"' || *c == '\\') *p++ = '\\';
            *p++ = (char)tolower((unsigned char)*c);
        }
        *p++ = '"';
    }
    *p++ = '}';
    *p = '\0';
    return buf;
}

/* Row of the notes table (id, slug, title, kind, tags, archived) → object.
 * Column indices must match the SELECT/RETURNING lists used below. */
static cJSON *note_row_brief(PGresult *res, int row) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", atoi(PQgetvalue(res, row, 0)));
    cJSON_AddStringToObject(o, "slug", PQgetvalue(res, row, 1));
    cJSON_AddStringToObject(o, "title", PQgetvalue(res, row, 2));
    cJSON_AddStringToObject(o, "kind", PQgetvalue(res, row, 3));
    cJSON_AddStringToObject(o, "tags", PQgetvalue(res, row, 4));
    cJSON_AddBoolToObject(o, "archived", PQgetvalue(res, row, 5)[0] == 't');
    return o;
}

#define NOTE_BRIEF_COLS "id, slug, title, kind, tags::text, archived"


/* ============================================================================
 * Write API (kbai-docs #332)
 * ============================================================================ */

static cJSON *tool_docs_create_note(McpContext *ctx, cJSON *id, cJSON *params) {
    const char *slug  = param_str(params, "slug");
    const char *title = param_str(params, "title");
    const char *body  = param_str(params, "body");
    if (!slug || !title || !body)
        return mcp_tool_err(id, "Missing required parameters: slug, title, body");

    const char *kind = param_str(params, "kind");
    if (!kind) kind = "note";

    cJSON *tags_j = cJSON_GetObjectItemCaseSensitive(params, "tags");
    char *tags = NULL;
    if (cJSON_IsArray(tags_j)) {
        tags = tags_to_pg_array(tags_j);
        if (!tags) return mcp_tool_err(id, "Invalid tags: must be an array of strings");
    }

    cJSON *projects_j = cJSON_GetObjectItemCaseSensitive(params, "project_ids");
    if (projects_j && !cJSON_IsArray(projects_j)) {
        free(tags);
        return mcp_tool_err(id, "Invalid project_ids: must be an array of numbers");
    }

    if (!db_begin_transaction(ctx->db)) {
        free(tags);
        return mcp_tool_err(id, "Failed to start transaction");
    }

    const char *ins_params[5] = {slug, title, kind, body, tags ? tags : "{}"};
    PGresult *res = PQexecParams(ctx->db->conn,
        "INSERT INTO notes (slug, title, kind, body, tags) "
        "VALUES ($1, $2, $3, $4, $5::text[]) RETURNING " NOTE_BRIEF_COLS,
        5, NULL, ins_params, NULL, NULL, 0);
    free(tags);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = docs_db_error(res);
        if (res) PQclear(res);
        db_rollback_transaction(ctx->db);
        return mcp_tool_err(id, msg ? msg : "Failed to create note");
    }

    cJSON *note = note_row_brief(res, 0);
    char note_id_str[32];
    snprintf(note_id_str, sizeof(note_id_str), "%d", atoi(PQgetvalue(res, 0, 0)));
    PQclear(res);

    cJSON *assigned = cJSON_CreateArray();
    cJSON *pj;
    cJSON_ArrayForEach(pj, projects_j) {
        if (!cJSON_IsNumber(pj)) continue;
        char proj_str[32];
        snprintf(proj_str, sizeof(proj_str), "%d", (int)pj->valueint);
        const char *np_params[2] = {note_id_str, proj_str};
        PGresult *np = PQexecParams(ctx->db->conn,
            "INSERT INTO note_projects (note_id, project_id) VALUES ($1, $2)",
            2, NULL, np_params, NULL, NULL, 0);
        if (!np || PQresultStatus(np) != PGRES_COMMAND_OK) {
            const char *msg = docs_db_error(np);
            if (np) PQclear(np);
            cJSON_Delete(note);
            cJSON_Delete(assigned);
            db_rollback_transaction(ctx->db);
            return mcp_tool_err(id, msg ? msg : "Failed to assign note to project");
        }
        PQclear(np);
        cJSON_AddItemToArray(assigned, cJSON_CreateNumber(pj->valueint));
    }

    if (!db_commit_transaction(ctx->db)) {
        cJSON_Delete(note);
        cJSON_Delete(assigned);
        return mcp_tool_err(id, "Failed to commit transaction");
    }

    cJSON_AddItemToObject(note, "project_ids", assigned);
    return mcp_tool_ok(id, note);
}

static cJSON *tool_docs_update_note(McpContext *ctx, cJSON *id, cJSON *params) {
    int note_id;
    if (!param_num(params, "note_id", &note_id))
        return mcp_tool_err(id, "Missing required parameter: note_id");

    const char *title = param_str(params, "title");
    const char *body  = param_str(params, "body");
    const char *kind  = param_str(params, "kind");
    cJSON *tags_j = cJSON_GetObjectItemCaseSensitive(params, "tags");
    char *tags = NULL;
    if (cJSON_IsArray(tags_j)) {
        tags = tags_to_pg_array(tags_j);
        if (!tags) return mcp_tool_err(id, "Invalid tags: must be an array of strings");
    }
    int ticket_id = 0;
    param_num(params, "ticket_id", &ticket_id);

    if (!title && !body && !kind && !tags) {
        return mcp_tool_err(id, "No updatable fields provided (title, body, kind, tags)");
    }

    char note_id_str[32], ticket_id_str[32];
    snprintf(note_id_str, sizeof(note_id_str), "%d", note_id);
    snprintf(ticket_id_str, sizeof(ticket_id_str), "%d", ticket_id);

    /* COALESCE keeps unspecified fields; updated_by_ticket_id records
     * provenance for the future revision history (#339). */
    const char *upd_params[6] = {
        note_id_str, title, body, kind, tags,
        ticket_id > 0 ? ticket_id_str : NULL
    };
    PGresult *res = PQexecParams(ctx->db->conn,
        "UPDATE notes SET "
        "  title = COALESCE($2, title), "
        "  body  = COALESCE($3, body), "
        "  kind  = COALESCE($4, kind), "
        "  tags  = COALESCE($5::text[], tags), "
        "  updated_by_ticket_id = COALESCE($6::int, updated_by_ticket_id) "
        "WHERE id = $1 RETURNING " NOTE_BRIEF_COLS,
        6, NULL, upd_params, NULL, NULL, 0);
    free(tags);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = docs_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to update note");
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        return mcp_tool_err(id, "Note not found");
    }

    cJSON *note = note_row_brief(res, 0);
    PQclear(res);
    return mcp_tool_ok(id, note);
}

static cJSON *tool_docs_archive_note(McpContext *ctx, cJSON *id, cJSON *params) {
    int note_id;
    if (!param_num(params, "note_id", &note_id))
        return mcp_tool_err(id, "Missing required parameter: note_id");
    const char *reason = param_str(params, "reason");
    if (!reason || reason[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: reason (required for audit trail)");

    char note_id_str[32];
    snprintf(note_id_str, sizeof(note_id_str), "%d", note_id);
    const char *q_params[1] = {note_id_str};
    PGresult *res = PQexecParams(ctx->db->conn,
        "UPDATE notes SET archived = TRUE WHERE id = $1 RETURNING slug",
        1, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Note not found");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "archived_note_id", note_id);
    cJSON_AddStringToObject(r, "slug", PQgetvalue(res, 0, 0));
    cJSON_AddStringToObject(r, "reason", reason);
    PQclear(res);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_docs_link_notes(McpContext *ctx, cJSON *id, cJSON *params) {
    int from_id, to_id;
    const char *link_type = param_str(params, "link_type");
    if (!param_num(params, "from_note_id", &from_id) ||
        !param_num(params, "to_note_id", &to_id) || !link_type)
        return mcp_tool_err(id,
            "Missing required parameters: from_note_id, to_note_id, link_type");

    char from_str[32], to_str[32];
    snprintf(from_str, sizeof(from_str), "%d", from_id);
    snprintf(to_str, sizeof(to_str), "%d", to_id);
    const char *q_params[3] = {from_str, to_str, link_type};
    PGresult *res = PQexecParams(ctx->db->conn,
        "INSERT INTO note_links (from_note_id, to_note_id, link_type) VALUES ($1, $2, $3)",
        3, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        const char *msg = docs_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to link notes");
    }
    PQclear(res);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_docs_unlink_notes(McpContext *ctx, cJSON *id, cJSON *params) {
    int from_id, to_id;
    const char *link_type = param_str(params, "link_type");
    if (!param_num(params, "from_note_id", &from_id) ||
        !param_num(params, "to_note_id", &to_id) || !link_type)
        return mcp_tool_err(id,
            "Missing required parameters: from_note_id, to_note_id, link_type");

    char from_str[32], to_str[32];
    snprintf(from_str, sizeof(from_str), "%d", from_id);
    snprintf(to_str, sizeof(to_str), "%d", to_id);
    const char *q_params[3] = {from_str, to_str, link_type};
    PGresult *res = PQexecParams(ctx->db->conn,
        "DELETE FROM note_links WHERE from_note_id = $1 AND to_note_id = $2 AND link_type = $3",
        3, NULL, q_params, NULL, NULL, 0);

    int deleted = (res && PQresultStatus(res) == PGRES_COMMAND_OK)
                ? atoi(PQcmdTuples(res)) : 0;
    if (res) PQclear(res);
    if (!deleted)
        return mcp_tool_err(id, "Link not found or could not be deleted");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_docs_assign_project(McpContext *ctx, cJSON *id, cJSON *params) {
    int note_id, project_id;
    if (!param_num(params, "note_id", &note_id) ||
        !param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameters: note_id, project_id");

    char note_str[32], proj_str[32];
    snprintf(note_str, sizeof(note_str), "%d", note_id);
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    const char *q_params[2] = {note_str, proj_str};
    PGresult *res = PQexecParams(ctx->db->conn,
        "INSERT INTO note_projects (note_id, project_id) VALUES ($1, $2) "
        "ON CONFLICT DO NOTHING",
        2, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        const char *msg = docs_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to assign note to project");
    }
    PQclear(res);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_docs_unassign_project(McpContext *ctx, cJSON *id, cJSON *params) {
    int note_id, project_id;
    if (!param_num(params, "note_id", &note_id) ||
        !param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameters: note_id, project_id");

    char note_str[32], proj_str[32];
    snprintf(note_str, sizeof(note_str), "%d", note_id);
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    const char *q_params[2] = {note_str, proj_str};
    PGresult *res = PQexecParams(ctx->db->conn,
        "DELETE FROM note_projects WHERE note_id = $1 AND project_id = $2",
        2, NULL, q_params, NULL, NULL, 0);

    int deleted = (res && PQresultStatus(res) == PGRES_COMMAND_OK)
                ? atoi(PQcmdTuples(res)) : 0;
    if (res) PQclear(res);
    if (!deleted)
        return mcp_tool_err(id, "Assignment not found or could not be deleted");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Registration
 * ============================================================================ */

void docs_register_tools(McpRegistry *r) {
    cJSON *s;

    s = schema_new();
    schema_str(s, "slug",  "Global, permanent, human-readable key (kebab-case, e.g. 'delta-compression'). Cannot be changed later.", true);
    schema_str(s, "title", "Note title", true);
    schema_str(s, "body",  "Markdown content. Keep notes ATOMIC: one concept per note — split large topics into several linked notes instead of writing a monolith.", true);
    schema_str(s, "kind",  "'note' (default), 'adr' (architecture decision), or 'hub' (overview page whose 'contains' links define a collection)", false);
    {
        cJSON *arr = cJSON_CreateObject();
        cJSON_AddStringToObject(arr, "type", "array");
        cJSON *items = cJSON_CreateObject();
        cJSON_AddStringToObject(items, "type", "string");
        cJSON_AddItemToObject(arr, "items", items);
        cJSON_AddStringToObject(arr, "description", "Free-form tags (lowercased) for search/filter, e.g. ['network','performance']");
        cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(s, "properties"), "tags", arr);
    }
    schema_num_array(s, "project_ids", "Projects this note belongs to (0..n — the zettelkasten is global, notes may span projects)", false);
    mcp_registry_add(r, "kb.ai_docs_create_note",
        "Create an atomic note in the knowledge base (zettelkasten). "
        "Search with kb.ai_docs_search FIRST to avoid duplicates. "
        "Structure emerges from links, not hierarchy: connect the note to related notes "
        "via kb.ai_docs_link_notes and to the originating ticket via kb.ai_docs_link_ticket.",
        s, tool_docs_create_note);

    s = schema_new();
    schema_num(s, "note_id", "Numeric note ID", true);
    schema_str(s, "title", "New title (optional)", false);
    schema_str(s, "body",  "New markdown body, replaces the old one (optional)", false);
    schema_str(s, "kind",  "New kind: note, adr, or hub (optional)", false);
    {
        cJSON *arr = cJSON_CreateObject();
        cJSON_AddStringToObject(arr, "type", "array");
        cJSON *items = cJSON_CreateObject();
        cJSON_AddStringToObject(items, "type", "string");
        cJSON_AddItemToObject(arr, "items", items);
        cJSON_AddStringToObject(arr, "description", "New tag set, replaces the old one (optional)");
        cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(s, "properties"), "tags", arr);
    }
    schema_num(s, "ticket_id", "Ticket that caused this update (optional but recommended — provenance for the revision history)", false);
    mcp_registry_add(r, "kb.ai_docs_update_note",
        "Update fields of a note. Unspecified fields keep their value; the slug is permanent. "
        "If the old content is superseded rather than corrected, consider a NEW note "
        "plus a 'supersedes' link instead of overwriting history.",
        s, tool_docs_update_note);

    s = schema_new();
    schema_num(s, "note_id", "Numeric note ID", true);
    schema_str(s, "reason",  "Required reason for archiving (audit trail — record it in your ticket's work log too)", true);
    mcp_registry_add(r, "kb.ai_docs_archive_note",
        "Archive (soft-delete) a note. Archived notes keep their links and stay resolvable "
        "but are excluded from search and listings by default. There is no hard delete: "
        "tickets may reference the note.",
        s, tool_docs_archive_note);

    s = schema_new();
    schema_num(s, "from_note_id", "Source note ID", true);
    schema_num(s, "to_note_id",   "Target note ID", true);
    schema_str(s, "link_type",
        "One of: references (generic), contains (hub -> member, defines overview pages), "
        "supersedes (from replaces to), contradicts (marks a detected inconsistency for review)", true);
    mcp_registry_add(r, "kb.ai_docs_link_notes",
        "Create a directed, typed link between two notes. Links are the structure of the "
        "zettelkasten: hubs use 'contains' to define collections, ADR succession uses "
        "'supersedes', detected inconsistencies get 'contradicts'.",
        s, tool_docs_link_notes);

    s = schema_new();
    schema_num(s, "from_note_id", "Source note ID", true);
    schema_num(s, "to_note_id",   "Target note ID", true);
    schema_str(s, "link_type",    "The link to remove (must match exactly what was created)", true);
    mcp_registry_add(r, "kb.ai_docs_unlink_notes",
        "Remove a directed link between two notes", s, tool_docs_unlink_notes);

    s = schema_new();
    schema_num(s, "note_id",    "Numeric note ID", true);
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kb.ai_docs_assign_project",
        "Assign a note to a project (n:m — a note may belong to several projects, "
        "e.g. process knowledge shared across teams). Idempotent.",
        s, tool_docs_assign_project);

    s = schema_new();
    schema_num(s, "note_id",    "Numeric note ID", true);
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kb.ai_docs_unassign_project",
        "Remove a note's assignment to a project", s, tool_docs_unassign_project);
}
