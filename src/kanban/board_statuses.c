#include "kanban/board_statuses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BoardStatus *board_status_create(
    DatabaseConnection *db,
    int project_id,
    const char *name,
    const char *display_name,
    int position,
    const char *agent_role_instruction,
    const char *special_type
) {
    if (!db || !name || !display_name) return NULL;
    db_clear_error(db);

    char proj_str[32], pos_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    snprintf(pos_str,  sizeof(pos_str),  "%d", position);

    PGresult *res;

    if (agent_role_instruction && special_type) {
        const char *p[6] = {proj_str, name, display_name, pos_str, agent_role_instruction, special_type};
        res = PQexecParams(db->conn,
            "INSERT INTO board_statuses"
            " (project_id, name, display_name, position, agent_role_instruction, special_type)"
            " VALUES ($1, $2, $3, $4, $5, $6) RETURNING id",
            6, NULL, p, NULL, NULL, 0);
    } else if (agent_role_instruction) {
        const char *p[5] = {proj_str, name, display_name, pos_str, agent_role_instruction};
        res = PQexecParams(db->conn,
            "INSERT INTO board_statuses (project_id, name, display_name, position, agent_role_instruction)"
            " VALUES ($1, $2, $3, $4, $5) RETURNING id",
            5, NULL, p, NULL, NULL, 0);
    } else if (special_type) {
        const char *p[5] = {proj_str, name, display_name, pos_str, special_type};
        res = PQexecParams(db->conn,
            "INSERT INTO board_statuses (project_id, name, display_name, position, special_type)"
            " VALUES ($1, $2, $3, $4, $5) RETURNING id",
            5, NULL, p, NULL, NULL, 0);
    } else {
        const char *p[4] = {proj_str, name, display_name, pos_str};
        res = PQexecParams(db->conn,
            "INSERT INTO board_statuses (project_id, name, display_name, position)"
            " VALUES ($1, $2, $3, $4) RETURNING id",
            4, NULL, p, NULL, NULL, 0);
    }

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "board_status_create: %s\n",
                res ? PQresultErrorMessage(res) : "null result");
        db_capture_error(db, res);
        if (res) PQclear(res);
        return NULL;
    }

    int id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    BoardStatus *bs = malloc(sizeof(BoardStatus));
    if (!bs) return NULL;

    bs->id                   = id;
    bs->project_id           = project_id;
    bs->name                 = strdup(name);
    bs->display_name         = strdup(display_name);
    bs->position             = position;
    bs->agent_role_instruction = agent_role_instruction ? strdup(agent_role_instruction) : NULL;
    bs->special_type         = special_type ? strdup(special_type) : NULL;
    return bs;
}

BoardStatus **board_status_list_by_project(DatabaseConnection *db, int project_id) {
    if (!db) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", project_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "SELECT id, project_id, name, display_name, position, agent_role_instruction, special_type"
        " FROM board_statuses WHERE project_id = $1 ORDER BY position",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }

    int count = PQntuples(res);
    BoardStatus **arr = calloc(count + 1, sizeof(BoardStatus *));
    if (!arr) { PQclear(res); return NULL; }

    for (int i = 0; i < count; i++) {
        arr[i] = malloc(sizeof(BoardStatus));
        if (!arr[i]) {
            board_status_free_array(arr);
            PQclear(res);
            return NULL;
        }
        arr[i]->id           = atoi(PQgetvalue(res, i, 0));
        arr[i]->project_id   = atoi(PQgetvalue(res, i, 1));
        arr[i]->name         = strdup(PQgetvalue(res, i, 2));
        arr[i]->display_name = strdup(PQgetvalue(res, i, 3));
        arr[i]->position     = atoi(PQgetvalue(res, i, 4));

        const char *ari = PQgetvalue(res, i, 5);
        arr[i]->agent_role_instruction = (ari && *ari) ? strdup(ari) : NULL;

        const char *st = PQgetvalue(res, i, 6);
        arr[i]->special_type = (st && *st) ? strdup(st) : NULL;
    }

    arr[count] = NULL;
    PQclear(res);
    return arr;
}

int status_transition_create(
    DatabaseConnection *db,
    int project_id,
    int from_status_id,
    int to_status_id
) {
    if (!db) return 0;
    db_clear_error(db);

    char proj_str[32], from_str[32], to_str[32];
    snprintf(proj_str, sizeof(proj_str), "%d", project_id);
    snprintf(from_str, sizeof(from_str), "%d", from_status_id);
    snprintf(to_str,   sizeof(to_str),   "%d", to_status_id);
    const char *params[3] = {proj_str, from_str, to_str};

    PGresult *res = PQexecParams(db->conn,
        "INSERT INTO status_transitions (project_id, from_status_id, to_status_id)"
        " VALUES ($1, $2, $3) ON CONFLICT DO NOTHING",
        3, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "status_transition_create: %s\n",
                res ? PQresultErrorMessage(res) : "null result");
        db_capture_error(db, res);
        if (res) PQclear(res);
        return 0;
    }

    PQclear(res);
    return 1;
}

StatusTransition **status_transition_list_by_project(DatabaseConnection *db, int project_id) {
    if (!db) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", project_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "SELECT from_status_id, to_status_id"
        " FROM status_transitions WHERE project_id = $1"
        " ORDER BY from_status_id, to_status_id",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }

    int count = PQntuples(res);
    StatusTransition **arr = calloc(count + 1, sizeof(StatusTransition *));
    if (!arr) { PQclear(res); return NULL; }

    for (int i = 0; i < count; i++) {
        arr[i] = malloc(sizeof(StatusTransition));
        if (!arr[i]) {
            status_transition_free_array(arr);
            PQclear(res);
            return NULL;
        }
        arr[i]->from_status_id = atoi(PQgetvalue(res, i, 0));
        arr[i]->to_status_id   = atoi(PQgetvalue(res, i, 1));
    }

    arr[count] = NULL;
    PQclear(res);
    return arr;
}

void board_status_free(BoardStatus *bs) {
    if (!bs) return;
    free(bs->name);
    free(bs->display_name);
    free(bs->agent_role_instruction);
    free(bs->special_type);
    free(bs);
}

void board_status_free_array(BoardStatus **arr) {
    if (!arr) return;
    for (int i = 0; arr[i]; i++)
        board_status_free(arr[i]);
    free(arr);
}

void status_transition_free_array(StatusTransition **arr) {
    if (!arr) return;
    for (int i = 0; arr[i]; i++)
        free(arr[i]);
    free(arr);
}
