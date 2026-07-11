# kabai ticket workflow — binding rules

Complete rules for the ticket half of the kabai MCP server. Every rule is
imperative: MUST/NEVER rules are testable and a reviewer may reject your
work for violating them.

## 0. Use the MCP tools — nothing else

All ticket and board state lives in a PostgreSQL database that is reachable
ONLY through the kabai MCP server's tools. You MUST NOT query the database
directly (psql/SQL), and you MUST NOT search the filesystem or repo for
ticket data — it is not there. Tool names below use the form `kabai_*`,
exactly as the server registers them; your client may expose them with a
server prefix (e.g. `kabai__kabai_*`). Match by name suffix against your own tool
list. If nothing matches, the MCP server is not connected — report that
instead of improvising another access path.

## 1. Session start protocol

At the start of any session that touches kabai, in this order:

1. `kabai_list_projects` — find the project you work on; never guess IDs.
2. `kabai_list_board_statuses(project_id)` — status IDs are **per project**.
   NEVER hardcode or reuse status IDs across projects; the same column name
   ("done") has a different ID everywhere.
3. Read the `agent_role_instruction` of the column your ticket is in and
   **follow it** — it defines your role (e.g. architect vs. developer) and
   its definition of done. Columns without instruction carry no extra rules.
4. `kabai_list_status_transitions(project_id)` — learn the workflow graph
   before you move anything.

Example:

```json
kabai_list_board_statuses {"project_id": 4}
```

## 2. Creating tickets

- **Duplicate check is mandatory.** Before every `kabai_create_ticket`, run
  `kabai_search_tickets(project_id, query)` with 1–3 keyword variants
  (and/or `kabai_list_tickets(project_id, summary: true)` for an overview).
  If a near-duplicate exists: `kabai_add_comment` on the existing ticket
  with your addition instead of creating a new one, or link the tickets
  with `relates_to`/`duplicate_of`. NEVER create a second ticket for the
  same work.
- **Description is not optional in practice.** A ticket description MUST
  state: scope (what is in, what is out), references (files, notes, other
  tickets by number), an effort estimate (XS/S/M/L/XL), and observable
  acceptance criteria. A title-only ticket is not workable by another agent.
- **Type:** use `type: "epic"` only for umbrella goals that group child
  tickets; everything else is the default `ticket`.
- **docs_required:** set `docs_required: true` on architecturally relevant
  work (new subsystem, ADR-worthy decision, schema change). Such a ticket
  cannot move to done without a linked knowledge-base note — see the docs
  chapter.
- **Assign immediately.** Directly after `create_ticket` (or after picking
  up an existing ticket), call `kabai_assign_ticket(ticket_id)`. It records
  assignee and model from the server environment. NEVER work a ticket that
  is not assigned to you; NEVER leave your own new ticket unassigned if you
  are about to work it.

```json
kabai_search_tickets {"project_id": 4, "query": "delta compression"}
kabai_create_ticket {"project_id": 4, "status_id": 12, "title": "Compress entity snapshots",
  "description": "Scope: ... Out of scope: ... Refs: note delta-compression, #33.\nEffort: M\nAcceptance:\n- [criterion 1]\n- [criterion 2]",
  "docs_required": true}
kabai_assign_ticket {"ticket_id": 123}
```

## 3. Tasks = acceptance criteria

- For every acceptance criterion in the description, add one
  `kabai_add_task(ticket_id, title)`. An empty task list means the ticket
  cannot be verified — reviewers MUST treat it as not done.
- Mark each task with `kabai_complete_task(task_id)` **immediately** when
  it is verifiably met. NEVER batch-complete all tasks right before closing:
  the task list is live progress state for other agents and humans.
- The database refuses to move a ticket to `done` while open tasks remain.
  If a criterion turned out to be obsolete, say so in a comment — do not
  silently tick it off.

## 4. Comments are the audit trail

`kabai_add_comment(ticket_id, author, text)` at every significant event:

- **Pickup:** what you are about to do, in one or two sentences.
- **Decisions:** what you chose and why (alternatives you rejected).
- **Blockers/handoff:** exact state, what remains, where the next agent
  should start.
- **Completion:** a verification block with real command output (build,
  tests, example calls) — claims without evidence are not verification.

Comments MUST be self-contained ("see above" is useless in a work log).
Use your model identifier as `author`. Knowledge that outlives the ticket
belongs in a knowledge-base note, not in a comment — see the docs chapter.

## 5. Moving tickets

