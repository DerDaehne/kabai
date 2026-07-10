# kb.ai - MCP Server

[![CI](https://codeberg.org/danszek/kb.ai/actions/workflows/build.yml/badge.svg)](https://codeberg.org/danszek/kb.ai/actions)

> **Database-driven kanban board + knowledge base for agentic AI workflows**

`kb.ai` is an **MCP server** written in C that exposes kanban operations and
a zettelkasten-style knowledge base as MCP tools. **PostgreSQL** (libpq) is
the backend and the source of truth; builds use **Nix flakes**.

## Architecture

- **MCP server (C)**: one lightweight stdio process per session, spawned by
  the MCP client, talking directly to Postgres — no daemon. Tools carry the
  `kb.ai_*` prefix (clients normalise to `kb_ai_*`).
- **PostgreSQL**: multi-project schema, workflow graphs, and rule
  enforcement via triggers (illegal moves, open acceptance criteria,
  docs_required gating are rejected by the database itself).
- **Tool framework** (`src/mcp/`): one registry entry per tool; modules
  (`src/kanban/`, `src/docs/`) register themselves. Design:
  [docs/MCP_FRAMEWORK_DESIGN.md](docs/MCP_FRAMEWORK_DESIGN.md).
- **Knowledge base** (`src/docs/`): atomic notes with typed links, full-text
  search, and note↔ticket relations. Design:
  [docs/adr/001-kbai-docs-postgres-zettelkasten.md](docs/adr/001-kbai-docs-postgres-zettelkasten.md).

## Prerequisites

- **Nix** with flakes support (`nix --experimental-features 'nix-command flakes'`)
- PostgreSQL 14+

## Quick start

### Build

```bash
nix develop      # dev shell with all dependencies
nix build        # standard (dynamically linked) build
nix build .#static   # statically linked release binary
```

### Create the database schema

```bash
createdb kb_ai
# apply all migrations in order
for f in migrations/V*.sql; do psql -U postgres -d kb_ai -f "$f"; done
```

Migrations are idempotent (`IF NOT EXISTS` style); re-running is safe.

### Run the MCP server

```bash
KB_AI_DB_HOST=localhost \
KB_AI_DB_PORT=5432 \
KB_AI_DB_NAME=kb_ai \
KB_AI_DB_USER=postgres \
KB_AI_DB_PASSWORD=yourpassword \
KB_AI_AGENT_NAME=my-agent \
KB_AI_AGENT_MODEL=my-model \
nix run .
```

The server reads JSON-RPC from stdin and writes to stdout (MCP over stdio).
`KB_AI_AGENT_NAME`/`KB_AI_AGENT_MODEL` identify the agent for ticket
assignment.

### Install the agent skill (recommended)

The repo ships a Claude-Code skill that teaches agents the binding kbai
conventions (assignment, tasks, work log, note links) — without it, agents
use the tools only partially. From a fresh system to a working setup:

1. Configure the MCP server in your client (see above / `docs/MCP_USAGE.md`).
2. Install the skill:

   ```bash
   mkdir -p ~/.claude/skills
   cp -r skill/kbai ~/.claude/skills/kbai
   ```

3. Done — the skill triggers whenever the agent works with kbai tickets or
   the knowledge base. Core rules live in
   [skill/kbai/SKILL.md](skill/kbai/SKILL.md), full chapters in
   [skill/kbai/references/](skill/kbai/references/).

The skill text is agent-neutral except for the SKILL.md frontmatter; other
MCP-capable agents can use the same files as plain instructions — see
[docs/MCP_USAGE.md](docs/MCP_USAGE.md) for per-client setup (Claude Code,
Gemini CLI, Codex, generic MCP clients), including how to get the skill
text into each agent's context.

**Version coupling (maintainer process):** the skill documents the tool
surface, so they must move together — any release that adds, removes, or
changes the semantics of an MCP tool MUST update `skill/kbai/` in the same
commit/release. Review checklist: does `tools/list` mention a tool that the
skill chapters do not?

## Project structure

```
kb.ai/
├── src/
│   ├── main.c              # bootstrap only: DB, registry, modules, stdio loop
│   ├── mcp/                # MCP framework: registry, dispatch, schema/param helpers
│   ├── db/                 # libpq connection + transactions
│   ├── kanban/             # kanban domain logic + MCP adapter (kanban_tools.c)
│   └── docs/               # knowledge base module (docs_tools.c)
├── migrations/             # V1..V8 plain-SQL migrations
├── skill/kbai/             # agent skill: SKILL.md + references/
├── docs/                   # design docs and ADRs
└── flake.nix
```

## MCP tools

### Projects & board
| Tool | Purpose |
|------|---------|
| `kb.ai_create_project` / `kb.ai_list_projects` / `kb.ai_get_project` | project management |
| `kb.ai_list_board_statuses` / `kb.ai_create_board_status` | columns incl. per-column `agent_role_instruction` |
| `kb.ai_list_status_transitions` / `kb.ai_create_status_transition` | workflow graph (DB-enforced) |

### Tickets
| Tool | Purpose |
|------|---------|
| `kb.ai_create_ticket` / `kb.ai_update_ticket` / `kb.ai_delete_ticket` | lifecycle; `docs_required` gates closing on a linked note |
| `kb.ai_list_tickets` / `kb.ai_search_tickets` / `kb.ai_get_ticket` / `kb.ai_get_ticket_detailed` | reading; detailed includes tasks, work log, relations, linked notes |
| `kb.ai_move_ticket` / `kb.ai_move_tickets` | workflow moves (graph + guards enforced) |
| `kb.ai_assign_ticket` | assignee + model from env |
| `kb.ai_link_tickets` / `kb.ai_unlink_tickets` | parent_of, blocks, duplicate_of, relates_to |
| `kb.ai_add_task` / `kb.ai_complete_task` | acceptance criteria (open tasks block done) |
| `kb.ai_add_comment` / `kb.ai_list_comments` | work log |

### Knowledge base
| Tool | Purpose |
|------|---------|
| `kb.ai_docs_create_note` / `kb.ai_docs_update_note` / `kb.ai_docs_archive_note` | atomic notes (kind: note/adr/hub, tags, permanent slug) |
| `kb.ai_docs_get_note` / `kb.ai_docs_list_notes` | reading incl. link neighbourhood; summary mode + body_chars |
| `kb.ai_docs_search` | weighted full-text search with trigram fallback |
| `kb.ai_docs_link_notes` / `kb.ai_docs_unlink_notes` | typed graph: references, contains, supersedes, contradicts |
| `kb.ai_docs_link_ticket` / `kb.ai_docs_unlink_ticket` | note↔ticket relations |
| `kb.ai_docs_verify_note` | staleness metadata (last verified by/at) |
| `kb.ai_docs_assign_project` / `kb.ai_docs_unassign_project` | n:m project scoping |
| `kb.ai_docs_suggest_for_ticket` | note suggestions on ticket pickup |

## Releases

Statically linked binaries are built for:
- Linux x86_64 / aarch64
- macOS x86_64 / arm64
- Windows (via MinGW)

Build with: `nix build .#static`
