#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canvas/canvas_tools.h"
#include "mcp/schema.h"

/* ============================================================================
 * Schema helper: a free-form JSON object parameter (content). Mirrors the
 * inline array-property pattern docs_tools.c uses for "tags" — there is no
 * shared schema_object() in mcp/schema.h because canvas is the first module
 * that needs an object-typed tool parameter.
 * ============================================================================ */

static void schema_object(cJSON *s, const char *name, const char *desc, bool required) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "object");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(s, "properties"), name, p);
    if (required)
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(s, "required"),
                             cJSON_CreateString(name));
}

/* ============================================================================
 * Error mapping (mirrors docs_db_error / attachment_db_error)
 * ============================================================================ */

static const char *canvas_db_error(PGresult *res) {
    const char *state      = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
    const char *constraint = res ? PQresultErrorField(res, PG_DIAG_CONSTRAINT_NAME) : NULL;
    const char *primary     = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : NULL;

    /* Trigger RAISE EXCEPTION (verify_canvas_element_parent / _edge_endpoints,
     * V12): the message text is already speaking ("Canvas integrity: ...") —
     * pass it through unchanged, same as kanban_db_error does for P0001. */
    if (state && strcmp(state, "P0001") == 0 && primary)
        return primary;

    if (state && strcmp(state, "23514") == 0) {  /* check_violation */
        if (constraint && strstr(constraint, "canvas_elements_type"))
            return "Invalid element type: must be text, image, sketch, ref, or frame";
        if (constraint && strstr(constraint, "not_self"))
            return "An edge cannot connect an element to itself";
        return "Constraint violation";
    }
    if (state && strcmp(state, "23503") == 0) {  /* foreign_key_violation */
        if (constraint && strstr(constraint, "parent_frame_id"))
            return "parent_frame_id does not exist: check kabai_get_canvas";
        if (constraint && strstr(constraint, "from_element_id"))
            return "from_element_id does not exist: check kabai_get_canvas";
        if (constraint && strstr(constraint, "to_element_id"))
            return "to_element_id does not exist: check kabai_get_canvas";
        if (constraint && strstr(constraint, "canvas_id"))
            return "canvas_id does not exist: check kabai_list_canvases";
        if (constraint && strstr(constraint, "project_id"))
            return "project_id does not exist: check kabai_list_projects";
        return "Referenced row does not exist (foreign key violation)";
    }
    return NULL;
}

/* ============================================================================
 * Row helpers
 * ============================================================================ */

/* JSONB columns arrive as text over the wire; parse into structured JSON so
 * agents get a real object instead of an escaped string-in-string. content
 * is NOT NULL JSONB, so parse failure should not happen in practice — the
 * empty-object fallback only guards against a corrupt row. */
static cJSON *parse_jsonb(const char *text) {
    cJSON *v = text ? cJSON_Parse(text) : NULL;
    return v ? v : cJSON_CreateObject();
}

static cJSON *element_row_to_json(PGresult *res, int row) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", atoi(PQgetvalue(res, row, 0)));
    cJSON_AddStringToObject(o, "type", PQgetvalue(res, row, 1));
    cJSON_AddNumberToObject(o, "position_x", atof(PQgetvalue(res, row, 2)));
    cJSON_AddNumberToObject(o, "position_y", atof(PQgetvalue(res, row, 3)));
    if (!PQgetisnull(res, row, 4)) cJSON_AddNumberToObject(o, "width", atof(PQgetvalue(res, row, 4)));
    if (!PQgetisnull(res, row, 5)) cJSON_AddNumberToObject(o, "height", atof(PQgetvalue(res, row, 5)));
    cJSON_AddNumberToObject(o, "z_order", atoi(PQgetvalue(res, row, 6)));
    cJSON_AddItemToObject(o, "content", parse_jsonb(PQgetvalue(res, row, 7)));
    /* description is always present in the response (possibly null) — the
     * alt-text gap for image/sketch elements must be visible, never omitted
     * silently (concept decision 8: agents/UI show a "missing" indicator). */
    if (!PQgetisnull(res, row, 8))
        cJSON_AddStringToObject(o, "description", PQgetvalue(res, row, 8));
    else
        cJSON_AddNullToObject(o, "description");
    if (!PQgetisnull(res, row, 9))
        cJSON_AddNumberToObject(o, "parent_frame_id", atoi(PQgetvalue(res, row, 9)));
    cJSON_AddStringToObject(o, "created_at", PQgetvalue(res, row, 10));
    cJSON_AddStringToObject(o, "updated_at", PQgetvalue(res, row, 11));
    return o;
}

