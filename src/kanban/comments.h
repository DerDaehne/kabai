#ifndef KANBAN_COMMENTS_H
#define KANBAN_COMMENTS_H

#include "db/connection.h"

/**
 * @brief Ticket Comment structure (Work Log entry)
 */
typedef struct {
    int id;
    int ticket_id;
    char *author;
    char *comment_text;
    char *created_at;
} TicketComment;

/**
 * @brief Add a comment/work log entry to a ticket
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @param author Author identifier (e.g., "AI-Architect", "johndoe")
 * @param comment_text The comment/work log text
 * @return TicketComment* or NULL on failure
 */
TicketComment* comment_add(
    DatabaseConnection *db,
    int ticket_id,
    const char *author,
    const char *comment_text
);

/**
 * @brief Get all comments for a ticket (work log)
 * @param db Database connection
 * @param ticket_id Ticket ID
 * @return Array of TicketComment* (NULL-terminated)
 */
TicketComment** comment_list_by_ticket(DatabaseConnection *db, int ticket_id);

/**
 * @brief Get a specific comment by ID
 * @param db Database connection
 * @param comment_id Comment ID
 * @return TicketComment* or NULL if not found
 */
TicketComment* comment_get_by_id(DatabaseConnection *db, int comment_id);

/**
 * @brief Update a comment
 * @param db Database connection
 * @param comment_id Comment ID
 * @param new_text New comment text
 * @return 1 on success, 0 on failure
 */
int comment_update(
    DatabaseConnection *db,
    int comment_id,
    const char *new_text
);

/**
 * @brief Delete a comment
 * @param db Database connection
 * @param comment_id Comment ID
 * @return 1 on success, 0 on failure
 */
int comment_delete(DatabaseConnection *db, int comment_id);

/**
 * @brief Free comment memory
 * @param comment Comment to free
 */
void comment_free(TicketComment *comment);

/**
 * @brief Free comment array
 * @param comments NULL-terminated array of TicketComment*
 */
void comment_free_array(TicketComment **comments);

#endif // KANBAN_COMMENTS_H
