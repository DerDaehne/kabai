#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "db/connection.h"
#include "kanban/projects.h"
#include "kanban/tickets.h"
#include "kanban/comments.h"

// ============================================================================
// MCP Protocol Constants
// ============================================================================

#define MCP_VERSION "0.1.0"

// ============================================================================
// Database Connection Configuration
// ============================================================================

// Default connection parameters - can be overridden via environment variables
#define DEFAULT_DB_HOST "localhost"
#define DEFAULT_DB_PORT "5432"
#define DEFAULT_DB_NAME "kb_ai"
#define DEFAULT_DB_USER "postgres"
#define DEFAULT_DB_PASSWORD ""

static DatabaseConnection* global_db = NULL;

// ============================================================================
// JSON Helper Functions (Minimal Implementation)
// ============================================================================

/**
 * Escapes a string for JSON output
 */
char* json_escape(const char *str) {
    if (!str) return strdup("null");
    
    // Count required space
    int count = 0;
    for (int i = 0; str[i]; i++) {
        switch (str[i]) {
            case '"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t':
                count += 2;
                break;
            default:
                count += 1;
        }
    }
    
    char *result = malloc(count + 1);
    if (!result) return NULL;
    
    int j = 0;
    for (int i = 0; str[i]; i++) {
        switch (str[i]) {
            case '"': result[j++] = '\\'; result[j++] = '"'; break;
            case '\\': result[j++] = '\\'; result[j++] = '\\'; break;
            case '\b': result[j++] = '\\'; result[j++] = 'b'; break;
            case '\f': result[j++] = '\\'; result[j++] = 'f'; break;
            case '\n': result[j++] = '\\'; result[j++] = 'n'; break;
            case '\r': result[j++] = '\\'; result[j++] = 'r'; break;
            case '\t': result[j++] = '\\'; result[j++] = 't'; break;
            default: result[j++] = str[i];
        }
    }
    result[j] = '\0';
    return result;
}

/**
 * Creates a JSON response for MCP
 */
char* mcp_response(const char *request_id, const char *content) {
    char *escaped_content = json_escape(content);
    char *result = malloc(strlen(request_id) + strlen(escaped_content) + 100);
    if (!result || !escaped_content) {
        free(escaped_content);
        free(result);
        return NULL;
    }
    sprintf(result, "{\"id\":\"%s\",\"result\":\"%s\"}", request_id, escaped_content);
    free(escaped_content);
    return result;
}

char* mcp_error(const char *request_id, const char *message) {
    char *escaped_msg = json_escape(message);
    char *result = malloc(strlen(request_id) + strlen(escaped_msg) + 100);
    if (!result || !escaped_msg) {
        free(escaped_msg);
        free(result);
        return NULL;
    }
    sprintf(result, "{\"id\":\"%s\",\"error\":\"%s\"}", request_id, escaped_msg);
    free(escaped_msg);
    return result;
}

// ============================================================================
// MCP Tool: kb.ai_create_project
// ============================================================================

char* mcp_tool_kb_ai_create_project(const char *request_id, const char *params_json) {
    // Parse params: {"slug": "...", "name": "...", "description": "..."}
    // For now, we'll use a simple parser
    const char *slug = NULL;
    const char *name = NULL;
    const char *description = NULL;
    
    // Simple JSON parsing (very basic - replace with proper parser)
    if (params_json) {
        // This is a placeholder - need proper JSON parsing
        // For now, we'll just acknowledge the tool exists
    }
    
    if (!slug || !name) {
        return mcp_error(request_id, "Missing required parameters: slug, name");
    }
    
    Project *project = project_create(global_db, slug, name, description);
    if (!project) {
        return mcp_error(request_id, "Failed to create project");
    }
    
    char response[1024];
    snprintf(response, sizeof(response), 
        "{\"id\":%d,\"slug\":\"%s\",\"name\":\"%s\"}",
        project->id, project->slug, project->name);
    
    project_free(project);
    return mcp_response(request_id, response);
}

// ============================================================================
// MCP Tool: kb.ai_list_projects
// ============================================================================

