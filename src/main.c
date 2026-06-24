#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <cjson/cJSON.h>
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

#define DEFAULT_DB_HOST "localhost"
#define DEFAULT_DB_PORT "5432"
#define DEFAULT_DB_NAME "kb_ai"
#define DEFAULT_DB_USER "postgres"
#define DEFAULT_DB_PASSWORD ""

static DatabaseConnection* global_db = NULL;

// ============================================================================
// JSON Helper Functions (using cJSON)
// ============================================================================

/**
 * Create a JSON response for MCP
 */
cJSON* mcp_response_json(const char *request_id, cJSON *result) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "id", request_id);
    cJSON_AddItemToObject(response, "result", result);
    return response;
}

cJSON* mcp_error_json(const char *request_id, const char *message) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "id", request_id);
    cJSON_AddStringToObject(response, "error", message);
    return response;
}

/**
 * Send JSON response to stdout
 */
void mcp_send_json(cJSON *json) {
    char *json_str = cJSON_PrintUnformatted(json);
    if (json_str) {
        printf("%s\n", json_str);
        fflush(stdout);
        cJSON_free(json_str);
    }
    cJSON_Delete(json);
}

// ============================================================================
// Database Error Handling
// ============================================================================

/**
 * Check if a PGresult indicates an error and log it
 * Returns 1 if error, 0 if OK
 */
static int db_check_and_log(PGresult *res, const char *context) {
    if (!res) {
        fprintf(stderr, "DB Error (%s): %s\n", context ? context : "unknown", "Database connection error");
        return 1;
    }
    
    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        const char *error = PQresultErrorMessage(res);
        fprintf(stderr, "DB Error (%s): %s\n", context ? context : "unknown",
                error && strlen(error) > 0 ? error : "Database operation failed");
        return 1;
    }
    return 0;
}

/**
 * Execute query with error checking and cleanup on failure
 */
PGresult* db_query_checked(DatabaseConnection *db, const char *query, const char *context) {
    if (!db || !db->conn || !query) {
        fprintf(stderr, "DB Error (%s): %s\n", context ? context : "unknown", "Invalid arguments");
        return NULL;
    }
    
    PGresult *res = PQexec(db->conn, query);
    if (db_check_and_log(res, context)) {
        if (res) PQclear(res);
        return NULL;
    }
    return res;
}

// ============================================================================
// MCP Tool: kb.ai_create_project
// ============================================================================

