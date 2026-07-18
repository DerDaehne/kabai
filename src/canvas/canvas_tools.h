/*
 * MCP adapter for the canvas module (V12__Canvas_Schema.sql, ticket #523):
 * registers the kabai_canvas_* / kabai_*_canvas_element / kabai_*_canvas_edge
 * tool family with the framework registry.
 */

#ifndef CANVAS_TOOLS_H
#define CANVAS_TOOLS_H

#include "mcp/mcp.h"

void canvas_register_tools(McpRegistry *r);

#endif /* CANVAS_TOOLS_H */
