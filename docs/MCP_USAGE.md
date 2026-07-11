# kabai MCP server — setup and usage

`kabai` is an MCP (Model Context Protocol) server that exposes a kanban
board and a zettelkasten-style knowledge base as tools, backed by
PostgreSQL. This document covers installation, database setup, environment
variables, and per-client configuration for the most common AI agents.

The **binding usage rules** for agents (workflow conventions, mandatory
fields, knowledge-base linking) live in [`skill/kabai/`](../skill/kabai/) —
install them alongside the server (see [Agent skill](#agent-skill-required-for-good-results)).

## Installation

```bash
nix profile install git+https://codeberg.org/danszek/kb.ai.git
```

Or build from source:

```bash
git clone https://codeberg.org/danszek/kb.ai.git
cd kb.ai
nix build             # dynamically linked
nix build .#static    # statically linked release binary
```

The resulting binary (`kabai`) speaks MCP over stdio: it reads JSON-RPC 2.0
from stdin and writes responses to stdout. **There is no server process to
run or keep running** — the agent's MCP client spawns one `kabai` process
per session; the only long-running component is PostgreSQL.

## Database setup

kabai expects a PostgreSQL (14+) database with the schema applied. The
recommended way to set it up — and to get a human-friendly board view —
is the sister project **[Kabai UI](https://codeberg.org/danszek/kbai-ui)**,
which runs the schema migrations for you.

Developers working on kabai itself can apply the plain-SQL migrations
directly from this repo (`migrations/`, source of truth, idempotent,
currently V1–V9):

```bash
createdb kabai
for f in migrations/V*.sql; do psql -d kabai -f "$f"; done
```

Projects, board columns, and workflow transitions are then created via the
MCP tools (`kabai_create_project`, `kabai_create_board_status`,
`kabai_create_status_transition`) or through Kabai UI.

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `KABAI_DB_HOST` | `localhost` | PostgreSQL host |
| `KABAI_DB_PORT` | `5432` | PostgreSQL port |
| `KABAI_DB_NAME` | `kabai` | Database name |
| `KABAI_DB_USER` | `postgres` | Database user |
| `KABAI_DB_PASSWORD` | *(empty)* | Database password |
| `KABAI_AGENT_NAME` | *(unset)* | Agent identity for `kabai_assign_ticket` — used as the default assignee |
| `KABAI_AGENT_MODEL` | *(unset)* | Model identifier — written to the ticket's `model` field on assignment |

**Set `KABAI_AGENT_NAME` and `KABAI_AGENT_MODEL`.** Without them,
`kabai_assign_ticket` fails unless the agent passes an explicit `assignee`
parameter on every call. Use one name per agent/client installation
(e.g. `claude-code-laptop`, `gemini-ci`).

## Client configuration

You never start `kabai` yourself: each client config below tells the
agent's MCP client which command to spawn (the `kabai` binary) and which
environment variables to hand it. Only the config file format differs per
client.

> **Tool names:** the server registers tools as `kabai_*`. Some clients
> prefix the server alias (e.g. `kabai__kabai_list_projects`). Agents
> should match tools by name suffix; the skill text explains this to them.

### Claude Code

Project-scoped, via `.mcp.json` in the repo root (or user-scoped with
`claude mcp add --scope user`):

```json
{
  "mcpServers": {
    "kabai": {
      "command": "/path/to/kabai",
      "env": {
        "KABAI_DB_HOST": "localhost",
        "KABAI_DB_NAME": "kabai",
        "KABAI_DB_USER": "postgres",
        "KABAI_DB_PASSWORD": "secret",
        "KABAI_AGENT_NAME": "my-agent",
        "KABAI_AGENT_MODEL": "claude-sonnet-5"
      }
    }
  }
}
```

Skill install (teaches the binding conventions, auto-triggers on kabai
work):

```bash
mkdir -p ~/.claude/skills
cp -r skill/kabai ~/.claude/skills/kabai
```

### Gemini CLI / Antigravity

MCP servers go into `~/.gemini/settings.json` (user) or
`.gemini/settings.json` (project):

```json
{
  "mcpServers": {
    "kabai": {
      "command": "/path/to/kabai",
      "env": {
        "KABAI_DB_HOST": "localhost",
        "KABAI_DB_NAME": "kabai",
        "KABAI_DB_USER": "postgres",
        "KABAI_DB_PASSWORD": "secret",
        "KABAI_AGENT_NAME": "my-gemini-agent",
        "KABAI_AGENT_MODEL": "gemini-2.5-pro"
      }
    }
  }
}
```

Verify with `/mcp` inside Gemini CLI — the server and its tools must be
listed.

Gemini CLI / Antigravity have a file-based skill system (no CLI installer).
Copy the whole skill directory — `SKILL.md` plus `references/` — to one of
these locations:

| Scope | Path |
|-------|------|
| Global (Antigravity) | `~/.gemini/antigravity-cli/skills/kabai/SKILL.md` |
| Shared | `~/.gemini/skills/kabai/SKILL.md` |
| Workspace | `./.agents/skills/kabai/SKILL.md` |

```bash
mkdir -p ~/.gemini/skills
cp -r skill/kabai ~/.gemini/skills/kabai
```

Fallback for setups without skill support: append the three skill files to
your global or project `GEMINI.md` (`cat skill/kabai/SKILL.md
skill/kabai/references/*.md >> ~/.gemini/GEMINI.md`). The frontmatter block
at the top of SKILL.md is skill metadata; it is harmless in a context
file, but you can strip it.

### Codex CLI

MCP servers are configured in `~/.codex/config.toml`:

```toml
[mcp_servers.kabai]
command = "/path/to/kabai"

[mcp_servers.kabai.env]
KABAI_DB_HOST = "localhost"
KABAI_DB_NAME = "kabai"
KABAI_DB_USER = "postgres"
KABAI_DB_PASSWORD = "secret"
KABAI_AGENT_NAME = "my-codex-agent"
KABAI_AGENT_MODEL = "gpt-5"
```

Codex reads `AGENTS.md` as its instruction file. Append the skill files to
your global `~/.codex/AGENTS.md` or the project `AGENTS.md`:

```bash
cat skill/kabai/SKILL.md \
    skill/kabai/references/ticket-workflow.md \
    skill/kabai/references/docs-zettelkasten.md >> ~/.codex/AGENTS.md
```

### Any other MCP client

`kabai` is a standard stdio MCP server (JSON-RPC 2.0, `initialize`,
`tools/list`, `tools/call`). Any client that can spawn a command with
environment variables can use it — configure the `kabai` binary as a stdio
server and inject the skill files through whatever instruction/context
mechanism the client offers (rules file, system prompt, project context).

## Agent skill (required for good results)

The tools alone do not make an agent follow the workflow conventions
(assign before working, tasks per acceptance criterion, work-log comments,
note linking). Field testing showed that agents without the rules in
context ignore the tools or use them partially — one tested agent even
tried to bypass the MCP server and query PostgreSQL directly.

Always install the skill text into the agent's context, as shown per
client above. The files in [`skill/kabai/`](../skill/kabai/) are
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

(All names carry the `kabai_` prefix.)

Rule enforcement lives in PostgreSQL triggers, not in the server: illegal
workflow moves, moving to `done` with open tasks, and closing a
`docs_required` ticket without a linked note are rejected by the database
with a descriptive error message.

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| "Connection refused" | PostgreSQL not running or wrong `KABAI_DB_*` values |
| "Illegal Kanban-Move" | Transition not in the project's workflow graph — read `kabai_list_status_transitions` first |
| "Open acceptance criteria" | Ticket has incomplete tasks; complete them before moving to `done` |
| Move to `done` rejected with docs message | Ticket has `docs_required: true` and no linked note — link one via `kabai_docs_link_ticket` |
| "Missing assignee" on `assign_ticket` | `KABAI_AGENT_NAME` not set in the server env — set it (and `KABAI_AGENT_MODEL`) or pass `assignee` explicitly |
| "Project/Status not found" | Wrong `project_id`/`status_id` — status IDs are per project, discover them via `kabai_list_board_statuses` |
| Agent sees the tools but never calls them | Skill/rules text not in the agent's context — install it per the client sections above |
| Agent claims a tool does not exist | Tool-name prefix variance — the agent must match by suffix (the skill explains this); verify the server is listed in the client's MCP status view |
