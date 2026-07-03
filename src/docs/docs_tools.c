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
 * Read API (kbai-docs #324)
 * ============================================================================ */

static cJSON *tool_docs_get_note(McpContext *ctx, cJSON *id, cJSON *params) {
    int note_id = 0;
    param_num(params, "note_id", &note_id);
    const char *slug = param_str(params, "slug");
    if (note_id <= 0 && !slug)
        return mcp_tool_err(id, "Missing required parameter: note_id or slug");

    char note_id_str[32];
    snprintf(note_id_str, sizeof(note_id_str), "%d", note_id);
    const char *q_params[2] = {note_id > 0 ? note_id_str : NULL, slug};
    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT id, slug, title, kind, tags::text, archived, body, "
        "       created_at::text, updated_at::text, "
        "       last_verified_ticket_id, last_verified_at::text "
        "FROM notes WHERE ($1::int IS NOT NULL AND id = $1) "
        "   OR ($1::int IS NULL AND slug = $2)",
        2, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Note not found");
    }

    cJSON *note = note_row_brief(res, 0);
    cJSON_AddStringToObject(note, "body", PQgetvalue(res, 0, 6));
    cJSON_AddStringToObject(note, "created_at", PQgetvalue(res, 0, 7));
    cJSON_AddStringToObject(note, "updated_at", PQgetvalue(res, 0, 8));
    if (!PQgetisnull(res, 0, 9))
        cJSON_AddNumberToObject(note, "last_verified_ticket_id", atoi(PQgetvalue(res, 0, 9)));
    if (!PQgetisnull(res, 0, 10))
        cJSON_AddStringToObject(note, "last_verified_at", PQgetvalue(res, 0, 10));
    snprintf(note_id_str, sizeof(note_id_str), "%d", atoi(PQgetvalue(res, 0, 0)));
    PQclear(res);

    const char *id_param[1] = {note_id_str};

    /* Projects */
    cJSON *projects = cJSON_CreateArray();
    res = PQexecParams(ctx->db->conn,
        "SELECT project_id FROM note_projects WHERE note_id = $1 ORDER BY project_id",
        1, NULL, id_param, NULL, NULL, 0);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK)
        for (int i = 0; i < PQntuples(res); i++)
            cJSON_AddItemToArray(projects, cJSON_CreateNumber(atoi(PQgetvalue(res, i, 0))));
    if (res) PQclear(res);
    cJSON_AddItemToObject(note, "project_ids", projects);

    /* Link neighbourhood — metadata only, never neighbour bodies: the
     * agent decides which link to follow (token economy). */
    cJSON *links = cJSON_CreateArray();
    res = PQexecParams(ctx->db->conn,
        "SELECT 'outgoing', nl.link_type, n.id, n.slug, n.title, n.kind "
        "  FROM note_links nl JOIN notes n ON n.id = nl.to_note_id "
        " WHERE nl.from_note_id = $1 "
        "UNION ALL "
        "SELECT 'incoming', nl.link_type, n.id, n.slug, n.title, n.kind "
        "  FROM note_links nl JOIN notes n ON n.id = nl.from_note_id "
        " WHERE nl.to_note_id = $1 "
        "ORDER BY 1, 2",
        1, NULL, id_param, NULL, NULL, 0);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); i++) {
            cJSON *l = cJSON_CreateObject();
            cJSON_AddStringToObject(l, "direction", PQgetvalue(res, i, 0));
            cJSON_AddStringToObject(l, "link_type", PQgetvalue(res, i, 1));
            cJSON_AddNumberToObject(l, "note_id", atoi(PQgetvalue(res, i, 2)));
            cJSON_AddStringToObject(l, "slug", PQgetvalue(res, i, 3));
            cJSON_AddStringToObject(l, "title", PQgetvalue(res, i, 4));
            cJSON_AddStringToObject(l, "kind", PQgetvalue(res, i, 5));
            cJSON_AddItemToArray(links, l);
        }
    }
    if (res) PQclear(res);
    cJSON_AddItemToObject(note, "links", links);

    /* Ticket links */
    cJSON *tickets = cJSON_CreateArray();
    res = PQexecParams(ctx->db->conn,
        "SELECT ntl.ticket_id, ntl.relation, t.title "
        "  FROM note_ticket_links ntl JOIN tickets t ON t.id = ntl.ticket_id "
        " WHERE ntl.note_id = $1 ORDER BY ntl.ticket_id",
        1, NULL, id_param, NULL, NULL, 0);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "ticket_id", atoi(PQgetvalue(res, i, 0)));
            cJSON_AddStringToObject(t, "relation", PQgetvalue(res, i, 1));
            cJSON_AddStringToObject(t, "ticket_title", PQgetvalue(res, i, 2));
            cJSON_AddItemToArray(tickets, t);
        }
    }
    if (res) PQclear(res);
    cJSON_AddItemToObject(note, "ticket_links", tickets);

    return mcp_tool_ok(id, note);
}

