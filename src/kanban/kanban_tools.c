#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kanban/kanban_tools.h"
#include "mcp/schema.h"
#include "kanban/projects.h"
#include "kanban/tickets.h"
#include "kanban/comments.h"
#include "kanban/board_statuses.h"

/* ============================================================================
 * Error mapping
 * ============================================================================ */

/* Map the captured SQLSTATE/constraint of the last failed service call to
 * an actionable tool error (Kanban AI #345); mirrors docs_db_error in the
 * docs module. Falls back to the generic text when nothing was captured. */
static cJSON *kanban_db_error(McpContext *ctx, cJSON *id, const char *fallback) {
    DatabaseConnection *db = ctx->db;
    const char *st = db->last_sqlstate;
    const char *cn = db->last_constraint;

    if (strcmp(st, "23503") == 0) {  /* foreign_key_violation */
        if (strcmp(cn, "check_ticket_status_project") == 0 ||
            strcmp(cn, "check_same_project_from") == 0 ||
            strcmp(cn, "check_same_project_to") == 0)
            return mcp_tool_err(id,
                "status_id does not belong to the given project_id: board columns are "
                "per project — look them up via kabai_list_board_statuses(project_id)");
        if (strstr(cn, "project_id"))
            return mcp_tool_err(id,
                "project_id does not exist: check kabai_list_projects");
        if (strstr(cn, "status_id"))
            return mcp_tool_err(id,
                "status_id does not exist: check kabai_list_board_statuses(project_id)");
        if (strstr(cn, "ticket"))
            return mcp_tool_err(id,
                "ticket_id does not exist: check the ID (the ticket may have been deleted)");
        return mcp_tool_err(id, "Referenced row does not exist (foreign key violation)");
    }
    if (strcmp(st, "23505") == 0) {  /* unique_violation */
        if (strstr(cn, "slug"))
            return mcp_tool_err(id, "Slug already exists: project slugs are unique");
        if (strstr(cn, "board_statuses"))
            return mcp_tool_err(id, "A column with this name already exists in this project");
        return mcp_tool_err(id, "Already exists (duplicate)");
    }
    if (strcmp(st, "23514") == 0) {  /* check_violation */
        if (strstr(cn, "self"))
            return mcp_tool_err(id, "A ticket cannot relate to or block itself");
        return mcp_tool_err(id, db->last_primary[0] ? db->last_primary
                                                    : "Constraint violation");
    }
    if (strcmp(st, "P0001") == 0 && db->last_primary[0])  /* trigger RAISE */
        return mcp_tool_err(id, db->last_primary);
    if (db->last_primary[0]) {
        char buf[600];
        snprintf(buf, sizeof(buf), "%s: %s", fallback, db->last_primary);
        return mcp_tool_err(id, buf);
    }
    return mcp_tool_err(id, fallback);
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
    project_free(p);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_list_projects(McpContext *ctx, cJSON *id, cJSON *params) {
    (void)params;
    Project **projects = project_list_all(ctx->db);
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
    project_free(p);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Tickets
 * ============================================================================ */

static cJSON *tool_create_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id, status_id;
    const char *title = param_str(params, "title");
    if (!param_num(params, "project_id", &project_id) ||
        !param_num(params, "status_id", &status_id) || !title)
        return mcp_tool_err(id, "Missing required parameters: project_id, status_id, title");

    const char *type_val = param_str(params, "type");
    if (type_val && strcmp(type_val, "ticket") != 0 && strcmp(type_val, "epic") != 0)
        return mcp_tool_err(id, "Invalid type: must be 'ticket' or 'epic'");

    Ticket *t = ticket_create(ctx->db, project_id, status_id, title,
                              param_str(params, "description"), type_val);
    if (!t) return kanban_db_error(ctx, id, "Failed to create ticket");

    bool docs_required = param_bool(params, "docs_required", false);
    if (docs_required) {
        char tid_str[32];
        snprintf(tid_str, sizeof(tid_str), "%d", t->id);
        const char *dp[1] = {tid_str};
        PGresult *dres = PQexecParams(ctx->db->conn,
            "UPDATE tickets SET docs_required = TRUE WHERE id = $1",
            1, NULL, dp, NULL, NULL, 0);
        if (dres) PQclear(dres);
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", t->id);
    cJSON_AddNumberToObject(r, "project_id", t->project_id);
    cJSON_AddNumberToObject(r, "status_id", t->status_id);
    cJSON_AddStringToObject(r, "type", t->type ? t->type : "ticket");
    cJSON_AddStringToObject(r, "title", t->title);
    if (t->description) cJSON_AddStringToObject(r, "description", t->description);
    if (t->assignee)    cJSON_AddStringToObject(r, "assignee", t->assignee);
    if (docs_required)  cJSON_AddBoolToObject(r, "docs_required", 1);
    ticket_free(t);
    return mcp_tool_ok(id, r);
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
    bool summary = param_bool(params, "summary", false);

    Ticket **tickets = ticket_list_filtered(ctx->db, project_id, status_id, type_filter, limit, offset);
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
    ticket_free_array(tickets);
    return mcp_tool_ok(id, arr);
}

static cJSON *tool_search_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id;
    const char *query = param_str(params, "query");
    if (!param_num(params, "project_id", &project_id) || !query)
        return mcp_tool_err(id, "Missing required parameters: project_id, query");

    Ticket **tickets = ticket_search(ctx->db, project_id, query);
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
        if (tickets[i]->effort_estimate)
            cJSON_AddNumberToObject(o, "effort_estimate", atof(tickets[i]->effort_estimate));
        if (tickets[i]->effort_actual)
            cJSON_AddNumberToObject(o, "effort_actual", atof(tickets[i]->effort_actual));
        if (tickets[i]->effort_unit)
            cJSON_AddStringToObject(o, "effort_unit", tickets[i]->effort_unit);
        if (tickets[i]->created_at)
            cJSON_AddStringToObject(o, "created_at", tickets[i]->created_at);
        cJSON_AddItemToArray(arr, o);
    }
    ticket_free_array(tickets);
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
    cJSON_AddItemToObject(r, "linked_notes", notes_arr);
    cJSON_AddItemToObject(r, "attachments", attachments_arr);

    ticket_detailed_free(d);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_move_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id, new_status_id;
    if (!param_num(params, "ticket_id", &ticket_id) ||
        !param_num(params, "new_status_id", &new_status_id))
        return mcp_tool_err(id, "Missing required parameters: ticket_id, new_status_id");

    if (!ticket_update_status(ctx->db, ticket_id, new_status_id)) {
        /* Read error BEFORE any subsequent query clobbers it */
        const char *raw = PQerrorMessage(ctx->db->conn);
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
    if (!cJSON_IsArray(ids_j) || !param_num(params, "new_status_id", &new_status_id))
        return mcp_tool_err(id, "Missing required parameters: ticket_ids (array), new_status_id");

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
            if (raw && strstr(raw, "Illegaler Kanban-Move"))
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
        return mcp_tool_err(id, "Ticket not found (or assign failed)");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_update_ticket(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    if (!param_num(params, "ticket_id", &ticket_id))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");

    int updated = 0;
    int provided = 0;

    const char *title = param_str(params, "title");
    if (title) {
        provided++;
        updated += ticket_update_title(ctx->db, ticket_id, title);
    }

    if (param_present(params, "description")) {
        provided++;
        const char *new_desc = param_is_null(params, "description")
                             ? NULL : param_str(params, "description");
        updated += ticket_update_description(ctx->db, ticket_id, new_desc);
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
    }

    if (param_present(params, "effort_unit")) {
        provided++;
        const char *unit = param_is_null(params, "effort_unit")
                          ? NULL : param_str(params, "effort_unit");
        updated += ticket_update_effort_unit(ctx->db, ticket_id, unit);
    }

    if (!provided)
        return mcp_tool_err(id, "No updatable fields provided (title, description, docs_required, "
                                 "effort_estimate, effort_actual, effort_unit)");
    if (!updated)
        return mcp_tool_err(id, "Ticket not found");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "updated_fields", updated);
    return mcp_tool_ok(id, r);
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
        return mcp_tool_err(id, "Failed to delete ticket");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    cJSON_AddNumberToObject(r, "deleted_ticket_id", ticket_id);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Relations
 * ============================================================================ */

static cJSON *tool_link_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    int from_id, to_id;
    const char *rel = param_str(params, "relation_type");
    if (!param_num(params, "from_ticket_id", &from_id) ||
        !param_num(params, "to_ticket_id", &to_id) || !rel)
        return mcp_tool_err(id,
            "Missing required parameters: from_ticket_id, to_ticket_id, relation_type");

    if (strcmp(rel, "parent_of")    != 0 &&
        strcmp(rel, "blocks")       != 0 &&
        strcmp(rel, "duplicate_of") != 0 &&
        strcmp(rel, "relates_to")   != 0)
        return mcp_tool_err(id,
            "Invalid relation_type: must be parent_of, blocks, duplicate_of, or relates_to");

    if (!ticket_link(ctx->db, from_id, to_id, rel))
        return kanban_db_error(ctx, id,
            "Failed to create relation (may already exist or ticket not found)");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}

static cJSON *tool_unlink_tickets(McpContext *ctx, cJSON *id, cJSON *params) {
    int from_id, to_id;
    const char *rel = param_str(params, "relation_type");
    if (!param_num(params, "from_ticket_id", &from_id) ||
        !param_num(params, "to_ticket_id", &to_id) || !rel)
        return mcp_tool_err(id,
            "Missing required parameters: from_ticket_id, to_ticket_id, relation_type");

    if (!ticket_unlink(ctx->db, from_id, to_id, rel))
        return mcp_tool_err(id, "Relation not found or could not be deleted");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
    return mcp_tool_ok(id, r);
}


/* ============================================================================
 * Tasks
 * ============================================================================ */

static cJSON *tool_add_task(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    const char *title = param_str(params, "title");
    if (!param_num(params, "ticket_id", &ticket_id) || !title)
        return mcp_tool_err(id, "Missing required parameters: ticket_id, title");

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
        return mcp_tool_err(id, "Task not found (or update failed)");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "success", 1);
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

static cJSON *tool_list_status_transitions(McpContext *ctx, cJSON *id, cJSON *params) {
    int project_id;
    if (!param_num(params, "project_id", &project_id))
        return mcp_tool_err(id, "Missing required parameter: project_id");

    StatusTransition **trans = status_transition_list_by_project(ctx->db, project_id);

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
        if (res) PQclear(res);
        return mcp_tool_err(id, "Task not found");
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
        if (res) PQclear(res);
        return mcp_tool_err(id, "Task not found");
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
    mcp_registry_add(r, "kabai_list_projects",
        "List all projects", s, tool_list_projects);

    s = schema_new();
    schema_num(s, "project_id", "Numeric project ID", true);
    mcp_registry_add(r, "kabai_get_project",
        "Get project details", s, tool_get_project);

    /* ---- Tickets ---- */

    s = schema_new();
    schema_num(s, "project_id",  "ID of the project this ticket belongs to", true);
    schema_num(s, "status_id",   "ID of the initial board column/status", true);
    schema_str(s, "title",       "Ticket title", true);
    schema_str(s, "description", "Optional detailed description", false);
    schema_str(s, "type",        "'ticket' (default) or 'epic'", false);
    schema_bool(s, "docs_required",
        "If true, the ticket cannot move to done without a linked knowledge-base note "
        "(kabai_docs_link_ticket). Set it on architecturally relevant work.", false);
    mcp_registry_add(r, "kabai_create_ticket",
        "Create a new ticket or epic in a project. "
        "Use type='epic' for high-level goals that group child tickets via link_tickets(parent_of).",
        s, tool_create_ticket);

    s = schema_new();
    schema_num(s, "project_id", "ID of the project to list tickets from", true);
    schema_num(s, "status_id",  "Filter by status column (optional)", false);
    schema_str(s, "type",       "Filter by type: 'ticket' or 'epic' (optional)", false);
    schema_num(s, "limit",      "Max tickets to return (optional, default unlimited)", false);
    schema_num(s, "offset",     "Tickets to skip for pagination (optional, default 0)", false);
    schema_bool(s, "summary",
        "If true, omit description fields — returns only id/status_id/title/assignee/timestamps. "
        "Use for an overview when descriptions are not needed.", false);
    mcp_registry_add(r, "kabai_list_tickets",
        "List tickets in a project. Supports status filter, pagination (limit/offset), "
        "and summary mode (omits description). Use summary:true + status_id for cheap overview calls.",
        s, tool_list_tickets);

    s = schema_new();
    schema_num(s, "project_id", "ID of the project to search in", true);
    schema_str(s, "query",      "Search string matched case-insensitively against title and description", true);
    mcp_registry_add(r, "kabai_search_tickets",
        "Search tickets by title/description substring (ILIKE). Returns up to 50 matches. "
        "Use before create_ticket to detect duplicates.",
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
        "Target column/status ID. Must be an allowed transition per workflow graph.", true);
    mcp_registry_add(r, "kabai_move_ticket",
        "Move a ticket to a new column. Rejected by the database if the transition "
        "is not in the workflow graph or if acceptance criteria are unmet.",
        s, tool_move_ticket);

    s = schema_new();
    schema_num_array(s, "ticket_ids", "Array of ticket IDs to move", true);
    schema_num(s, "new_status_id", "Target status ID for all tickets", true);
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
    mcp_registry_add(r, "kabai_update_ticket",
        "Edit a ticket's title, description, docs_required flag, and/or effort tracking "
        "(effort_estimate/effort_actual/effort_unit — generic, unit-agnostic; MCP has no "
        "protocol-level usage/cost data to fill this automatically)",
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
        "One of: parent_of (epic→child), blocks (from blocks to), "
        "duplicate_of (from is duplicate of to), relates_to (generic)", true);
    mcp_registry_add(r, "kabai_link_tickets",
        "Create a directed relation between two tickets. "
        "Use parent_of to link an epic to its child tickets. "
        "Relations are visible in get_ticket_detailed as the 'relations' array.",
        s, tool_link_tickets);

    s = schema_new();
    schema_num(s, "from_ticket_id", "Source ticket ID", true);
    schema_num(s, "to_ticket_id",   "Target ticket ID", true);
    schema_str(s, "relation_type",
        "The relation to remove (must match exactly what was created)", true);
    mcp_registry_add(r, "kabai_unlink_tickets",
        "Remove a directed relation between two tickets", s, tool_unlink_tickets);

    /* ---- Tasks ---- */

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID", true);
    schema_str(s, "title",     "Task / acceptance criterion description", true);
    mcp_registry_add(r, "kabai_add_task",
        "Add an acceptance criterion task to a ticket. "
        "All tasks must be completed before the ticket can be moved to 'done'.",
        s, tool_add_task);

    s = schema_new();
    schema_num(s, "task_id", "Numeric task ID (from get_ticket_detailed)", true);
    mcp_registry_add(r, "kabai_complete_task",
        "Mark an acceptance criterion task as completed", s, tool_complete_task);

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
        "List all allowed workflow transitions for a project",
        s, tool_list_status_transitions);

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
