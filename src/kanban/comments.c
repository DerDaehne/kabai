#include "kanban/comments.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TicketComment *comment_add(
    DatabaseConnection *db,
    int ticket_id,
    const char *author,
    const char *comment_text
) {
    if (!db || !author || !comment_text) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);
    const char *params[3] = {id_str, author, comment_text};

    PGresult *res = PQexecParams(db->conn,
        "INSERT INTO ticket_comments (ticket_id, author, comment_text)"
        " VALUES ($1, $2, $3) RETURNING id, created_at::text",
        3, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "comment_add: %s\n", res ? PQresultErrorMessage(res) : "null result");
        if (res) PQclear(res);
        return NULL;
    }

    int id = atoi(PQgetvalue(res, 0, 0));
    const char *cat = PQgetvalue(res, 0, 1);

    TicketComment *c = malloc(sizeof(TicketComment));
    if (!c) { PQclear(res); return NULL; }

    c->id           = id;
    c->ticket_id    = ticket_id;
    c->author       = strdup(author);
    c->comment_text = strdup(comment_text);
    c->created_at   = (cat && *cat) ? strdup(cat) : NULL;
    PQclear(res);
    return c;
}

TicketComment **comment_list_by_ticket(DatabaseConnection *db, int ticket_id) {
    if (!db) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", ticket_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "SELECT id, ticket_id, author, comment_text, created_at::text"
        " FROM ticket_comments WHERE ticket_id = $1 ORDER BY created_at",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }

    int count = PQntuples(res);
    TicketComment **comments = calloc(count + 1, sizeof(TicketComment *));
    if (!comments) { PQclear(res); return NULL; }

    for (int i = 0; i < count; i++) {
        comments[i] = malloc(sizeof(TicketComment));
        if (!comments[i]) {
            comment_free_array(comments);
            PQclear(res);
            return NULL;
        }
        comments[i]->id           = atoi(PQgetvalue(res, i, 0));
        comments[i]->ticket_id    = atoi(PQgetvalue(res, i, 1));
        comments[i]->author       = strdup(PQgetvalue(res, i, 2));
        comments[i]->comment_text = strdup(PQgetvalue(res, i, 3));
        const char *cat           = PQgetvalue(res, i, 4);
        comments[i]->created_at   = (cat && *cat) ? strdup(cat) : NULL;
    }

    comments[count] = NULL;
    PQclear(res);
    return comments;
}

TicketComment *comment_get_by_id(DatabaseConnection *db, int comment_id) {
    if (!db) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", comment_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "SELECT id, ticket_id, author, comment_text, created_at::text"
        " FROM ticket_comments WHERE id = $1",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }

    TicketComment *c = malloc(sizeof(TicketComment));
    if (!c) { PQclear(res); return NULL; }

    c->id           = atoi(PQgetvalue(res, 0, 0));
    c->ticket_id    = atoi(PQgetvalue(res, 0, 1));
    c->author       = strdup(PQgetvalue(res, 0, 2));
    c->comment_text = strdup(PQgetvalue(res, 0, 3));
    const char *cat = PQgetvalue(res, 0, 4);
    c->created_at   = (cat && *cat) ? strdup(cat) : NULL;

    PQclear(res);
    return c;
}

int comment_update(DatabaseConnection *db, int comment_id, const char *new_text) {
    if (!db || !new_text) return 0;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", comment_id);
    const char *params[2] = {new_text, id_str};

    PGresult *res = PQexecParams(db->conn,
        "UPDATE ticket_comments SET comment_text = $1 WHERE id = $2",
        2, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }

    PQclear(res);
    return 1;
}

int comment_delete(DatabaseConnection *db, int comment_id) {
    if (!db) return 0;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", comment_id);
    const char *params[1] = {id_str};

    PGresult *res = PQexecParams(db->conn,
        "DELETE FROM ticket_comments WHERE id = $1",
        1, NULL, params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }

    PQclear(res);
    return 1;
}

void comment_free(TicketComment *c) {
    if (!c) return;
    free(c->author);
    free(c->comment_text);
    free(c->created_at);
    free(c);
}

void comment_free_array(TicketComment **comments) {
    if (!comments) return;
    for (int i = 0; comments[i]; i++)
        comment_free(comments[i]);
    free(comments);
}
