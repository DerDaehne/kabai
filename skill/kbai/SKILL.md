---
name: kbai
description: Binding usage rules for the kb.ai MCP server (kb_ai_* / kb.ai_* tools). Use whenever working with kbai tickets, kanban boards, projects, or the kbai knowledge base (notes/zettelkasten) — creating or picking up tickets, moving them through the workflow, writing or searching notes, linking notes to tickets. Ensures conventions (assignment, tasks, work log, note links) are followed without project-specific instructions.
---

# kbai — how to use the kb.ai MCP server correctly

kb.ai is a kanban board plus a knowledge base ("zettelkasten") in
PostgreSQL, exposed as MCP tools. Two tool families:

- `kb_ai_*` — projects, board columns, workflow transitions, tickets,
  tasks (acceptance criteria), comments (work log), ticket relations.
- `kb_ai_docs_*` — atomic knowledge notes with typed links, connected to
  tickets.

Mental model: **tickets are the record of work; notes are the record of
knowledge.** Everything below is binding. Full rules with examples:
[references/ticket-workflow.md](references/ticket-workflow.md) and
[references/docs-zettelkasten.md](references/docs-zettelkasten.md) — read
the relevant one before your first kbai action in a session.

## Session start (always)

1. `kb_ai_list_projects` → find your project id.
2. `kb_ai_list_board_statuses(project_id)` → column ids are **per
   project**, never reuse them across projects. Read each column's
   `agent_role_instruction` and follow it.
3. `kb_ai_list_status_transitions(project_id)` → moves are only legal
   along this graph.

## Golden rules — tickets

1. **Search before create.** `kb_ai_search_tickets` first; comment on a
   near-duplicate instead of creating a twin.
2. **Full descriptions.** Scope, references, effort estimate (XS–XL),
   observable acceptance criteria. Title-only tickets are not workable.
3. **Assign immediately.** `kb_ai_assign_ticket` right after creating or
   picking up a ticket. Never work unassigned tickets.
4. **One task per acceptance criterion** (`kb_ai_add_task`);
   `kb_ai_complete_task` immediately when met, never batched at the end.
5. **Comment the work log** (`kb_ai_add_comment`): pickup, decisions,
   blockers, and a completion comment with real verification output.
6. **Move only along the transition graph** (`kb_ai_move_ticket`). The DB
   rejects illegal moves, open tasks at done, and missing note links when
   `docs_required` is set.
7. **Blocked on a human decision?** Write the question as a comment, move
   the ticket to the `human_intervention` column, stop. Resume from
   `human_answered`.
8. **Epics** group children via `kb_ai_link_tickets(parent_of)`; `blocks`
   expresses ordering; duplicates get merged, not abandoned.

## Golden rules — knowledge base

1. **Search first** (`kb_ai_docs_search`) — update existing notes instead
   of creating duplicates.
2. **Atomic notes**: one concept per note; split big topics into linked
   notes. Slugs are permanent, kebab-case; recommended prefixes
   `adr-*`, `arch-*`, `concept-*`, `*-hub`.
3. **Link everything**: new notes get a `contains` link from the matching
   hub, `references` to related notes, and `kb_ai_docs_link_ticket` to the
   ticket that produced them (`created_by`/`documents`). Orphan notes are
   invisible.
4. **Never rewrite decision history**: a replaced decision gets a new note
   plus a `supersedes` link. Conflicts you cannot resolve get
   `contradicts`.
5. **Verify what you read**: confirmed a note still matches reality →
   `kb_ai_docs_verify_note(note_id, ticket_id)`.
6. **Archive, don't delete** (`kb_ai_docs_archive_note` with reason) —
   and only for wrong/irrelevant content, not for old age.
7. **On ticket pickup**: read `linked_notes` from
   `kb_ai_get_ticket_detailed` and call `kb_ai_docs_suggest_for_ticket`.

## Efficiency

- `summary: true` + `limit`/`offset` for all listings; `body_chars` tells
  you a note's retrieval cost before `get_note`.
- `kb_ai_get_ticket_detailed` only on pickup; pass
  `include_role_instruction: false` on repeat calls.
- Discover knowledge entry points via
  `kb_ai_docs_list_notes(kind: "hub", summary: true)`.

## Anti-patterns (reviewers reject these)

Unassigned work · empty task lists · batch-completing tasks · skipping
columns · stale descriptions · questions to humans without the
human_intervention move · knowledge only in comments · duplicate
tickets/notes · orphan notes · editing ADRs instead of superseding them.
