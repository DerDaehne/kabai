#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kanban/tickets.h"
#include "kanban/comments.h"
#include "db/connection.h"

/* ============================================================================
 * TicketDetailed
 * ============================================================================ */

TicketDetailed *ticket_get_detailed(DatabaseConnection *db, int ticket_id) {
    Ticket *ticket = ticket_get_by_id(db, ticket_id);
    if (!ticket) return NULL;

    TicketDetailed *d = malloc(sizeof(TicketDetailed));
    if (!d) { ticket_free(ticket); return NULL; }

    d->ticket   = ticket;
    d->tasks    = ticket_get_tasks(db, ticket_id);
    d->comments = comment_list_by_ticket(db, ticket_id);
    return d;
}

void ticket_detailed_free(TicketDetailed *d) {
    if (!d) return;
    ticket_free(d->ticket);
    if (d->tasks)    ticket_task_free_array(d->tasks);
    if (d->comments) comment_free_array(d->comments);
    free(d);
}

/* ============================================================================
 * Ticket CRUD
 * ============================================================================ */

Ticket *ticket_create(
    DatabaseConnection *db,
    int project_id,
    int status_id,
    const char *title,
    const char *description
) {
    if (!db || !title) return NULL;

    /*
     * No pre-flight project_exists/status_exists checks: the database FK
     * constraint and the workflow trigger will reject invalid combinations
     * atomically and without a TOCTOU race.
     */

    char proj_str[32], stat_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    snprintf(stat_str, sizeof(stat_str), "%d", status_id);

    PGresult *res;

    if (description) {
        const char *params[4] = {proj_str, stat_str, title, description};
        res = PQexecParams(db->conn,
            "INSERT INTO tickets (project_id, status_id, title, description)"
            " VALUES ($1, $2, $3, $4) RETURNING id",
            4, NULL, params, NULL, NULL, 0);
    } else {
        const char *params[3] = {proj_str, stat_str, title};
        res = PQexecParams(db->conn,
            "INSERT INTO tickets (project_id, status_id, title)"
            " VALUES ($1, $2, $3) RETURNING id",
            3, NULL, params, NULL, NULL, 0);
    }

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "ticket_create: %s\n", res ? PQresultErrorMessage(res) : "null result");
        if (res) PQclear(res);
        return NULL;
    }

    int id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    Ticket *t = malloc(sizeof(Ticket));
    if (!t) return NULL;

    t->id                     = id;
    t->project_id             = project_id;
    t->status_id              = status_id;
    t->title                  = strdup(title);
    t->description            = description ? strdup(description) : NULL;
    t->assignee               = NULL;
    t->model                  = NULL;
    t->created_at             = NULL;
    t->updated_at             = NULL;
    t->status_name            = NULL;
    t->agent_role_instruction = NULL;
    return t;
}

Ticket *ticket_get_by_id(DatabaseConnection *db, int ticket_id) {
    if (!db) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);
    const char *params[1] = {id_str};

    /* JOIN board_statuses to fetch status_name and agent_role_instruction */
    PGresult *res = PQexecParams(db->conn,
        "SELECT t.id, t.project_id, t.status_id, t.title, t.description, t.assignee, t.model,"
        "       t.created_at::text, t.updated_at::text, bs.name, bs.agent_role_instruction"
        " FROM tickets t"
        " JOIN board_statuses bs ON bs.id = t.status_id"
        " WHERE t.id = $1",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }

    Ticket *t = malloc(sizeof(Ticket));
    if (!t) { PQclear(res); return NULL; }

    t->id         = atoi(PQgetvalue(res, 0, 0));
    t->project_id = atoi(PQgetvalue(res, 0, 1));
    t->status_id  = atoi(PQgetvalue(res, 0, 2));
    t->title      = strdup(PQgetvalue(res, 0, 3));

    const char *desc = PQgetvalue(res, 0, 4);
    t->description = (desc && *desc) ? strdup(desc) : NULL;

    const char *assignee = PQgetvalue(res, 0, 5);
    t->assignee = (assignee && *assignee) ? strdup(assignee) : NULL;

    const char *model = PQgetvalue(res, 0, 6);
    t->model = (model && *model) ? strdup(model) : NULL;

    const char *cat = PQgetvalue(res, 0, 7);
    t->created_at = (cat && *cat) ? strdup(cat) : NULL;

    const char *uat = PQgetvalue(res, 0, 8);
    t->updated_at = (uat && *uat) ? strdup(uat) : NULL;

    t->status_name = strdup(PQgetvalue(res, 0, 9));

    const char *ari = PQgetvalue(res, 0, 10);
    t->agent_role_instruction = (ari && *ari) ? strdup(ari) : NULL;

    PQclear(res);
    return t;
}