char* mcp_tool_kb_ai_list_projects(const char *request_id) {
    Project **projects = project_list_all(global_db);
    if (!projects) {
        return mcp_response(request_id, "[]");
    }
    
    // Build JSON array
    char buffer[8192];
    char *ptr = buffer;
    ptr[0] = '[';
    ptr[1] = '\0';
    
    bool first = true;
    for (int i = 0; projects[i] != NULL; i++) {
        if (!first) {
            strcat(ptr, ",");
            ptr += strlen(ptr);
        }
        first = false;
        
        char item[512];
        snprintf(item, sizeof(item),
            "{\"id\":%d,\"slug\":\"%s\",\"name\":\"%s\"}",
            projects[i]->id, projects[i]->slug, projects[i]->name);
        strcat(ptr, item);
        ptr += strlen(ptr);
    }
    strcat(ptr, "]");
    
    project_free_array(projects);
    return mcp_response(request_id, buffer);
}

// ============================================================================
// MCP Tool: kb.ai_get_project
// ============================================================================

char* mcp_tool_kb_ai_get_project(const char *request_id, const char *params_json) {
    // Parse project_id from params
    int project_id = 0;
    // Placeholder: parse from JSON
    
    Project *project = project_get_by_id(global_db, project_id);
    if (!project) {
        return mcp_error(request_id, "Project not found");
    }
    
    char response[1024];
    snprintf(response, sizeof(response),
        "{\"id\":%d,\"slug\":\"%s\",\"name\":\"%s\",\"description\":\"%s\"}",
        project->id, project->slug, project->name,
        project->description ? project->description : "");
    
    project_free(project);
    return mcp_response(request_id, response);
}

// ============================================================================
// MCP Tool: kb.ai_create_ticket
// ============================================================================

char* mcp_tool_kb_ai_create_ticket(const char *request_id, const char *params_json) {
    // Parse params: {"project_id": 1, "status_id": 1, "title": "...", "description": "..."}
    int project_id = 0;
    int status_id = 0;
    const char *title = NULL;
    const char *description = NULL;
    
    // Placeholder parsing
    
    if (!project_id || !status_id || !title) {
        return mcp_error(request_id, "Missing required parameters: project_id, status_id, title");
    }
    
    Ticket *ticket = ticket_create(global_db, project_id, status_id, title, description);
    if (!ticket) {
        return mcp_error(request_id, "Failed to create ticket");
    }
    
    char response[1024];
    snprintf(response, sizeof(response),
        "{\"id\":%d,\"project_id\":%d,\"status_id\":%d,\"title\":\"%s\"}",
        ticket->id, ticket->project_id, ticket->status_id, ticket->title);
    
    ticket_free(ticket);
    return mcp_response(request_id, response);
}

// ============================================================================
// MCP Tool: kb.ai_list_tickets
// ============================================================================

char* mcp_tool_kb_ai_list_tickets(const char *request_id, const char *params_json) {
    // Parse project_id from params
    int project_id = 0;
    // Placeholder parsing
    
    Ticket **tickets = ticket_list_by_project(global_db, project_id);
    if (!tickets) {
        return mcp_response(request_id, "[]");
    }
    
    char buffer[8192];
    char *ptr = buffer;
    ptr[0] = '[';
    ptr[1] = '\0';
    
    bool first = true;
    for (int i = 0; tickets[i] != NULL; i++) {
        if (!first) {
            strcat(ptr, ",");
            ptr += strlen(ptr);
        }
        first = false;
        
        char item[512];
        snprintf(item, sizeof(item),
            "{\"id\":%d,\"title\":\"%s\",\"status_id\":%d,\"assignee\":\"%s\"}",
            tickets[i]->id, tickets[i]->title, tickets[i]->status_id,
            tickets[i]->assignee ? tickets[i]->assignee : "");
        strcat(ptr, item);
        ptr += strlen(ptr);
    }
    strcat(ptr, "]");
    
    ticket_free_array(tickets);
    return mcp_response(request_id, buffer);
}

// ============================================================================
// MCP Tool: kb.ai_get_ticket
// ============================================================================

