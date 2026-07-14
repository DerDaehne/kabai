#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db/connection.h"

DatabaseConnection* db_connect(
    const char *host,
    const char *port,
    const char *dbname,
    const char *user,
    const char *password
) {
    char conninfo[1024];
    snprintf(conninfo, sizeof(conninfo),
        "host=%s port=%s dbname=%s user=%s password=%s",
        host ? host : "localhost",
        port ? port : "5432",
        dbname ? dbname : "kabai",
        user ? user : "postgres",
        password ? password : "");
    
    PGconn *conn = PQconnectdb(conninfo);
    
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }
    
    DatabaseConnection *db = malloc(sizeof(DatabaseConnection));
    if (!db) {
        PQfinish(conn);
        return NULL;
    }
    
    db->conn = conn;
    db_clear_error(db);
    return db;
}

void db_clear_error(DatabaseConnection *db) {
    if (!db) return;
    db->last_sqlstate[0]   = '\0';
    db->last_constraint[0] = '\0';
    db->last_primary[0]    = '\0';
}

static void copy_field(char *dst, size_t dst_size, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, dst_size, "%s", src);
}

void db_capture_error(DatabaseConnection *db, PGresult *res) {
    if (!db) return;
    if (res) {
        copy_field(db->last_sqlstate,   sizeof(db->last_sqlstate),
                   PQresultErrorField(res, PG_DIAG_SQLSTATE));
        copy_field(db->last_constraint, sizeof(db->last_constraint),
                   PQresultErrorField(res, PG_DIAG_CONSTRAINT_NAME));
        copy_field(db->last_primary,    sizeof(db->last_primary),
                   PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY));
    } else {
        db->last_sqlstate[0]   = '\0';
        db->last_constraint[0] = '\0';
        copy_field(db->last_primary, sizeof(db->last_primary),
                   db->conn ? PQerrorMessage(db->conn) : NULL);
    }
}

void db_disconnect(DatabaseConnection *db) {
    if (db && db->conn) {
        PQfinish(db->conn);
        db->conn = NULL;
    }
    if (db) {
        free(db);
    }
}