static Ticket **parse_ticket_rows(PGresult *res) {
    int count = PQntuples(res);
    Ticket **tickets = calloc(count + 1, sizeof(Ticket *));
    if (!tickets) { PQclear(res); return NULL; }

    for (int i = 0; i < count; i++) {
        tickets[i] = malloc(sizeof(Ticket));
        if (!tickets[i]) {
            ticket_free_array(tickets);
            PQclear(res);
            return NULL;
        }
        tickets[i]->id         = atoi(PQgetvalue(res, i, 0));
        tickets[i]->project_id = atoi(PQgetvalue(res, i, 1));
        tickets[i]->status_id  = atoi(PQgetvalue(res, i, 2));
        tickets[i]->title      = strdup(PQgetvalue(res, i, 3));

        const char *desc = PQgetvalue(res, i, 4);
        tickets[i]->description = (desc && *desc) ? strdup(desc) : NULL;

        const char *assignee = PQgetvalue(res, i, 5);
        tickets[i]->assignee = (assignee && *assignee) ? strdup(assignee) : NULL;

        const char *cat = PQgetvalue(res, i, 6);
        tickets[i]->created_at = (cat && *cat) ? strdup(cat) : NULL;

        const char *uat = PQgetvalue(res, i, 7);
        tickets[i]->updated_at = (uat && *uat) ? strdup(uat) : NULL;

        tickets[i]->model                  = NULL;
        tickets[i]->status_name            = NULL;
        tickets[i]->agent_role_instruction = NULL;
    }

    tickets[count] = NULL;
    PQclear(res);
    return tickets;
}

Ticket **ticket_list_by_project(DatabaseConnection *db, int project_id) {
    return ticket_list_filtered(db, project_id, 0, 0, 0);
}

Ticket **ticket_list_filtered(DatabaseConnection *db, int project_id, int status_id, int limit, int offset) {
    if (!db) return NULL;

    char proj_str[32], sid_str[32], lim_str[32], off_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    snprintf(sid_str,  sizeof(sid_str),  "%d", status_id);
    snprintf(lim_str,  sizeof(lim_str),  "%d", limit);
    snprintf(off_str,  sizeof(off_str),  "%d", offset);

    PGresult *res;
    if (status_id > 0 && limit > 0) {
        const char *p[4] = {proj_str, sid_str, lim_str, off_str};
        res = PQexecParams(db->conn,
            "SELECT id, project_id, status_id, title, description, assignee,"
            "       created_at::text, updated_at::text"
            " FROM tickets WHERE project_id = $1 AND status_id = $2"
            " ORDER BY created_at LIMIT $3 OFFSET $4",
            4, NULL, p, NULL, NULL, 0);
    } else if (status_id > 0) {
        const char *p[2] = {proj_str, sid_str};
        res = PQexecParams(db->conn,
            "SELECT id, project_id, status_id, title, description, assignee,"
            "       created_at::text, updated_at::text"
            " FROM tickets WHERE project_id = $1 AND status_id = $2"
            " ORDER BY created_at",
            2, NULL, p, NULL, NULL, 0);
    } else if (limit > 0) {
        const char *p[3] = {proj_str, lim_str, off_str};
        res = PQexecParams(db->conn,
            "SELECT id, project_id, status_id, title, description, assignee,"
            "       created_at::text, updated_at::text"
            " FROM tickets WHERE project_id = $1"
            " ORDER BY created_at LIMIT $2 OFFSET $3",
            3, NULL, p, NULL, NULL, 0);
    } else {
        const char *p[1] = {proj_str};
        res = PQexecParams(db->conn,
            "SELECT id, project_id, status_id, title, description, assignee,"
            "       created_at::text, updated_at::text"
            " FROM tickets WHERE project_id = $1 ORDER BY created_at",
            1, NULL, p, NULL, NULL, 0);
    }

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    return parse_ticket_rows(res);
}

