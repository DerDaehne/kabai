/*
 * kb.ai MCP Server
 *
 * Implements the Model Context Protocol (MCP) over STDIO using JSON-RPC 2.0.
 * Exposes Kanban operations as MCP tools backed by a PostgreSQL database.
 *
 * Protocol: https://spec.modelcontextprotocol.io/
 * Supported methods: initialize, notifications/initialized, tools/list, tools/call
 *
 * main.c is bootstrap only: DB connection, tool registry, module
 * registration, stdio loop. Tools live in their modules
 * (src/kanban/kanban_tools.c; src/docs/ to follow) and register themselves
 * via <module>_register_tools(McpRegistry*).
 */

#include <stdio.h>
#include <stdlib.h>
#include <cjson/cJSON.h>
#include "db/connection.h"
#include "mcp/mcp.h"
#include "kanban/kanban_tools.h"

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_SERVER_VERSION   "0.5.0"
#define MCP_SERVER_NAME      "kb.ai"

#define DEFAULT_DB_HOST     "localhost"
#define DEFAULT_DB_PORT     "5432"
#define DEFAULT_DB_NAME     "kb_ai"
#define DEFAULT_DB_USER     "postgres"
#define DEFAULT_DB_PASSWORD ""

static DatabaseConnection *db_init(void) {
    const char *host     = getenv("KB_AI_DB_HOST");
    const char *port     = getenv("KB_AI_DB_PORT");
    const char *dbname   = getenv("KB_AI_DB_NAME");
    const char *user     = getenv("KB_AI_DB_USER");
    const char *password = getenv("KB_AI_DB_PASSWORD");

    DatabaseConnection *db = db_connect(
        host     ? host     : DEFAULT_DB_HOST,
        port     ? port     : DEFAULT_DB_PORT,
        dbname   ? dbname   : DEFAULT_DB_NAME,
        user     ? user     : DEFAULT_DB_USER,
        password ? password : DEFAULT_DB_PASSWORD
    );

    if (!db) {
        fprintf(stderr, "kb.ai: failed to connect to database\n");
        return NULL;
    }

    fprintf(stderr, "kb.ai: connected to %s:%s/%s\n",
            host   ? host   : DEFAULT_DB_HOST,
            port   ? port   : DEFAULT_DB_PORT,
            dbname ? dbname : DEFAULT_DB_NAME);
    return db;
}

int main(void) {
    cJSON_Hooks hooks = {malloc, free};
    cJSON_InitHooks(&hooks);

    DatabaseConnection *db = db_init();
    if (!db)
        return EXIT_FAILURE;

    McpContext ctx = {
        .db          = db,
        .agent_name  = getenv("KB_AI_AGENT_NAME"),
        .agent_model = getenv("KB_AI_AGENT_MODEL"),
    };

    McpRegistry *registry = mcp_registry_new();
    kanban_register_tools(registry);

    McpServerInfo info = {
        .name             = MCP_SERVER_NAME,
        .version          = MCP_SERVER_VERSION,
        .protocol_version = MCP_PROTOCOL_VERSION,
    };

    fprintf(stderr, "kb.ai MCP Server %s (MCP protocol %s) ready\n",
            MCP_SERVER_VERSION, MCP_PROTOCOL_VERSION);

    mcp_run_stdio_loop(registry, &ctx, &info);

    mcp_registry_free(registry);
    db_disconnect(db);
    fprintf(stderr, "kb.ai MCP Server shut down\n");
    return EXIT_SUCCESS;
}