static cJSON *tool_docs_list_notes(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id = 0, limit = 0, offset = 0;
    param_num(params, "project_id", &project_id);
    param_num(params, "limit", &limit);
    param_num(params, "offset", &offset);
    const char *kind = param_str(params, "kind");
    const char *tag  = param_str(params, "tag");
    bool summary = param_bool(params, "summary", false);
    bool include_archived = param_bool(params, "include_archived", false);

    char proj_str[32], limit_str[32], offset_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    snprintf(limit_str, sizeof(limit_str), "%d", limit);
    snprintf(offset_str, sizeof(offset_str), "%d", offset);

    const char *q_params[6] = {
        project_id > 0 ? proj_str : NULL,
        kind, tag,
        include_archived ? "t" : "f",
        limit > 0 ? limit_str : NULL,
        offset > 0 ? offset_str : NULL
    };
    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT id, slug, title, kind, tags::text, archived, "
        "       length(body) AS body_chars, body, updated_at::text "
        "FROM notes n "
        "WHERE ($1::int IS NULL OR EXISTS (SELECT 1 FROM note_projects np "
        "         WHERE np.note_id = n.id AND np.project_id = $1)) "
        "  AND ($2::text IS NULL OR n.kind = $2) "
        "  AND ($3::text IS NULL OR lower($3) = ANY(n.tags)) "
        "  AND ($4::bool OR NOT n.archived) "
        "ORDER BY n.updated_at DESC "
        "LIMIT COALESCE($5::int, 100) OFFSET COALESCE($6::int, 0)",
        6, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = docs_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to list notes");
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < PQntuples(res); i++) {
        cJSON *o = note_row_brief(res, i);
        cJSON_AddNumberToObject(o, "body_chars", atoi(PQgetvalue(res, i, 6)));
        if (!summary)
            cJSON_AddStringToObject(o, "body", PQgetvalue(res, i, 7));
        cJSON_AddStringToObject(o, "updated_at", PQgetvalue(res, i, 8));
        cJSON_AddItemToArray(arr, o);
    }
    PQclear(res);
    return mcp_tool_ok(id, arr);
}


/* ============================================================================
 * Search (kbai-docs #323)
 * ============================================================================ */

/* Shared filter tail: $2 project, $3 kind, $4 tag, archived always hidden. */
#define SEARCH_FILTERS \
    "  AND NOT n.archived " \
    "  AND ($2::int IS NULL OR EXISTS (SELECT 1 FROM note_projects np " \
    "         WHERE np.note_id = n.id AND np.project_id = $2)) " \
    "  AND ($3::text IS NULL OR n.kind = $3) " \
    "  AND ($4::text IS NULL OR lower($4) = ANY(n.tags)) "