Ticket **ticket_search(DatabaseConnection *db, int project_id, const char *query) {
    if (!db || !query) return NULL;

    char proj_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);

    /* Wrap query in % for ILIKE pattern matching */
    size_t qlen = strlen(query);
    char *pattern = malloc(qlen + 3);
    if (!pattern) return NULL;
    pattern[0] = '%';
    memcpy(pattern + 1, query, qlen);
    pattern[qlen + 1] = '%';
    pattern[qlen + 2] = '\0';

    const char *p[2] = {proj_str, pattern};
    PGresult *res = PQexecParams(db->conn,
        "SELECT id, project_id, status_id, title, description, assignee,"
        "       created_at::text, updated_at::text"
        " FROM tickets WHERE project_id = $1"
        "   AND (title ILIKE $2 OR description ILIKE $2)"
        " ORDER BY created_at LIMIT 50",
        2, NULL, p, NULL, NULL, 0);
    free(pattern);

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    return parse_ticket_rows(res);
}

int ticket_delete(DatabaseConnection *db, int ticket_id) {
    if (!db) return 0;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);
    const char *p[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "DELETE FROM tickets WHERE id = $1",
        1, NULL, p, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    int affected = atoi(PQcmdTuples(res));
    PQclear(res);
    return affected > 0;
}

int ticket_update_status(DatabaseConnection *db, int ticket_id, int new_status_id) {
    if (!db) return 0;

    char tid_str[32], sid_str[32];
    snprintf(tid_str, sizeof(tid_str), "%d", ticket_id);
    snprintf(sid_str, sizeof(sid_str), "%d", new_status_id);
    const char *params[2] = {sid_str, tid_str};

    /*
     * updated_at is set by the enforce_kanban_workflow_integrity trigger;
     * no need to set it here as well.
     */
    PGresult *res = PQexecParams(db->conn,
        "UPDATE tickets SET status_id = $1 WHERE id = $2",
        2, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }

    int affected = atoi(PQcmdTuples(res));
    PQclear(res);
    return affected > 0;
}

int ticket_assign(DatabaseConnection *db, int ticket_id, const char *assignee, const char *model) {
    if (!db || !assignee) return 0;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);

    PGresult *res;
    if (model) {
        const char *params[3] = {assignee, model, id_str};
        res = PQexecParams(db->conn,
            "UPDATE tickets SET assignee = $1, model = $2, updated_at = NOW() WHERE id = $3",
            3, NULL, params, NULL, NULL, 0);
    } else {
        const char *params[2] = {assignee, id_str};
        res = PQexecParams(db->conn,
            "UPDATE tickets SET assignee = $1, updated_at = NOW() WHERE id = $2",
            2, NULL, params, NULL, NULL, 0);
    }

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }

    PQclear(res);
    return 1;
}

int ticket_update_title(DatabaseConnection *db, int ticket_id, const char *new_title) {
    if (!db || !new_title) return 0;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);
    const char *params[2] = {new_title, id_str};

    PGresult *res = PQexecParams(db->conn,
        "UPDATE tickets SET title = $1, updated_at = NOW() WHERE id = $2",
        2, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }

    PQclear(res);
    return 1;
}