#define ELEMENT_COLS \
    "id, type, position_x::text, position_y::text, width::text, height::text, " \
    "z_order, content::text, description, parent_frame_id, " \
    "created_at::text, updated_at::text"

static cJSON *edge_row_to_json(PGresult *res, int row) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", atoi(PQgetvalue(res, row, 0)));
    cJSON_AddNumberToObject(o, "from_element_id", atoi(PQgetvalue(res, row, 1)));
    cJSON_AddNumberToObject(o, "to_element_id", atoi(PQgetvalue(res, row, 2)));
    if (!PQgetisnull(res, row, 3))
        cJSON_AddStringToObject(o, "label", PQgetvalue(res, row, 3));
    cJSON_AddStringToObject(o, "created_at", PQgetvalue(res, row, 4));
    return o;
}

#define EDGE_COLS "id, from_element_id, to_element_id, label, created_at::text"

/* description is mandatory for image/sketch (concept decision 8: alt-text
 * for non-multimodal agents) — enforced here, not in the schema (V12 leaves
 * canvas_elements.description nullable on purpose; the requirement is a
 * per-type MCP-layer policy, same reasoning as attachments.description in
 * V13/ADR-004). */
static int description_required(const char *type) {
    return type && (strcmp(type, "image") == 0 || strcmp(type, "sketch") == 0);
}

/* Existence check for ref-type content ({"target_type":"ticket"|"note","target_id":N}).
 * References are deliberately cross-project (concept decision 1). Returns
 * 1 if the target exists, 0 otherwise (including on an unrecognised
 * target_type, which the caller treats as a validation failure). */
