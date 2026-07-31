#ifndef KANBAN_PROJECTS_H
#define KANBAN_PROJECTS_H

#include <stdbool.h>
#include "db/connection.h"

/**
 * @brief Project structure
 */
typedef struct {
    int id;
    char *slug;
    char *name;
    char *description;
    /* Archiving (kbai-ui Codeberg#7, Kanban AI #502) — set only by a human
     * via kbai-ui's own UPDATE, never by an MCP tool. */
    bool archived;
} Project;

/**
 * @brief Create a new project
 * @param db Database connection
 * @param slug Project slug
 * @param name Project name
 * @param description Project description
 * @return Project* or NULL on failure
 */
Project* project_create(
    DatabaseConnection *db,
    const char *slug,
    const char *name,
    const char *description
);

/**
 * @brief Get project by ID
 * @param db Database connection
 * @param project_id Project ID
 * @return Project* or NULL if not found
 */
Project* project_get_by_id(DatabaseConnection *db, int project_id);

/**
 * @brief List projects
 * @param db Database connection
 * @param include_archived If false (default tool behavior), archived projects are excluded
 * @return Array of Project* (NULL-terminated)
 */
Project** project_list_all(DatabaseConnection *db, bool include_archived);

/**
 * @brief Free project memory
 * @param project Project to free
 */
void project_free(Project *project);

/**
 * @brief Free project array
 * @param projects NULL-terminated array of Project*
 */
void project_free_array(Project **projects);

#endif // KANBAN_PROJECTS_H
