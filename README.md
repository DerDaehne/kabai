# kabai - MCP Server

[![CI](https://codeberg.org/danszek/kb.ai/actions/workflows/build.yml/badge.svg)](https://codeberg.org/danszek/kb.ai/actions)

> **Database-driven kanban board + knowledge base for agentic AI workflows**

`kabai` is an **MCP server** written in C that exposes kanban operations and
a zettelkasten-style knowledge base as MCP tools. **PostgreSQL** (libpq) is
the backend and the source of truth; builds use **Nix flakes**.

## Architecture

- **MCP server (C)**: one lightweight stdio process per session, spawned by
  the MCP client, talking directly to Postgres — no daemon. Tools carry the
  `kabai_*` prefix.
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
createdb kabai
# apply all migrations in order
for f in migrations/V*.sql; do psql -U postgres -d kabai -f "$f"; done
```

Migrations are idempotent (`IF NOT EXISTS` style); re-running is safe.

### Run the MCP server

```bash
KABAI_DB_HOST=localhost \
KABAI_DB_PORT=5432 \
KABAI_DB_NAME=kabai \
KABAI_DB_USER=postgres \
KABAI_DB_PASSWORD=yourpassword \
KABAI_AGENT_NAME=my-agent \
KABAI_AGENT_MODEL=my-model \
nix run .
```

The server reads JSON-RPC from stdin and writes to stdout (MCP over stdio).
`KABAI_AGENT_NAME`/`KABAI_AGENT_MODEL` identify the agent for ticket
assignment.

### Install the agent skill (recommended)

The repo ships a Claude-Code skill that teaches agents the binding kabai
conventions (assignment, tasks, work log, note links) — without it, agents
use the tools only partially. From a fresh system to a working setup:

1. Configure the MCP server in your client (see above / `docs/MCP_USAGE.md`).
2. Install the skill:

   ```bash
   mkdir -p ~/.claude/skills
   cp -r skill/kabai ~/.claude/skills/kabai
   ```

3. Done — the skill triggers whenever the agent works with kabai tickets or
   the knowledge base. Core rules live in
   [skill/kabai/SKILL.md](skill/kabai/SKILL.md), full chapters in
   [skill/kabai/references/](skill/kabai/references/).

The skill text is agent-neutral except for the SKILL.md frontmatter; other
MCP-capable agents can use the same files as plain instructions — see
[docs/MCP_USAGE.md](docs/MCP_USAGE.md) for per-client setup (Claude Code,
Gemini CLI, Codex, generic MCP clients), including how to get the skill
text into each agent's context.

**Version coupling (maintainer process):** the skill documents the tool
surface, so they must move together — any release that adds, removes, or
changes the semantics of an MCP tool MUST update `skill/kabai/` in the same
commit/release. Review checklist: does `tools/list` mention a tool that the
skill chapters do not?

## Project structure

```
kabai/
├── src/
│   ├── main.c              # bootstrap only: DB, registry, modules, stdio loop
│   ├── mcp/                # MCP framework: registry, dispatch, schema/param helpers
│   ├── db/                 # libpq connection + transactions
│   ├── kanban/             # kanban domain logic + MCP adapter (kanban_tools.c)
│   └── docs/               # knowledge base module (docs_tools.c)
├── migrations/             # V1..V9 plain-SQL migrations
├── skill/kabai/            # agent skill: SKILL.md + references/
├── docs/                   # design docs and ADRs
└── flake.nix
```

## MCP tools

### Projects & board
| Tool | Purpose |
|------|---------|
| `kabai_create_project` / `kabai_list_projects` / `kabai_get_project` | project management |
| `kabai_list_board_statuses` / `kabai_create_board_status` | columns incl. per-column `agent_role_instruction` |
| `kabai_list_status_transitions` / `kabai_create_status_transition` | workflow graph (DB-enforced) |

### Tickets
| Tool | Purpose |
|------|---------|
| `kabai_create_ticket` / `kabai_update_ticket` / `kabai_delete_ticket` | lifecycle; `docs_required` gates closing on a linked note |
| `kabai_list_tickets` / `kabai_search_tickets` / `kabai_get_ticket` / `kabai_get_ticket_detailed` | reading; detailed includes tasks, work log, relations, linked notes |
| `kabai_move_ticket` / `kabai_move_tickets` | workflow moves (graph + guards enforced) |
| `kabai_assign_ticket` | assignee + model from env |
| `kabai_link_tickets` / `kabai_unlink_tickets` | parent_of, blocks, duplicate_of, relates_to |
| `kabai_add_task` / `kabai_complete_task` | acceptance criteria (open tasks block done) |
| `kabai_add_comment` / `kabai_list_comments` | work log |

### Knowledge base
| Tool | Purpose |
|------|---------|
| `kabai_docs_create_note` / `kabai_docs_update_note` / `kabai_docs_archive_note` | atomic notes (kind: note/adr/hub, tags, permanent slug) |
| `kabai_docs_get_note` / `kabai_docs_list_notes` | reading incl. link neighbourhood; summary mode + body_chars |
| `kabai_docs_search` | weighted full-text search with trigram fallback |
| `kabai_docs_link_notes` / `kabai_docs_unlink_notes` | typed graph: references, contains, supersedes, contradicts |
| `kabai_docs_link_ticket` / `kabai_docs_unlink_ticket` | note↔ticket relations |
| `kabai_docs_verify_note` | staleness metadata (last verified by/at) |
| `kabai_docs_assign_project` / `kabai_docs_unassign_project` | n:m project scoping |
| `kabai_docs_suggest_for_ticket` | note suggestions on ticket pickup |

## Releases

Statically linked binaries are built for:
- Linux x86_64 / aarch64
- macOS x86_64 / arm64
- Windows (via MinGW)

Build with: `nix build .#static`
