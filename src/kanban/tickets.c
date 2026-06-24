#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/kanban/tickets.h"
#include "../include/kanban/comments.h"
#include "../include/db/connection.h"

// ============================================================================
// TicketDetailed functions
// ============================================================================

TicketDetailed* ticket_get_detailed(DatabaseConnection *db, int ticket_id) {
    Ticket *ticket = ticket_get_by_id(db, ticket_id);
    if (!ticket) {
        return NULL;
    }
    
    TicketDetailed *detailed = malloc(sizeof(TicketDetailed));
    if (!detailed) {
        ticket_free(ticket);
        return NULL;
    }
    
    detailed->ticket = ticket;
    detailed->tasks = ticket_get_tasks(db, ticket_id);
    detailed->comments = comment_list_by_ticket(db, ticket_id);
    
    return detailed;
}

void ticket_detailed_free(TicketDetailed *detailed) {
    if (!detailed) {
        return;
    }
    if (detailed->ticket) {
        ticket_free(detailed->ticket);
    }
    if (detailed->tasks) {
        ticket_task_free_array(detailed->tasks);
    }
    if (detailed->comments) {
        comment_free_array(detailed->comments);
    }
    free(detailed);
}

Ticket* ticket_create(
    DatabaseConnection *db,
    int project_id,
    int status_id,
    const char *title,
    const char *description
) {
    if (!db || !title) {
        return NULL;
    }
    
    char *esc_title = malloc(2 * strlen(title) + 1);
    char *esc_desc = NULL;
    
    if (!esc_title) {
        return NULL;
    }
    
    PQescapeStringConn(db->conn, esc_title, title, strlen(title), NULL);
    
    if (description) {
        esc_desc = malloc(2 * strlen(description) + 1);
        if (!esc_desc) {
            free(esc_title);
            return NULL;
        }
        PQescapeStringConn(db->conn, esc_desc, description, strlen(description), NULL);
    }
    
    char query[2048];
    if (esc_desc) {
        snprintf(query, sizeof(query),
            "INSERT INTO tickets (project_id, status_id, title, description) VALUES (%d, %d, '%s', '%s') RETURNING id",
            project_id, status_id, esc_title, esc_desc);
    } else {
        snprintf(query, sizeof(query),
            "INSERT INTO tickets (project_id, status_id, title) VALUES (%d, %d, '%s') RETURNING id",
            project_id, status_id, esc_title);
    }
    
    PGresult *res = db_query(db, query);
    free(esc_title);
    free(esc_desc);
    
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return NULL;
    }
    
    int id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    
    Ticket *ticket = malloc(sizeof(Ticket));
    if (!ticket) {
        return NULL;
    }
    
    ticket->id = id;
    ticket->project_id = project_id;
    ticket->status_id = status_id;
    ticket->title = strdup(title);
    ticket->description = description ? strdup(description) : NULL;
    ticket->assignee = NULL;
    
    return ticket;
}

