#ifndef DB_TRANSACTION_H
#define DB_TRANSACTION_H

#include "connection.h"

/**
 * @brief Start a database transaction
 * @param db Database connection
 * @return 1 on success, 0 on failure
 */
int db_begin_transaction(DatabaseConnection *db);

/**
 * @brief Commit a database transaction
 * @param db Database connection
 * @return 1 on success, 0 on failure
 */
int db_commit_transaction(DatabaseConnection *db);

/**
 * @brief Rollback a database transaction
 * @param db Database connection
 * @return 1 on success, 0 on failure
 */
int db_rollback_transaction(DatabaseConnection *db);

/**
 * @brief Execute multiple queries in a single transaction
 * @param db Database connection
 * @param queries NULL-terminated array of SQL query strings
 * @return 1 on success, 0 on failure (rollback on error)
 */
int db_execute_transaction(DatabaseConnection *db, const char **queries);

/**
 * @brief Check if we are currently in a transaction
 * @param db Database connection
 * @return 1 if in transaction, 0 otherwise
 */
int db_in_transaction(DatabaseConnection *db);

#endif // DB_TRANSACTION_H
