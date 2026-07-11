---
name: kabai
description: Binding usage rules for the kabai MCP server (kabai_* tools). Use whenever working with kabai tickets, kanban boards, projects, or the kabai knowledge base (notes/zettelkasten) — creating or picking up tickets, moving them through the workflow, writing or searching notes, linking notes to tickets. Ensures conventions (assignment, tasks, work log, note links) are followed without project-specific instructions.
---

# kabai — how to use the kabai MCP server correctly

kabai is a kanban board plus a knowledge base ("zettelkasten") in
PostgreSQL, exposed as MCP tools. Two tool families:

- `kabai_*` — projects, board columns, workflow transitions, tickets,
  tasks (acceptance criteria), comments (work log), ticket relations.
- `kabai_docs_*` — atomic knowledge notes with typed links, connected to
  tickets.

Mental model: **tickets are the record of work; notes are the record of
knowledge.** Everything below is binding. Full rules with examples:
[references/ticket-workflow.md](references/ticket-workflow.md) and
[references/docs-zettelkasten.md](references/docs-zettelkasten.md) — read
the relevant one before your first kabai action in a session. (If those
relative links do not resolve in your environment, the two files live next
to this one; they also work concatenated as plain instructions.)

## Rule zero — the MCP tools are the ONLY interface

The connected kabai MCP server provides every operation you need. You MUST
do all board and knowledge-base work through its tools. NEVER bypass them:

- NEVER query or modify the PostgreSQL database directly (no psql, no SQL,
  no inspecting the schema to "help yourself").
- NEVER search the filesystem, repo, or neighbouring directories for
  tickets, board state, or knowledge notes — that data lives only in the
  database behind the MCP server.
- If a tool call fails or a tool seems missing, the correct reaction is to
  resolve the tool name (next section) or report the MCP setup problem —
  not to fall back to another access path.

## Tool names vary by client

This document writes tool names as `kabai_*`, exactly as the server
registers them. Some MCP clients expose them with a server-alias prefix
(e.g. `kabai__kabai_list_projects` or `mcp__kabai__kabai_list_projects`,
depending on how the server is named in the client config). Resolve names
by suffix: look through YOUR available tool list for the tool whose name
ends in the name used here. If no tool matches, the MCP server is not
connected — say so and stop; do not conclude the capability is missing
and improvise.

## Session start (always)

1. `kabai_list_projects` → find your project id.
2. `kabai_list_board_statuses(project_id)` → column ids are **per
   project**, never reuse them across projects. Read each column's
   `agent_role_instruction` and follow it.
3. `kabai_list_status_transitions(project_id)` → moves are only legal
   along this graph.

## Golden rules — tickets

1. **Search before create.** `kabai_search_tickets` first; comment on a
   near-duplicate instead of creating a twin.
2. **Full descriptions.** Scope, references, effort estimate (XS–XL),
   observable acceptance criteria. Title-only tickets are not workable —
   and when you pick up a rough ticket, refining its description via
   `kabai_update_ticket` is your first work step.
3. **Assign immediately.** `kabai_assign_ticket` right after creating or
   picking up a ticket. Never work unassigned tickets.
4. **One task per acceptance criterion** (`kabai_add_task`);
   `kabai_complete_task` immediately when met, never batched at the end.
5. **Comment the work log** (`kabai_add_comment`): pickup, decisions,
   blockers, and a completion comment with real verification output.
6. **Move only along the transition graph** (`kabai_move_ticket`). The DB
   rejects illegal moves, open tasks at done, and missing note links when
   `docs_required` is set.
7. **Blocked on a human decision?** Write the question as a comment, move
   the ticket to the `human_intervention` column, stop. Resume from
   `human_answered`.
8. **Epics** group children via `kabai_link_tickets(parent_of)`; `blocks`
   expresses ordering; duplicates get merged, not abandoned.

## Golden rules — knowledge base

1. **Search first** (`kabai_docs_search`) — update existing notes instead
   of creating duplicates.
2. **Atomic notes**: one concept per note; split big topics into linked
   notes. Slugs are permanent, kebab-case; recommended prefixes
   `adr-*`, `arch-*`, `concept-*`, `*-hub`.
3. **Link everything**: new notes get a `contains` link from the matching
   hub, `references` to related notes, and `kabai_docs_link_ticket` to the
   ticket that produced them (`created_by`/`documents`). Orphan notes are
   invisible.
4. **Never rewrite decision history**: a replaced decision gets a new note
   plus a `supersedes` link. Conflicts you cannot resolve get
   `contradicts`.
5. **Verify what you read**: confirmed a note still matches reality →
   `kabai_docs_verify_note(note_id, ticket_id)`.
6. **Archive, don't delete** (`kabai_docs_archive_note` with reason) —
   and only for wrong/irrelevant content, not for old age.
7. **On ticket pickup**: read `linked_notes` from
   `kabai_get_ticket_detailed` and call `kabai_docs_suggest_for_ticket`.

## Efficiency

- `summary: true` + `limit`/`offset` for all listings; `body_chars` tells
  you a note's retrieval cost before `get_note`.
- `kabai_get_ticket_detailed` only on pickup; pass
  `include_role_instruction: false` on repeat calls.
- Discover knowledge entry points via
  `kabai_docs_list_notes(kind: "hub", summary: true)`.

## Anti-patterns (reviewers reject these)

Bypassing the MCP tools (direct SQL, grepping the filesystem for board
data) · unassigned work · empty task lists · batch-completing tasks ·
skipping columns · stale descriptions · questions to humans without the
human_intervention move · knowledge only in comments · duplicate
tickets/notes · orphan notes · editing ADRs instead of superseding them.
