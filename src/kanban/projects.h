#ifndef KANBAN_PROJECTS_H
#define KANBAN_PROJECTS_H

#include "db/connection.h"

/**
 * @brief Project structure
 */
typedef struct {
    int id;
    char *slug;
    char *name;
    char *description;
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
 * @brief List all projects
 * @param db Database connection
 * @return Array of Project* (NULL-terminated)
 */
Project** project_list_all(DatabaseConnection *db);

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