char* mcp_tool_kb_ai_get_ticket(const char *request_id, const char *params_json) {
    // Parse ticket_id from params
    int ticket_id = 0;
    // Placeholder parsing
    
    Ticket *ticket = ticket_get_by_id(global_db, ticket_id);
    if (!ticket) {
        return mcp_error(request_id, "Ticket not found");
    }
    
    // Get tasks
    TicketTask **tasks = ticket_get_tasks(global_db, ticket_id);
    
    char tasks_json[2048] = "";
    if (tasks && tasks[0] != NULL) {
        char *tptr = tasks_json;
        tptr[0] = '[';
        tptr[1] = '\0';
        
        bool first_task = true;
        for (int i = 0; tasks[i] != NULL; i++) {
            if (!first_task) {
                strcat(tptr, ",");
                tptr += strlen(tptr);
            }
            first_task = false;
            
            char item[256];
            snprintf(item, sizeof(item),
                "{\"id\":%d,\"title\":\"%s\",\"is_completed\":%s}",
                tasks[i]->id, tasks[i]->title,
                tasks[i]->is_completed ? "true" : "false");
            strcat(tptr, item);
            tptr += strlen(tptr);
        }
        strcat(tptr, "]");
    }
    
    ticket_task_free_array(tasks);
    
    char response[4096];
    snprintf(response, sizeof(response),
        "{\"id\":%d,\"title\":\"%s\",\"description\":\"%s\",\"status_id\":%d,\"assignee\":\"%s\",\"tasks\":%s}",
        ticket->id, ticket->title,
        ticket->description ? ticket->description : "",
        ticket->status_id,
        ticket->assignee ? ticket->assignee : "",
        tasks_json[0] ? tasks_json : "[]");
    
    ticket_free(ticket);
    return mcp_response(request_id, response);
}

// ============================================================================
// MCP Tool: kb.ai_move_ticket
// ============================================================================

char* mcp_tool_kb_ai_move_ticket(const char *request_id, const char *params_json) {
    // Parse ticket_id and new_status_id from params
    int ticket_id = 0;
    int new_status_id = 0;
    // Placeholder parsing
    
    if (!ticket_id || !new_status_id) {
        return mcp_error(request_id, "Missing required parameters: ticket_id, new_status_id");
    }
    
    if (!ticket_update_status(global_db, ticket_id, new_status_id)) {
        return mcp_error(request_id, "Failed to move ticket - check workflow rules");
    }
    
    return mcp_response(request_id, "{\"success\":true}");
}

// ============================================================================
// MCP Tool: kb.ai_assign_ticket
// ============================================================================

char* mcp_tool_kb_ai_assign_ticket(const char *request_id, const char *params_json) {
    // Parse ticket_id and assignee from params
    int ticket_id = 0;
    const char *assignee = NULL;
    // Placeholder parsing
    
    if (!ticket_id || !assignee) {
        return mcp_error(request_id, "Missing required parameters: ticket_id, assignee");
    }
    
    if (!ticket_assign(global_db, ticket_id, assignee)) {
        return mcp_error(request_id, "Failed to assign ticket");
    }
    
    return mcp_response(request_id, "{\"success\":true}");
}

// ============================================================================
// MCP Tool: kb.ai_add_task
// ============================================================================

char* mcp_tool_kb_ai_add_task(const char *request_id, const char *params_json) {
    // Parse ticket_id and title from params
    int ticket_id = 0;
    const char *title = NULL;
    // Placeholder parsing
    
    if (!ticket_id || !title) {
        return mcp_error(request_id, "Missing required parameters: ticket_id, title");
    }
    
    TicketTask *task = ticket_add_task(global_db, ticket_id, title);
    if (!task) {
        return mcp_error(request_id, "Failed to add task");
    }
    
    char response[256];
    snprintf(response, sizeof(response),
        "{\"id\":%d,\"ticket_id\":%d,\"title\":\"%s\",\"is_completed\":false}",
        task->id, task->ticket_id, task->title);
    
    ticket_task_free(task);
    return mcp_response(request_id, response);
}

// ============================================================================
// MCP Tool: kb.ai_complete_task
// ============================================================================

char* mcp_tool_kb_ai_complete_task(const char *request_id, const char *params_json) {
    // Parse task_id from params
    int task_id = 0;
    // Placeholder parsing
    
    if (!task_id) {
        return mcp_error(request_id, "Missing required parameter: task_id");
    }
    
    if (!ticket_complete_task(global_db, task_id)) {
        return mcp_error(request_id, "Failed to complete task");
    }
    
    return mcp_response(request_id, "{\"success\":true}");
}

// ============================================================================
// MCP Tool Dispatcher
// ============================================================================

