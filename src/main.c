#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db/connection.h"
#include "kanban/projects.h"
#include "kanban/tickets.h"

int main(int argc, char *argv[]) {
    printf("kb.ai - Database-Driven Kanban Engine\n");
    printf("=====================================\n\n");
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args...]\n", argv[0]);
        fprintf(stderr, "Commands: start, init-db\n");
        return EXIT_FAILURE;
    }
    
    const char *command = argv[1];
    
    if (strcmp(command, "start") == 0) {
        printf("Starting MCP server...\n");
        // TODO: Implement MCP server start
        return EXIT_SUCCESS;
    } else if (strcmp(command, "init-db") == 0) {
        printf("Initializing database...\n");
        // TODO: Implement database initialization
        return EXIT_SUCCESS;
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        return EXIT_FAILURE;
    }
}
