#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kanban/projects.h"
#include "db/connection.h"

Project* project_create(
    DatabaseConnection *db,
    const char *slug,
    const char *name,
    const char *description
) {
    if (!db || !slug || !name) {
        return NULL;
    }
    
    char *esc_slug = malloc(2 * strlen(slug) + 1);
    char *esc_name = malloc(2 * strlen(name) + 1);
    char *esc_desc = NULL;
    
    if (!esc_slug || !esc_name) {
        free(esc_slug);
        free(esc_name);
        return NULL;
    }
    
    PQescapeStringConn(db->conn, esc_slug, slug, strlen(slug), NULL);
    PQescapeStringConn(db->conn, esc_name, name, strlen(name), NULL);
    
    if (description) {
        esc_desc = malloc(2 * strlen(description) + 1);
        if (!esc_desc) {
            free(esc_slug);
            free(esc_name);
            return NULL;
        }
        PQescapeStringConn(db->conn, esc_desc, description, strlen(description), NULL);
    }
    
    char query[2048];
    if (esc_desc) {
        snprintf(query, sizeof(query),
            "INSERT INTO projects (slug, name, description) VALUES ('%s', '%s', '%s') RETURNING id",
            esc_slug, esc_name, esc_desc);
    } else {
        snprintf(query, sizeof(query),
            "INSERT INTO projects (slug, name) VALUES ('%s', '%s') RETURNING id",
            esc_slug, esc_name);
    }
    
    PGresult *res = db_query(db, query);
    free(esc_slug);
    free(esc_name);
    free(esc_desc);
    
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return NULL;
    }
    
    int id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    
    Project *project = malloc(sizeof(Project));
    if (!project) {
        return NULL;
    }
    
    project->id = id;
    project->slug = strdup(slug);
    project->name = strdup(name);
    project->description = description ? strdup(description) : NULL;
    
    return project;
}

Project* project_get_by_id(DatabaseConnection *db, int project_id) {
    if (!db) {
        return NULL;
    }
    
    char query[256];
    snprintf(query, sizeof(query), "SELECT id, slug, name, description FROM projects WHERE id = %d", project_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    
    Project *project = malloc(sizeof(Project));
    if (!project) {
        PQclear(res);
        return NULL;
    }
    
    project->id = atoi(PQgetvalue(res, 0, 0));
    project->slug = strdup(PQgetvalue(res, 0, 1));
    project->name = strdup(PQgetvalue(res, 0, 2));
    char *desc = PQgetvalue(res, 0, 3);
    project->description = desc && strlen(desc) > 0 ? strdup(desc) : NULL;
    
    PQclear(res);
    return project;
}

Project* project_get_by_slug(DatabaseConnection *db, const char *slug) {
    if (!db || !slug) {
        return NULL;
    }
    
    char *esc_slug = malloc(2 * strlen(slug) + 1);
    if (!esc_slug) {
        return NULL;
    }
    
    PQescapeStringConn(db->conn, esc_slug, slug, strlen(slug), NULL);
    
    char query[256];
    snprintf(query, sizeof(query), "SELECT id, slug, name, description FROM projects WHERE slug = '%s'", esc_slug);
    free(esc_slug);
    
    PGresult *res = db_query(db, query);
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    
    Project *project = malloc(sizeof(Project));
    if (!project) {
        PQclear(res);
        return NULL;
    }
    
    project->id = atoi(PQgetvalue(res, 0, 0));
    project->slug = strdup(PQgetvalue(res, 0, 1));
    project->name = strdup(PQgetvalue(res, 0, 2));
    char *desc = PQgetvalue(res, 0, 3);
    project->description = desc && strlen(desc) > 0 ? strdup(desc) : NULL;
    
    PQclear(res);
    return project;
}

Project** project_list_all(DatabaseConnection *db) {
    if (!db) {
        return NULL;
    }
    
    PGresult *res = db_query(db, "SELECT id, slug, name, description FROM projects ORDER BY created_at");
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    
    int count = PQntuples(res);
    Project **projects = calloc(count + 1, sizeof(Project*));
    if (!projects) {
        PQclear(res);
        return NULL;
    }
    
    for (int i = 0; i < count; i++) {
        projects[i] = malloc(sizeof(Project));
        if (!projects[i]) {
            project_free_array(projects);
            PQclear(res);
            return NULL;
        }
        
        projects[i]->id = atoi(PQgetvalue(res, i, 0));
        projects[i]->slug = strdup(PQgetvalue(res, i, 1));
        projects[i]->name = strdup(PQgetvalue(res, i, 2));
        char *desc = PQgetvalue(res, i, 3);
        projects[i]->description = desc && strlen(desc) > 0 ? strdup(desc) : NULL;
    }
    
    projects[count] = NULL;
    PQclear(res);
    return projects;
}

void project_free(Project *project) {
    if (!project) {
        return;
    }
    free(project->slug);
    free(project->name);
    free(project->description);
    free(project);
}

void project_free_array(Project **projects) {
    if (!projects) {
        return;
    }
    for (int i = 0; projects[i] != NULL; i++) {
        project_free(projects[i]);
    }
    free(projects);
}
