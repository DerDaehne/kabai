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
    char *title;
    char *description;
    char *assignee;
    char *model;
    /* Populated by ticket_get_by_id (via JOIN); NULL in list results */
    char *status_name;
    char *agent_role_instruction;
} Ticket;

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
 * @brief Extended Ticket structure with tasks and comments
 */
typedef struct {
    Ticket *ticket;
    TicketTask **tasks;
    TicketComment **comments;
} TicketDetailed;

/**
 * @brief Create a new ticket
 * @param db Database connection
 * @param project_id Project ID
 * @param status_id Initial status ID
 * @param title Ticket title
 * @param description Ticket description
 * @return Ticket* or NULL on failure
 */
Ticket* ticket_create(
    DatabaseConnection *db,
    int project_id,
    int status_id,
    const char *title,
    const char *description
);

/**
 * @brief Get ticket by ID
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @return Ticket* or NULL if not found
 */
Ticket* ticket_get_by_id(DatabaseConnection *db, int ticket_id);

/**
 * @brief List tickets by project
 * @param db Database connection
 * @param project_id Project ID
 * @return Array of Ticket* (NULL-terminated)
 */
Ticket** ticket_list_by_project(DatabaseConnection *db, int project_id);

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

#endif // KANBAN_TICKETS_H
