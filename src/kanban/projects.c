#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kanban/projects.h"
#include "db/connection.h"

Project *project_create(
    DatabaseConnection *db,
    const char *slug,
    const char *name,
    const char *description
) {
    if (!db || !slug || !name) return NULL;
    db_clear_error(db);

    PGresult *res;

    if (description) {
        const char *params[3] = {slug, name, description};
        res = PQexecParams(db->conn,
            "INSERT INTO projects (slug, name, description) VALUES ($1, $2, $3) RETURNING id",
            3, NULL, params, NULL, NULL, 0);
    } else {
        const char *params[2] = {slug, name};
        res = PQexecParams(db->conn,
            "INSERT INTO projects (slug, name) VALUES ($1, $2) RETURNING id",
            2, NULL, params, NULL, NULL, 0);
    }

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "project_create: %s\n", res ? PQresultErrorMessage(res) : "null result");
        db_capture_error(db, res);
        if (res) PQclear(res);
        return NULL;
    }

    int id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    Project *p = malloc(sizeof(Project));
    if (!p) return NULL;

    p->id          = id;
    p->slug        = strdup(slug);
    p->name        = strdup(name);
    p->description = description ? strdup(description) : NULL;
    return p;
}

Project *project_get_by_id(DatabaseConnection *db, int project_id) {
    if (!db) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", project_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "SELECT id, slug, name, description FROM projects WHERE id = $1",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }

    Project *p = malloc(sizeof(Project));
    if (!p) { PQclear(res); return NULL; }

    p->id   = atoi(PQgetvalue(res, 0, 0));
    p->slug = strdup(PQgetvalue(res, 0, 1));
    p->name = strdup(PQgetvalue(res, 0, 2));
    const char *desc = PQgetvalue(res, 0, 3);
    p->description = (desc && *desc) ? strdup(desc) : NULL;

    PQclear(res);
    return p;
}

Project **project_list_all(DatabaseConnection *db) {
    if (!db) return NULL;

    PGresult *res = PQexec(db->conn,
        "SELECT id, slug, name, description FROM projects ORDER BY created_at");

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }

    int count = PQntuples(res);
    Project **projects = calloc(count + 1, sizeof(Project *));
    if (!projects) { PQclear(res); return NULL; }

    for (int i = 0; i < count; i++) {
        projects[i] = malloc(sizeof(Project));
        if (!projects[i]) {
            project_free_array(projects);
            PQclear(res);
            return NULL;
        }
        projects[i]->id   = atoi(PQgetvalue(res, i, 0));
        projects[i]->slug = strdup(PQgetvalue(res, i, 1));
        projects[i]->name = strdup(PQgetvalue(res, i, 2));
        const char *desc = PQgetvalue(res, i, 3);
        projects[i]->description = (desc && *desc) ? strdup(desc) : NULL;
    }

    projects[count] = NULL;
    PQclear(res);
    return projects;
}

void project_free(Project *project) {
    if (!project) return;
    free(project->slug);
    free(project->name);
    free(project->description);
    free(project);
}

void project_free_array(Project **projects) {
    if (!projects) return;
    for (int i = 0; projects[i]; i++)
        project_free(projects[i]);
    free(projects);
}