int ticket_update_description(DatabaseConnection *db, int ticket_id, const char *new_description) {
    if (!db) return 0;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);

    PGresult *res;
    if (new_description) {
        const char *params[2] = {new_description, id_str};
        res = PQexecParams(db->conn,
            "UPDATE tickets SET description = $1, updated_at = NOW() WHERE id = $2",
            2, NULL, params, NULL, NULL, 0);
    } else {
        const char *params[1] = {id_str};
        res = PQexecParams(db->conn,
            "UPDATE tickets SET description = NULL, updated_at = NOW() WHERE id = $1",
            1, NULL, params, NULL, NULL, 0);
    }

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }

    PQclear(res);
    return 1;
}

/* ============================================================================
 * Tasks
 * ============================================================================ */

TicketTask *ticket_add_task(DatabaseConnection *db, int ticket_id, const char *title) {
    if (!db || !title) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);
    const char *params[2] = {id_str, title};

    PGresult *res = PQexecParams(db->conn,
        "INSERT INTO ticket_tasks (ticket_id, title) VALUES ($1, $2) RETURNING id",
        2, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "ticket_add_task: %s\n", res ? PQresultErrorMessage(res) : "null result");
        if (res) PQclear(res);
        return NULL;
    }

    int id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    TicketTask *task = malloc(sizeof(TicketTask));
    if (!task) return NULL;

    task->id          = id;
    task->ticket_id   = ticket_id;
    task->title       = strdup(title);
    task->is_completed = 0;
    return task;
}

int ticket_complete_task(DatabaseConnection *db, int task_id) {
    if (!db) return 0;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", task_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "UPDATE ticket_tasks SET is_completed = TRUE WHERE id = $1",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }

    PQclear(res);
    return 1;
}

TicketTask **ticket_get_tasks(DatabaseConnection *db, int ticket_id) {
    if (!db) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "SELECT id, ticket_id, title, is_completed"
        " FROM ticket_tasks WHERE ticket_id = $1 ORDER BY created_at",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }

    int count = PQntuples(res);
    TicketTask **tasks = calloc(count + 1, sizeof(TicketTask *));
    if (!tasks) { PQclear(res); return NULL; }

    for (int i = 0; i < count; i++) {
        tasks[i] = malloc(sizeof(TicketTask));
        if (!tasks[i]) {
            ticket_task_free_array(tasks);
            PQclear(res);
            return NULL;
        }
        tasks[i]->id          = atoi(PQgetvalue(res, i, 0));
        tasks[i]->ticket_id   = atoi(PQgetvalue(res, i, 1));
        tasks[i]->title       = strdup(PQgetvalue(res, i, 2));
        tasks[i]->is_completed = strcmp(PQgetvalue(res, i, 3), "t") == 0;
    }

    tasks[count] = NULL;
    PQclear(res);
    return tasks;
}

/* ============================================================================
 * Memory management
 * ============================================================================ */

void ticket_free(Ticket *t) {
    if (!t) return;
    free(t->title);
    free(t->description);
    free(t->assignee);
    free(t->model);
    free(t->created_at);
    free(t->updated_at);
    free(t->status_name);
    free(t->agent_role_instruction);
    free(t);
}

void ticket_free_array(Ticket **tickets) {
    if (!tickets) return;
    for (int i = 0; tickets[i]; i++)
        ticket_free(tickets[i]);
    free(tickets);
}

void ticket_task_free(TicketTask *task) {
    if (!task) return;
    free(task->title);
    free(task);
}

void ticket_task_free_array(TicketTask **tasks) {
    if (!tasks) return;
    for (int i = 0; tasks[i]; i++)
        ticket_task_free(tasks[i]);
    free(tasks);
}
