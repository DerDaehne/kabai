#ifndef KANBAN_TICKETS_H
#define KANBAN_TICKETS_H

#include "db/connection.h"
#include "comments.h"

/**
 * @brief Ticket structure
 */
typedef struct {
    int   id;
    int   project_id;
    int   status_id;
    char *type;           /* "ticket" or "epic" */
    char *title;
    char *description;
    char *assignee;
    char *model;
    /* Generic effort tracking (Codeberg kbai-ui#16) — numeric-as-text like
     * other Postgres numeric columns read via libpq's text format; unit is
     * free text (days, story points, tokens, ...), not an enum. */
    char *effort_estimate;
    char *effort_actual;
    char *effort_unit;
    char *created_at;
    char *updated_at;
    /* Populated by ticket_get_by_id (via JOIN); NULL in list results */
    char *status_name;
    char *agent_role_instruction;
} Ticket;

/**
 * @brief Directed relation between two tickets
 */
typedef struct {
    int   id;
    int   from_ticket_id;
    char *from_ticket_title;
    int   to_ticket_id;
    char *to_ticket_title;
    char *relation_type;  /* "parent_of", "blocks", "duplicate_of", "relates_to" */
    char *created_at;
} TicketRelation;

/**
 * @brief Ticket Task structure (Acceptance Criteria)
 */
typedef struct {
    int id;
    int ticket_id;
    char *title;
    int is_completed;
} TicketTask;

/**
 * @brief Extended Ticket structure with tasks, relations, and comments
 */
typedef struct {
    Ticket          *ticket;
    TicketTask      **tasks;
    TicketRelation  **relations;
    TicketComment   **comments;
} TicketDetailed;

/**
 * @brief Create a new ticket
 * @param db Database connection
 * @param project_id Project ID
 * @param status_id Initial status ID
 * @param title Ticket title
 * @param description Ticket description
 * @param type "ticket" or "epic" (NULL defaults to "ticket")
 * @return Ticket* or NULL on failure
 */
Ticket* ticket_create(
    DatabaseConnection *db,
    int project_id,
    int status_id,
    const char *title,
    const char *description,
    const char *type
);

/**
 * @brief Get ticket by ID
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @return Ticket* or NULL if not found
 */
Ticket* ticket_get_by_id(DatabaseConnection *db, int ticket_id);

/**
 * @brief List tickets with optional status filter and pagination
 * @param db Database connection
 * @param project_id Project ID
 * @param status_id Filter by status (0 = no filter)
 * @param limit Max results (0 = no limit)
 * @param offset Results to skip (only used when limit > 0)
 * @return Array of Ticket* (NULL-terminated)
 */
Ticket** ticket_list_filtered(DatabaseConnection *db, int project_id, int status_id, const char *type_filter, int limit, int offset);

/**
 * @brief Search tickets by title/description substring (case-insensitive)
 * @param db Database connection
 * @param project_id Project ID
 * @param query Search string (matched with ILIKE)
 * @return Array of Ticket* (NULL-terminated), max 50 results
 */
Ticket** ticket_search(DatabaseConnection *db, int project_id, const char *query);

/**
 * @brief Delete a ticket (cascades tasks, comments, documents)
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @return 1 on success, 0 on failure
 */
int ticket_delete(DatabaseConnection *db, int ticket_id);

/**
 * @brief Update ticket status
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @param new_status_id New status ID
 * @return 1 on success, 0 on failure
 */
int ticket_update_status(DatabaseConnection *db, int ticket_id, int new_status_id);

/**
 * @brief Assign ticket to agent
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @param assignee Agent identifier
 * @return 1 on success, 0 on failure
 */
int ticket_assign(DatabaseConnection *db, int ticket_id, const char *assignee, const char *model);

/**
 * @brief Update ticket title
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @param new_title New title
 * @return 1 on success, 0 on failure
 */
int ticket_update_title(DatabaseConnection *db, int ticket_id, const char *new_title);

/**
 * @brief Update ticket description
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @param new_description New description
 * @return 1 on success, 0 on failure
 */
int ticket_update_description(DatabaseConnection *db, int ticket_id, const char *new_description);

/**
 * @brief Update effort estimate/actual/unit (NULL clears the field)
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @return 1 on success, 0 on failure
 */
int ticket_update_effort_estimate(DatabaseConnection *db, int ticket_id, const char *estimate);
int ticket_update_effort_actual(DatabaseConnection *db, int ticket_id, const char *actual);
int ticket_update_effort_unit(DatabaseConnection *db, int ticket_id, const char *unit);

/**
 * @brief Get detailed ticket with tasks and comments (work log)
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @return TicketDetailed* or NULL if not found
 */
TicketDetailed* ticket_get_detailed(DatabaseConnection *db, int ticket_id);

/**
 * @brief Free detailed ticket
 * @param detailed TicketDetailed to free
 */
void ticket_detailed_free(TicketDetailed *detailed);

/**
 * @brief Add task to ticket
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @param title Task title
 * @return TicketTask* or NULL on failure
 */
TicketTask* ticket_add_task(DatabaseConnection *db, int ticket_id, const char *title);

/**
 * @brief Complete a ticket task
 * @param db Database connection
 * @param task_id Task ID
 * @return 1 on success, 0 on failure
 */
int ticket_complete_task(DatabaseConnection *db, int task_id);

/**
 * @brief Get tasks for a ticket
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @return Array of TicketTask* (NULL-terminated)
 */
TicketTask** ticket_get_tasks(DatabaseConnection *db, int ticket_id);

/**
 * @brief Free ticket memory
 * @param ticket Ticket to free
 */
void ticket_free(Ticket *ticket);

/**
 * @brief Free ticket array
 * @param tickets NULL-terminated array of Ticket*
 */
void ticket_free_array(Ticket **tickets);

/**
 * @brief Free ticket task
 * @param task Task to free
 */
void ticket_task_free(TicketTask *task);

/**
 * @brief Free ticket task array
 * @param tasks NULL-terminated array of TicketTask*
 */
void ticket_task_free_array(TicketTask **tasks);

/* ---- Relations ---- */

/**
 * @brief Create a directed relation between two tickets
 * @param relation_type One of: parent_of, blocks, duplicate_of, relates_to
 * @return 1 on success, 0 on failure (including duplicate)
 */
int ticket_link(DatabaseConnection *db, int from_id, int to_id, const char *relation_type);

/**
 * @brief Remove a directed relation
 * @return 1 on success, 0 on failure
 */
int ticket_unlink(DatabaseConnection *db, int from_id, int to_id, const char *relation_type);

/**
 * @brief Get all relations involving this ticket (as from OR to)
 * @return NULL-terminated array, or NULL if none
 */
TicketRelation **ticket_get_relations(DatabaseConnection *db, int ticket_id);

void ticket_relation_free(TicketRelation *r);
void ticket_relation_free_array(TicketRelation **arr);

#endif // KANBAN_TICKETS_H
