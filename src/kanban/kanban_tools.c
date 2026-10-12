#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kanban/kanban_tools.h"
#include "mcp/schema.h"
#include "kanban/projects.h"
#include "kanban/tickets.h"
#include "kanban/comments.h"
#include "kanban/board_statuses.h"
#include "db/transaction.h"

/* ============================================================================
 * Error mapping
 * ============================================================================ */

/* Map the captured SQLSTATE/constraint of the last failed service call to
 * an actionable message (Kanban AI #345); mirrors docs_db_error in the
 * docs module. Falls back to the generic text when nothing was captured.
 * Returns a literal, db->last_primary or buf. */
static const char *kanban_db_msg(DatabaseConnection *db, char *buf, size_t size,
                                 const char *fallback) {
    const char *st = db->last_sqlstate;
    const char *cn = db->last_constraint;

    if (strcmp(st, "23503") == 0) {  /* foreign_key_violation */
        if (strcmp(cn, "check_ticket_status_project") == 0 ||
            strcmp(cn, "check_same_project_from") == 0 ||
            strcmp(cn, "check_same_project_to") == 0)
            return "status_id does not belong to the given project_id: board columns are "
                   "per project — look them up via kabai_list_board_statuses(project_id)";
        if (strstr(cn, "project_id"))
            return "project_id does not exist: check kabai_list_projects";
        if (strstr(cn, "status_id"))
            return "status_id does not exist: check kabai_list_board_statuses(project_id)";
        if (strstr(cn, "ticket"))
            return "ticket_id does not exist: check the ID (the ticket may have been deleted)";
        return "Referenced row does not exist (foreign key violation)";
    }
    if (strcmp(st, "23505") == 0) {  /* unique_violation */
        if (strstr(cn, "slug"))
            return "Slug already exists: project slugs are unique";
        if (strstr(cn, "board_statuses"))
            return "A column with this name already exists in this project";
        return "Already exists (duplicate)";
    }
    if (strcmp(st, "23514") == 0) {  /* check_violation */
        if (strstr(cn, "self"))
            return "A ticket cannot relate to or block itself";
        return db->last_primary[0] ? db->last_primary : "Constraint violation";
    }
    if (strcmp(st, "P0001") == 0 && db->last_primary[0])  /* trigger RAISE */
        return db->last_primary;
    if (db->last_primary[0]) {
        snprintf(buf, size, "%s: %s", fallback, db->last_primary);
        return buf;
    }
    return fallback;
}

static cJSON *kanban_db_error(McpContext *ctx, cJSON *id, const char *fallback) {
    char buf[600];
    return mcp_tool_err(id, kanban_db_msg(ctx->db, buf, sizeof(buf), fallback));
}

/* Roll back the open transaction and fail with the captured DB error,
 * prefixed with label (e.g. "blocks[1]=999") when given so the caller can
 * see which value was bad. The captured error survives the ROLLBACK. */
static cJSON *tx_db_fail(McpContext *ctx, cJSON *id, const char *label, const char *fallback) {
    char msg[600], out[700];
    snprintf(msg, sizeof(msg), "%s", kanban_db_msg(ctx->db, out, sizeof(out), fallback));
    db_rollback_transaction(ctx->db);
    if (!label) return mcp_tool_err(id, msg);
    snprintf(out, sizeof(out), "%s: %s", label, msg);
    return mcp_tool_err(id, out);
}

static cJSON *tx_fail(McpContext *ctx, cJSON *id, const char *msg) {
    db_rollback_transaction(ctx->db);
    return mcp_tool_err(id, msg);
}


/* ============================================================================
 * Projects
 * ============================================================================ */

static cJSON *tool_create_project(McpContext *ctx, cJSON *id, cJSON *params) {
    const char *slug = param_str(params, "slug");
    const char *name = param_str(params, "name");
    if (!slug || !name)
        return mcp_tool_err(id, "Missing required parameters: slug, name");

    Project *p = project_create(ctx->db, slug, name, param_str(params, "description"));
    if (!p) return kanban_db_error(ctx, id, "Failed to create project");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", p->id);
    cJSON_AddStringToObject(r, "slug", p->slug);
    cJSON_AddStringToObject(r, "name", p->name);
    if (p->description) cJSON_AddStringToObject(r, "description", p->description);
    cJSON_AddBoolToObject(r, "archived", p->archived);
    project_free(p);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_list_projects(McpContext *ctx, cJSON *id, cJSON *params) {
    bool include_archived = param_bool(params, "include_archived", false);
    Project **projects = project_list_all(ctx->db, include_archived);
    cJSON *arr = cJSON_CreateArray();
    if (!projects) return mcp_tool_ok(id, arr);

    for (int i = 0; projects[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", projects[i]->id);
        cJSON_AddStringToObject(o, "slug", projects[i]->slug);
        cJSON_AddStringToObject(o, "name", projects[i]->name);
        if (projects[i]->description)
            cJSON_AddStringToObject(o, "description", projects[i]->description);
        cJSON_AddBoolToObject(o, "archived", projects[i]->archived);
        cJSON_AddItemToArray(arr, o);
    }
    project_free_array(projects);
    return mcp_tool_ok(id, arr);
}

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
    cJSON_AddBoolToObject(r, "archived", p->archived);
    project_free(p);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Tickets
 * ============================================================================ */

/* ---- Bulk planning (#798): ticket + tasks + relations in one transaction ---- */

#define MAX_BATCH_ITEMS 50
#define REF_MAX 64

/* Batch-local names ("t1") of the items being created, with their ids. */
typedef struct {
    int         n;
    const char *ref[MAX_BATCH_ITEMS];
    int         item[MAX_BATCH_ITEMS];  /* index of the item that owns the ref */
    int         id[MAX_BATCH_ITEMS];    /* filled once the ids are reserved */
} RefTable;

static bool valid_relation_type(const char *rel) {
    return strcmp(rel, "parent_of")    == 0 || strcmp(rel, "blocks")     == 0 ||
           strcmp(rel, "duplicate_of") == 0 || strcmp(rel, "relates_to") == 0;
}

static bool ref_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Insert one ticket row. preset_id 0 takes the sequence default. */
static bool insert_ticket(McpContext *ctx, int preset_id, int project_id, int status_id,
                          const char *title, const char *description, const char *type,
                          bool docs_required, int *out_id) {
    DatabaseConnection *db = ctx->db;
    char pre[32], proj[32], stat[32];
    snprintf(pre, sizeof(pre), "%d", preset_id);
    snprintf(proj, sizeof(proj), "%d", project_id);
    snprintf(stat, sizeof(stat), "%d", status_id);
    const char *p[6] = {preset_id ? pre : NULL, proj, stat, title, description,
                        (type && type[0]) ? type : "ticket"};
    db_clear_error(db);
    PGresult *res = PQexecParams(db->conn,
        "INSERT INTO tickets (id, project_id, status_id, title, description, type)"
        " VALUES (COALESCE($1::int, nextval(pg_get_serial_sequence('tickets','id'))),"
        " $2, $3, $4, $5, $6) RETURNING id",
        6, NULL, p, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        db_capture_error(db, res);
        if (res) PQclear(res);
        return false;
    }
    *out_id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    if (!docs_required) return true;

    char tid[32];
    snprintf(tid, sizeof(tid), "%d", *out_id);
    const char *up[1] = {tid};
    res = PQexecParams(db->conn, "UPDATE tickets SET docs_required = TRUE WHERE id = $1",
                       1, NULL, up, NULL, NULL, 0);
    bool ok = res && PQresultStatus(res) == PGRES_COMMAND_OK;
    if (!ok) db_capture_error(db, res);
    if (res) PQclear(res);
    return ok;
}

/* Id of the batch ref that starts at s[1..len), or 0. */
static int ref_lookup(const RefTable *refs, const char *name, size_t len) {
    for (int i = 0; refs && i < refs->n; i++)
        if (strlen(refs->ref[i]) == len && strncmp(refs->ref[i], name, len) == 0)
            return refs->id[i];
    return 0;
}

/* Copy of s with every "$ref" naming a batch item replaced by "#<id>";
 * other "$..." text stays as written. Caller frees. NULL for NULL/OOM. */
static char *expand_refs(const char *s, const RefTable *refs) {
    if (!s) return NULL;
    char *out = NULL;
    for (int pass = 0; pass < 2; pass++) {
        size_t n = 0;
        for (const char *c = s; *c; ) {
            size_t k = 0;
            int rid = 0;
            if (*c == '$') {
                while (ref_char(c[1 + k])) k++;
                rid = k ? ref_lookup(refs, c + 1, k) : 0;
            }
            if (rid) {
                char num[40];
                int w = snprintf(num, sizeof(num), "#%d", rid);
                if (pass) memcpy(out + n, num, (size_t)w);
                n += (size_t)w;
                c += 1 + k;
            } else {
                if (pass) out[n] = *c;
                n++;
                c++;
            }
        }
        if (pass) { out[n] = '\0'; return out; }
        out = malloc(n + 1);
        if (!out) return NULL;
    }
    return NULL;
}

/* Ticket id from a number or, with refs, a "$ref" string. 0 and a reason in
 * err when the value is neither. */
static int resolve_target(const cJSON *v, const RefTable *refs, char *err, size_t size) {
    if (cJSON_IsNumber(v) && v->valueint > 0 && v->valuedouble == (double)v->valueint)
        return v->valueint;
    if (cJSON_IsString(v) && refs && v->valuestring[0] == '$') {
        int rid = ref_lookup(refs, v->valuestring + 1, strlen(v->valuestring + 1));
        if (!rid) snprintf(err, size, "unknown ref %s (no item in this batch has that ref)",
                           v->valuestring);
        return rid;
    }
    char *txt = cJSON_PrintUnformatted(v);
    snprintf(err, size, "invalid value %s: expected a ticket id%s", txt ? txt : "?",
             refs ? " or a \"$ref\" of this batch" : "");
    if (txt) cJSON_free(txt);
    return 0;
}

/* Fail inside the transaction naming the offending value: "<where><what>: <why>". */
static cJSON *extras_fail(McpContext *ctx, cJSON *id, const char *where, const char *what,
                          const char *why) {
    char buf[700];
    snprintf(buf, sizeof(buf), "%s%s: %s", where, what, why);
    return tx_fail(ctx, id, buf);
}

/* Link ticket new_id with every entry of spec[field] (ids or "$refs").
 * incoming: entry -> new_id (blocked_by), else new_id -> entry (blocks). */
static cJSON *link_list(McpContext *ctx, cJSON *id, int new_id, cJSON *spec, const char *field,
                        const RefTable *refs, const char *where) {
    cJSON *list = cJSON_GetObjectItemCaseSensitive(spec, field);
    if (!list) return NULL;
    if (!cJSON_IsArray(list))
        return extras_fail(ctx, id, where, field, "must be an array of ticket ids");
    bool incoming = strcmp(field, "blocked_by") == 0;
    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, list) {
        char what[80], err[300];
        snprintf(what, sizeof(what), "%s[%d]", field, i++);
        int other = resolve_target(e, refs, err, sizeof(err));
        if (!other) return extras_fail(ctx, id, where, what, err);
        if (!(incoming ? ticket_link(ctx->db, other, new_id, "blocks")
                       : ticket_link(ctx->db, new_id, other, "blocks"))) {
            char label[200];
            snprintf(label, sizeof(label), "%s%s=%d", where, what, other);
            return tx_db_fail(ctx, id, label, "Failed to create relation");
        }
    }
    return NULL;
}

/* Tasks and relations of the freshly inserted ticket new_id, from the
 * create_ticket fields in spec. NULL on success, else an error response
 * with the transaction already rolled back. */
static cJSON *apply_extras(McpContext *ctx, cJSON *id, int new_id, cJSON *spec,
                           const RefTable *refs, const char *where) {
    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(spec, "tasks");
    if (tasks) {
        if (!cJSON_IsArray(tasks))
            return extras_fail(ctx, id, where, "tasks", "must be an array of strings");
        int i = 0;
        cJSON *t;
        cJSON_ArrayForEach(t, tasks) {
            char what[80];
            snprintf(what, sizeof(what), "tasks[%d]", i++);
            if (!cJSON_IsString(t) || !t->valuestring[0])
                return extras_fail(ctx, id, where, what, "must be a non-empty string");
            TicketTask *tk = ticket_add_task(ctx->db, new_id, t->valuestring);
            if (!tk) {
                char label[200];
                snprintf(label, sizeof(label), "%s%s", where, what);
                return tx_db_fail(ctx, id, label, "Failed to add task");
            }
            ticket_task_free(tk);
        }
    }

    cJSON *parent = cJSON_GetObjectItemCaseSensitive(spec, "parent_id");
    if (!parent) parent = cJSON_GetObjectItemCaseSensitive(spec, "parent");
    if (parent) {
        char err[300];
        int p = resolve_target(parent, refs, err, sizeof(err));
        if (!p) return extras_fail(ctx, id, where, "parent_id", err);
        if (!ticket_link(ctx->db, p, new_id, "parent_of")) {
            char label[200];
            snprintf(label, sizeof(label), "%sparent_id=%d", where, p);
            return tx_db_fail(ctx, id, label, "Failed to create relation");
        }
    }

    cJSON *err = link_list(ctx, id, new_id, spec, "blocks", refs, where);
    return err ? err : link_list(ctx, id, new_id, spec, "blocked_by", refs, where);
}

static cJSON *tool_create_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id, status_id;
    const char *title = param_str(params, "title");
    bool ok[3] = {param_num(params, "project_id", &project_id),
                  param_num_alias(params, "status_id", "new_status_id", &status_id), title != NULL};
    if (!ok[0] || !ok[1] || !ok[2]) {
        const char *names[3] = {"project_id", "status_id", "title"};
        char buf[120];
        return mcp_tool_err(id, missing_params_msg(buf, sizeof(buf), names, ok, 3));
    }

    const char *type_val = param_str(params, "type");
    if (type_val && strcmp(type_val, "ticket") != 0 && strcmp(type_val, "epic") != 0)
        return mcp_tool_err(id, "Invalid type: must be 'ticket' or 'epic'");

    const char *description = param_str(params, "description");
    bool docs_required = param_bool(params, "docs_required", false);
    int new_id;
    if (!db_begin_transaction(ctx->db))
        return mcp_tool_err(id, "Failed to start transaction");
    if (!insert_ticket(ctx, 0, project_id, status_id, title, description, type_val,
                       docs_required, &new_id))
        return tx_db_fail(ctx, id, NULL, "Failed to create ticket");
    cJSON *err = apply_extras(ctx, id, new_id, params, NULL, "");
    if (err) return err;
    if (!db_commit_transaction(ctx->db))
        return tx_fail(ctx, id, "Failed to commit transaction");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", new_id);
    cJSON_AddNumberToObject(r, "project_id", project_id);
    cJSON_AddNumberToObject(r, "status_id", status_id);
    cJSON_AddStringToObject(r, "title", title);
    if (param_bool(params, "verbose", false)) {
        cJSON_AddStringToObject(r, "type", type_val ? type_val : "ticket");
        if (description) cJSON_AddStringToObject(r, "description", description);
        if (docs_required) cJSON_AddBoolToObject(r, "docs_required", 1);
    }
    return mcp_tool_ok(id, r);
}

