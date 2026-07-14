#ifndef DB_CONNECTION_H
#define DB_CONNECTION_H

#include <libpq-fe.h>

/**
 * @brief Database connection handle
 *
 * last_* fields hold details of the most recent failed query, captured
 * via db_capture_error() before the PGresult is cleared (the service
 * layer discards results; the tool layer maps these to actionable
 * errors). Empty strings when the last capture-enabled call succeeded.
 */
typedef struct {
    PGconn *conn;
    char last_sqlstate[6];
    char last_constraint[64];
    char last_primary[512];
} DatabaseConnection;

/**
 * @brief Connect to PostgreSQL database
 * @param host Database host
 * @param port Database port
 * @param dbname Database name
 * @param user Database user
 * @param password Database password
 * @return DatabaseConnection* or NULL on failure
 */
DatabaseConnection* db_connect(
    const char *host,
    const char *port,
    const char *dbname,
    const char *user,
    const char *password
);

/**
 * @brief Disconnect from database
 * @param db Database connection
 */
void db_disconnect(DatabaseConnection *db);

/**
 * @brief Reset the captured error details (call before a query whose
 *        failure details should be reported)
 */
void db_clear_error(DatabaseConnection *db);

/**
 * @brief Capture SQLSTATE/constraint/primary message of a failed query
 * @param db  Database connection
 * @param res Failed PGresult (may be NULL: falls back to PQerrorMessage)
 */
void db_capture_error(DatabaseConnection *db, PGresult *res);

#endif // DB_CONNECTION_H
