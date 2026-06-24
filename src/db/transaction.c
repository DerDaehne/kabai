#include "db/transaction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db_begin_transaction(DatabaseConnection *db) {
    if (!db || !db->conn) {
        return 0;
    }
    
    PGresult *res = PQexec(db->conn, "BEGIN");
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

int db_commit_transaction(DatabaseConnection *db) {
    if (!db || !db->conn) {
        return 0;
    }
    
    PGresult *res = PQexec(db->conn, "COMMIT");
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

int db_rollback_transaction(DatabaseConnection *db) {
    if (!db || !db->conn) {
        return 0;
    }
    
    PGresult *res = PQexec(db->conn, "ROLLBACK");
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        if (res) PQclear(res);
        return 0;
    }
    
    PQclear(res);
    return 1;
}

int db_execute_transaction(DatabaseConnection *db, const char **queries) {
    if (!db || !db->conn || !queries) {
        return 0;
    }
    
    // Begin transaction
    if (!db_begin_transaction(db)) {
        return 0;
    }
    
    int success = 1;
    PGresult *res = NULL;
    
    for (int i = 0; queries[i] != NULL; i++) {
        res = PQexec(db->conn, queries[i]);
        if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
            success = 0;
            break;
        }
        if (res) PQclear(res);
        res = NULL;
    }
    
    if (success) {
        // Commit on success
        if (!db_commit_transaction(db)) {
            success = 0;
        }
    } else {
        // Rollback on failure
        db_rollback_transaction(db);
    }
    
    if (res) PQclear(res);
    return success;
}

int db_in_transaction(DatabaseConnection *db) {
    if (!db || !db->conn) {
        return 0;
    }
    
    return PQtransactionStatus(db->conn) == PQTRANS_INTRANS;
}