static void item_where(char *buf, size_t size, int index, const char *ref) {
    if (ref) snprintf(buf, size, "items[%d] (ref %s): ", index, ref);
    else     snprintf(buf, size, "items[%d]: ", index);
}

static cJSON *tool_create_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id, default_status = 0;
    cJSON *items = cJSON_GetObjectItemCaseSensitive(params, "items");
    bool ok[2] = {param_num(params, "project_id", &project_id), cJSON_IsArray(items)};
    if (!ok[0] || !ok[1]) {
        const char *names[2] = {"project_id", "items (array)"};
        char buf[120];
        return mcp_tool_err(id, missing_params_msg(buf, sizeof(buf), names, ok, 2));
    }
    param_num_alias(params, "status_id", "new_status_id", &default_status);

    int n = cJSON_GetArraySize(items);
    if (n == 0) return mcp_tool_err(id, "items is empty: nothing to create");
    if (n > MAX_BATCH_ITEMS) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Too many items: %d (max %d per call). Nothing was "
                 "created - split the batch (refs only resolve within one call)",
                 n, MAX_BATCH_ITEMS);
        return mcp_tool_err(id, buf);
    }

    /* Validate every item and collect the refs before touching the database. */
    RefTable refs = {0};
    int status[MAX_BATCH_ITEMS];
    const char *item_ref[MAX_BATCH_ITEMS];
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(items, i);
        char where[120], buf[400];
        cJSON *rj = cJSON_IsObject(it) ? cJSON_GetObjectItemCaseSensitive(it, "ref") : NULL;
        item_ref[i] = cJSON_IsString(rj) ? rj->valuestring : NULL;
        item_where(where, sizeof(where), i, item_ref[i]);
        const char *msg = NULL;
        const char *type_val = cJSON_IsObject(it) ? param_str(it, "type") : NULL;
        if (!cJSON_IsObject(it)) msg = "must be an object";
        else if (!param_str(it, "title")) msg = "missing title";
        else if (!param_num_alias(it, "status_id", "new_status_id", &status[i]) &&
                 !(status[i] = default_status))
            msg = "missing status_id (set it per item or once on the call)";
        else if (type_val && strcmp(type_val, "ticket") != 0 && strcmp(type_val, "epic") != 0)
            msg = "invalid type: must be 'ticket' or 'epic'";
        else if (rj && !item_ref[i]) msg = "ref must be a string";
        else if (item_ref[i]) {
            size_t len = strlen(item_ref[i]);
            bool chars = len > 0 && len < REF_MAX;
            for (size_t k = 0; chars && k < len; k++) chars = ref_char(item_ref[i][k]);
            if (!chars) msg = "invalid ref: use 1-63 letters, digits or underscores (no '$')";
            else if (ref_lookup(&refs, item_ref[i], len)) msg = "duplicate ref";
            else {
                refs.ref[refs.n] = item_ref[i];
                refs.item[refs.n] = i;
                refs.id[refs.n++] = -1;  /* placeholder: marks the ref as taken */
            }
        }
        if (msg) {
            snprintf(buf, sizeof(buf), "%s%s. Nothing was created.", where, msg);
            return mcp_tool_err(id, buf);
        }
    }

    if (!db_begin_transaction(ctx->db)) return mcp_tool_err(id, "Failed to start transaction");

    /* Reserve all ids first so refs, also to later items, resolve everywhere. */
    int item_id[MAX_BATCH_ITEMS];
    char count[16];
    snprintf(count, sizeof(count), "%d", n);
    const char *cp[1] = {count};
    db_clear_error(ctx->db);
    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT nextval(pg_get_serial_sequence('tickets','id')) FROM generate_series(1, $1::int)",
        1, NULL, cp, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != n) {
        db_capture_error(ctx->db, res);
        if (res) PQclear(res);
        return tx_db_fail(ctx, id, NULL, "Failed to reserve ticket ids");
    }
    for (int i = 0; i < n; i++) item_id[i] = atoi(PQgetvalue(res, i, 0));
    PQclear(res);
    for (int k = 0; k < refs.n; k++) refs.id[k] = item_id[refs.item[k]];

    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(items, i);
        char where[120];
        item_where(where, sizeof(where), i, item_ref[i]);
        char *title = expand_refs(param_str(it, "title"), &refs);
        char *desc = expand_refs(param_str(it, "description"), &refs);
        int got;
        bool done = title && insert_ticket(ctx, item_id[i], project_id, status[i], title, desc,
                                           param_str(it, "type"),
                                           param_bool(it, "docs_required", false), &got);
        free(title);
        free(desc);
        if (!done) return tx_db_fail(ctx, id, where, "Failed to create ticket");
    }
    for (int i = 0; i < n; i++) {
        char where[120];
        item_where(where, sizeof(where), i, item_ref[i]);
        cJSON *err = apply_extras(ctx, id, item_id[i], cJSON_GetArrayItem(items, i), &refs, where);
        if (err) return err;
    }
    if (!db_commit_transaction(ctx->db))
        return tx_fail(ctx, id, "Failed to commit transaction");

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (item_ref[i]) cJSON_AddStringToObject(o, "ref", item_ref[i]);
        cJSON_AddNumberToObject(o, "id", item_id[i]);
        cJSON_AddNumberToObject(o, "status_id", status[i]);
        cJSON_AddItemToArray(arr, o);
    }
    return mcp_tool_ok(id, arr);
}

