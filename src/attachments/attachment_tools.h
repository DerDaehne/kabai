/*
 * MCP adapter for the attachments module (V13__Attachments.sql, ticket #468):
 * registers kabai_get_attachment, kabai_add_attachment and kabai_delete_attachment
 * with the framework registry.
 *
 * Ticket-facing attachment metadata (the `attachments` array on
 * kabai_get_ticket_detailed) is implemented in src/kanban/kanban_tools.c —
 * it is a property of the ticket response, not a standalone tool, so it
 * stays with the other ticket-detail assembly code.
 */

#ifndef ATTACHMENT_TOOLS_H
#define ATTACHMENT_TOOLS_H

#include "mcp/mcp.h"

void attachments_register_tools(McpRegistry *r);

#endif /* ATTACHMENT_TOOLS_H */