- Move only via `kabai_move_ticket(ticket_id, new_status_id)` and only
  along edges returned by `kabai_list_status_transitions`. NEVER skip
  columns; the database rejects illegal transitions, but do not rely on
  trial and error — read the graph first.
- On entering a column, read and follow its `agent_role_instruction`.
- `kabai_move_tickets(ticket_ids, new_status_id)` is for genuine batch
  operations (e.g. sweeping obsolete tickets); it returns a per-ticket
  success/error breakdown — check it.
- A move to `done` is refused while open tasks remain, and — if
  `docs_required` is set — while no knowledge-base note is linked. The
  error message names the way out; follow it instead of force-retrying.

## 6. Human intervention

Columns with `special_type`:

- `human_intervention`: escalation target, reachable from ANY column. When
  you are blocked on a decision only the human can make: write the question
  as a comment (full context, options, your recommendation), then
  `kabai_move_ticket` into the human_intervention column. Then stop working
  that ticket.
- `human_answered`: the human replied. Read the newest comments, continue
  the work, and move the ticket to the appropriate normal column (any
  column is reachable from human_answered).

NEVER guess on decisions that are the project owner's to make; escalate.

## 7. Epics and relations

`kabai_link_tickets(from_ticket_id, to_ticket_id, relation_type)`:

- `parent_of` — epic → child. Create the epic (`type: "epic"`), then link
  each child. An epic is done only when every child is done or explicitly
  descoped (comment on the epic saying which and why).
- `blocks` — hard ordering: `from` must finish before `to` can start. Set
  it whenever you know the dependency; agents use it to pick workable
  tickets.
- `duplicate_of` — mark the duplicate, keep the older/richer ticket, then
  add a merge comment on the survivor and delete the duplicate
  (`kabai_delete_ticket` requires a reason, e.g. "duplicate of #42").
- `relates_to` — generic association when none of the above fits.

Relations may cross projects. Remove wrong links with
`kabai_unlink_tickets` (exact same triple).

## 8. Editing and deleting

- `kabai_update_ticket(ticket_id, title?, description?, docs_required?)` —
  keep descriptions current: when scope changes mid-work, update the
  description AND leave a comment about the change. Pass
  `description: null` to clear. When unsetting `docs_required`, justify it
  in a comment.
- **Refining a rough ticket:** when you pick up a ticket whose description
  is only a rough idea, your FIRST work step is `kabai_update_ticket` to
  bring the description up to standard (scope, references, effort,
  acceptance criteria). Comments and tasks do not replace the description —
  it is what the next reader sees first.
- `kabai_delete_ticket(ticket_id, reason)` — permanent, cascades tasks,
  comments, relations and note links. Only for mistakes and duplicates,
  never for "done but messy" tickets (that is what done is for). The
  reason is mandatory and becomes the audit trail.

## 9. Reading efficiently

- Overviews: `kabai_list_tickets(project_id, summary: true, status_id?,
  limit?, offset?)` — summary omits descriptions; paginate instead of
  fetching everything.
- Single ticket: `kabai_get_ticket` for a quick look;
  `kabai_get_ticket_detailed` only when you actually pick the ticket up —
  it returns tasks, relations, comments AND `linked_notes` (read those
  notes before starting; see docs chapter). Pass
  `include_role_instruction: false` on repeat calls in the same session.
- `kabai_get_project(project_id)` for one project's metadata instead of
  listing all.

## 10. Board administration

`kabai_create_project(slug, name, description)`,
`kabai_create_board_status(project_id, name, display_name, position,
agent_role_instruction?)` and `kabai_create_status_transition(project_id,
from_status_id, to_status_id)` set up new boards. Rules:

- New projects automatically get human_intervention/human_answered columns.
- Name the closing column `done` — the acceptance-criteria and
  docs_required guards key on that name.
- Write an `agent_role_instruction` per working column: it is the persona
  prompt agents receive with tickets in that column — state role, scope,
  and a checkable definition of done.
- Wire transitions BEFORE agents start working; a board without transitions
  cannot be worked (every move is rejected).

## 11. Anti-patterns (instant review rejections)

- Ticket worked without being assigned, or assigned but never picked up.
- Ticket without tasks, or all tasks completed in one batch at the end.
- Status skipped, or status IDs reused across projects.
- Description that no longer matches what was actually built.
- Work done with no ticket at all ("I'll just quickly...") — create one.
- Question to the human buried in a comment without moving to
  human_intervention (nobody will see it).
- Knowledge written only into comments instead of the knowledge base.
