# kb.ai MCP server — setup and usage

`kb.ai` is an MCP (Model Context Protocol) server that exposes a kanban
board and a zettelkasten-style knowledge base as tools, backed by
PostgreSQL. This document covers installation, database setup, environment
variables, and per-client configuration for the most common AI agents.

The **binding usage rules** for agents (workflow conventions, mandatory
fields, knowledge-base linking) live in [`skill/kbai/`](../skill/kbai/) —
install them alongside the server (see [Agent skill](#agent-skill-required-for-good-results)).

## Installation

```bash
nix profile install codeberg:danszek/kb.ai
```

Or build from source:

```bash
git clone https://codeberg.org/danszek/kb.ai.git
cd kb.ai
nix build             # dynamically linked
nix build .#static    # statically linked release binary
```

The resulting binary (`kbai`) speaks MCP over stdio: it reads JSON-RPC 2.0
from stdin and writes responses to stdout. One process is spawned per
session by the MCP client; there is no daemon.

## Database setup

1. Ensure PostgreSQL 14+ is running.
2. Create the database and apply **all** migrations in order:

```bash
createdb kb_ai
for f in migrations/V*.sql; do psql -U postgres -d kb_ai -f "$f"; done
```

Migrations are idempotent (`IF NOT EXISTS` style); re-running is safe.
Current range: V1–V8 (kanban schema, ticket relations/epics,
human-intervention statuses, knowledge-base notes, docs_required guard).

3. Create projects, board columns, and workflow transitions via the MCP
   tools (`kb.ai_create_project`, `kb.ai_create_board_status`,
   `kb.ai_create_status_transition`) — or directly in SQL if you are
   setting up as a human administrator.

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `KB_AI_DB_HOST` | `localhost` | PostgreSQL host |
| `KB_AI_DB_PORT` | `5432` | PostgreSQL port |
| `KB_AI_DB_NAME` | `kb_ai` | Database name |
| `KB_AI_DB_USER` | `postgres` | Database user |
| `KB_AI_DB_PASSWORD` | *(empty)* | Database password |
| `KB_AI_AGENT_NAME` | *(unset)* | Agent identity for `kb.ai_assign_ticket` — used as the default assignee |
| `KB_AI_AGENT_MODEL` | *(unset)* | Model identifier — written to the ticket's `model` field on assignment |

**Set `KB_AI_AGENT_NAME` and `KB_AI_AGENT_MODEL`.** Without them,
`kb.ai_assign_ticket` fails unless the agent passes an explicit `assignee`
parameter on every call. Use one name per agent/client installation
(e.g. `claude-code-laptop`, `gemini-ci`).

## Client configuration

The server command is the same everywhere: run the `kbai` binary (or
`nix run codeberg:danszek/kb.ai`) with the environment variables above.
Only the config file format differs per client.

> **Tool names:** the server registers tools as `kb.ai_*`. Clients may
> normalise the dot (`kb_ai_*`) and/or prefix the server alias
> (e.g. `kbai__kb_ai_list_projects`). Agents should match tools by name
> suffix; the skill text explains this to them.

### Claude Code

Project-scoped, via `.mcp.json` in the repo root (or user-scoped with
`claude mcp add --scope user`):

```json
{
  "mcpServers": {
    "kbai": {
      "command": "/path/to/kbai",
      "env": {
        "KB_AI_DB_HOST": "localhost",
        "KB_AI_DB_NAME": "kb_ai",
        "KB_AI_DB_USER": "postgres",
        "KB_AI_DB_PASSWORD": "secret",
        "KB_AI_AGENT_NAME": "my-agent",
        "KB_AI_AGENT_MODEL": "claude-sonnet-5"
      }
    }
  }
}
```

Skill install (teaches the binding conventions, auto-triggers on kbai
work):

```bash
mkdir -p ~/.claude/skills
cp -r skill/kbai ~/.claude/skills/kbai
```

### Gemini CLI

MCP servers go into `~/.gemini/settings.json` (user) or
`.gemini/settings.json` (project):

```json
{
  "mcpServers": {
    "kbai": {
      "command": "/path/to/kbai",
      "env": {
        "KB_AI_DB_HOST": "localhost",
        "KB_AI_DB_NAME": "kb_ai",
        "KB_AI_DB_USER": "postgres",
        "KB_AI_DB_PASSWORD": "secret",
        "KB_AI_AGENT_NAME": "my-gemini-agent",
        "KB_AI_AGENT_MODEL": "gemini-2.5-pro"
      }
    }
  }
}
```

Verify with `/mcp` inside Gemini CLI — the server and its tools must be
listed.

Gemini has no skill mechanism; put the rules into the context file
instead. Append the three skill files to your global or project
`GEMINI.md`:

```bash
cat skill/kbai/SKILL.md \
    skill/kbai/references/ticket-workflow.md \
    skill/kbai/references/docs-zettelkasten.md >> ~/.gemini/GEMINI.md
```

(The frontmatter block at the top of SKILL.md is Claude-specific metadata;
it is harmless in a context file, but you can strip it.)

### Codex CLI

MCP servers are configured in `~/.codex/config.toml`:

```toml
[mcp_servers.kbai]
command = "/path/to/kbai"

[mcp_servers.kbai.env]
KB_AI_DB_HOST = "localhost"
KB_AI_DB_NAME = "kb_ai"
KB_AI_DB_USER = "postgres"
KB_AI_DB_PASSWORD = "secret"
KB_AI_AGENT_NAME = "my-codex-agent"
KB_AI_AGENT_MODEL = "gpt-5"
```

Codex reads `AGENTS.md` as its instruction file. Append the skill files to
your global `~/.codex/AGENTS.md` or the project `AGENTS.md`:

```bash
cat skill/kbai/SKILL.md \
    skill/kbai/references/ticket-workflow.md \
    skill/kbai/references/docs-zettelkasten.md >> ~/.codex/AGENTS.md
```

### Any other MCP client

`kb.ai` is a standard stdio MCP server (JSON-RPC 2.0, `initialize`,
`tools/list`, `tools/call`). Any client that can spawn a command with
environment variables can use it — configure the `kbai` binary as a stdio
server and inject the skill files through whatever instruction/context
mechanism the client offers (rules file, system prompt, project context).

## Agent skill (required for good results)

The tools alone do not make an agent follow the workflow conventions
(assign before working, tasks per acceptance criterion, work-log comments,
note linking). Field testing showed that agents without the rules in
context ignore the tools or use them partially — one tested agent even
tried to bypass the MCP server and query PostgreSQL directly.

Always install the skill text into the agent's context, as shown per
client above. The files in [`skill/kbai/`](../skill/kbai/) are
agent-neutral: `SKILL.md` is the compact core, `references/` holds the two
full chapters (ticket workflow, knowledge base). They work as separate
files or concatenated.

## Tools overview

41 tools in two families. The authoritative reference is the server's own
`tools/list` response (every tool carries a full JSON schema with
parameter descriptions); the skill chapters document the usage rules. Do
not rely on third-party summaries of parameters — query `tools/list`.

| Group | Tools |
|-------|-------|
| Projects | `create_project`, `update_project`, `list_projects`, `get_project` |
| Board setup | `list_board_statuses`, `create_board_status`, `update_board_status`, `list_status_transitions`, `create_status_transition` |
| Tickets | `create_ticket`, `update_ticket`, `delete_ticket`, `list_tickets`, `search_tickets`, `get_ticket`, `get_ticket_detailed`, `move_ticket`, `move_tickets`, `assign_ticket`, `link_tickets`, `unlink_tickets` |
| Tasks & work log | `add_task`, `update_task`, `complete_task`, `delete_task`, `add_comment`, `list_comments` |
| Knowledge base | `docs_create_note`, `docs_update_note`, `docs_archive_note`, `docs_get_note`, `docs_list_notes`, `docs_search`, `docs_link_notes`, `docs_unlink_notes`, `docs_link_ticket`, `docs_unlink_ticket`, `docs_verify_note`, `docs_suggest_for_ticket`, `docs_assign_project`, `docs_unassign_project` |

(All names carry the `kb.ai_` prefix.)

Rule enforcement lives in PostgreSQL triggers, not in the server: illegal
workflow moves, moving to `done` with open tasks, and closing a
`docs_required` ticket without a linked note are rejected by the database
with a descriptive error message.

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| "Connection refused" | PostgreSQL not running or wrong `KB_AI_DB_*` values |
| "Illegal Kanban-Move" | Transition not in the project's workflow graph — read `kb.ai_list_status_transitions` first |
| "Open acceptance criteria" | Ticket has incomplete tasks; complete them before moving to `done` |
| Move to `done` rejected with docs message | Ticket has `docs_required: true` and no linked note — link one via `kb.ai_docs_link_ticket` |
| "Missing assignee" on `assign_ticket` | `KB_AI_AGENT_NAME` not set in the server env — set it (and `KB_AI_AGENT_MODEL`) or pass `assignee` explicitly |
| "Project/Status not found" | Wrong `project_id`/`status_id` — status IDs are per project, discover them via `kb.ai_list_board_statuses` |
| Agent sees the tools but never calls them | Skill/rules text not in the agent's context — install it per the client sections above |
| Agent claims a tool does not exist | Tool-name prefix variance — the agent must match by suffix (the skill explains this); verify the server is listed in the client's MCP status view |
