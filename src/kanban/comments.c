#include "../include/kanban/comments.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TicketComment* comment_add(
    DatabaseConnection *db,
    int ticket_id,
    const char *author,
    const char *comment_text
) {
    if (!db || !author || !comment_text) {
        return NULL;
    }
    
    char *esc_author = malloc(2 * strlen(author) + 1);
    char *esc_text = malloc(2 * strlen(comment_text) + 1);
    
    if (!esc_author || !esc_text) {
        free(esc_author);
        free(esc_text);
        return NULL;
    }
    
    PQescapeStringConn(db->conn, esc_author, author, strlen(author), NULL);
    PQescapeStringConn(db->conn, esc_text, comment_text, strlen(comment_text), NULL);
    
    char query[2048];
    snprintf(query, sizeof(query),
        "INSERT INTO ticket_comments (ticket_id, author, comment_text) VALUES (%d, '%s', '%s') RETURNING id",
        ticket_id, esc_author, esc_text);
    
    PGresult *res = db_query(db, query);
    free(esc_author);
    free(esc_text);
    
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        if (res) PQclear(res);
        return NULL;
    }
    
    int id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    
    TicketComment *comment = malloc(sizeof(TicketComment));
    if (!comment) {
        return NULL;
    }
    
    comment->id = id;
    comment->ticket_id = ticket_id;
    comment->author = strdup(author);
    comment->comment_text = strdup(comment_text);
    
    return comment;
}

TicketComment** comment_list_by_ticket(DatabaseConnection *db, int ticket_id) {
    if (!db) {
        return NULL;
    }
    
    char query[256];
    snprintf(query, sizeof(query),
        "SELECT id, ticket_id, author, comment_text FROM ticket_comments WHERE ticket_id = %d ORDER BY created_at",
        ticket_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    
    int count = PQntuples(res);
    TicketComment **comments = calloc(count + 1, sizeof(TicketComment*));
    if (!comments) {
        PQclear(res);
        return NULL;
    }
    
    for (int i = 0; i < count; i++) {
        comments[i] = malloc(sizeof(TicketComment));
        if (!comments[i]) {
            comment_free_array(comments);
            PQclear(res);
            return NULL;
        }
        
        comments[i]->id = atoi(PQgetvalue(res, i, 0));
        comments[i]->ticket_id = atoi(PQgetvalue(res, i, 1));
        comments[i]->author = strdup(PQgetvalue(res, i, 2));
        comments[i]->comment_text = strdup(PQgetvalue(res, i, 3));
    }
    
    comments[count] = NULL;
    PQclear(res);
    return comments;
}

TicketComment* comment_get_by_id(DatabaseConnection *db, int comment_id) {
    if (!db) {
        return NULL;
    }
    
    char query[256];
    snprintf(query, sizeof(query),
        "SELECT id, ticket_id, author, comment_text FROM ticket_comments WHERE id = %d",
        comment_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQntuples(res) == 0) {
        if (res) PQclear(res);
        return NULL;
    }
    
    TicketComment *comment = malloc(sizeof(TicketComment));
    if (!comment) {
        PQclear(res);
        return NULL;
    }
    
    comment->id = atoi(PQgetvalue(res, 0, 0));
    comment->ticket_id = atoi(PQgetvalue(res, 0, 1));
    comment->author = strdup(PQgetvalue(res, 0, 2));
    comment->comment_text = strdup(PQgetvalue(res, 0, 3));
    
    PQclear(res);
    return comment;
}

int comment_update(
    DatabaseConnection *db,
    int comment_id,
    const char *new_text
) {
    if (!db || !new_text) {
        return 0;
    }
    
    char *esc_text = malloc(2 * strlen(new_text) + 1);
    if (!esc_text) {
        return 0;
    }
    
    PQescapeStringConn(db->conn, esc_text, new_text, strlen(new_text), NULL);
    
    char query[2048];
    snprintf(query, sizeof(query),
        "UPDATE ticket_comments SET comment_text = '%s' WHERE id = %d",
        esc_text, comment_id);
    free(esc_text);
    
    PGresult *res = db_query(db, query);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

int comment_delete(DatabaseConnection *db, int comment_id) {
    if (!db) {
        return 0;
    }
    
    char query[256];
    snprintf(query, sizeof(query),
        "DELETE FROM ticket_comments WHERE id = %d",
        comment_id);
    
    PGresult *res = db_query(db, query);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

void comment_free(TicketComment *comment) {
    if (!comment) {
        return;
    }
    free(comment->author);
    free(comment->comment_text);
    free(comment);
}

void comment_free_array(TicketComment **comments) {
    if (!comments) {
        return;
    }
    for (int i = 0; comments[i] != NULL; i++) {
        comment_free(comments[i]);
    }
    free(comments);
}