char* mcp_dispatch_tool(const char *request_id, const char *tool_name, const char *params_json) {
    if (strcmp(tool_name, "kb.ai_create_project") == 0) {
        return mcp_tool_kb_ai_create_project(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_list_projects") == 0) {
        return mcp_tool_kb_ai_list_projects(request_id);
    } else if (strcmp(tool_name, "kb.ai_get_project") == 0) {
        return mcp_tool_kb_ai_get_project(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_create_ticket") == 0) {
        return mcp_tool_kb_ai_create_ticket(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_list_tickets") == 0) {
        return mcp_tool_kb_ai_list_tickets(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_get_ticket") == 0) {
        return mcp_tool_kb_ai_get_ticket(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_move_ticket") == 0) {
        return mcp_tool_kb_ai_move_ticket(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_assign_ticket") == 0) {
        return mcp_tool_kb_ai_assign_ticket(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_add_task") == 0) {
        return mcp_tool_kb_ai_add_task(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_complete_task") == 0) {
        return mcp_tool_kb_ai_complete_task(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_update_ticket") == 0) {
        return mcp_tool_kb_ai_update_ticket(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_get_ticket_detailed") == 0) {
        return mcp_tool_kb_ai_get_ticket_detailed(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_add_comment") == 0) {
        return mcp_tool_kb_ai_add_comment(request_id, params_json);
    } else if (strcmp(tool_name, "kb.ai_list_comments") == 0) {
        return mcp_tool_kb_ai_list_comments(request_id, params_json);
    } else {
        return mcp_error(request_id, "Unknown tool");
    }
}

// ============================================================================
// New MCP Tools: Ticket Editing and Work Log
// ============================================================================

char* mcp_tool_kb_ai_update_ticket(const char *request_id, const char *params_json) {
    // Parse params: {"ticket_id": 1, "title": "...", "description": "..."}
    int ticket_id = 0;
    const char *title = NULL;
    const char *description = NULL;
    // Placeholder parsing
    
    if (!ticket_id) {
        return mcp_error(request_id, "Missing required parameter: ticket_id");
    }
    
    int updated = 0;
    
    if (title) {
        updated += ticket_update_title(global_db, ticket_id, title);
    }
    
    if (description != NULL) {
        // description can be empty string to clear
        updated += ticket_update_description(global_db, ticket_id, description);
    }
    
    if (updated > 0) {
        return mcp_response(request_id, "{\"success\":true,\"updated_fields\":true}");
    }
    
    return mcp_response(request_id, "{\"success\":false,\"error\":\"No fields to update\"}");
}

char* mcp_tool_kb_ai_get_ticket_detailed(const char *request_id, const char *params_json) {
    // Parse ticket_id from params
    int ticket_id = 0;
    // Placeholder parsing
    
    if (!ticket_id) {
        return mcp_error(request_id, "Missing required parameter: ticket_id");
    }
    
    TicketDetailed *detailed = ticket_get_detailed(global_db, ticket_id);
    if (!detailed) {
        return mcp_error(request_id, "Ticket not found");
    }
    
    // Build comprehensive JSON response
    char tasks_json[4096] = "";
    if (detailed->tasks && detailed->tasks[0] != NULL) {
        char *tptr = tasks_json;
        tptr[0] = '[';
        tptr[1] = '\0';
        
        bool first_task = true;
        for (int i = 0; detailed->tasks[i] != NULL; i++) {
            if (!first_task) {
                strcat(tptr, ",");
                tptr += strlen(tptr);
            }
            first_task = false;
            
            char item[512];
            snprintf(item, sizeof(item),
                "{\"id\":%d,\"title\":\"%s\",\"is_completed\":%s}",
                detailed->tasks[i]->id, detailed->tasks[i]->title,
                detailed->tasks[i]->is_completed ? "true" : "false");
            strcat(tptr, item);
            tptr += strlen(tptr);
        }
        strcat(tptr, "]");
    }
    
    char comments_json[8192] = "";
    if (detailed->comments && detailed->comments[0] != NULL) {
        char *cptr = comments_json;
        cptr[0] = '[';
        cptr[1] = '\0';
        
        bool first_comment = true;
        for (int i = 0; detailed->comments[i] != NULL; i++) {
            if (!first_comment) {
                strcat(cptr, ",");
                cptr += strlen(cptr);
            }
            first_comment = false;
            
            char item[1024];
            snprintf(item, sizeof(item),
                "{\"id\":%d,\"author\":\"%s\",\"text\":\"%s\",\"created_at\":\"%s\"}",
                detailed->comments[i]->id, detailed->comments[i]->author,
                detailed->comments[i]->comment_text, "TODO"); // created_at placeholder
            strcat(cptr, item);
            cptr += strlen(cptr);
        }
        strcat(cptr, "]");
    }
    
    char response[16384];
    snprintf(response, sizeof(response),
        "{\"ticket\":{\"id\":%d,\"title\":\"%s\",\"description\":\"%s\",\"status_id\":%d,\"assignee\":\"%s\"},\"tasks\":%s,\"comments\":%s}",
        detailed->ticket->id,
        detailed->ticket->title,
        detailed->ticket->description ? detailed->ticket->description : "",
        detailed->ticket->status_id,
        detailed->ticket->assignee ? detailed->ticket->assignee : "",
        tasks_json[0] ? tasks_json : "[]",
        comments_json[0] ? comments_json : "[]");
    
    ticket_detailed_free(detailed);
    return mcp_response(request_id, response);
}

char* mcp_tool_kb_ai_add_comment(const char *request_id, const char *params_json) {
    // Parse params: {"ticket_id": 1, "author": "...", "text": "..."}
    int ticket_id = 0;
    const char *author = NULL;
    const char *text = NULL;
    // Placeholder parsing
    
    if (!ticket_id || !author || !text) {
        return mcp_error(request_id, "Missing required parameters: ticket_id, author, text");
    }
    
    TicketComment *comment = comment_add(global_db, ticket_id, author, text);
    if (!comment) {
        return mcp_error(request_id, "Failed to add comment");
    }
    
    char response[1024];
    snprintf(response, sizeof(response),
        "{\"id\":%d,\"ticket_id\":%d,\"author\":\"%s\",\"text\":\"%s\"}",
        comment->id, comment->ticket_id, comment->author, comment->comment_text);
    
    comment_free(comment);
    return mcp_response(request_id, response);
}

char* mcp_tool_kb_ai_list_comments(const char *request_id, const char *params_json) {
    // Parse ticket_id from params
    int ticket_id = 0;
    // Placeholder parsing
    
    if (!ticket_id) {
        return mcp_error(request_id, "Missing required parameter: ticket_id");
    }
    
    TicketComment **comments = comment_list_by_ticket(global_db, ticket_id);
    if (!comments) {
        return mcp_response(request_id, "[]");
    }
    
    char buffer[8192];
    char *ptr = buffer;
    ptr[0] = '[';
    ptr[1] = '\0';
    
    bool first = true;
    for (int i = 0; comments[i] != NULL; i++) {
        if (!first) {
            strcat(ptr, ",");
            ptr += strlen(ptr);
        }
        first = false;
        
        char item[1024];
        snprintf(item, sizeof(item),
            "{\"id\":%d,\"author\":\"%s\",\"text\":\"%s\"}",
            comments[i]->id, comments[i]->author, comments[i]->comment_text);
        strcat(ptr, item);
        ptr += strlen(ptr);
    }
    strcat(ptr, "]");
    
    comment_free_array(comments);
    return mcp_response(request_id, buffer);
}

// ============================================================================
// MCP Protocol Handler
// ============================================================================

/**
 * Handles an MCP request from stdin
 */
void mcp_handle_request(const char *request_json) {
    // Parse request JSON (simplified)
    // Expected format: {"id": "req-1", "method": "tools/call", "params": {"name": "kb.ai_tool", "arguments": {...}}}
    
    char request_id[256] = "";
    char tool_name[256] = "";
    char params_json[4096] = "";
    
    // Very basic JSON parsing - TODO: replace with proper JSON parser
    const char *ptr = request_json;
    
    // Extract request_id
    const char *id_start = strstr(ptr, "\"id\":\"");
    if (id_start) {
        id_start += 6; // Skip "\"id\":\""
        const char *id_end = strchr(id_start, '"');
        if (id_end) {
            strncpy(request_id, id_start, id_end - id_start);
            request_id[id_end - id_start] = '\0';
        }
    }
    
    // Extract tool name
    const char *name_start = strstr(ptr, "\"name\":\"");
    if (name_start) {
        name_start += 8; // Skip "\"name\":\""
        const char *name_end = strchr(name_start, '"');
        if (name_end) {
            strncpy(tool_name, name_start, name_end - name_start);
            tool_name[name_end - name_start] = '\0';
        }
    }
    
    // Extract arguments
    const char *args_start = strstr(ptr, "\"arguments\":");
    if (args_start) {
        args_start += 12; // Skip "\"arguments\":"
        const char *args_end = strchr(args_start, '}');
        if (args_end) {
            args_end++; // Include the closing brace
            strncpy(params_json, args_start, args_end - args_start);
            params_json[args_end - args_start] = '\0';
        }
    }
    
    // Dispatch to tool
    char *response = mcp_dispatch_tool(request_id, tool_name, params_json);
    if (response) {
        printf("%s\n", response);
        fflush(stdout);
        free(response);
    } else {
        // Error response
        printf("{\"id\":\"%s\",\"error\":\"Internal server error\"}\n", request_id);
        fflush(stdout);
    }
}

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize database connection
 */
int mcp_init() {
    const char *host = getenv("KB_AI_DB_HOST");
    const char *port = getenv("KB_AI_DB_PORT");
    const char *dbname = getenv("KB_AI_DB_NAME");
    const char *user = getenv("KB_AI_DB_USER");
    const char *password = getenv("KB_AI_DB_PASSWORD");
    
    global_db = db_connect(
        host ? host : DEFAULT_DB_HOST,
        port ? port : DEFAULT_DB_PORT,
        dbname ? dbname : DEFAULT_DB_NAME,
        user ? user : DEFAULT_DB_USER,
        password ? password : DEFAULT_DB_PASSWORD
    );
    
    if (!global_db) {
        fprintf(stderr, "Failed to connect to database\n");
        return 0;
    }
    
    return 1;
}

/**
 * Cleanup
 */
void mcp_cleanup() {
    if (global_db) {
        db_disconnect(global_db);
        global_db = NULL;
    }
}

// ============================================================================
// MCP Server Capabilities
// ============================================================================

/**
 * Send MCP server info
 */
void mcp_send_server_info() {
    printf("{\"version\":\"%s\",\"name\":\"kb.ai\",\"capabilities\":{\"tools\":[", MCP_VERSION);
    printf("{\"name\":\"kb.ai_create_project\"},");
    printf("{\"name\":\"kb.ai_list_projects\"},");
    printf("{\"name\":\"kb.ai_get_project\"},");
    printf("{\"name\":\"kb.ai_create_ticket\"},");
    printf("{\"name\":\"kb.ai_list_tickets\"},");
    printf("{\"name\":\"kb.ai_get_ticket\"},");
    printf("{\"name\":\"kb.ai_get_ticket_detailed\"},");
    printf("{\"name\":\"kb.ai_move_ticket\"},");
    printf("{\"name\":\"kb.ai_assign_ticket\"},");
    printf("{\"name\":\"kb.ai_update_ticket\"},");
    printf("{\"name\":\"kb.ai_add_task\"},");
    printf("{\"name\":\"kb.ai_complete_task\"},");
    printf("{\"name\":\"kb.ai_add_comment\"},");
    printf("{\"name\":\"kb.ai_list_comments\"}");
    printf("]}}\n");
    fflush(stdout);
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char *argv[]) {
    // Initialize
    if (!mcp_init()) {
        fprintf(stderr, "kb.ai MCP Server - Initialization failed\n");
        return EXIT_FAILURE;
    }
    
    fprintf(stderr, "kb.ai MCP Server v%s - Starting\n", MCP_VERSION);
    fprintf(stderr, "Connected to PostgreSQL database\n");
    fprintf(stderr, "Reading from STDIN, writing to STDOUT\n");
    fprintf(stderr, "Environment variables: KB_AI_DB_HOST, KB_AI_DB_PORT, KB_AI_DB_NAME, KB_AI_DB_USER, KB_AI_DB_PASSWORD\n");
    
    // Send server info
    mcp_send_server_info();
    
    // Main loop - read from stdin
    char line[8192];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        // Remove newline
        line[strcspn(line, "\n\r")] = 0;
        
        // Skip empty lines
        if (line[0] == '\0') continue;
        
        // Handle MCP request
        mcp_handle_request(line);
    }
    
    // Cleanup
    mcp_cleanup();
    fprintf(stderr, "kb.ai MCP Server - Shutting down\n");
    
    return EXIT_SUCCESS;
}