cJSON* mcp_tool_kb_ai_create_project(const char *request_id, cJSON *params) {
    cJSON *slug_json = cJSON_GetObjectItemCaseSensitive(params, "slug");
    cJSON *name_json = cJSON_GetObjectItemCaseSensitive(params, "name");
    cJSON *desc_json = cJSON_GetObjectItemCaseSensitive(params, "description");
    
    if (!cJSON_IsString(slug_json) || !cJSON_IsString(name_json)) {
        return mcp_error_json(request_id, "Missing required parameters: slug, name");
    }
    
    Project *project = project_create(global_db, slug_json->valuestring, name_json->valuestring,
                                       desc_json ? desc_json->valuestring : NULL);
    
    if (!project) {
        return mcp_error_json(request_id, "Failed to create project");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", project->id);
    cJSON_AddStringToObject(result, "slug", project->slug);
    cJSON_AddStringToObject(result, "name", project->name);
    
    project_free(project);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_list_projects
// ============================================================================

cJSON* mcp_tool_kb_ai_list_projects(const char *request_id) {
    Project **projects = project_list_all(global_db);
    if (!projects) {
        cJSON *result = cJSON_CreateArray();
        return mcp_response_json(request_id, result);
    }
    
    cJSON *result_array = cJSON_CreateArray();
    
    for (int i = 0; projects[i] != NULL; i++) {
        cJSON *project_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(project_json, "id", projects[i]->id);
        cJSON_AddStringToObject(project_json, "slug", projects[i]->slug);
        cJSON_AddStringToObject(project_json, "name", projects[i]->name);
        if (projects[i]->description) {
            cJSON_AddStringToObject(project_json, "description", projects[i]->description);
        }
        cJSON_AddItemToArray(result_array, project_json);
    }
    
    project_free_array(projects);
    return mcp_response_json(request_id, result_array);
}

// ============================================================================
// MCP Tool: kb.ai_get_project
// ============================================================================

cJSON* mcp_tool_kb_ai_get_project(const char *request_id, cJSON *params) {
    cJSON *id_json = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    
    if (!cJSON_IsNumber(id_json)) {
        return mcp_error_json(request_id, "Missing required parameter: project_id");
    }
    
    Project *project = project_get_by_id(global_db, (int)id_json->valueint);
    if (!project) {
        return mcp_error_json(request_id, "Project not found");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", project->id);
    cJSON_AddStringToObject(result, "slug", project->slug);
    cJSON_AddStringToObject(result, "name", project->name);
    if (project->description) {
        cJSON_AddStringToObject(result, "description", project->description);
    }
    
    project_free(project);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_create_ticket
// ============================================================================

cJSON* mcp_tool_kb_ai_create_ticket(const char *request_id, cJSON *params) {
    cJSON *project_id_json = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    cJSON *status_id_json = cJSON_GetObjectItemCaseSensitive(params, "status_id");
    cJSON *title_json = cJSON_GetObjectItemCaseSensitive(params, "title");
    cJSON *desc_json = cJSON_GetObjectItemCaseSensitive(params, "description");
    
    if (!cJSON_IsNumber(project_id_json) || !cJSON_IsNumber(status_id_json) || !cJSON_IsString(title_json)) {
        return mcp_error_json(request_id, "Missing required parameters: project_id, status_id, title");
    }
    
    Ticket *ticket = ticket_create(global_db, (int)project_id_json->valueint,
                                   (int)status_id_json->valueint,
                                   title_json->valuestring,
                                   desc_json ? desc_json->valuestring : NULL);
    
    if (!ticket) {
        return mcp_error_json(request_id, "Failed to create ticket");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", ticket->id);
    cJSON_AddNumberToObject(result, "project_id", ticket->project_id);
    cJSON_AddNumberToObject(result, "status_id", ticket->status_id);
    cJSON_AddStringToObject(result, "title", ticket->title);
    if (ticket->description) {
        cJSON_AddStringToObject(result, "description", ticket->description);
    }
    if (ticket->assignee) {
        cJSON_AddStringToObject(result, "assignee", ticket->assignee);
    }
    
    ticket_free(ticket);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_list_tickets
// ============================================================================

cJSON* mcp_tool_kb_ai_list_tickets(const char *request_id, cJSON *params) {
    cJSON *project_id_json = cJSON_GetObjectItemCaseSensitive(params, "project_id");
    
    int project_id = 0;
    if (cJSON_IsNumber(project_id_json)) {
        project_id = (int)project_id_json->valueint;
    }
    
    Ticket **tickets = ticket_list_by_project(global_db, project_id);
    if (!tickets) {
        cJSON *result = cJSON_CreateArray();
        return mcp_response_json(request_id, result);
    }
    
    cJSON *result_array = cJSON_CreateArray();
    
    for (int i = 0; tickets[i] != NULL; i++) {
        cJSON *ticket_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(ticket_json, "id", tickets[i]->id);
        cJSON_AddNumberToObject(ticket_json, "project_id", tickets[i]->project_id);
        cJSON_AddNumberToObject(ticket_json, "status_id", tickets[i]->status_id);
        cJSON_AddStringToObject(ticket_json, "title", tickets[i]->title);
        if (tickets[i]->assignee) {
            cJSON_AddStringToObject(ticket_json, "assignee", tickets[i]->assignee);
        }
        cJSON_AddItemToArray(result_array, ticket_json);
    }
    
    ticket_free_array(tickets);
    return mcp_response_json(request_id, result_array);
}

// ============================================================================
// MCP Tool: kb.ai_get_ticket
// ============================================================================

cJSON* mcp_tool_kb_ai_get_ticket(const char *request_id, cJSON *params) {
    cJSON *id_json = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    
    if (!cJSON_IsNumber(id_json)) {
        return mcp_error_json(request_id, "Missing required parameter: ticket_id");
    }
    
    Ticket *ticket = ticket_get_by_id(global_db, (int)id_json->valueint);
    if (!ticket) {
        return mcp_error_json(request_id, "Ticket not found");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", ticket->id);
    cJSON_AddNumberToObject(result, "project_id", ticket->project_id);
    cJSON_AddNumberToObject(result, "status_id", ticket->status_id);
    cJSON_AddStringToObject(result, "title", ticket->title);
    if (ticket->description) {
        cJSON_AddStringToObject(result, "description", ticket->description);
    }
    if (ticket->assignee) {
        cJSON_AddStringToObject(result, "assignee", ticket->assignee);
    }
    
    ticket_free(ticket);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_get_ticket_detailed
// ============================================================================

cJSON* mcp_tool_kb_ai_get_ticket_detailed(const char *request_id, cJSON *params) {
    cJSON *id_json = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    
    if (!cJSON_IsNumber(id_json)) {
        return mcp_error_json(request_id, "Missing required parameter: ticket_id");
    }
    
    TicketDetailed *detailed = ticket_get_detailed(global_db, (int)id_json->valueint);
    if (!detailed) {
        return mcp_error_json(request_id, "Ticket not found");
    }
    
    // Build result
    cJSON *result = cJSON_CreateObject();
    
    // Ticket info
    cJSON *ticket_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(ticket_json, "id", detailed->ticket->id);
    cJSON_AddNumberToObject(ticket_json, "project_id", detailed->ticket->project_id);
    cJSON_AddNumberToObject(ticket_json, "status_id", detailed->ticket->status_id);
    cJSON_AddStringToObject(ticket_json, "title", detailed->ticket->title);
    if (detailed->ticket->description) {
        cJSON_AddStringToObject(ticket_json, "description", detailed->ticket->description);
    }
    if (detailed->ticket->assignee) {
        cJSON_AddStringToObject(ticket_json, "assignee", detailed->ticket->assignee);
    }
    cJSON_AddItemToObject(result, "ticket", ticket_json);
    
    // Tasks
    cJSON *tasks_array = cJSON_CreateArray();
    if (detailed->tasks) {
        for (int i = 0; detailed->tasks[i] != NULL; i++) {
            cJSON *task_json = cJSON_CreateObject();
            cJSON_AddNumberToObject(task_json, "id", detailed->tasks[i]->id);
            cJSON_AddStringToObject(task_json, "title", detailed->tasks[i]->title);
            cJSON_AddBoolToObject(task_json, "is_completed", detailed->tasks[i]->is_completed);
            cJSON_AddItemToArray(tasks_array, task_json);
        }
    }
    cJSON_AddItemToObject(result, "tasks", tasks_array);
    
    // Comments (Work Log)
    cJSON *comments_array = cJSON_CreateArray();
    if (detailed->comments) {
        for (int i = 0; detailed->comments[i] != NULL; i++) {
            cJSON *comment_json = cJSON_CreateObject();
            cJSON_AddNumberToObject(comment_json, "id", detailed->comments[i]->id);
            cJSON_AddStringToObject(comment_json, "author", detailed->comments[i]->author);
            cJSON_AddStringToObject(comment_json, "text", detailed->comments[i]->comment_text);
            cJSON_AddItemToArray(comments_array, comment_json);
        }
    }
    cJSON_AddItemToObject(result, "comments", comments_array);
    
    ticket_detailed_free(detailed);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_move_ticket
// ============================================================================

cJSON* mcp_tool_kb_ai_move_ticket(const char *request_id, cJSON *params) {
    cJSON *ticket_id_json = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *status_id_json = cJSON_GetObjectItemCaseSensitive(params, "new_status_id");
    
    if (!cJSON_IsNumber(ticket_id_json) || !cJSON_IsNumber(status_id_json)) {
        return mcp_error_json(request_id, "Missing required parameters: ticket_id, new_status_id");
    }
    
    // Check if transition is valid by attempting the update
    // The database trigger will reject invalid transitions
    if (!ticket_update_status(global_db, (int)ticket_id_json->valueint, (int)status_id_json->valueint)) {
        // Save error BEFORE running any other query (PQerrorMessage is per-connection)
        const char *raw_error = PQerrorMessage(global_db->conn);
        
        if (raw_error && strstr(raw_error, "Illegaler Kanban-Move")) {
            return mcp_error_json(request_id, "Invalid ticket transition: Check workflow rules");
        } else if (raw_error && strstr(raw_error, "Akzeptanzkriterium")) {
            return mcp_error_json(request_id, "Cannot close ticket: Open tasks remain");
        }
        return mcp_error_json(request_id, "Failed to move ticket");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", true);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_assign_ticket
// ============================================================================

cJSON* mcp_tool_kb_ai_assign_ticket(const char *request_id, cJSON *params) {
    cJSON *ticket_id_json = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *assignee_json = cJSON_GetObjectItemCaseSensitive(params, "assignee");
    
    if (!cJSON_IsNumber(ticket_id_json) || !cJSON_IsString(assignee_json)) {
        return mcp_error_json(request_id, "Missing required parameters: ticket_id, assignee");
    }
    
    if (!ticket_assign(global_db, (int)ticket_id_json->valueint, assignee_json->valuestring)) {
        return mcp_error_json(request_id, "Failed to assign ticket");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", true);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_update_ticket
// ============================================================================

cJSON* mcp_tool_kb_ai_update_ticket(const char *request_id, cJSON *params) {
    cJSON *ticket_id_json = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *title_json = cJSON_GetObjectItemCaseSensitive(params, "title");
    cJSON *desc_json = cJSON_GetObjectItemCaseSensitive(params, "description");
    
    if (!cJSON_IsNumber(ticket_id_json)) {
        return mcp_error_json(request_id, "Missing required parameter: ticket_id");
    }
    
    int ticket_id = (int)ticket_id_json->valueint;
    int updated = 0;
    
    if (title_json && cJSON_IsString(title_json)) {
        updated += ticket_update_title(global_db, ticket_id, title_json->valuestring);
    }
    
    if (desc_json) {
        // description can be null (empty string or explicit null)
        const char *new_desc = cJSON_IsNull(desc_json) ? NULL : desc_json->valuestring;
        updated += ticket_update_description(global_db, ticket_id, new_desc);
    }
    
    if (updated > 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "success", true);
        cJSON_AddNumberToObject(result, "updated_fields", updated);
        return mcp_response_json(request_id, result);
    }
    
    return mcp_error_json(request_id, "No fields to update");
}

// ============================================================================
// MCP Tool: kb.ai_add_task
// ============================================================================

cJSON* mcp_tool_kb_ai_add_task(const char *request_id, cJSON *params) {
    cJSON *ticket_id_json = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *title_json = cJSON_GetObjectItemCaseSensitive(params, "title");
    
    if (!cJSON_IsNumber(ticket_id_json) || !cJSON_IsString(title_json)) {
        return mcp_error_json(request_id, "Missing required parameters: ticket_id, title");
    }
    
    TicketTask *task = ticket_add_task(global_db, (int)ticket_id_json->valueint, title_json->valuestring);
    if (!task) {
        return mcp_error_json(request_id, "Failed to add task");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", task->id);
    cJSON_AddNumberToObject(result, "ticket_id", task->ticket_id);
    cJSON_AddStringToObject(result, "title", task->title);
    cJSON_AddBoolToObject(result, "is_completed", task->is_completed);
    
    ticket_task_free(task);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_complete_task
// ============================================================================

cJSON* mcp_tool_kb_ai_complete_task(const char *request_id, cJSON *params) {
    cJSON *task_id_json = cJSON_GetObjectItemCaseSensitive(params, "task_id");
    
    if (!cJSON_IsNumber(task_id_json)) {
        return mcp_error_json(request_id, "Missing required parameter: task_id");
    }
    
    if (!ticket_complete_task(global_db, (int)task_id_json->valueint)) {
        return mcp_error_json(request_id, "Failed to complete task");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", true);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_add_comment
// ============================================================================

cJSON* mcp_tool_kb_ai_add_comment(const char *request_id, cJSON *params) {
    cJSON *ticket_id_json = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    cJSON *author_json = cJSON_GetObjectItemCaseSensitive(params, "author");
    cJSON *text_json = cJSON_GetObjectItemCaseSensitive(params, "text");
    
    if (!cJSON_IsNumber(ticket_id_json) || !cJSON_IsString(author_json) || !cJSON_IsString(text_json)) {
        return mcp_error_json(request_id, "Missing required parameters: ticket_id, author, text");
    }
    
    TicketComment *comment = comment_add(global_db, (int)ticket_id_json->valueint,
                                          author_json->valuestring, text_json->valuestring);
    if (!comment) {
        return mcp_error_json(request_id, "Failed to add comment");
    }
    
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", comment->id);
    cJSON_AddNumberToObject(result, "ticket_id", comment->ticket_id);
    cJSON_AddStringToObject(result, "author", comment->author);
    cJSON_AddStringToObject(result, "text", comment->comment_text);
    
    comment_free(comment);
    return mcp_response_json(request_id, result);
}

// ============================================================================
// MCP Tool: kb.ai_list_comments
// ============================================================================

cJSON* mcp_tool_kb_ai_list_comments(const char *request_id, cJSON *params) {
    cJSON *ticket_id_json = cJSON_GetObjectItemCaseSensitive(params, "ticket_id");
    
    if (!cJSON_IsNumber(ticket_id_json)) {
        return mcp_error_json(request_id, "Missing required parameter: ticket_id");
    }
    
    TicketComment **comments = comment_list_by_ticket(global_db, (int)ticket_id_json->valueint);
    if (!comments) {
        cJSON *result = cJSON_CreateArray();
        return mcp_response_json(request_id, result);
    }
    
    cJSON *result_array = cJSON_CreateArray();
    
    for (int i = 0; comments[i] != NULL; i++) {
        cJSON *comment_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(comment_json, "id", comments[i]->id);
        cJSON_AddNumberToObject(comment_json, "ticket_id", comments[i]->ticket_id);
        cJSON_AddStringToObject(comment_json, "author", comments[i]->author);
        cJSON_AddStringToObject(comment_json, "text", comments[i]->comment_text);
        cJSON_AddItemToArray(result_array, comment_json);
    }
    
    comment_free_array(comments);
    return mcp_response_json(request_id, result_array);
}

// ============================================================================
// MCP Tool Dispatcher
// ============================================================================

cJSON* mcp_dispatch_tool(const char *request_id, const char *tool_name, cJSON *params) {
    if (strcmp(tool_name, "kb.ai_create_project") == 0) {
        return mcp_tool_kb_ai_create_project(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_list_projects") == 0) {
        return mcp_tool_kb_ai_list_projects(request_id);
    } else if (strcmp(tool_name, "kb.ai_get_project") == 0) {
        return mcp_tool_kb_ai_get_project(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_create_ticket") == 0) {
        return mcp_tool_kb_ai_create_ticket(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_list_tickets") == 0) {
        return mcp_tool_kb_ai_list_tickets(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_get_ticket") == 0) {
        return mcp_tool_kb_ai_get_ticket(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_get_ticket_detailed") == 0) {
        return mcp_tool_kb_ai_get_ticket_detailed(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_move_ticket") == 0) {
        return mcp_tool_kb_ai_move_ticket(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_assign_ticket") == 0) {
        return mcp_tool_kb_ai_assign_ticket(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_update_ticket") == 0) {
        return mcp_tool_kb_ai_update_ticket(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_add_task") == 0) {
        return mcp_tool_kb_ai_add_task(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_complete_task") == 0) {
        return mcp_tool_kb_ai_complete_task(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_add_comment") == 0) {
        return mcp_tool_kb_ai_add_comment(request_id, params);
    } else if (strcmp(tool_name, "kb.ai_list_comments") == 0) {
        return mcp_tool_kb_ai_list_comments(request_id, params);
    } else {
        return mcp_error_json(request_id, "Unknown tool");
    }
}

// ============================================================================
// MCP Protocol Handler
// ============================================================================

/**
 * Handles an MCP request from stdin
 * Expected format: {"id": "req-1", "method": "tools/call", "params": {"name": "kb.ai_tool", "arguments": {...}}}
 */
cJSON* mcp_handle_request(const char *request_json) {
    cJSON *request = cJSON_Parse(request_json);
    if (!request) {
        // Invalid JSON
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Invalid JSON");
        return error;
    }
    
    // Check for server info request
    cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
    if (method && cJSON_IsString(method) && strcmp(method->valuestring, "server_info") == 0) {
        cJSON_Delete(request);
        return NULL; // Will be handled separately
    }
    
    // Extract request_id
    cJSON *id_json = cJSON_GetObjectItemCaseSensitive(request, "id");
    const char *request_id = id_json && cJSON_IsString(id_json) ? id_json->valuestring : "unknown";
    
    // Extract tool name and arguments
    cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
    if (!params) {
        cJSON_Delete(request);
        return mcp_error_json(request_id, "Missing params");
    }
    
    cJSON *name_json = cJSON_GetObjectItemCaseSensitive(params, "name");
    cJSON *args_json = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    
    if (!name_json || !cJSON_IsString(name_json)) {
        cJSON_Delete(request);
        return mcp_error_json(request_id, "Missing tool name");
    }
    
    const char *tool_name = name_json->valuestring;
    cJSON *tool_params = args_json ? cJSON_Duplicate(args_json, 1) : cJSON_CreateObject();
    
    cJSON_Delete(request);
    
    // Dispatch to tool
    cJSON *response = mcp_dispatch_tool(request_id, tool_name, tool_params);
    if (tool_params) {
        cJSON_Delete(tool_params);
    }
    
    return response;
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
        const char *error = PQerrorMessage(NULL);
        fprintf(stderr, "Failed to connect to database: %s\n", error ? error : "unknown error");
        return 0;
    }
    
    fprintf(stderr, "Connected to PostgreSQL database at %s:%s/%s\n",
            host ? host : DEFAULT_DB_HOST,
            port ? port : DEFAULT_DB_PORT,
            dbname ? dbname : DEFAULT_DB_NAME);
    
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
    cJSON *server_info = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info, "version", MCP_VERSION);
    cJSON_AddStringToObject(server_info, "name", "kb.ai");
    
    cJSON *tools_array = cJSON_CreateArray();
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_create_project"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_list_projects"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_get_project"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_create_ticket"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_list_tickets"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_get_ticket"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_get_ticket_detailed"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_move_ticket"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_assign_ticket"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_update_ticket"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_add_task"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_complete_task"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_add_comment"));
    cJSON_AddItemToArray(tools_array, cJSON_CreateString("kb.ai_list_comments"));
    
    cJSON_AddItemToObject(server_info, "tools", tools_array);
    
    // Send as notification
    char *json_str = cJSON_PrintUnformatted(server_info);
    if (json_str) {
        printf("%%[server_info] %s\n", json_str);
        fflush(stdout);
        cJSON_free(json_str);
    }
    cJSON_Delete(server_info);
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char *argv[]) {
    // Initialize cJSON hook for malloc/free
    cJSON_Hooks hooks = {malloc, free};
    cJSON_InitHooks(&hooks);
    
    // Initialize
    if (!mcp_init()) {
        fprintf(stderr, "kb.ai MCP Server v%s - Initialization failed\n", MCP_VERSION);
        return EXIT_FAILURE;
    }
    
    fprintf(stderr, "kb.ai MCP Server v%s - Starting\n", MCP_VERSION);
    fprintf(stderr, "Reading from STDIN, writing to STDOUT\n");
    fprintf(stderr, "Environment variables: KB_AI_DB_HOST, KB_AI_DB_PORT, KB_AI_DB_NAME, KB_AI_DB_USER, KB_AI_DB_PASSWORD\n");
    
    // Send server info
    mcp_send_server_info();
    
    // Main loop - read from stdin
    char line[16384];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        // Remove newline
        line[strcspn(line, "\n\r")] = 0;
        
        // Skip empty lines
        if (line[0] == '\0') continue;
        
        // Handle MCP request
        cJSON *response = mcp_handle_request(line);
        if (response) {
            mcp_send_json(response);
        }
    }
    
    // Cleanup
    mcp_cleanup();
    fprintf(stderr, "kb.ai MCP Server - Shutting down\n");
    
    return EXIT_SUCCESS;
}
