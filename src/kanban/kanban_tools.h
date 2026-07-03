/*
 * MCP adapter for the kanban module: registers all kanban tools
 * (handlers + schemas) with the framework registry.
 */

#ifndef KANBAN_TOOLS_H
#define KANBAN_TOOLS_H

#include "mcp/mcp.h"

void kanban_register_tools(McpRegistry *r);

#endif /* KANBAN_TOOLS_H */
