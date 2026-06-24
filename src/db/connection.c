#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/db/connection.h"

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
        dbname ? dbname : "kb_ai",
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
    return db;
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

PGresult* db_query(DatabaseConnection *db, const char *query) {
    if (!db || !db->conn || !query) {
        return NULL;
    }
    
    return PQexec(db->conn, query);
}

int db_is_connected(DatabaseConnection *db) {
    if (!db || !db->conn) {
        return 0;
    }
    return PQstatus(db->conn) == CONNECTION_OK;
}
