#include <stdio.h>
#include <stdlib.h>
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
        fprintf(stderr, "db_query: invalid arguments\n");
        return NULL;
    }
    
    PGresult *res = PQexec(db->conn, query);
    
    if (res) {
        ExecStatusType status = PQresultStatus(res);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            fprintf(stderr, "db_query error: %s\n", PQresultErrorMessage(res));
        }
    } else {
        fprintf(stderr, "db_query: PQexec returned NULL: %s\n", PQerrorMessage(db->conn));
    }
    
    return res;
}

int db_is_connected(DatabaseConnection *db) {
    if (!db || !db->conn) {
        return 0;
    }
    return PQstatus(db->conn) == CONNECTION_OK;
}