static cJSON *search_rows_to_json(PGresult *res, const char *match_type) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < PQntuples(res); i++) {
        cJSON *o = note_row_brief(res, i);
        cJSON_AddNumberToObject(o, "rank", atof(PQgetvalue(res, i, 6)));
        cJSON_AddStringToObject(o, "snippet", PQgetvalue(res, i, 7));
        cJSON_AddNumberToObject(o, "body_chars", atoi(PQgetvalue(res, i, 8)));
        cJSON_AddStringToObject(o, "match_type", match_type);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static cJSON *tool_docs_search(McpContext *ctx, cJSON *id, cJSON *params) {
    const char *query = param_str(params, "query");
    if (!query || query[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: query");

    int project_id = 0, limit = 0;
    param_num(params, "project_id", &project_id);
    param_num(params, "limit", &limit);
    const char *kind = param_str(params, "kind");
    const char *tag  = param_str(params, "tag");

    char proj_str[32], limit_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    snprintf(limit_str, sizeof(limit_str), "%d", limit);
    const char *q_params[5] = {
        query,
        project_id > 0 ? proj_str : NULL,
        kind, tag,
        limit > 0 ? limit_str : NULL
    };

    /* Primary: weighted full-text search ('simple' config, ADR D3). */
    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT n.id, n.slug, n.title, n.kind, n.tags::text, n.archived, "
        "       ts_rank(n.search_tsv, q)::text AS rank, "
        "       ts_headline('simple', n.body, q, "
        "                   'MaxWords=30, MinWords=10, MaxFragments=2') AS snippet, "
        "       length(n.body) AS body_chars "
        "FROM notes n, websearch_to_tsquery('simple', $1) q "
        "WHERE n.search_tsv @@ q "
        SEARCH_FILTERS
        "ORDER BY ts_rank(n.search_tsv, q) DESC "
        "LIMIT COALESCE($5::int, 20)",
        5, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Search failed (invalid query syntax?)");
    }

    if (PQntuples(res) > 0) {
        cJSON *arr = search_rows_to_json(res, "fts");
        PQclear(res);
        return mcp_tool_ok(id, arr);
    }
    PQclear(res);

    /* Fallback: trigram similarity on titles — catches typos and partial
     * identifiers that FTS tokenisation misses. */
    res = PQexecParams(ctx->db->conn,
        "SELECT n.id, n.slug, n.title, n.kind, n.tags::text, n.archived, "
        "       similarity(n.title, $1)::text AS rank, "
        "       left(n.body, 160) AS snippet, "
        "       length(n.body) AS body_chars "
        "FROM notes n "
        "WHERE n.title % $1 "
        SEARCH_FILTERS
        "ORDER BY similarity(n.title, $1) DESC "
        "LIMIT COALESCE($5::int, 20)",
        5, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Search failed");
    }

    cJSON *arr = search_rows_to_json(res, "title_similarity");
    PQclear(res);
    return mcp_tool_ok(id, arr);
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

    s = schema_new();
    schema_num(s, "note_id", "Numeric note ID (or use slug)", false);
    schema_str(s, "slug",    "Note slug (alternative to note_id)", false);
    mcp_registry_add(r, "kb.ai_docs_get_note",
        "Get one note: full body, tags, projects, verification metadata, plus the link "
        "neighbourhood as METADATA (linked notes with slug/title/link_type and linked "
        "tickets — not their bodies). Follow a link with another get_note call; on a hub "
        "note the 'contains' links ARE the table of contents.",
        s, tool_docs_get_note);

    s = schema_new();
    schema_num(s, "project_id", "Filter by project (optional — omit to list across all projects)", false);
    schema_str(s, "kind",       "Filter by kind: note, adr, or hub (optional). Tip: kind=hub lists the entry points.", false);
    schema_str(s, "tag",        "Filter by tag (optional)", false);
    schema_bool(s, "summary",   "If true, omit bodies — returns metadata plus body_chars only. Use for cheap overviews.", false);
    schema_num(s, "limit",      "Max notes to return (optional, default 100)", false);
    schema_num(s, "offset",     "Notes to skip for pagination (optional, default 0)", false);
    schema_bool(s, "include_archived", "Include archived notes (default false)", false);
    mcp_registry_add(r, "kb.ai_docs_list_notes",
        "List notes ordered by last update. Every entry carries body_chars so you can "
        "judge retrieval cost before calling get_note. Use summary:true + kind=hub to "
        "discover entry points cheaply.",
        s, tool_docs_list_notes);

    s = schema_new();
    schema_str(s, "query",      "Free-text search (words, phrases in quotes, -exclusions). Matches title, tags, and body.", true);
    schema_num(s, "project_id", "Restrict to one project (optional — omit for a global search)", false);
    schema_str(s, "kind",       "Filter by kind: note, adr, or hub (optional)", false);
    schema_str(s, "tag",        "Filter by tag (optional)", false);
    schema_num(s, "limit",      "Max results (optional, default 20)", false);
    mcp_registry_add(r, "kb.ai_docs_search",
        "Full-text search over the knowledge base. Returns ranked matches with a snippet, "
        "match_type (fts, or title_similarity as typo-tolerant fallback) and body_chars "
        "for judging retrieval cost. Use this BEFORE create_note to detect duplicates and "
        "BEFORE reading code to check what is already documented.",
        s, tool_docs_search);
}