static cJSON *tool_list_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id;
    if (!param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameter: project_id");
    if (project_id <= 0)
        return mcp_tool_err(id, "Invalid project_id: must be a positive integer");

    int status_id = 0, limit = 0, offset = 0;
    param_num(params, "status_id", &status_id);
    param_num(params, "limit", &limit);
    param_num(params, "offset", &offset);
    const char *type_filter = param_str(params, "type");
    const char *assignee_filter = param_str(params, "assignee");
    bool summary = param_bool(params, "summary", false);
    bool workable = param_bool(params, "workable", false);

    if (workable) {
        char pid[32];
        snprintf(pid, sizeof(pid), "%d", project_id);
        const char *qp[1] = {pid};
        PGresult *ar = PQexecParams(ctx->db->conn,
            "SELECT archived FROM projects WHERE id = $1", 1, NULL, qp, NULL, NULL, 0);
        bool archived = ar && PQresultStatus(ar) == PGRES_TUPLES_OK && PQntuples(ar) > 0 &&
                        PQgetvalue(ar, 0, 0)[0] == 't';
        if (ar) PQclear(ar);
        if (archived) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddItemToObject(r, "tickets", cJSON_CreateArray());
            cJSON_AddStringToObject(r, "hint",
                "Project is archived and read-only: nothing can be picked up. "
                "look in an active project (kabai_list_projects)");
            return mcp_tool_ok(id, r);
        }
    }

    Ticket **tickets = ticket_list_filtered(ctx->db, project_id, status_id, type_filter,
                                            assignee_filter, workable, limit, offset);
    cJSON *arr = cJSON_CreateArray();
    if (!tickets) return mcp_tool_ok(id, arr);

    /* open_tasks per ticket, one query for the whole project (workable mode) */
    cJSON *open_tasks = NULL;
    if (workable) {
        char pid[32];
        snprintf(pid, sizeof(pid), "%d", project_id);
        const char *qp[1] = {pid};
        PGresult *cr = PQexecParams(ctx->db->conn,
            "SELECT tt.ticket_id, count(*) FROM ticket_tasks tt JOIN tickets t ON t.id = tt.ticket_id "
            "WHERE t.project_id = $1 AND NOT tt.is_completed GROUP BY tt.ticket_id",
            1, NULL, qp, NULL, NULL, 0);
        if (cr && PQresultStatus(cr) == PGRES_TUPLES_OK) {
            open_tasks = cJSON_CreateObject();
            for (int k = 0; k < PQntuples(cr); k++)
                cJSON_AddNumberToObject(open_tasks, PQgetvalue(cr, k, 0), atoi(PQgetvalue(cr, k, 1)));
        }
        if (cr) PQclear(cr);
    }

    for (int i = 0; tickets[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", tickets[i]->id);
        cJSON_AddNumberToObject(o, "project_id", tickets[i]->project_id);
        cJSON_AddNumberToObject(o, "status_id", tickets[i]->status_id);
        if (open_tasks) {
            char key[32];
            snprintf(key, sizeof(key), "%d", tickets[i]->id);
            cJSON *n = cJSON_GetObjectItemCaseSensitive(open_tasks, key);
            cJSON_AddNumberToObject(o, "open_tasks", n ? n->valuedouble : 0);
        }
        cJSON_AddStringToObject(o, "type", tickets[i]->type ? tickets[i]->type : "ticket");
        cJSON_AddStringToObject(o, "title", tickets[i]->title);
        if (!summary && tickets[i]->description)
            cJSON_AddStringToObject(o, "description", tickets[i]->description);
        if (tickets[i]->assignee)
            cJSON_AddStringToObject(o, "assignee", tickets[i]->assignee);
        if (tickets[i]->effort_estimate)
            cJSON_AddNumberToObject(o, "effort_estimate", atof(tickets[i]->effort_estimate));
        if (tickets[i]->effort_actual)
            cJSON_AddNumberToObject(o, "effort_actual", atof(tickets[i]->effort_actual));
        if (tickets[i]->effort_unit)
            cJSON_AddStringToObject(o, "effort_unit", tickets[i]->effort_unit);
        if (tickets[i]->created_at)
            cJSON_AddStringToObject(o, "created_at", tickets[i]->created_at);
        if (tickets[i]->updated_at)
            cJSON_AddStringToObject(o, "updated_at", tickets[i]->updated_at);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_Delete(open_tasks);
    ticket_free_array(tickets);
    return mcp_tool_ok(id, arr);
}

/* Full-text ticket search (#905, #973). Lean rows by default; full:true adds
 * the description and bookkeeping fields. Result columns of both queries:
 * 0 id, 1 project_id, 2 status_id, 3 status_name, 4 type, 5 title, 6 assignee,
 * 7 rank, 8 snippet, 9 description, 10-12 effort estimate/actual/unit,
 * 13 created_at, 14 updated_at. */
#define TICKET_SEARCH_COLS \
    "t.id, t.project_id, t.status_id, bs.name, t.type, t.title, t.assignee, "
#define TICKET_SEARCH_TAIL \
    "t.description, t.effort_estimate::text, t.effort_actual::text, t.effort_unit, " \
    "t.created_at::text, t.updated_at::text " \
    "FROM tickets t JOIN board_statuses bs ON bs.id = t.status_id " \
    "JOIN projects pr ON pr.id = t.project_id "

static cJSON *search_ticket_rows_to_json(PGresult *res, const char *match_type, bool full) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < PQntuples(res); i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", atoi(PQgetvalue(res, i, 0)));
        cJSON_AddNumberToObject(o, "project_id", atoi(PQgetvalue(res, i, 1)));
        cJSON_AddNumberToObject(o, "status_id", atoi(PQgetvalue(res, i, 2)));
        cJSON_AddStringToObject(o, "status_name", PQgetvalue(res, i, 3));
        cJSON_AddStringToObject(o, "type", PQgetvalue(res, i, 4));
        cJSON_AddStringToObject(o, "title", PQgetvalue(res, i, 5));
        if (!PQgetisnull(res, i, 6))
            cJSON_AddStringToObject(o, "assignee", PQgetvalue(res, i, 6));
        cJSON_AddNumberToObject(o, "rank", atof(PQgetvalue(res, i, 7)));
        cJSON_AddStringToObject(o, "match_type", match_type);
        cJSON_AddStringToObject(o, "snippet", PQgetvalue(res, i, 8));
        if (full) {
            if (!PQgetisnull(res, i, 9))
                cJSON_AddStringToObject(o, "description", PQgetvalue(res, i, 9));
            if (!PQgetisnull(res, i, 10))
                cJSON_AddNumberToObject(o, "effort_estimate", atof(PQgetvalue(res, i, 10)));
            if (!PQgetisnull(res, i, 11))
                cJSON_AddNumberToObject(o, "effort_actual", atof(PQgetvalue(res, i, 11)));
            if (!PQgetisnull(res, i, 12))
                cJSON_AddStringToObject(o, "effort_unit", PQgetvalue(res, i, 12));
            cJSON_AddStringToObject(o, "created_at", PQgetvalue(res, i, 13));
            cJSON_AddStringToObject(o, "updated_at", PQgetvalue(res, i, 14));
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static cJSON *tool_search_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    const char *query = param_str(params, "query");
    if (!query || query[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: query");

    int project_id = 0, limit = 0;
    param_num(params, "project_id", &project_id);
    param_num(params, "limit", &limit);
    bool full = param_bool(params, "full", false);
    bool include_archived = param_bool(params, "include_archived", false);

    char proj_str[32], limit_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    snprintf(limit_str, sizeof(limit_str), "%d", limit);
    const char *q_params[4] = {
        query,
        project_id > 0 ? proj_str : NULL,
        limit > 0 ? limit_str : NULL,
        include_archived ? "true" : "false"
    };

    /* ponytail: the tsvector is computed per row on the fly (no column, no
     * index), fine for a few thousand tickets. Upgrade path: a migration with
     * a generated tickets.search_tsv column (title A, description C) plus a
     * GIN index, then match against that column here. */
    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT " TICKET_SEARCH_COLS
        "       ts_rank(v.tsv, q)::text, "
        "       ts_headline('simple', COALESCE(t.description, t.title), q, "
        "                   'MaxWords=20, MinWords=5, MaxFragments=1'), "
        TICKET_SEARCH_TAIL
        ", websearch_to_tsquery('simple', $1) q, "
        "  LATERAL (SELECT setweight(to_tsvector('simple', t.title), 'A') || "
        "                  setweight(to_tsvector('simple', COALESCE(t.description, '')), 'C') "
        "                  AS tsv) v "
        "WHERE v.tsv @@ q AND ($2::int IS NULL OR t.project_id = $2) "
        "  AND ($2::int IS NOT NULL OR $4::boolean OR NOT pr.archived) "
        "ORDER BY ts_rank(v.tsv, q) DESC, t.id DESC "
        "LIMIT LEAST(COALESCE($3::int, 20), 100)",
        4, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return mcp_tool_err(id,
            "Search failed: use plain words, \"quoted phrases\", OR and -exclusions");
    }
    if (PQntuples(res) > 0) {
        cJSON *arr = search_ticket_rows_to_json(res, "fts", full);
        PQclear(res);
        return mcp_tool_ok(id, arr);
    }
    PQclear(res);

    /* Fallback: trigram similarity on titles catches typos and partial words. */
    res = PQexecParams(ctx->db->conn,
        "SELECT " TICKET_SEARCH_COLS
        "       similarity(t.title, $1)::text, left(COALESCE(t.description, t.title), 160), "
        TICKET_SEARCH_TAIL
        "WHERE t.title % $1 AND ($2::int IS NULL OR t.project_id = $2) "
        "  AND ($2::int IS NOT NULL OR $4::boolean OR NOT pr.archived) "
        "ORDER BY similarity(t.title, $1) DESC, t.id DESC "
        "LIMIT LEAST(COALESCE($3::int, 20), 100)",
        4, NULL, q_params, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Search failed");
    }
    cJSON *arr = search_ticket_rows_to_json(res, "title_similarity", full);
    PQclear(res);
    return mcp_tool_ok(id, arr);
}

static cJSON *tool_get_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    if (!param_num(params, "ticket_id", &ticket_id))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    Ticket *t = ticket_get_by_id(ctx->db, ticket_id);
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
    if (t->effort_estimate) cJSON_AddNumberToObject(r, "effort_estimate", atof(t->effort_estimate));
    if (t->effort_actual)   cJSON_AddNumberToObject(r, "effort_actual", atof(t->effort_actual));
    if (t->effort_unit)     cJSON_AddStringToObject(r, "effort_unit", t->effort_unit);
    if (t->created_at)  cJSON_AddStringToObject(r, "created_at", t->created_at);
    if (t->updated_at)  cJSON_AddStringToObject(r, "updated_at", t->updated_at);
    if (t->agent_role_instruction)
        cJSON_AddStringToObject(r, "agent_role_instruction", t->agent_role_instruction);
    ticket_free(t);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_get_ticket_detailed(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    if (!param_num(params, "ticket_id", &ticket_id))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    /* include_role_instruction defaults to true; pass false to suppress it */
    bool include_ari = param_bool(params, "include_role_instruction", true);

    TicketDetailed *d = ticket_get_detailed(ctx->db, ticket_id);
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
    if (d->ticket->effort_estimate)
        cJSON_AddNumberToObject(ticket_j, "effort_estimate", atof(d->ticket->effort_estimate));
    if (d->ticket->effort_actual)
        cJSON_AddNumberToObject(ticket_j, "effort_actual", atof(d->ticket->effort_actual));
    if (d->ticket->effort_unit)
        cJSON_AddStringToObject(ticket_j, "effort_unit", d->ticket->effort_unit);
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

    int comments_total = 0, comments_limit = 0;
    while (d->comments && d->comments[comments_total]) comments_total++;
    param_num(params, "comments_limit", &comments_limit);
    int comments_from = (comments_limit > 0 && comments_total > comments_limit)
                        ? comments_total - comments_limit : 0;

    cJSON *comments_arr = cJSON_CreateArray();
    if (d->comments) {
        for (int i = comments_from; d->comments[i]; i++) {
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
            {
                bool out = rel->from_ticket_id == d->ticket->id;
                const char *t = rel->relation_type, *role = t;
                if (strcmp(t, "blocks") == 0)            role = out ? "blocks" : "blocked_by";
                else if (strcmp(t, "parent_of") == 0)    role = out ? "parent_of" : "child_of";
                else if (strcmp(t, "duplicate_of") == 0) role = out ? "duplicate_of" : "has_duplicate";
                cJSON_AddStringToObject(rj, "role", role);
                if (strcmp(t, "blocks") == 0) {
                    char meaning[160];
                    int other = out ? rel->to_ticket_id : rel->from_ticket_id;
                    snprintf(meaning, sizeof(meaning), out
                        ? "this ticket must be done before #%d can start (#%d waits for this one)"
                        : "#%d must be done before this ticket can start (this one waits for #%d)",
                        other, other);
                    cJSON_AddStringToObject(rj, "meaning", meaning);
                }
            }
            if (rel->created_at)
                cJSON_AddStringToObject(rj, "created_at", rel->created_at);
            cJSON_AddItemToArray(relations_arr, rj);
        }
    }

    /* Linked knowledge-base notes (kbai-docs, note_ticket_links) — metadata
     * only; fetch bodies via kabai_docs_get_note. */
    cJSON *notes_arr = cJSON_CreateArray();
    {
        char tid_str[32];
        snprintf(tid_str, sizeof(tid_str), "%d", d->ticket->id);
        const char *np[1] = {tid_str};

        PGresult *dres = PQexecParams(ctx->db->conn,
            "SELECT docs_required FROM tickets WHERE id = $1",
            1, NULL, np, NULL, NULL, 0);
        if (dres && PQresultStatus(dres) == PGRES_TUPLES_OK && PQntuples(dres) > 0 &&
            PQgetvalue(dres, 0, 0)[0] == 't')
            cJSON_AddBoolToObject(ticket_j, "docs_required", 1);
        if (dres) PQclear(dres);
        PGresult *nres = PQexecParams(ctx->db->conn,
            "SELECT n.id, n.slug, n.title, n.kind, ntl.relation "
            "  FROM note_ticket_links ntl JOIN notes n ON n.id = ntl.note_id "
            " WHERE ntl.ticket_id = $1 ORDER BY n.id",
            1, NULL, np, NULL, NULL, 0);
        if (nres && PQresultStatus(nres) == PGRES_TUPLES_OK) {
            for (int i = 0; i < PQntuples(nres); i++) {
                cJSON *nj = cJSON_CreateObject();
                cJSON_AddNumberToObject(nj, "note_id", atoi(PQgetvalue(nres, i, 0)));
                cJSON_AddStringToObject(nj, "slug", PQgetvalue(nres, i, 1));
                cJSON_AddStringToObject(nj, "title", PQgetvalue(nres, i, 2));
                cJSON_AddStringToObject(nj, "kind", PQgetvalue(nres, i, 3));
                cJSON_AddStringToObject(nj, "relation", PQgetvalue(nres, i, 4));
                cJSON_AddItemToArray(notes_arr, nj);
            }
        }
        if (nres) PQclear(nres);
    }

    /* Attachment metadata (ticket #468) — never the binary data itself;
     * fetch the image content via kabai_get_attachment on explicit need. */
    cJSON *attachments_arr = cJSON_CreateArray();
    {
        char tid_str[32];
        snprintf(tid_str, sizeof(tid_str), "%d", d->ticket->id);
        const char *ap[1] = {tid_str};

        PGresult *ares = PQexecParams(ctx->db->conn,
            "SELECT a.id, a.filename, a.mime_type, a.size_bytes, a.description, "
            "       a.uploaded_by, a.created_at::text "
            "  FROM ticket_attachments ta JOIN attachments a ON a.id = ta.attachment_id "
            " WHERE ta.ticket_id = $1 ORDER BY a.id",
            1, NULL, ap, NULL, NULL, 0);
        if (ares && PQresultStatus(ares) == PGRES_TUPLES_OK) {
            for (int i = 0; i < PQntuples(ares); i++) {
                cJSON *aj = cJSON_CreateObject();
                cJSON_AddNumberToObject(aj, "id", atoi(PQgetvalue(ares, i, 0)));
                cJSON_AddStringToObject(aj, "filename", PQgetvalue(ares, i, 1));
                cJSON_AddStringToObject(aj, "mime_type", PQgetvalue(ares, i, 2));
                cJSON_AddNumberToObject(aj, "size_bytes", atoi(PQgetvalue(ares, i, 3)));
                if (!PQgetisnull(ares, i, 4))
                    cJSON_AddStringToObject(aj, "description", PQgetvalue(ares, i, 4));
                if (!PQgetisnull(ares, i, 5))
                    cJSON_AddStringToObject(aj, "uploaded_by", PQgetvalue(ares, i, 5));
                cJSON_AddStringToObject(aj, "created_at", PQgetvalue(ares, i, 6));
                cJSON_AddItemToArray(attachments_arr, aj);
            }
        }
        if (ares) PQclear(ares);
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "ticket", ticket_j);
    cJSON_AddItemToObject(r, "tasks", tasks_arr);
    cJSON_AddItemToObject(r, "relations", relations_arr);
    cJSON_AddItemToObject(r, "comments", comments_arr);
    if (comments_from > 0)
        cJSON_AddNumberToObject(r, "comments_total", comments_total);
    cJSON_AddItemToObject(r, "linked_notes", notes_arr);
    cJSON_AddItemToObject(r, "attachments", attachments_arr);

    ticket_detailed_free(d);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_move_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id, new_status_id;
    bool ok[2] = {param_num(params, "ticket_id", &ticket_id),
                  param_num_alias(params, "new_status_id", "status_id", &new_status_id)};
    if (!ok[0] || !ok[1]) {
        const char *names[2] = {"ticket_id", "new_status_id"};
        char buf[100];
        return mcp_tool_err(id, missing_params_msg(buf, sizeof(buf), names, ok, 2));
    }

    if (!ticket_update_status(ctx->db, ticket_id, new_status_id)) {
        /* Read error BEFORE any subsequent query clobbers it */
        const char *raw = PQerrorMessage(ctx->db->conn);
        if (raw && strstr(raw, "is archived and read-only"))
            return mcp_tool_err(id, ctx->db->last_primary[0] ? ctx->db->last_primary : "Project is archived and read-only");
        if (raw && strstr(raw, "Illegaler Kanban-Move"))
            return mcp_tool_err(id, "Invalid ticket transition: check workflow rules");
        if (raw && strstr(raw, "Akzeptanzkriterium"))
            return mcp_tool_err(id, "Cannot close ticket: open acceptance criteria remain");
        if (raw && strstr(raw, "Epic documentation duty"))
            return mcp_tool_err(id,
                "Cannot close epic: an epic must not close without at least one "
                "knowledge-base note created or substantially updated during its "
                "lifetime. Create/update the note and link it via kabai_docs_link_ticket");
        if (raw && strstr(raw, "Docs requirement"))
            return mcp_tool_err(id,
                "Cannot close ticket: docs_required is set but no knowledge-base note is "
                "linked. Link one via kabai_docs_link_ticket or unset docs_required with "
                "a justification (update_ticket + work-log comment)");
        return mcp_tool_err(id, "Failed to move ticket");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_move_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    cJSON *ids_j = cJSON_GetObjectItemCaseSensitive(params, "ticket_ids");
    int new_status_id;
    bool ok[2] = {cJSON_IsArray(ids_j),
                  param_num_alias(params, "new_status_id", "status_id", &new_status_id)};
    if (!ok[0] || !ok[1]) {
        const char *names[2] = {"ticket_ids (array)", "new_status_id"};
        char buf[120];
        return mcp_tool_err(id, missing_params_msg(buf, sizeof(buf), names, ok, 2));
    }

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

        if (ticket_update_status(ctx->db, ticket_id, new_status_id)) {
            cJSON_AddBoolToObject(entry, "success", 1);
            success++;
        } else {
            cJSON_AddBoolToObject(entry, "success", 0);
            const char *raw = PQerrorMessage(ctx->db->conn);
            if (raw && strstr(raw, "is archived and read-only"))
                cJSON_AddStringToObject(entry, "error", "Project is archived and read-only");
            else if (raw && strstr(raw, "Illegaler Kanban-Move"))
                cJSON_AddStringToObject(entry, "error", "Invalid transition");
            else if (raw && strstr(raw, "Akzeptanzkriterium"))
                cJSON_AddStringToObject(entry, "error", "Open acceptance criteria remain");
            else if (raw && strstr(raw, "Epic documentation duty"))
                cJSON_AddStringToObject(entry, "error", "epic cannot close without a linked note");
            else if (raw && strstr(raw, "Docs requirement"))
                cJSON_AddStringToObject(entry, "error", "docs_required set but no note linked");
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

static cJSON *tool_assign_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    if (!param_num(params, "ticket_id", &ticket_id))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    /* Use provided assignee, fall back to KABAI_AGENT_NAME, then error */
    const char *assignee = param_str(params, "assignee");
    if (!assignee) assignee = ctx->agent_name;
    if (!assignee)
        return mcp_tool_err(id,
            "Missing assignee: provide 'assignee' parameter or set the KABAI_AGENT_NAME "
            "environment variable in the MCP server config (KABAI_AGENT_MODEL is also "
            "recommended for tracking which model worked the ticket)");

    if (!ticket_assign(ctx->db, ticket_id, assignee, ctx->agent_model))
        return ctx->db->last_primary[0]
            ? kanban_db_error(ctx, id, "Failed to assign ticket")
            : mcp_tool_err(id, "Ticket not found (or assign failed)");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

/* in_tx: running under the expected_updated_at lock; stop at the first failed
 * statement so the caller reports its error instead of "transaction aborted". */
static cJSON *update_ticket_fields(McpContext *ctx, cJSON *id, cJSON *params, int ticket_id,
                                   bool in_tx) {
    int updated = 0;
    int provided = 0;

    const char *title = param_str(params, "title");
    if (title) {
        provided++;
        updated += ticket_update_title(ctx->db, ticket_id, title);
        if (in_tx && ctx->db->last_primary[0]) goto done;
    }

    if (param_present(params, "description")) {
        provided++;
        const char *new_desc = param_is_null(params, "description")
                             ? NULL : param_str(params, "description");
        updated += ticket_update_description(ctx->db, ticket_id, new_desc);
        if (in_tx && ctx->db->last_primary[0]) goto done;
    }

    if (param_present(params, "docs_required")) {
        provided++;
        char tid_str[32];
        snprintf(tid_str, sizeof(tid_str), "%d", ticket_id);
        const char *dp[2] = {tid_str,
                             param_bool(params, "docs_required", false) ? "t" : "f"};
        PGresult *dres = PQexecParams(ctx->db->conn,
            "UPDATE tickets SET docs_required = $2::bool WHERE id = $1",
            2, NULL, dp, NULL, NULL, 0);
        if (dres && PQresultStatus(dres) == PGRES_COMMAND_OK &&
            atoi(PQcmdTuples(dres)) > 0)
            updated += 1;
        else if (dres && strstr(PQresultErrorMessage(dres), "Epic documentation duty")) {
            PQclear(dres);
            return mcp_tool_err(id,
                "docs_required cannot be unset on an epic: an epic must not close "
                "without a linked knowledge-base note");
        }
        if (dres) PQclear(dres);
    }

    if (in_tx && ctx->db->last_primary[0]) goto done;

    if (param_present(params, "effort_estimate")) {
        provided++;
        double v;
        const char *estr = NULL;
        char buf[64];
        if (!param_is_null(params, "effort_estimate") && param_double(params, "effort_estimate", &v)) {
            if (v < 0)
                return mcp_tool_err(id, "effort_estimate must be >= 0");
            snprintf(buf, sizeof(buf), "%g", v);
            estr = buf;
        }
        updated += ticket_update_effort_estimate(ctx->db, ticket_id, estr);
        if (in_tx && ctx->db->last_primary[0]) goto done;
    }

    if (param_present(params, "effort_actual")) {
        provided++;
        double v;
        const char *astr = NULL;
        char buf[64];
        if (!param_is_null(params, "effort_actual") && param_double(params, "effort_actual", &v)) {
            if (v < 0)
                return mcp_tool_err(id, "effort_actual must be >= 0");
            snprintf(buf, sizeof(buf), "%g", v);
            astr = buf;
        }
        updated += ticket_update_effort_actual(ctx->db, ticket_id, astr);
        if (in_tx && ctx->db->last_primary[0]) goto done;
    }

    if (param_present(params, "effort_unit")) {
        provided++;
        const char *unit = param_is_null(params, "effort_unit")
                          ? NULL : param_str(params, "effort_unit");
        updated += ticket_update_effort_unit(ctx->db, ticket_id, unit);
        if (in_tx && ctx->db->last_primary[0]) goto done;
    }

done:
    if (!provided)
        return mcp_tool_err(id, "No updatable fields provided (title, description, docs_required, "
                                 "effort_estimate, effort_actual, effort_unit)");
    if (!updated)
        return ctx->db->last_primary[0]
            ? kanban_db_error(ctx, id, "Failed to update ticket")
            : mcp_tool_err(id, "Ticket not found");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "updated_fields", updated);
    if (in_tx) {  /* the token for the next expected_updated_at */
        char tid[32];
        snprintf(tid, sizeof(tid), "%d", ticket_id);
        const char *p[1] = {tid};
        PGresult *res = PQexecParams(ctx->db->conn,
            "SELECT updated_at::text FROM tickets WHERE id = $1", 1, NULL, p, NULL, NULL, 0);
        if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
            cJSON_AddStringToObject(r, "updated_at", PQgetvalue(res, 0, 0));
        if (res) PQclear(res);
    }
    return mcp_tool_ok(id, r);
}

static bool is_tool_error(const cJSON *resp) {
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError"));
}

static cJSON *tool_update_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    if (!param_num(params, "ticket_id", &ticket_id))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");
    const char *expected = param_str(params, "expected_updated_at");
    if (!expected) return update_ticket_fields(ctx, id, params, ticket_id, false);

    /* Check and write under a row lock so no other writer slips in between. */
    if (!db_begin_transaction(ctx->db)) return mcp_tool_err(id, "Failed to start transaction");
    char tid[32];
    snprintf(tid, sizeof(tid), "%d", ticket_id);
    const char *sel[1] = {tid};
    PGresult *cur = PQexecParams(ctx->db->conn,
        "SELECT updated_at::text FROM tickets WHERE id = $1 FOR NO KEY UPDATE",
        1, NULL, sel, NULL, NULL, 0);
    if (!cur || PQresultStatus(cur) != PGRES_TUPLES_OK || PQntuples(cur) == 0) {
        bool missing = cur && PQresultStatus(cur) == PGRES_TUPLES_OK;
        if (cur) PQclear(cur);
        return tx_fail(ctx, id, missing ? "Ticket not found" : "Failed to read ticket");
    }
    char current[64];
    snprintf(current, sizeof(current), "%s", PQgetvalue(cur, 0, 0));
    PQclear(cur);
    if (strcmp(expected, current) != 0) {
        char err[400];
        snprintf(err, sizeof(err),
            "Conflict: ticket changed since you read it (expected_updated_at=%s, current "
            "updated_at=%s). Re-read with kabai_get_ticket, re-apply your change, then retry "
            "with expected_updated_at=%s.", expected, current, current);
        return tx_fail(ctx, id, err);
    }

    db_clear_error(ctx->db);
    cJSON *resp = update_ticket_fields(ctx, id, params, ticket_id, true);
    if (is_tool_error(resp)) {
        db_rollback_transaction(ctx->db);
        return resp;
    }
    if (ctx->db->last_primary[0]) {  /* a partial write must not commit */
        cJSON_Delete(resp);
        return tx_db_fail(ctx, id, NULL, "Failed to update ticket");
    }
    if (!db_commit_transaction(ctx->db)) {
        cJSON_Delete(resp);
        return tx_fail(ctx, id, "Failed to commit transaction");
    }
    return resp;
}

static cJSON *tool_delete_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    if (!param_num(params, "ticket_id", &ticket_id))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");
    const char *reason = param_str(params, "reason");
    if (!reason || reason[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: reason (required for audit trail)");

    /* Verify the ticket exists before deleting */
    Ticket *t = ticket_get_by_id(ctx->db, ticket_id);
    if (!t) return mcp_tool_err(id, "Ticket not found");
    ticket_free(t);

    if (!ticket_delete(ctx->db, ticket_id))
        return ctx->db->last_primary[0]
            ? kanban_db_error(ctx, id, "Failed to delete ticket")
            : mcp_tool_err(id, "Failed to delete ticket");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "deleted_ticket_id", ticket_id);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Relations
 * ============================================================================ */

/* Shared argument parsing of link/unlink (#970, #830). relation_type is
 * canonical, link_type its alias (same name as kabai_docs_link_notes).
 * blocked_by is the natural reading of blocks: "from is blocked by to" is
 * stored as "to blocks from". Returns NULL, or the error text (in buf). */
static const char *relation_args_msg(cJSON *params, int *from_id, int *to_id,
                                     const char **rel, char *buf, size_t size) {
    *rel = param_str_alias(params, "relation_type", "link_type");
    bool ok[3] = {param_num(params, "from_ticket_id", from_id),
                  param_num(params, "to_ticket_id", to_id), *rel != NULL};
    if (!ok[0] || !ok[1] || !ok[2]) {
        const char *names[3] = {"from_ticket_id", "to_ticket_id",
                                "relation_type (alias: link_type)"};
        return missing_params_msg(buf, size, names, ok, 3);
    }
    if (strcmp(*rel, "blocked_by") == 0) {
        int tmp = *from_id; *from_id = *to_id; *to_id = tmp;
        *rel = "blocks";
    }
    return NULL;
}

static cJSON *read_relation_args(cJSON *id, cJSON *params, int *from_id, int *to_id,
                                 const char **rel) {
    char buf[200];
    const char *msg = relation_args_msg(params, from_id, to_id, rel, buf, sizeof(buf));
    return msg ? mcp_tool_err(id, msg) : NULL;
}

static const char *INVALID_RELATION_MSG =
    "Invalid relation_type: must be parent_of, blocks, blocked_by, duplicate_of, or relates_to";

/* links: [{from_ticket_id, to_ticket_id, relation_type}] in one transaction. */
static cJSON *link_tickets_batch(McpContext *ctx, cJSON *id, cJSON *links) {
    int n = cJSON_GetArraySize(links);
    if (!cJSON_IsArray(links) || n == 0)
        return mcp_tool_err(id, "links must be a non-empty array of "
                                "{from_ticket_id, to_ticket_id, relation_type}");
    if (!db_begin_transaction(ctx->db)) return mcp_tool_err(id, "Failed to start transaction");
    for (int i = 0; i < n; i++) {
        cJSON *l = cJSON_GetArrayItem(links, i);
        int from_id, to_id;
        const char *rel;
        char buf[200], msg[300];
        const char *bad = cJSON_IsObject(l)
            ? relation_args_msg(l, &from_id, &to_id, &rel, buf, sizeof(buf))
            : "must be an object";
        if (!bad && !valid_relation_type(rel)) bad = INVALID_RELATION_MSG;
        if (bad) {
            snprintf(msg, sizeof(msg), "links[%d]: %s. Nothing was linked.", i, bad);
            return tx_fail(ctx, id, msg);
        }
        if (!ticket_link(ctx->db, from_id, to_id, rel)) {
            snprintf(msg, sizeof(msg), "links[%d] (%d -> %d %s)", i, from_id, to_id, rel);
            return tx_db_fail(ctx, id, msg, "Failed to create relation");
        }
    }
    if (!db_commit_transaction(ctx->db)) return tx_fail(ctx, id, "Failed to commit transaction");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "linked", n);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_link_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    cJSON *links = cJSON_GetObjectItemCaseSensitive(params, "links");
    if (links) return link_tickets_batch(ctx, id, links);

    int from_id, to_id;
    const char *rel;
    cJSON *err = read_relation_args(id, params, &from_id, &to_id, &rel);
    if (err) return err;

    if (!valid_relation_type(rel))
        return mcp_tool_err(id, INVALID_RELATION_MSG);

    if (!ticket_link(ctx->db, from_id, to_id, rel))
        return kanban_db_error(ctx, id,
            "Failed to create relation (may already exist or ticket not found)");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    const char *given = param_str_alias(params, "relation_type", "link_type");
    if (given && strcmp(given, "blocked_by") == 0) {  /* show the swap */
        cJSON_AddNumberToObject(r, "from_ticket_id", from_id);
        cJSON_AddNumberToObject(r, "to_ticket_id", to_id);
        cJSON_AddStringToObject(r, "relation_type", rel);
    }
    return mcp_tool_ok(id, r);
}

static cJSON *tool_unlink_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    int from_id, to_id;
    const char *rel;
    cJSON *err = read_relation_args(id, params, &from_id, &to_id, &rel);
    if (err) return err;

    if (!ticket_unlink(ctx->db, from_id, to_id, rel))
        return ctx->db->last_primary[0]
            ? kanban_db_error(ctx, id, "Failed to delete relation")
            : mcp_tool_err(id, "Relation not found or could not be deleted");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Tasks
 * ============================================================================ */

/* titles: [string] (plus an optional single title) in one transaction. */
static cJSON *add_tasks_batch(McpContext *ctx, cJSON *id, int ticket_id, const char *title,
                              cJSON *titles) {
    if (!cJSON_IsArray(titles) || cJSON_GetArraySize(titles) == 0)
        return mcp_tool_err(id, "titles must be a non-empty array of strings");
    if (!db_begin_transaction(ctx->db)) return mcp_tool_err(id, "Failed to start transaction");
    cJSON *out = cJSON_CreateArray();
    int i = title ? -1 : 0;  /* -1: the single title goes first */
    cJSON *t = titles->child;
    while (i < cJSON_GetArraySize(titles)) {
        const char *text = i < 0 ? title : (cJSON_IsString(t) ? t->valuestring : NULL);
        char label[40];
        snprintf(label, sizeof(label), i < 0 ? "title" : "titles[%d]", i);
        if (!text || !text[0]) {
            cJSON_Delete(out);
            return extras_fail(ctx, id, "", label, "must be a non-empty string; nothing was added");
        }
        TicketTask *task = ticket_add_task(ctx->db, ticket_id, text);
        if (!task) {
            cJSON_Delete(out);
            return tx_db_fail(ctx, id, label, "Failed to add task");
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", task->id);
        cJSON_AddStringToObject(o, "title", task->title);
        cJSON_AddItemToArray(out, o);
        ticket_task_free(task);
        if (i >= 0) t = t->next;
        i++;
    }
    if (!db_commit_transaction(ctx->db)) {
        cJSON_Delete(out);
        return tx_fail(ctx, id, "Failed to commit transaction");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "ticket_id", ticket_id);
    cJSON_AddItemToObject(r, "tasks", out);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_add_task(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    const char *title = param_str(params, "title");
    cJSON *titles = cJSON_GetObjectItemCaseSensitive(params, "titles");
    if (!param_num(params, "ticket_id", &ticket_id) || (!title && !titles))
        return mcp_tool_err(id, titles ? "Missing required parameter: ticket_id"
                                       : "Missing required parameters: ticket_id, title");
    if (titles) return add_tasks_batch(ctx, id, ticket_id, title, titles);

    TicketTask *task = ticket_add_task(ctx->db, ticket_id, title);
    if (!task) return kanban_db_error(ctx, id, "Failed to add task");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", task->id);
    cJSON_AddNumberToObject(r, "ticket_id", task->ticket_id);
    cJSON_AddStringToObject(r, "title", task->title);
    cJSON_AddBoolToObject(r, "is_completed", task->is_completed);
    ticket_task_free(task);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_complete_task(McpContext *ctx, cJSON *id, cJSON *params) {
    int task_id;
    if (!param_num(params, "task_id", &task_id))
        return mcp_tool_err(id, "Missing required parameter: task_id");

    if (!ticket_complete_task(ctx->db, task_id))
        return ctx->db->last_primary[0]
            ? kanban_db_error(ctx, id, "Failed to complete task")
            : mcp_tool_err(id, "Task not found (or update failed)");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}


static cJSON *tool_complete_tasks(McpContext *ctx, cJSON *id, cJSON *params) {
    cJSON *ids_j = cJSON_GetObjectItemCaseSensitive(params, "task_ids");
    if (!cJSON_IsArray(ids_j))
        return mcp_tool_err(id, "Missing required parameter: task_ids (array of task IDs "
                                "from kabai_get_ticket_detailed)");

    int total = cJSON_GetArraySize(ids_j), done = 0;
    cJSON *not_found = cJSON_CreateArray();
    cJSON *failed = cJSON_CreateArray();
    cJSON *invalid = cJSON_CreateArray();
    for (int i = 0; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(ids_j, i);
        if (!cJSON_IsNumber(item) || item->valuedouble != (double)(int)item->valuedouble) {
            cJSON_AddItemToArray(invalid, cJSON_Duplicate(item, 1));
        } else if (ticket_complete_task(ctx->db, item->valueint)) {
            done++;
        } else if (ctx->db->last_primary[0]) {
            cJSON *f = cJSON_CreateObject();
            cJSON_AddNumberToObject(f, "task_id", item->valueint);
            cJSON_AddStringToObject(f, "error", ctx->db->last_primary);
            cJSON_AddItemToArray(failed, f);
        } else {
            cJSON_AddItemToArray(not_found, cJSON_CreateNumber(item->valueint));
        }
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "total", total);
    cJSON_AddNumberToObject(r, "completed", done);
    cJSON_AddItemToObject(r, "not_found", not_found);
    cJSON_AddItemToObject(r, "failed", failed);
    cJSON_AddItemToObject(r, "invalid", invalid);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Comments / Work Log
 * ============================================================================ */

static cJSON *tool_add_comment(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    const char *author = param_str(params, "author");
    const char *text   = param_str(params, "text");
    if (!param_num(params, "ticket_id", &ticket_id) || !author || !text)
        return mcp_tool_err(id, "Missing required parameters: ticket_id, author, text");

    TicketComment *c = comment_add(ctx->db, ticket_id, author, text);
    if (!c) return kanban_db_error(ctx, id, "Failed to add comment");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", c->id);
    cJSON_AddNumberToObject(r, "ticket_id", c->ticket_id);
    cJSON_AddStringToObject(r, "author", c->author);
    cJSON_AddStringToObject(r, "text", c->comment_text);
    if (c->created_at) cJSON_AddStringToObject(r, "created_at", c->created_at);
    comment_free(c);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_list_comments(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    if (!param_num(params, "ticket_id", &ticket_id))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    TicketComment **comments = comment_list_by_ticket(ctx->db, ticket_id);
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


/* ============================================================================
 * Board Statuses & Workflow
 * ============================================================================ */

static cJSON *tool_list_board_statuses(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id;
    if (!param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    BoardStatus **statuses = board_status_list_by_project(ctx->db, project_id);
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

static cJSON *tool_create_board_status(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id, position;
    const char *name = param_str(params, "name");
    const char *disp = param_str(params, "display_name");
    if (!param_num(params, "project_id", &project_id) || !name || !disp ||
        !param_num(params, "position", &position))
        return mcp_tool_err(id,
            "Missing required parameters: project_id, name, display_name, position");

    BoardStatus *bs = board_status_create(
        ctx->db, project_id, name, disp, position,
        param_str(params, "agent_role_instruction"),
        NULL  /* special_type not exposed via MCP — managed internally */
    );
    if (!bs) return kanban_db_error(ctx, id, "Failed to create board status");

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

static cJSON *tool_create_status_transition(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id, from_id, to_id;
    if (!param_num(params, "project_id", &project_id) ||
        !param_num(params, "from_status_id", &from_id) ||
        !param_num(params, "to_status_id", &to_id))
        return mcp_tool_err(id,
            "Missing required parameters: project_id, from_status_id, to_status_id");

    if (!status_transition_create(ctx->db, project_id, from_id, to_id))
        return kanban_db_error(ctx, id, "Failed to create status transition");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

/* Removes one explicit workflow edge (#847). Errors name the way out. */
static cJSON *tool_delete_status_transition(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id, from_id, to_id;
    bool ok[3] = {param_num(params, "project_id", &project_id),
                  param_num(params, "from_status_id", &from_id),
                  param_num(params, "to_status_id", &to_id)};
    if (!ok[0] || !ok[1] || !ok[2]) {
        const char *names[3] = {"project_id", "from_status_id", "to_status_id"};
        char buf[120];
        return mcp_tool_err(id, missing_params_msg(buf, sizeof(buf), names, ok, 3));
    }

    /* Guard the escalation exit: a ticket in human_intervention must keep an
     * explicit edge out (to human_answered), otherwise it can never leave. */
    char gp[32], gf[32], gt[32];
    snprintf(gp, sizeof(gp), "%d", project_id);
    snprintf(gf, sizeof(gf), "%d", from_id);
    snprintf(gt, sizeof(gt), "%d", to_id);
    const char *gq[3] = {gp, gf, gt};
    bool from_is_hi = false, still_implicit = false;
    PGresult *g = PQexecParams(ctx->db->conn,
        "SELECT f.special_type = 'human_intervention', "
        "       t.special_type = 'human_intervention' OR f.special_type = 'human_answered', "
        "       (SELECT count(*) FROM status_transitions s WHERE s.project_id = $1 "
        "          AND s.from_status_id = $2 AND s.to_status_id <> $3), "
        "       (SELECT count(*) FROM status_transitions s WHERE s.project_id = $1 "
        "          AND s.from_status_id = $2 AND s.to_status_id = $3) "
        "FROM board_statuses f, board_statuses t "
        "WHERE f.id = $2 AND f.project_id = $1 AND t.id = $3 AND t.project_id = $1",
        3, NULL, gq, NULL, NULL, 0);
    if (g && PQresultStatus(g) == PGRES_TUPLES_OK && PQntuples(g) > 0) {
        from_is_hi     = PQgetvalue(g, 0, 0)[0] == 't';
        still_implicit = PQgetvalue(g, 0, 1)[0] == 't';
        if (from_is_hi && atoi(PQgetvalue(g, 0, 2)) == 0 && atoi(PQgetvalue(g, 0, 3)) == 1) {
            PQclear(g);
            return mcp_tool_err(id,
                "Refused: this is the last explicit edge out of a human_intervention column; "
                "tickets escalated there could never leave (escalation flow: human_intervention -> "
                "human_answered -> any column). Create the replacement edge first with "
                "kabai_create_status_transition, then delete this one");
        }
    }
    if (g) PQclear(g);

    int n = status_transition_delete(ctx->db, project_id, from_id, to_id);
    if (n < 0)
        return kanban_db_error(ctx, id, "Failed to delete status transition");

    char pid[32], fid[32], tid[32];
    snprintf(pid, sizeof(pid), "%d", project_id);
    snprintf(fid, sizeof(fid), "%d", from_id);
    snprintf(tid, sizeof(tid), "%d", to_id);

    if (n == 0) {
        char msg[700];
        const char *qp[3] = {pid, fid, tid};
        PGresult *res = PQexecParams(ctx->db->conn,
            "SELECT (SELECT special_type FROM board_statuses WHERE id = $2 AND project_id = $1),"
            "       (SELECT special_type FROM board_statuses WHERE id = $3 AND project_id = $1),"
            "       EXISTS (SELECT 1 FROM board_statuses WHERE id = $2 AND project_id = $1),"
            "       EXISTS (SELECT 1 FROM board_statuses WHERE id = $3 AND project_id = $1)",
            3, NULL, qp, NULL, NULL, 0);
        bool have_from = res && PQresultStatus(res) == PGRES_TUPLES_OK &&
                         PQgetvalue(res, 0, 2)[0] == 't';
        bool have_to   = res && PQresultStatus(res) == PGRES_TUPLES_OK &&
                         PQgetvalue(res, 0, 3)[0] == 't';
        bool implicit  = have_from && have_to &&
            (strcmp(PQgetvalue(res, 0, 1), "human_intervention") == 0 ||
             strcmp(PQgetvalue(res, 0, 0), "human_answered") == 0);
        if (res) PQclear(res);

        if (!have_from || !have_to)
            return mcp_tool_err(id,
                "from_status_id/to_status_id does not belong to the given project_id: "
                "look the columns up via kabai_list_board_statuses(project_id)");
        if (implicit)
            return mcp_tool_err(id,
                "This is an implicit edge (into human_intervention / out of human_answered): "
                "the database always allows it and it cannot be removed. "
                "kabai_list_status_transitions marks such edges with implicit:true");

        int pos = snprintf(msg, sizeof(msg), "No such transition %d -> %d. Transitions out of %d:",
                           from_id, to_id, from_id);
        StatusTransition **trans = status_transition_list_by_project(ctx->db, project_id);
        int found = 0;
        for (int i = 0; trans && trans[i] && pos < (int)sizeof(msg) - 16; i++)
            if (trans[i]->from_status_id == from_id) {
                pos += snprintf(msg + pos, sizeof(msg) - pos, "%s %d",
                                found ? "," : "", trans[i]->to_status_id);
                found++;
            }
        status_transition_free_array(trans);
        if (!found) snprintf(msg + pos, sizeof(msg) - pos, " none");
        return mcp_tool_err(id, msg);
    }

    /* Tickets are not bound to transitions; report who is affected. */
    const char *cp[2] = {pid, fid};
    int in_source = 0;
    PGresult *cnt = PQexecParams(ctx->db->conn,
        "SELECT count(*) FROM tickets WHERE project_id = $1 AND status_id = $2",
        2, NULL, cp, NULL, NULL, 0);
    if (cnt && PQresultStatus(cnt) == PGRES_TUPLES_OK) in_source = atoi(PQgetvalue(cnt, 0, 0));
    if (cnt) PQclear(cnt);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "tickets_in_source_column", in_source);
    if (still_implicit) cJSON_AddBoolToObject(r, "still_allowed_implicitly", 1);
    return mcp_tool_ok(id, r);
}

/* Explicit edges plus the implicit ones the trigger always allows (#800):
 * any column -> human_intervention, human_answered -> any column. */
static cJSON *tool_list_status_transitions(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id;
    if (!param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    StatusTransition **trans = status_transition_list_by_project(ctx->db, project_id);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; trans && trans[i]; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "from_status_id", trans[i]->from_status_id);
        cJSON_AddNumberToObject(o, "to_status_id",   trans[i]->to_status_id);
        cJSON_AddItemToArray(arr, o);
    }
    status_transition_free_array(trans);

    char pid[32];
    snprintf(pid, sizeof(pid), "%d", project_id);
    const char *qp[1] = {pid};
    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT a.id, b.id, b.special_type = 'human_intervention' "
        "FROM board_statuses a JOIN board_statuses b "
        "  ON b.project_id = a.project_id AND b.id <> a.id "
        "WHERE a.project_id = $1 "
        "  AND (b.special_type = 'human_intervention' OR a.special_type = 'human_answered') "
        "  AND NOT EXISTS (SELECT 1 FROM status_transitions st WHERE st.project_id = a.project_id "
        "                  AND st.from_status_id = a.id AND st.to_status_id = b.id) "
        "ORDER BY a.id, b.id",
        1, NULL, qp, NULL, NULL, 0);
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(res); i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "from_status_id", atoi(PQgetvalue(res, i, 0)));
            cJSON_AddNumberToObject(o, "to_status_id",   atoi(PQgetvalue(res, i, 1)));
            cJSON_AddBoolToObject(o, "implicit", 1);
            cJSON_AddStringToObject(o, "reason",
                PQgetvalue(res, i, 2)[0] == 't'
                    ? "human_intervention is reachable from any column"
                    : "human_answered can move to any column");
            cJSON_AddItemToArray(arr, o);
        }
    }
    if (res) PQclear(res);
    return mcp_tool_ok(id, arr);
}


/* ============================================================================
 * Maintenance tools (Kanban AI #342, #343, #344)
 * ============================================================================ */

static cJSON *tool_update_board_status(McpContext *ctx, cJSON *id, cJSON *params) {
    int status_id;
    if (!param_num(params, "status_id", &status_id))
        return mcp_tool_err(id, "Missing required parameter: status_id");

    const char *disp = param_str(params, "display_name");
    const char *ari  = param_str(params, "agent_role_instruction");
    bool clear_ari   = param_is_null(params, "agent_role_instruction");
    int position     = -1;
    bool has_pos     = param_num(params, "position", &position);

    if (!disp && !ari && !clear_ari && !has_pos)
        return mcp_tool_err(id,
            "No updatable fields provided (display_name, agent_role_instruction, position)");

    char sid_str[32], pos_str[32];
    snprintf(sid_str, sizeof(sid_str), "%d", status_id);
    snprintf(pos_str, sizeof(pos_str), "%d", position);

    const char *q_params[5] = {
        sid_str, disp,
        clear_ari ? NULL : ari,
        clear_ari ? "t" : "f",
        has_pos ? pos_str : NULL
    };
    /* name and special_type are stable keys — deliberately not editable. */
    PGresult *res = PQexecParams(ctx->db->conn,
        "UPDATE board_statuses SET "
        "  display_name = COALESCE($2, display_name), "
        "  agent_role_instruction = CASE WHEN $4::bool THEN NULL "
        "                                ELSE COALESCE($3, agent_role_instruction) END, "
        "  position = COALESCE($5::int, position) "
        "WHERE id = $1 RETURNING id, project_id, name, display_name, position",
        5, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Board status not found");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", atoi(PQgetvalue(res, 0, 0)));
    cJSON_AddNumberToObject(r, "project_id", atoi(PQgetvalue(res, 0, 1)));
    cJSON_AddStringToObject(r, "name", PQgetvalue(res, 0, 2));
    cJSON_AddStringToObject(r, "display_name", PQgetvalue(res, 0, 3));
    cJSON_AddNumberToObject(r, "position", atoi(PQgetvalue(res, 0, 4)));
    PQclear(res);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_update_project(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id;
    if (!param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    const char *name = param_str(params, "name");
    const char *desc = param_str(params, "description");
    bool clear_desc  = param_is_null(params, "description");

    if (!name && !desc && !clear_desc)
        return mcp_tool_err(id, "No updatable fields provided (name, description)");

    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", project_id);
    const char *q_params[4] = {pid_str, name, clear_desc ? NULL : desc,
                               clear_desc ? "t" : "f"};
    /* slug is the stable key — deliberately not editable. */
    PGresult *res = PQexecParams(ctx->db->conn,
        "UPDATE projects SET "
        "  name = COALESCE($2, name), "
        "  description = CASE WHEN $4::bool THEN NULL "
        "                     ELSE COALESCE($3, description) END "
        "WHERE id = $1 RETURNING id, slug, name, description",
        4, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return mcp_tool_err(id, "Project not found");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", atoi(PQgetvalue(res, 0, 0)));
    cJSON_AddStringToObject(r, "slug", PQgetvalue(res, 0, 1));
    cJSON_AddStringToObject(r, "name", PQgetvalue(res, 0, 2));
    if (!PQgetisnull(res, 0, 3))
        cJSON_AddStringToObject(r, "description", PQgetvalue(res, 0, 3));
    PQclear(res);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_update_task(McpContext *ctx, cJSON *id, cJSON *params) {
    int task_id;
    const char *title = param_str(params, "title");
    if (!param_num(params, "task_id", &task_id) || !title)
        return mcp_tool_err(id, "Missing required parameters: task_id, title");

    char tid_str[32];
    snprintf(tid_str, sizeof(tid_str), "%d", task_id);
    const char *q_params[2] = {tid_str, title};
    PGresult *res = PQexecParams(ctx->db->conn,
        "UPDATE ticket_tasks SET title = $2 "
        "WHERE id = $1 RETURNING id, ticket_id, title, is_completed",
        2, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        db_capture_error(ctx->db, res);
        if (res) PQclear(res);
        return ctx->db->last_primary[0]
            ? kanban_db_error(ctx, id, "Failed to update task")
            : mcp_tool_err(id, "Task not found");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", atoi(PQgetvalue(res, 0, 0)));
    cJSON_AddNumberToObject(r, "ticket_id", atoi(PQgetvalue(res, 0, 1)));
    cJSON_AddStringToObject(r, "title", PQgetvalue(res, 0, 2));
    cJSON_AddBoolToObject(r, "is_completed", PQgetvalue(res, 0, 3)[0] == 't');
    PQclear(res);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_delete_task(McpContext *ctx, cJSON *id, cJSON *params) {
    int task_id;
    if (!param_num(params, "task_id", &task_id))
        return mcp_tool_err(id, "Missing required parameter: task_id");
    const char *reason = param_str(params, "reason");
    if (!reason || reason[0] == '\0')
        return mcp_tool_err(id, "Missing required parameter: reason (required for audit trail)");

    char tid_str[32];
    snprintf(tid_str, sizeof(tid_str), "%d", task_id);
    const char *q_params[1] = {tid_str};
    PGresult *res = PQexecParams(ctx->db->conn,
        "DELETE FROM ticket_tasks WHERE id = $1 RETURNING ticket_id, title",
        1, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        db_capture_error(ctx->db, res);
        if (res) PQclear(res);
        return ctx->db->last_primary[0]
            ? kanban_db_error(ctx, id, "Failed to delete task")
            : mcp_tool_err(id, "Task not found");
    }

    int ticket_id = atoi(PQgetvalue(res, 0, 0));
    char *task_title = strdup(PQgetvalue(res, 0, 1));
    PQclear(res);

    /* The deletion reason becomes part of the ticket's work log so the
     * removed acceptance criterion stays auditable. */
    const char *author = ctx->agent_name ? ctx->agent_name : "kabai";
    char text[1024];
    snprintf(text, sizeof(text),
             "Acceptance criterion deleted: \"%s\" — reason: %s",
             task_title ? task_title : "(unknown)", reason);
    free(task_title);
    TicketComment *c = comment_add(ctx->db, ticket_id, author, text);
    if (c) comment_free(c);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "deleted_task_id", task_id);
    cJSON_AddNumberToObject(r, "ticket_id", ticket_id);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Registration
 *
 * Order matches the pre-framework tools/list output so the migration is
 * byte-identical for MCP clients.
 * ============================================================================ */

void kanban_register_tools(McpRegistry *r) {
    cJSON *s;

    /* ---- Projects ---- */

    s = schema_new();
    schema_str(s, "slug",        "Short unique identifier (e.g. 'robot-game')", true);
    schema_str(s, "name",        "Human-readable display name", true);
    schema_str(s, "description", "Optional project description", false);
    mcp_registry_add(r, "kabai_create_project",
        "Create a new project/board", s, tool_create_project);

    s = schema_new();
    schema_bool(s, "include_archived",
        "Include archived projects (default false — archived projects are hidden by default; "
        "a human reactivates them in the UI, no MCP tool does this).", false);
    mcp_registry_add(r, "kabai_list_projects",
        "List projects. Archived projects are excluded by default (include_archived:true to see them).",
        s, tool_list_projects);

    s = schema_new();
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kabai_get_project",
        "Get project details", s, tool_get_project);

    /* ---- Tickets ---- */

    s = schema_new();
    schema_num(s, "project_id",  "ID of the project this ticket belongs to", true);
    schema_num(s, "status_id",   "ID of the initial board column/status (required; alias: new_status_id)", false);
    schema_num(s, "new_status_id", "Alias of status_id (optional)", false);
    schema_str(s, "title",       "Ticket title", true);
    schema_str(s, "description", "Optional detailed description", false);
    schema_str(s, "type",        "'ticket' (default) or 'epic'", false);
    schema_bool(s, "docs_required",
        "If true, the ticket cannot move to done without a linked knowledge-base note "
        "(kabai_docs_link_ticket). Set it on architecturally relevant work.", false);
    schema_str_array(s, "tasks",
        "Acceptance-criterion tasks to create with the ticket (optional; same transaction)", false);
    schema_num(s, "parent_id", "Epic/parent ticket ID: creates parent_of(parent_id -> new ticket) (optional)", false);
    schema_num_array(s, "blocks", "Ticket IDs that wait for this ticket (this blocks them) (optional)", false);
    schema_num_array(s, "blocked_by", "Ticket IDs this ticket waits for (they block this ticket) (optional)", false);
    schema_bool(s, "verbose", "Echo type, description and docs_required back (default false: slim response)", false);
    mcp_registry_add(r, "kabai_create_ticket",
        "Create a new ticket or epic in a project. "
        "Use type='epic' for high-level goals that group child tickets. "
        "tasks, parent_id, blocks and blocked_by are created in the same transaction: if any "
        "value is invalid nothing is created and the error names the value. "
        "Changed: the response is slim - {id, project_id, status_id, title} - and no longer echoes "
        "the description; pass verbose=true for the full echo. "
        "For many tickets at once use kabai_create_tickets.",
        s, tool_create_ticket);

    s = schema_new();
    schema_num(s, "project_id", "Project ID for all items", true);
    schema_num(s, "status_id", "Default board column for items without their own status_id (optional; alias: new_status_id)", false);
    schema_obj_array(s, "items",
        "1-50 items, created atomically. Each item takes the kabai_create_ticket fields (title, "
        "description, type, status_id, docs_required, tasks, blocks, blocked_by) plus ref (a batch-local "
        "name such as \"t1\": letters, digits, underscore) and parent (alias parent_id). parent, blocks "
        "and blocked_by take ticket IDs or \"$t1\" references to earlier OR later items of the same call. "
        "\"$t1\" inside title/description becomes \"#<id>\".", true);
    mcp_registry_add(r, "kabai_create_tickets",
        "Create up to 50 tickets (with tasks and relations) in one atomic call. Local references "
        "(ref / \"$ref\") let items link each other and mention each other's #id before the ids exist. "
        "Any invalid item, unknown or duplicate ref, or failed relation creates nothing; the error names "
        "the item index and ref. Returns [{ref, id, status_id}] in item order. Archived projects are refused.",
        s, tool_create_tickets);

    s = schema_new();
    schema_num(s, "project_id", "ID of the project to list tickets from", true);
    schema_num(s, "status_id",  "Filter by status column (optional)", false);
    schema_str(s, "type",       "Filter by type: 'ticket' or 'epic' (optional)", false);
    schema_str(s, "assignee",   "Filter by exact assignee name, e.g. to resume your own work (optional)", false);
    schema_num(s, "limit",      "Max tickets to return (optional, default unlimited)", false);
    schema_num(s, "offset",     "Tickets to skip for pagination (optional, default 0)", false);
    schema_bool(s, "workable",
        "If true, return only pickup-ready tickets (default false): unassigned, no open 'blocks' predecessor "
        "(every ticket that blocks it is in done), in a column an agent may pick up from (not done, not "
        "human_intervention/human_answered; without status_id only columns with an agent_role_instruction). "
        "Adds open_tasks per ticket. Works with project_id of an archived project like any explicit project.", false);
    schema_bool(s, "summary",
        "If true, omit description fields — returns only id/status_id/title/assignee/timestamps. "
        "Use for an overview when descriptions are not needed.", false);
    mcp_registry_add(r, "kabai_list_tickets",
        "List tickets in a project. Supports status, type and assignee filters, workable:true (pickup-ready tickets), pagination (limit/offset), "
        "and summary mode (omits description). Use summary:true + status_id for cheap overview calls.",
        s, tool_list_tickets);

    s = schema_new();
    schema_str(s, "query",
        "Search words. Several words must all match (anywhere, any order); "
        "\"quoted phrase\", OR, -excluded. No stemming: exact word forms only.", true);
    schema_num(s, "project_id", "Restrict to one project (optional, default: search all projects)", false);
    schema_num(s, "limit", "Max results (optional, default 20, capped at 100)", false);
    schema_bool(s, "include_archived",
        "Global search only: also search archived projects (optional, default false). "
        "Ignored when project_id is given (an explicit project is always searched).", false);
    schema_bool(s, "full",
        "If true, also return description, effort fields and timestamps per hit "
        "(optional, default false: lean rows only)", false);
    mcp_registry_add(r, "kabai_search_tickets",
        "Full-text search over ticket title and description, ranked by relevance (title "
        "hits first). Returns lean rows: id, project_id, status_id, status_name, type, "
        "title, assignee, rank, match_type (fts, or title_similarity as typo-tolerant "
        "fallback when nothing matches) and a snippet around the match. Pass full:true "
        "for descriptions. Searches all non-archived projects unless project_id is given. Use BEFORE "
        "create_ticket to detect duplicates. Changed: the default answer no longer "
        "contains descriptions, and the query is no longer a plain substring.",
        s, tool_search_tickets);

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    mcp_registry_add(r, "kabai_get_ticket",
        "Get basic ticket information including timestamps", s, tool_get_ticket);

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    schema_bool(s, "include_role_instruction",
        "Include agent_role_instruction in response (default true). "
        "Pass false after the first call to avoid repeating the same instruction for tickets in the same column.",
        false);
    schema_num(s, "comments_limit",
        "Return only the newest N work-log comments (optional, default: all). When "
        "older ones are cut, the answer carries comments_total; read them via "
        "kabai_list_comments.", false);
    mcp_registry_add(r, "kabai_get_ticket_detailed",
        "Get ticket with all tasks (acceptance criteria), work log, timestamps, "
        "linked knowledge-base notes (linked_notes — read them via kabai_docs_get_note "
        "before starting work), and attachments (image metadata incl. description — "
        "the alt-text for non-multimodal agents; fetch the actual image via "
        "kabai_get_attachment only when you need it, never automatically). Pass "
        "include_role_instruction:false on subsequent calls within the same session "
        "to avoid redundant context.",
        s, tool_get_ticket_detailed);

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    schema_num(s, "new_status_id",
        "Target column/status ID. Must be an allowed transition per workflow graph. Required (alias: status_id).", false);
    schema_num(s, "status_id", "Alias of new_status_id (optional)", false);
    mcp_registry_add(r, "kabai_move_ticket",
        "Move a ticket to a new column. Rejected by the database if the transition "
        "is not in the workflow graph or if acceptance criteria are unmet.",
        s, tool_move_ticket);

    s = schema_new();
    schema_num_array(s, "ticket_ids", "Array of ticket IDs to move", true);
    schema_num(s, "new_status_id", "Target status ID for all tickets (required; alias: status_id)", false);
    schema_num(s, "status_id", "Alias of new_status_id (optional)", false);
    mcp_registry_add(r, "kabai_move_tickets",
        "Batch move multiple tickets to the same new status in one call. "
        "Returns per-ticket success/error breakdown.",
        s, tool_move_tickets);

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    schema_str(s, "assignee",
        "Agent or user identifier. If omitted, falls back to KABAI_AGENT_NAME env var.", false);
    mcp_registry_add(r, "kabai_assign_ticket",
        "Assign a ticket to an agent or user. Uses KABAI_AGENT_NAME as default assignee "
        "and always writes KABAI_AGENT_MODEL to the model field. "
        "Both KABAI_AGENT_NAME and KABAI_AGENT_MODEL must be set in the MCP server environment.",
        s, tool_assign_ticket);

    s = schema_new();
    schema_num(s, "ticket_id",   "Numeric ticket ID", true);
    schema_str(s, "title",       "New title (optional)", false);
    schema_str(s, "description", "New description, or null to clear (optional)", false);
    schema_bool(s, "docs_required",
        "Require a linked knowledge-base note before the ticket can close (optional). "
        "When unsetting it, leave a work-log comment justifying why no docs are needed.",
        false);
    schema_num(s, "effort_estimate",
        "Estimated effort, or null to clear (optional). Unit-agnostic — pair with effort_unit.",
        false);
    schema_num(s, "effort_actual",
        "Actual effort spent, or null to clear (optional). Unit-agnostic — pair with effort_unit.",
        false);
    schema_str(s, "effort_unit",
        "Free-text unit for effort_estimate/effort_actual (e.g. days, story points, tokens), "
        "or null to clear (optional).",
        false);
    schema_str(s, "expected_updated_at",
        "Concurrency guard (optional, default: none = last write wins): the updated_at you got from "
        "get_ticket or the last update. If the ticket changed since, nothing is written and the "
        "Conflict error names the current updated_at - re-read, re-apply, retry. On success the "
        "answer carries the new updated_at", false);
    mcp_registry_add(r, "kabai_update_ticket",
        "Edit a ticket's title, description, docs_required flag, and/or effort tracking "
        "(effort_estimate/effort_actual/effort_unit — generic, unit-agnostic; MCP has no "
        "protocol-level usage/cost data to fill this automatically). With parallel agents pass "
        "expected_updated_at to avoid silently overwriting a concurrent edit",
        s, tool_update_ticket);

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    schema_str(s, "reason",
        "Required reason for deletion (e.g. 'duplicate of #42', 'created by mistake')", true);
    mcp_registry_add(r, "kabai_delete_ticket",
        "Permanently delete a ticket (cascades tasks, comments, relations, note links). "
        "Requires a non-empty reason. Use merge_into comment on the surviving ticket before deleting duplicates.",
        s, tool_delete_ticket);

    /* ---- Relations ---- */

    s = schema_new();
    schema_num(s, "from_ticket_id", "Source ticket ID", true);
    schema_num(s, "to_ticket_id",   "Target ticket ID", true);
    schema_str(s, "relation_type",
        "Canonical name; required (alternatively link_type). One of: parent_of (epic→child), "
        "blocks (from_ticket must be done BEFORE to_ticket can start; to waits for from), "
        "blocked_by (the reverse reading: from waits for to; stored as to blocks from), "
        "duplicate_of (from is duplicate of to), relates_to (generic)", false);
    schema_str(s, "link_type", "Alias of relation_type, same name as kabai_docs_link_notes (optional)", false);
    schema_obj_array(s, "links",
        "Batch form (optional): [{from_ticket_id, to_ticket_id, relation_type}] created in one transaction; "
        "aliases and blocked_by work as in the single form. If given, the single-form parameters are ignored. "
        "One invalid link creates none and the error names it", false);
    mcp_registry_add(r, "kabai_link_tickets",
        "Create a directed relation between two tickets. blocks means: from_ticket must be done "
        "before to_ticket can start (to waits for from); blocked_by is the same edge read the other way. "
        "Use parent_of to link an epic to its child tickets. "
        "Relations are visible in get_ticket_detailed as the 'relations' array.",
        s, tool_link_tickets);

    s = schema_new();
    schema_num(s, "from_ticket_id", "Source ticket ID", true);
    schema_num(s, "to_ticket_id",   "Target ticket ID", true);
    schema_str(s, "relation_type",
        "The relation to remove (must match exactly what was created, blocked_by allowed); required, alternatively link_type", false);
    schema_str(s, "link_type", "Alias of relation_type (optional)", false);
    mcp_registry_add(r, "kabai_unlink_tickets",
        "Remove a directed relation between two tickets", s, tool_unlink_tickets);

    /* ---- Tasks ---- */

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    schema_str(s, "title",     "Task / acceptance criterion description (required unless titles is given)", false);
    schema_str_array(s, "titles", "Batch form (optional): several task descriptions added in one transaction", false);
    mcp_registry_add(r, "kabai_add_task",
        "Add an acceptance criterion task to a ticket. "
        "All tasks must be completed before the ticket can be moved to 'done'.",
        s, tool_add_task);

    s = schema_new();
    schema_num(s, "task_id", "Numeric task ID (from get_ticket_detailed)", true);
    mcp_registry_add(r, "kabai_complete_task",
        "Mark an acceptance criterion task as completed", s, tool_complete_task);

    s = schema_new();
    schema_num_array(s, "task_ids", "Array of task IDs (from get_ticket_detailed)", true);
    mcp_registry_add(r, "kabai_complete_tasks",
        "Batch version of complete_task: mark several acceptance criteria completed in one "
        "call. Returns total, completed, not_found (IDs that do not exist), failed "
        "([{task_id, error}], e.g. archived project) and invalid (items that are not "
        "integers).",
        s, tool_complete_tasks);

    /* ---- Comments / Work Log ---- */

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    schema_str(s, "author",    "Author identifier (e.g. 'claude-sonnet-4-6')", true);
    schema_str(s, "text",      "Comment / work log entry text", true);
    mcp_registry_add(r, "kabai_add_comment",
        "Add a work log entry or comment to a ticket. "
        "Use this to document progress and hand-off notes.",
        s, tool_add_comment);

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    mcp_registry_add(r, "kabai_list_comments",
        "List all work log entries / comments for a ticket", s, tool_list_comments);

    /* ---- Board Statuses & Workflow ---- */

    s = schema_new();
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kabai_list_board_statuses",
        "List all columns (board statuses) of a project including their "
        "agent_role_instruction. Call this first to discover status IDs and "
        "agent personas before creating tickets or setting up transitions.",
        s, tool_list_board_statuses);

    s = schema_new();
    schema_num(s, "project_id",   "Numeric project ID", true);
    schema_str(s, "name",         "Machine name, e.g. 'in_progress'", true);
    schema_str(s, "display_name", "Human-readable label, e.g. 'In Arbeit'", true);
    schema_num(s, "position",     "Column order (0-based)", true);
    schema_str(s, "agent_role_instruction",
        "Dynamic persona prompt injected when an agent picks up a ticket in this column (optional)",
        false);
    mcp_registry_add(r, "kabai_create_board_status",
        "Create a new board column/status for a project", s, tool_create_board_status);

    s = schema_new();
    schema_num(s, "project_id",     "Numeric project ID", true);
    schema_num(s, "from_status_id", "Source column ID", true);
    schema_num(s, "to_status_id",   "Target column ID", true);
    mcp_registry_add(r, "kabai_create_status_transition",
        "Define an allowed workflow transition between two columns. "
        "The database will reject moves not defined here.",
        s, tool_create_status_transition);

    s = schema_new();
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kabai_list_status_transitions",
        "List the allowed workflow transitions of a project. Entries with implicit:true are not "
        "stored but always allowed by the database: any column -> human_intervention and "
        "human_answered -> any column. Every move the database accepts can be derived from this list.",
        s, tool_list_status_transitions);

    s = schema_new();
    schema_num(s, "project_id",     "Numeric project ID", true);
    schema_num(s, "from_status_id", "Source column ID", true);
    schema_num(s, "to_status_id",   "Target column ID", true);
    mcp_registry_add(r, "kabai_delete_status_transition",
        "Remove one workflow transition (inverse of create_status_transition). Returns "
        "tickets_in_source_column; tickets are not bound to transitions and stay where they are. "
        "Implicit human_intervention/human_answered edges cannot be removed. An unknown "
        "transition lists the transitions out of the source column.",
        s, tool_delete_status_transition);

    /* ---- Maintenance ---- */

    s = schema_new();
    schema_num(s, "status_id",    "Numeric board status ID", true);
    schema_str(s, "display_name", "New human-readable label (optional)", false);
    schema_str(s, "agent_role_instruction",
        "New persona prompt for this column, or null to clear (optional)", false);
    schema_num(s, "position",     "New column order, 0-based (optional)", false);
    mcp_registry_add(r, "kabai_update_board_status",
        "Edit a board column: display_name, agent_role_instruction, position. "
        "The machine name and special_type are stable keys and cannot be changed. "
        "Use this to evolve a column's role instruction as the project changes.",
        s, tool_update_board_status);

    s = schema_new();
    schema_num(s, "project_id",  "Numeric project ID", true);
    schema_str(s, "name",        "New display name (optional)", false);
    schema_str(s, "description", "New description, or null to clear (optional)", false);
    mcp_registry_add(r, "kabai_update_project",
        "Edit a project's name and/or description (the slug is permanent). "
        "Keep descriptions current — they are the first context agents read via list_projects.",
        s, tool_update_project);

    s = schema_new();
    schema_num(s, "task_id", "Numeric task ID (from get_ticket_detailed)", true);
    schema_str(s, "title",   "Corrected task / acceptance criterion text", true);
    mcp_registry_add(r, "kabai_update_task",
        "Correct the title of an acceptance criterion task", s, tool_update_task);

    s = schema_new();
    schema_num(s, "task_id", "Numeric task ID (from get_ticket_detailed)", true);
    schema_str(s, "reason",  "Required reason for removing the criterion (e.g. 'obsolete after scope change')", true);
    mcp_registry_add(r, "kabai_delete_task",
        "Delete an acceptance criterion task. The reason is recorded as a work-log "
        "comment on the ticket so the removal stays auditable. Use for obsolete or "
        "mistaken criteria — a deleted task no longer blocks the move to done.",
        s, tool_delete_task);
}
