# AI Agents Development Guide: `kb.ai`

This repository contains `kb.ai`, a database-driven kanban board plus
zettelkasten knowledge base for agentic AI workflows, exposed as an MCP
server written in C. Because the system is **database-driven**, the
business logic, state machines, and constraints live directly inside
PostgreSQL; the MCP layer stays lightweight, fast, and stateless.

## Architecture overview

1. **MCP server (the link):** a stateless stdio process (JSON-RPC 2.0)
   that exposes database operations as MCP tools. One registry entry per
   tool; the `src/kanban/` and `src/docs/` modules register themselves at
   bootstrap. Design: [docs/MCP_FRAMEWORK_DESIGN.md](docs/MCP_FRAMEWORK_DESIGN.md).
2. **PostgreSQL (the brain):** multi-project schema, per-project workflow
   graphs, and rule enforcement via triggers — illegal moves, open
   acceptance criteria at `done`, and missing note links on
   `docs_required` tickets are rejected by the database itself.
3. **Knowledge base (`src/docs/`):** atomic notes with typed links,
   full-text search, and note↔ticket relations. Design:
   [docs/adr/001-kbai-docs-postgres-zettelkasten.md](docs/adr/001-kbai-docs-postgres-zettelkasten.md).

## Core schema concepts

- **Projects & statuses** (`projects`, `board_statuses`): each project is
  an isolated board with ordered columns. A column's
  `agent_role_instruction` is a persona prompt: it tells the agent which
  role to adopt (planner, implementer, reviewer, …) for tickets in that
  column.
- **Workflows** (`status_transitions`): a directed graph of allowed moves
  per project. Moves outside the graph are rejected by a trigger.
- **Tickets, tasks, comments** (`tickets`, `ticket_tasks`,
  `ticket_comments`): tickets carry type (ticket/epic), assignee, model,
  and `docs_required`; tasks are acceptance criteria (open tasks block
  `done`); comments are the work log. Ticket relations (`parent_of`,
  `blocks`, `duplicate_of`, `relates_to`) live in their own table.
- **Notes** (`kbai_docs_*` tables, migration V7): the knowledge base —
  atomic notes with permanent slugs, typed note↔note links
  (references/contains/supersedes/contradicts), n:m project assignment,
  and note↔ticket relations.

## How to work in this repository

**Follow the binding usage rules in [skill/kbai/](skill/kbai/).** They
define the full workflow: session start protocol, duplicate checks,
assignment, tasks per acceptance criterion, work-log comments, workflow
moves, human-intervention escalation, and all knowledge-base conventions.
Do not duplicate or improvise those rules here — `skill/kbai/SKILL.md` is
the compact core, `skill/kbai/references/` the full chapters.

Repo-specific rules on top of the skill:

- Development work on kb.ai itself is tracked in the kbai project
  **"Kanban AI"** (and "kbai-docs" for the knowledge-base module) — use
  the MCP tools, not raw SQL, for all board and note operations.
- Conform strictly to the migration schemas in `migrations/` (V1..V8,
  idempotent plain SQL). Never bypass database triggers; they ARE the
  rule enforcement.
- Builds use Nix flakes: `nix develop`, `nix build`, `nix build .#static`.
- **Version coupling:** any change that adds, removes, or alters an MCP
  tool MUST update `skill/kbai/` (and the README tool table) in the same
  commit.
- Client setup and skill installation for the common agents (Claude Code,
  Gemini CLI, Codex) is documented in [docs/MCP_USAGE.md](docs/MCP_USAGE.md).
