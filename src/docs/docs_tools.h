/*
 * MCP adapter for the docs module (kbai-docs zettelkasten): registers the
 * kb.ai_docs_* tools with the framework registry.
 *
 * Design: docs/adr/001-kbai-docs-postgres-zettelkasten.md
 */

#ifndef DOCS_TOOLS_H
#define DOCS_TOOLS_H

#include "mcp/mcp.h"

void docs_register_tools(McpRegistry *r);

#endif /* DOCS_TOOLS_H */
