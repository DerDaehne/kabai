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

int db_in_transaction(DatabaseConnection *db) {
    if (!db || !db->conn) {
        return 0;
    }
    
    return PQtransactionStatus(db->conn) == PQTRANS_INTRANS;
}
