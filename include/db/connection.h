#ifndef DB_CONNECTION_H
#define DB_CONNECTION_H

#include <libpq-fe.h>

/**
 * @brief Database connection handle
 */
typedef struct {
    PGconn *conn;
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
 * @brief Execute a SQL query
 * @param db Database connection
 * @param query SQL query string
 * @return PGresult* or NULL on failure
 */
PGresult* db_query(DatabaseConnection *db, const char *query);

/**
 * @brief Get connection status
 * @param db Database connection
 * @return 1 if connected, 0 otherwise
 */
int db_is_connected(DatabaseConnection *db);

#endif // DB_CONNECTION_H