Ticket* ticket_get_by_id(DatabaseConnection *db, int ticket_id) {
    if (!db) {
        return NULL;
    }
    
    char query[256];
    snprintf(query, sizeof(query),
        "SELECT id, project_id, status_id, title, description, assignee FROM tickets WHERE id = %d",
        ticket_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    
    Ticket *ticket = malloc(sizeof(Ticket));
    if (!ticket) {
        PQclear(res);
        return NULL;
    }
    
    ticket->id = atoi(PQgetvalue(res, 0, 0));
    ticket->project_id = atoi(PQgetvalue(res, 0, 1));
    ticket->status_id = atoi(PQgetvalue(res, 0, 2));
    ticket->title = strdup(PQgetvalue(res, 0, 3));
    char *desc = PQgetvalue(res, 0, 4);
    ticket->description = desc && strlen(desc) > 0 ? strdup(desc) : NULL;
    char *assignee = PQgetvalue(res, 0, 5);
    ticket->assignee = assignee && strlen(assignee) > 0 ? strdup(assignee) : NULL;
    
    PQclear(res);
    return ticket;
}

Ticket** ticket_list_by_project(DatabaseConnection *db, int project_id) {
    if (!db) {
        return NULL;
    }
    
    char query[256];
    snprintf(query, sizeof(query),
        "SELECT id, project_id, status_id, title, description, assignee FROM tickets WHERE project_id = %d ORDER BY created_at",
        project_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    
    int count = PQntuples(res);
    Ticket **tickets = calloc(count + 1, sizeof(Ticket*));
    if (!tickets) {
        PQclear(res);
        return NULL;
    }
    
    for (int i = 0; i < count; i++) {
        tickets[i] = malloc(sizeof(Ticket));
        if (!tickets[i]) {
            ticket_free_array(tickets);
            PQclear(res);
            return NULL;
        }
        
        tickets[i]->id = atoi(PQgetvalue(res, i, 0));
        tickets[i]->project_id = atoi(PQgetvalue(res, i, 1));
        tickets[i]->status_id = atoi(PQgetvalue(res, i, 2));
        tickets[i]->title = strdup(PQgetvalue(res, i, 3));
        char *desc = PQgetvalue(res, i, 4);
        tickets[i]->description = desc && strlen(desc) > 0 ? strdup(desc) : NULL;
        char *assignee = PQgetvalue(res, i, 5);
        tickets[i]->assignee = assignee && strlen(assignee) > 0 ? strdup(assignee) : NULL;
    }
    
    tickets[count] = NULL;
    PQclear(res);
    return tickets;
}

int ticket_update_status(DatabaseConnection *db, int ticket_id, int new_status_id) {
    if (!db) {
        return 0;
    }
    
    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE tickets SET status_id = %d, updated_at = NOW() WHERE id = %d",
        new_status_id, ticket_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

int ticket_assign(DatabaseConnection *db, int ticket_id, const char *assignee) {
    if (!db || !assignee) {
        return 0;
    }
    
    char *esc_assignee = malloc(2 * strlen(assignee) + 1);
    if (!esc_assignee) {
        return 0;
    }
    
    PQescapeStringConn(db->conn, esc_assignee, assignee, strlen(assignee), NULL);
    
    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE tickets SET assignee = '%s', updated_at = NOW() WHERE id = %d",
        esc_assignee, ticket_id);
    free(esc_assignee);
    
    PGresult *res = db_query(db, query);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

TicketTask* ticket_add_task(DatabaseConnection *db, int ticket_id, const char *title) {
    if (!db || !title) {
        return NULL;
    }
    
    char *esc_title = malloc(2 * strlen(title) + 1);
    if (!esc_title) {
        return NULL;
    }
    
    PQescapeStringConn(db->conn, esc_title, title, strlen(title), NULL);
    
    char query[2048];
    snprintf(query, sizeof(query),
        "INSERT INTO ticket_tasks (ticket_id, title) VALUES (%d, '%s') RETURNING id",
        ticket_id, esc_title);
    free(esc_title);
    
    PGresult *res = db_query(db, query);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return NULL;
    }
    
    int id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    
    TicketTask *task = malloc(sizeof(TicketTask));
    if (!task) {
        return NULL;
    }
    
    task->id = id;
    task->ticket_id = ticket_id;
    task->title = strdup(title);
    task->is_completed = 0;
    
    return task;
}

int ticket_complete_task(DatabaseConnection *db, int task_id) {
    if (!db) {
        return 0;
    }
    
    char query[256];
    snprintf(query, sizeof(query),
        "UPDATE ticket_tasks SET is_completed = TRUE WHERE id = %d",
        task_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

// ============================================================================
// Ticket Update Functions
// ============================================================================

int ticket_update_title(DatabaseConnection *db, int ticket_id, const char *new_title) {
    if (!db || !new_title) {
        return 0;
    }
    
    char *esc_title = malloc(2 * strlen(new_title) + 1);
    if (!esc_title) {
        return 0;
    }
    
    PQescapeStringConn(db->conn, esc_title, new_title, strlen(new_title), NULL);
    
    char query[2048];
    snprintf(query, sizeof(query),
        "UPDATE tickets SET title = '%s', updated_at = NOW() WHERE id = %d",
        esc_title, ticket_id);
    free(esc_title);
    
    PGresult *res = db_query(db, query);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

int ticket_update_description(DatabaseConnection *db, int ticket_id, const char *new_description) {
    if (!db) {
        return 0;
    }
    
    char *esc_desc = NULL;
    if (new_description) {
        esc_desc = malloc(2 * strlen(new_description) + 1);
        if (!esc_desc) {
            return 0;
        }
        PQescapeStringConn(db->conn, esc_desc, new_description, strlen(new_description), NULL);
    }
    
    char query[2048];
    if (new_description) {
        snprintf(query, sizeof(query),
            "UPDATE tickets SET description = '%s', updated_at = NOW() WHERE id = %d",
            esc_desc, ticket_id);
    } else {
        snprintf(query, sizeof(query),
            "UPDATE tickets SET description = NULL, updated_at = NOW() WHERE id = %d",
            ticket_id);
    }
    
    if (esc_desc) free(esc_desc);
    
    PGresult *res = db_query(db, query);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

TicketTask** ticket_get_tasks(DatabaseConnection *db, int ticket_id) {
    if (!db) {
        return NULL;
    }
    
    char query[256];
    snprintf(query, sizeof(query),
        "SELECT id, ticket_id, title, is_completed FROM ticket_tasks WHERE ticket_id = %d ORDER BY created_at",
        ticket_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    
    int count = PQntuples(res);
    TicketTask **tasks = calloc(count + 1, sizeof(TicketTask*));
    if (!tasks) {
        PQclear(res);
        return NULL;
    }
    
    for (int i = 0; i < count; i++) {
        tasks[i] = malloc(sizeof(TicketTask));
        if (!tasks[i]) {
            ticket_task_free_array(tasks);
            PQclear(res);
            return NULL;
        }
        
        tasks[i]->id = atoi(PQgetvalue(res, i, 0));
        tasks[i]->ticket_id = atoi(PQgetvalue(res, i, 1));
        tasks[i]->title = strdup(PQgetvalue(res, i, 2));
        tasks[i]->is_completed = strcmp(PQgetvalue(res, i, 3), "t") == 0;
    }
    
    tasks[count] = NULL;
    PQclear(res);
    return tasks;
}

void ticket_free(Ticket *ticket) {
    if (!ticket) {
        return;
    }
    free(ticket->title);
    free(ticket->description);
    free(ticket->assignee);
    free(ticket);
}

void ticket_free_array(Ticket **tickets) {
    if (!tickets) {
        return;
    }
    for (int i = 0; tickets[i] != NULL; i++) {
        ticket_free(tickets[i]);
    }
    free(tickets);
}

void ticket_task_free(TicketTask *task) {
    if (!task) {
        return;
    }
    free(task->title);
    free(task);
}

void ticket_task_free_array(TicketTask **tasks) {
    if (!tasks) {
        return;
    }
    for (int i = 0; tasks[i] != NULL; i++) {
        ticket_task_free(tasks[i]);
    }
    free(tasks);
}