static int ref_target_exists(McpContext *ctx, const char *target_type, int target_id) {
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", target_id);
    const char *q_params[1] = {id_str};
    const char *sql;

    if (strcmp(target_type, "ticket") == 0)
        sql = "SELECT 1 FROM tickets WHERE id = $1";
    else if (strcmp(target_type, "note") == 0)
        sql = "SELECT 1 FROM notes WHERE id = $1";
    else
        return 0;

    PGresult *res = PQexecParams(ctx->db->conn, sql, 1, NULL, q_params, NULL, NULL, 0);
    int exists = (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    if (res) PQclear(res);
    return exists;
}


/* ============================================================================
 * Canvas CRUD
 * ============================================================================ */

static cJSON *tool_create_canvas(McpContext *ctx, cJSON *id, cJSON *params) {
    const char *name = param_str(params, "name");
    if (!name || name[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: name");

    cJSON *projects_j = cJSON_GetObjectItemCaseSensitive(params, "project_ids");
    if (projects_j && !cJSON_IsArray(projects_j))
        return mcp_tool_err(id, "Invalid project_ids: must be an array of numbers");

    const char *ins_params[1] = {name};
    PGresult *res = PQexecParams(ctx->db->conn,
        "INSERT INTO canvases (name) VALUES ($1) RETURNING id, name, created_at::text, updated_at::text",
        1, NULL, ins_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = canvas_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to create canvas");
    }

    cJSON *canvas = cJSON_CreateObject();
    cJSON_AddNumberToObject(canvas, "id", atoi(PQgetvalue(res, 0, 0)));
    cJSON_AddStringToObject(canvas, "name", PQgetvalue(res, 0, 1));
    cJSON_AddStringToObject(canvas, "created_at", PQgetvalue(res, 0, 2));
    cJSON_AddStringToObject(canvas, "updated_at", PQgetvalue(res, 0, 3));
    char canvas_id_str[32];
    snprintf(canvas_id_str, sizeof(canvas_id_str), "%d", atoi(PQgetvalue(res, 0, 0)));
    PQclear(res);

    cJSON *assigned = cJSON_CreateArray();
    cJSON *pj;
    cJSON_ArrayForEach(pj, projects_j) {
        if (!cJSON_IsNumber(pj)) continue;
        char proj_str[32];
        snprintf(proj_str, sizeof(proj_str), "%d", (int)pj->valueint);
        const char *cp_params[2] = {canvas_id_str, proj_str};
        PGresult *cp = PQexecParams(ctx->db->conn,
            "INSERT INTO canvas_projects (canvas_id, project_id) VALUES ($1, $2)",
            2, NULL, cp_params, NULL, NULL, 0);
        if (!cp || PQresultStatus(cp) != PGRES_COMMAND_OK) {
            const char *msg = canvas_db_error(cp);
            if (cp) PQclear(cp);
            cJSON_Delete(canvas);
            cJSON_Delete(assigned);
            return mcp_tool_err(id, msg ? msg : "Failed to assign canvas to project");
        }
        PQclear(cp);
        cJSON_AddItemToArray(assigned, cJSON_CreateNumber(pj->valueint));
    }

    cJSON_AddItemToObject(canvas, "project_ids", assigned);
    return mcp_tool_ok(id, canvas);
}

static cJSON *tool_update_canvas(McpContext *ctx, cJSON *id, cJSON *params) {
    int canvas_id;
    if (!param_num(params, "canvas_id", &canvas_id))
        return mcp_tool_err(id, "Missing required parameter: canvas_id");
    const char *name = param_str(params, "name");
    if (!name || name[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: name");

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", canvas_id);
    const char *q_params[2] = {id_str, name};
    PGresult *res = PQexecParams(ctx->db->conn,
        "UPDATE canvases SET name = $2 WHERE id = $1 "
        "RETURNING id, name, created_at::text, updated_at::text",
        2, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = canvas_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to update canvas");
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        return mcp_tool_err(id, "Canvas not found");
    }

    cJSON *canvas = cJSON_CreateObject();
    cJSON_AddNumberToObject(canvas, "id", atoi(PQgetvalue(res, 0, 0)));
    cJSON_AddStringToObject(canvas, "name", PQgetvalue(res, 0, 1));
    cJSON_AddStringToObject(canvas, "created_at", PQgetvalue(res, 0, 2));
    cJSON_AddStringToObject(canvas, "updated_at", PQgetvalue(res, 0, 3));
    PQclear(res);
    return mcp_tool_ok(id, canvas);
}

static cJSON *tool_delete_canvas(McpContext *ctx, cJSON *id, cJSON *params) {
    int canvas_id;
    if (!param_num(params, "canvas_id", &canvas_id))
        return mcp_tool_err(id, "Missing required parameter: canvas_id");
    const char *reason = param_str(params, "reason");
    if (!reason || reason[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: reason (required for audit trail)");

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", canvas_id);
    const char *q_params[1] = {id_str};
    /* Cascades canvas_projects, canvas_elements (and via those, canvas_edges
     * and any parent_frame_id references) — see V12. */
    PGresult *res = PQexecParams(ctx->db->conn,
        "DELETE FROM canvases WHERE id = $1 RETURNING name",
        1, NULL, q_params, NULL, NULL, 0);

    /* DELETE ... RETURNING yields PGRES_TUPLES_OK, not PGRES_COMMAND_OK
     * (that status is only for DML without RETURNING). */
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Canvas not found");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "deleted_canvas_id", canvas_id);
    cJSON_AddStringToObject(r, "name", PQgetvalue(res, 0, 0));
    cJSON_AddStringToObject(r, "reason", reason);
    PQclear(res);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_list_canvases(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id = 0, limit = 0, offset = 0;
    param_num(params, "project_id", &project_id);
    param_num(params, "limit", &limit);
    param_num(params, "offset", &offset);

    char proj_str[32], limit_str[32], offset_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    snprintf(limit_str, sizeof(limit_str), "%d", limit);
    snprintf(offset_str, sizeof(offset_str), "%d", offset);

    const char *q_params[3] = {
        project_id > 0 ? proj_str : NULL,
        limit > 0 ? limit_str : NULL,
        offset > 0 ? offset_str : NULL
    };
    /* project_ids/full elements are deliberately NOT included here (cheap
     * overview, same split as kabai_docs_list_notes vs get_note) — call
     * kabai_get_canvas for the full picture. */
    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT c.id, c.name, c.created_at::text, c.updated_at::text, "
        "       (SELECT COUNT(*) FROM canvas_elements ce WHERE ce.canvas_id = c.id) "
        "FROM canvases c "
        "WHERE ($1::int IS NULL OR EXISTS (SELECT 1 FROM canvas_projects cp "
        "         WHERE cp.canvas_id = c.id AND cp.project_id = $1)) "
        "ORDER BY c.updated_at DESC "
        "LIMIT COALESCE($2::int, 100) OFFSET COALESCE($3::int, 0)",
        3, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Failed to list canvases");
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < PQntuples(res); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", atoi(PQgetvalue(res, i, 0)));
        cJSON_AddStringToObject(o, "name", PQgetvalue(res, i, 1));
        cJSON_AddStringToObject(o, "created_at", PQgetvalue(res, i, 2));
        cJSON_AddStringToObject(o, "updated_at", PQgetvalue(res, i, 3));
        cJSON_AddNumberToObject(o, "element_count", atoi(PQgetvalue(res, i, 4)));
        cJSON_AddItemToArray(arr, o);
    }
    PQclear(res);
    return mcp_tool_ok(id, arr);
}

static cJSON *tool_get_canvas(McpContext *ctx, cJSON *id, cJSON *params) {
    int canvas_id;
    if (!param_num(params, "canvas_id", &canvas_id))
        return mcp_tool_err(id, "Missing required parameter: canvas_id");

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", canvas_id);
    const char *id_param[1] = {id_str};

    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT id, name, created_at::text, updated_at::text FROM canvases WHERE id = $1",
        1, NULL, id_param, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Canvas not found");
    }

    cJSON *canvas = cJSON_CreateObject();
    cJSON_AddNumberToObject(canvas, "id", atoi(PQgetvalue(res, 0, 0)));
    cJSON_AddStringToObject(canvas, "name", PQgetvalue(res, 0, 1));
    cJSON_AddStringToObject(canvas, "created_at", PQgetvalue(res, 0, 2));
    cJSON_AddStringToObject(canvas, "updated_at", PQgetvalue(res, 0, 3));
    PQclear(res);

    cJSON *projects = cJSON_CreateArray();
    res = PQexecParams(ctx->db->conn,
        "SELECT project_id FROM canvas_projects WHERE canvas_id = $1 ORDER BY project_id",
        1, NULL, id_param, NULL, NULL, 0);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK)
        for (int i = 0; i < PQntuples(res); i++)
            cJSON_AddItemToArray(projects, cJSON_CreateNumber(atoi(PQgetvalue(res, i, 0))));
    if (res) PQclear(res);
    cJSON_AddItemToObject(canvas, "project_ids", projects);

    cJSON *elements = cJSON_CreateArray();
    res = PQexecParams(ctx->db->conn,
        "SELECT " ELEMENT_COLS " FROM canvas_elements WHERE canvas_id = $1 ORDER BY id",
        1, NULL, id_param, NULL, NULL, 0);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK)
        for (int i = 0; i < PQntuples(res); i++)
            cJSON_AddItemToArray(elements, element_row_to_json(res, i));
    if (res) PQclear(res);
    cJSON_AddItemToObject(canvas, "elements", elements);

    cJSON *edges = cJSON_CreateArray();
    res = PQexecParams(ctx->db->conn,
        "SELECT " EDGE_COLS " FROM canvas_edges WHERE canvas_id = $1 ORDER BY id",
        1, NULL, id_param, NULL, NULL, 0);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK)
        for (int i = 0; i < PQntuples(res); i++)
            cJSON_AddItemToArray(edges, edge_row_to_json(res, i));
    if (res) PQclear(res);
    cJSON_AddItemToObject(canvas, "edges", edges);

    return mcp_tool_ok(id, canvas);
}


/* ============================================================================
 * Project links
 * ============================================================================ */

static cJSON *tool_canvas_assign_project(McpContext *ctx, cJSON *id, cJSON *params) {
    int canvas_id, project_id;
    if (!param_num(params, "canvas_id", &canvas_id) ||
        !param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameters: canvas_id, project_id");

    char canvas_str[32], proj_str[32];
    snprintf(canvas_str, sizeof(canvas_str), "%d", canvas_id);
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    const char *q_params[2] = {canvas_str, proj_str};
    PGresult *res = PQexecParams(ctx->db->conn,
        "INSERT INTO canvas_projects (canvas_id, project_id) VALUES ($1, $2) "
        "ON CONFLICT DO NOTHING",
        2, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        const char *msg = canvas_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to assign canvas to project");
    }
    PQclear(res);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_canvas_unassign_project(McpContext *ctx, cJSON *id, cJSON *params) {
    int canvas_id, project_id;
    if (!param_num(params, "canvas_id", &canvas_id) ||
        !param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameters: canvas_id, project_id");

    char canvas_str[32], proj_str[32];
    snprintf(canvas_str, sizeof(canvas_str), "%d", canvas_id);
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    const char *q_params[2] = {canvas_str, proj_str};
    PGresult *res = PQexecParams(ctx->db->conn,
        "DELETE FROM canvas_projects WHERE canvas_id = $1 AND project_id = $2",
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
 * Elements
 * ============================================================================ */

static cJSON *tool_add_canvas_element(McpContext *ctx, cJSON *id, cJSON *params) {
    int canvas_id;
    const char *type = param_str(params, "type");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(params, "content");
    if (!param_num(params, "canvas_id", &canvas_id) || !type || !content)
        return mcp_tool_err(id, "Missing required parameters: canvas_id, type, content");
    if (!cJSON_IsObject(content))
        return mcp_tool_err(id, "Invalid content: must be a JSON object");

    const char *description = param_str(params, "description");
    if (description_required(type) && (!description || description[0] == '\0'))
        return mcp_tool_err(id,
            "description (alt-text) is required for image and sketch elements — "
            "it is how non-multimodal agents understand what the image/sketch shows");

    if (strcmp(type, "ref") == 0) {
        const char *target_type = param_str(content, "target_type");
        int target_id;
        if (!target_type || !param_num(content, "target_id", &target_id))
            return mcp_tool_err(id,
                "ref elements require content.target_type ('ticket' or 'note') and content.target_id");
        if (!ref_target_exists(ctx, target_type, target_id)) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "ref target not found: no %s with id %d (target_type must be 'ticket' or 'note')",
                target_type, target_id);
            return mcp_tool_err(id, buf);
        }
    }

    double position_x = 0, position_y = 0;
    int has_width = 0, has_height = 0, z_order = 0, parent_frame_id = 0;
    {
        cJSON *v;
        if ((v = cJSON_GetObjectItemCaseSensitive(params, "position_x")) && cJSON_IsNumber(v)) position_x = v->valuedouble;
        if ((v = cJSON_GetObjectItemCaseSensitive(params, "position_y")) && cJSON_IsNumber(v)) position_y = v->valuedouble;
    }
    cJSON *width_j = cJSON_GetObjectItemCaseSensitive(params, "width");
    cJSON *height_j = cJSON_GetObjectItemCaseSensitive(params, "height");
    has_width  = width_j  && cJSON_IsNumber(width_j);
    has_height = height_j && cJSON_IsNumber(height_j);
    param_num(params, "z_order", &z_order);
    int has_parent = param_num(params, "parent_frame_id", &parent_frame_id);

    char canvas_str[32], px_str[32], py_str[32], w_str[32], h_str[32], z_str[32], parent_str[32];
    snprintf(canvas_str, sizeof(canvas_str), "%d", canvas_id);
    snprintf(px_str, sizeof(px_str), "%f", position_x);
    snprintf(py_str, sizeof(py_str), "%f", position_y);
    if (has_width)  snprintf(w_str, sizeof(w_str), "%f", width_j->valuedouble);
    if (has_height) snprintf(h_str, sizeof(h_str), "%f", height_j->valuedouble);
    snprintf(z_str, sizeof(z_str), "%d", z_order);
    if (has_parent) snprintf(parent_str, sizeof(parent_str), "%d", parent_frame_id);

    char *content_text = cJSON_PrintUnformatted(content);
    const char *ins_params[9] = {
        canvas_str, type, px_str, py_str,
        has_width  ? w_str : NULL,
        has_height ? h_str : NULL,
        z_str, content_text, description
    };
    /* parent_frame_id passed separately below since it needs its own NULL
     * handling and pushes the param count past a clean fixed array. */
    char sql[900];
    snprintf(sql, sizeof(sql),
        "INSERT INTO canvas_elements "
        "(canvas_id, type, position_x, position_y, width, height, z_order, content, description%s) "
        "VALUES ($1, $2, $3::double precision, $4::double precision, $5::double precision, "
        "$6::double precision, $7::int, $8::jsonb, $9%s) "
        "RETURNING " ELEMENT_COLS,
        has_parent ? ", parent_frame_id" : "",
        has_parent ? ", $10::int" : "");

    PGresult *res;
    if (has_parent) {
        const char *ins_params10[10] = {
            canvas_str, type, px_str, py_str,
            has_width ? w_str : NULL, has_height ? h_str : NULL,
            z_str, content_text, description, parent_str
        };
        res = PQexecParams(ctx->db->conn, sql, 10, NULL, ins_params10, NULL, NULL, 0);
    } else {
        res = PQexecParams(ctx->db->conn, sql, 9, NULL, ins_params, NULL, NULL, 0);
    }
    cJSON_free(content_text);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = canvas_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to add canvas element");
    }

    cJSON *element = element_row_to_json(res, 0);
    PQclear(res);
    return mcp_tool_ok(id, element);
}

static cJSON *tool_update_canvas_element(McpContext *ctx, cJSON *id, cJSON *params) {
    int element_id;
    if (!param_num(params, "element_id", &element_id))
        return mcp_tool_err(id, "Missing required parameter: element_id");

    char eid_str[32];
    snprintf(eid_str, sizeof(eid_str), "%d", element_id);
    const char *eid_param[1] = {eid_str};

    PGresult *cur = PQexecParams(ctx->db->conn,
        "SELECT type, description FROM canvas_elements WHERE id = $1",
        1, NULL, eid_param, NULL, NULL, 0);
    if (!cur || PQresultStatus(cur) != PGRES_TUPLES_OK || PQntuples(cur) == 0) {
        if (cur) PQclear(cur);
        return mcp_tool_err(id, "Canvas element not found");
    }
    char current_type[20];
    snprintf(current_type, sizeof(current_type), "%s", PQgetvalue(cur, 0, 0));
    PQclear(cur);

    cJSON *content = cJSON_GetObjectItemCaseSensitive(params, "content");
    if (content && !cJSON_IsObject(content))
        return mcp_tool_err(id, "Invalid content: must be a JSON object");

    bool description_present = param_present(params, "description");
    const char *description = param_str(params, "description");
    /* Absent parameter, explicit null, and empty string are all treated as
     * "clear the alt-text" — rejected outright for image/sketch elements. */
    bool clear_description = description_present && (!description || description[0] == '\0');
    if (clear_description && description_required(current_type))
        return mcp_tool_err(id,
            "description (alt-text) cannot be cleared on an image/sketch element — "
            "it is how non-multimodal agents understand what it shows");

    char *content_text = content ? cJSON_PrintUnformatted(content) : NULL;

    cJSON *v;
    double position_x = 0, position_y = 0, width = 0, height = 0;
    int z_order = 0, parent_frame_id = 0;
    bool has_px = false, has_py = false, has_w = false, has_h = false, has_z = false;
    bool clear_parent = param_is_null(params, "parent_frame_id");
    bool has_parent = !clear_parent && param_num(params, "parent_frame_id", &parent_frame_id);

    if ((v = cJSON_GetObjectItemCaseSensitive(params, "position_x")) && cJSON_IsNumber(v)) { position_x = v->valuedouble; has_px = true; }
    if ((v = cJSON_GetObjectItemCaseSensitive(params, "position_y")) && cJSON_IsNumber(v)) { position_y = v->valuedouble; has_py = true; }
    if ((v = cJSON_GetObjectItemCaseSensitive(params, "width"))      && cJSON_IsNumber(v)) { width      = v->valuedouble; has_w  = true; }
    if ((v = cJSON_GetObjectItemCaseSensitive(params, "height"))     && cJSON_IsNumber(v)) { height     = v->valuedouble; has_h  = true; }
    has_z = param_num(params, "z_order", &z_order);

    char px_str[32], py_str[32], w_str[32], h_str[32], z_str[32], parent_str[32];
    if (has_px) snprintf(px_str, sizeof(px_str), "%f", position_x);
    if (has_py) snprintf(py_str, sizeof(py_str), "%f", position_y);
    if (has_w)  snprintf(w_str, sizeof(w_str), "%f", width);
    if (has_h)  snprintf(h_str, sizeof(h_str), "%f", height);
    if (has_z)  snprintf(z_str, sizeof(z_str), "%d", z_order);
    if (has_parent) snprintf(parent_str, sizeof(parent_str), "%d", parent_frame_id);

    const char *q_params[11] = {
        eid_str,
        content_text,
        clear_description ? NULL : (description_present ? description : NULL),
        has_px ? px_str : NULL,
        has_py ? py_str : NULL,
        has_w  ? w_str  : NULL,
        has_h  ? h_str  : NULL,
        has_z  ? z_str  : NULL,
        has_parent ? parent_str : NULL,
        clear_parent ? "1" : NULL,       /* $10: non-NULL means "detach from frame" */
        clear_description ? "1" : NULL   /* $11: non-NULL means "clear alt-text" */
    };

    PGresult *res = PQexecParams(ctx->db->conn,
        "UPDATE canvas_elements SET "
        "  content         = COALESCE($2::jsonb, content), "
        "  description     = CASE WHEN $11::text IS NOT NULL THEN NULL "
        "                          WHEN $3::text IS NOT NULL THEN $3 ELSE description END, "
        "  position_x      = COALESCE($4::double precision, position_x), "
        "  position_y      = COALESCE($5::double precision, position_y), "
        "  width           = COALESCE($6::double precision, width), "
        "  height          = COALESCE($7::double precision, height), "
        "  z_order         = COALESCE($8::int, z_order), "
        "  parent_frame_id = CASE WHEN $10::text IS NOT NULL THEN NULL "
        "                          ELSE COALESCE($9::int, parent_frame_id) END "
        "WHERE id = $1 RETURNING " ELEMENT_COLS,
        11, NULL, q_params, NULL, NULL, 0);
    if (content_text) cJSON_free(content_text);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = canvas_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to update canvas element");
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        return mcp_tool_err(id, "Canvas element not found");
    }

    cJSON *element = element_row_to_json(res, 0);
    PQclear(res);
    return mcp_tool_ok(id, element);
}

static cJSON *tool_delete_canvas_element(McpContext *ctx, cJSON *id, cJSON *params) {
    int element_id;
    if (!param_num(params, "element_id", &element_id))
        return mcp_tool_err(id, "Missing required parameter: element_id");

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", element_id);
    const char *q_params[1] = {id_str};
    /* Cascades canvas_edges touching this element; children whose
     * parent_frame_id pointed here are freed (SET NULL), not deleted. */
    PGresult *res = PQexecParams(ctx->db->conn,
        "DELETE FROM canvas_elements WHERE id = $1",
        1, NULL, q_params, NULL, NULL, 0);

    int deleted = (res && PQresultStatus(res) == PGRES_COMMAND_OK)
                ? atoi(PQcmdTuples(res)) : 0;
    if (res) PQclear(res);
    if (!deleted)
        return mcp_tool_err(id, "Canvas element not found");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Edges
 * ============================================================================ */

static cJSON *tool_add_canvas_edge(McpContext *ctx, cJSON *id, cJSON *params) {
    int canvas_id, from_id, to_id;
    if (!param_num(params, "canvas_id", &canvas_id) ||
        !param_num(params, "from_element_id", &from_id) ||
        !param_num(params, "to_element_id", &to_id))
        return mcp_tool_err(id,
            "Missing required parameters: canvas_id, from_element_id, to_element_id");
    const char *label = param_str(params, "label");

    char canvas_str[32], from_str[32], to_str[32];
    snprintf(canvas_str, sizeof(canvas_str), "%d", canvas_id);
    snprintf(from_str, sizeof(from_str), "%d", from_id);
    snprintf(to_str, sizeof(to_str), "%d", to_id);
    const char *q_params[4] = {canvas_str, from_str, to_str, label};

    PGresult *res = PQexecParams(ctx->db->conn,
        "INSERT INTO canvas_edges (canvas_id, from_element_id, to_element_id, label) "
        "VALUES ($1, $2, $3, $4) RETURNING " EDGE_COLS,
        4, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = canvas_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to add canvas edge");
    }

    cJSON *edge = edge_row_to_json(res, 0);
    PQclear(res);
    return mcp_tool_ok(id, edge);
}

static cJSON *tool_delete_canvas_edge(McpContext *ctx, cJSON *id, cJSON *params) {
    int edge_id;
    if (!param_num(params, "edge_id", &edge_id))
        return mcp_tool_err(id, "Missing required parameter: edge_id");

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", edge_id);
    const char *q_params[1] = {id_str};
    PGresult *res = PQexecParams(ctx->db->conn,
        "DELETE FROM canvas_edges WHERE id = $1",
        1, NULL, q_params, NULL, NULL, 0);

    int deleted = (res && PQresultStatus(res) == PGRES_COMMAND_OK)
                ? atoi(PQcmdTuples(res)) : 0;
    if (res) PQclear(res);
    if (!deleted)
        return mcp_tool_err(id, "Canvas edge not found");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Registration
 * ============================================================================ */

void canvas_register_tools(McpRegistry *r) {
    cJSON *s;

    s = schema_new();
    schema_str(s, "name", "Canvas name", true);
    schema_num_array(s, "project_ids",
        "Projects to link this canvas to (0..n — a canvas is a standalone entity, "
        "not owned by any project; elements may reference tickets/epics/notes from "
        "ANY project regardless of this list)", false);
    mcp_registry_add(r, "kabai_create_canvas",
        "Create a new canvas: a cross-project planning surface above epics (frames "
        "replace milestones). Optionally link it to projects right away with "
        "project_ids, or later via kabai_canvas_assign_project.",
        s, tool_create_canvas);

    s = schema_new();
    schema_num(s, "canvas_id", "Numeric canvas ID", true);
    schema_str(s, "name", "New canvas name", true);
    mcp_registry_add(r, "kabai_update_canvas", "Rename a canvas", s, tool_update_canvas);

    s = schema_new();
    schema_num(s, "canvas_id", "Numeric canvas ID", true);
    schema_str(s, "reason", "Required reason for deletion (audit trail)", true);
    mcp_registry_add(r, "kabai_delete_canvas",
        "Permanently delete a canvas and everything on it (elements, edges, project "
        "links cascade). Requires a non-empty reason.",
        s, tool_delete_canvas);

    s = schema_new();
    schema_num(s, "project_id", "Filter by project (optional — omit to list across all projects)", false);
    schema_num(s, "limit",  "Max canvases to return (optional, default 100)", false);
    schema_num(s, "offset", "Canvases to skip for pagination (optional, default 0)", false);
    mcp_registry_add(r, "kabai_list_canvases",
        "List canvases (cheap overview: id, name, timestamps, element_count — no "
        "elements/edges/project_ids). Use kabai_get_canvas for the full picture.",
        s, tool_list_canvases);

    s = schema_new();
    schema_num(s, "canvas_id", "Numeric canvas ID", true);
    mcp_registry_add(r, "kabai_get_canvas",
        "Get a canvas with ALL its elements and edges. image/sketch elements always "
        "carry their description (alt-text) — read it before deciding whether you "
        "need the actual image via kabai_get_attachment (never fetch automatically). "
        "ref elements point at a ticket or note by id; frame elements group other "
        "elements via their parent_frame_id.",
        s, tool_get_canvas);

    s = schema_new();
    schema_num(s, "canvas_id",  "Numeric canvas ID", true);
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kabai_canvas_assign_project",
        "Link a canvas to a project (n:m, idempotent). This only affects where the "
        "canvas is discoverable via kabai_list_canvases — it does NOT restrict which "
        "tickets/notes its ref elements may point at.",
        s, tool_canvas_assign_project);

    s = schema_new();
    schema_num(s, "canvas_id",  "Numeric canvas ID", true);
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kabai_canvas_unassign_project",
        "Remove a canvas's link to a project", s, tool_canvas_unassign_project);

    s = schema_new();
    schema_num(s, "canvas_id", "Numeric canvas ID", true);
    schema_str(s, "type", "One of: text, image, sketch, ref, frame", true);
    schema_object(s, "content",
        "Type-dependent JSON payload — text: {\"text\":...} (markdown); "
        "image: {\"attachment_id\":N} (see kabai_get_attachment); "
        "sketch: {\"strokes\":[[[x,y,pressure],...],...]}; "
        "ref: {\"target_type\":\"ticket\"|\"note\",\"target_id\":N} (target must exist, "
        "may be in ANY project); frame: {\"title\":...} (a grouping container — frames "
        "replace milestones, see concept-kabai-canvas).",
        true);
    schema_num(s, "position_x", "X position on the canvas (default 0)", false);
    schema_num(s, "position_y", "Y position on the canvas (default 0)", false);
    schema_num(s, "width", "Element width (optional)", false);
    schema_num(s, "height", "Element height (optional)", false);
    schema_num(s, "z_order", "Stacking order (default 0)", false);
    schema_num(s, "parent_frame_id",
        "ID of a frame element on the SAME canvas to group this element under (optional)", false);
    schema_str(s, "description",
        "Alt-text. REQUIRED for type image/sketch — this is how non-multimodal agents "
        "understand what the image/sketch shows; optional for other types.", false);
    mcp_registry_add(r, "kabai_add_canvas_element",
        "Add an element to a canvas: a free text block, an image or sketch (both "
        "need description), a reference card to a ticket/epic/note in any project, "
        "or a frame (grouping container, replaces milestones).",
        s, tool_add_canvas_element);

    s = schema_new();
    schema_num(s, "element_id", "Numeric element ID", true);
    schema_object(s, "content", "New content (optional, replaces the old value — same shape rules as kabai_add_canvas_element)", false);
    schema_str(s, "description", "New alt-text (optional). Cannot be cleared on image/sketch elements.", false);
    schema_num(s, "position_x", "New X position (optional)", false);
    schema_num(s, "position_y", "New Y position (optional)", false);
    schema_num(s, "width", "New width (optional)", false);
    schema_num(s, "height", "New height (optional)", false);
    schema_num(s, "z_order", "New stacking order (optional)", false);
    schema_num(s, "parent_frame_id", "New parent frame ID, or explicit JSON null to detach from its frame (optional)", false);
    mcp_registry_add(r, "kabai_update_canvas_element",
        "Update an element's position, size, content, description, stacking order, "
        "or frame membership. Unspecified fields keep their value.",
        s, tool_update_canvas_element);

    s = schema_new();
    schema_num(s, "element_id", "Numeric element ID", true);
    mcp_registry_add(r, "kabai_delete_canvas_element",
        "Delete a canvas element. Its edges are removed too; elements that had it as "
        "their parent_frame are freed (not deleted).",
        s, tool_delete_canvas_element);

    s = schema_new();
    schema_num(s, "canvas_id", "Numeric canvas ID", true);
    schema_num(s, "from_element_id", "Source element ID (must be on the same canvas)", true);
    schema_num(s, "to_element_id",   "Target element ID (must be on the same canvas)", true);
    schema_str(s, "label", "Optional free-text label — there is deliberately no fixed edge taxonomy", false);
    mcp_registry_add(r, "kabai_add_canvas_edge",
        "Connect two elements on the same canvas with an edge and an optional "
        "free-text label.",
        s, tool_add_canvas_edge);

    s = schema_new();
    schema_num(s, "edge_id", "Numeric edge ID", true);
    mcp_registry_add(r, "kabai_delete_canvas_edge",
        "Remove an edge between two canvas elements", s, tool_delete_canvas_edge);
}
