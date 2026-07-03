# ADR 001: kbai-docs — Postgres zettelkasten as a docs module

Status: accepted · 2026-07-03 · Ticket: kbai-docs #330

## Context

kbai covers project management (tickets, roles, workflow). The second half
of an agent's context — "what is already known about this system, and how
does it relate to the current ticket" — used to run on raw markdown reading:
expensive, blind (grep guessing), with no structural connection to tickets,
and drift-prone (documented in epic kbai-docs #322).

## Foundational decisions (project owner, 2026-07-03 — recorded here)

1. **PostgreSQL is the source of truth.** No markdown files in the repo, no
   git sync. Markdown **export** from the DB is a different tool's job (out
   of scope). Rejected: git markdown as the source with a DB index — would
   have required permanent sync/drift machinery (the dropped ticket #334).
2. **Zettelkasten model.** Atomic notes (title + markdown body), typed
   directed note↔note links. No file/chapter tree; overview pages ("hubs")
   are ordinary notes whose `contains` links define the collection.
   Rejected: doc/section hierarchy — only half-solves targeted retrieval
   and breaks on restructuring.
3. **Global with n:m project assignment.** One zettelkasten for all
   projects; a note belongs to 0..n projects. Rejected: strictly
   project-scoped — duplicates cross-project knowledge (role/process notes).
4. **`kind` field without special logic** (note | adr | hub) plus free-form
   tags. No type-specific workflows in the MVP; ADR succession is a
   `supersedes` link.
5. **One binary.** docs is a module `src/docs/` inside the kbai server,
   registered via the MCP framework (docs/MCP_FRAMEWORK_DESIGN.md,
   implemented in Kanban AI #335). Same DB → note_ticket_link uses real FKs.
6. **Revision history later**, schema prepared for it (kbai-docs #339).

## Detail decisions

### D1: Note addressing — id + global slug

`note.id` (serial) plus `note.slug` (TEXT, UNIQUE, global). The slug is a
required parameter of `create_note` — agents produce good slugs, and
server-side transliteration (umlauts etc.) in C is not worth it. The slug
is the stable key and cannot change; the title is freely editable.
`get_note` accepts id or slug.

### D2: Link types for note_link

Minimal set as a CHECK constraint (mirroring ticket_relations):

| Type         | Meaning |
|--------------|---------|
| `references` | generic reference (default) |
| `contains`   | hub → member; defines overview pages |
| `supersedes` | new version replaces the old one (ADR convention) |
| `contradicts`| marked inconsistency — review signal for #326 |

Extension happens via migration (adjusting the CHECK), not via free-form
strings — typos in link types would otherwise become silent data errors.

### D3: Full-text search — 'simple' + pg_trgm

Content is mixed German/English; a language stemmer ('german' or 'english')
mangles the respective other language. Therefore:

- Generated column `search_tsv tsvector` over title (weight A), tags (B),
  body (C) using the **'simple'** configuration, GIN index. Queried via
  `websearch_to_tsquery('simple', $1)`, ranked with `ts_rank`.
- Additionally a **pg_trgm** GIN index on title for substring/typo matching
  as a fallback when the tsquery yields no hits.
- Rejected: embeddings/pgvector — only if FTS+tags demonstrably fall short
  (separate ticket, not MVP).

### D4: Tag model — text[]

`note.tags TEXT[] NOT NULL DEFAULT '{}'` with a GIN index; tags are
normalised to lowercase. Rejected: a note_tag table — more joins with no
benefit while tags are pure search/filter criteria.

### D5: Tool namespace

Server-side prefix `kb.ai_docs_` (consistent with `kb.ai_...`; MCP clients
normalise to `kb_ai_docs_...`). Planned tools:

| Tool | Ticket | Purpose |
|------|--------|---------|
| `kb.ai_docs_create_note(slug, title, kind, body, tags?, project_ids?)` | #332 | create a note |
| `kb.ai_docs_update_note(note_id, title?, body?, kind?, tags?)` | #332 | targeted field updates |
| `kb.ai_docs_archive_note(note_id, reason)` | #332 | soft delete |
| `kb.ai_docs_link_notes(from_note_id, to_note_id, link_type)` / `unlink_notes` | #332 | note↔note |
| `kb.ai_docs_assign_project(note_id, project_id)` / `unassign_project` | #332 | n:m project |
| `kb.ai_docs_get_note(note_id \| slug)` | #324 | body + link neighbourhood (metadata) |
| `kb.ai_docs_list_notes(project_id?, kind?, tag?, summary?, limit?, offset?)` | #324 | overview |
| `kb.ai_docs_search(query, project_id?, kind?, tag?)` | #323 | FTS, hits carry body_chars |
| `kb.ai_docs_link_ticket(note_id, ticket_id, relation)` / `unlink_ticket` | #325 | note↔ticket |
| `kb.ai_docs_verify_note(note_id, ticket_id)` | #326 | stamp last_verified |
| `kb.ai_docs_suggest_for_ticket(ticket_id)` | #327 | suggestions |
| `kb.ai_docs_get_note_history(note_id)` | #339 | later (revisions) |

note_ticket_link relations: `documents | created_by | verified_by |
references` (CHECK constraint, mirroring D2).

### D6: Where this ADR lives (bootstrapping)

Until kbai-docs is running: markdown under docs/adr/ in the kb.ai repo.
After the legacy import (#333) this ADR becomes one of the first notes in
the zettelkasten; from then on kbai design docs are written there directly.

### D7 (additional finding): drop legacy tables

`ticket_documents` (file-path links on tickets — exactly the concept that
note_ticket_link does properly) and `ticket_dependencies` (superseded by
ticket_relations) are both empty and referenced by no code. The V7
migration (#331) drops both so no second, dead "documents" concept exists
next to kbai-docs.

## Schema derivation for #331 (cross-check)

| Requirement | Schema element |
|-------------|----------------|
| #323 search | note.search_tsv (generated, GIN) + pg_trgm on title |
| #324 read/hubs | note, note_link (contains) — neighbourhood via join |
| #325 ticket links | note_ticket_link (note_id FK, ticket_id FK, relation CHECK) |
| #326 verification | note.last_verified_ticket_id FK, note.last_verified_at |
| #327 suggestions | no new objects (uses #323 + #325) |
| #328 docs guard | tickets.docs_required BOOL + check in the move path (kbai core) |
| #339 revisions | note.updated_by_ticket_id now; note_revision + trigger later |
| global n:m | note_project (note_id, project_id, PK on both) |

No open question blocks #331.

## Effort estimates

#331 schema: S · #332 module+write API: M · #323 search: S · #324 read API: S ·
#325 ticket links: S (incl. get_ticket_detailed extension) · #326 verify: XS ·
#327 suggest: M · #328 guard: S · #333 import: M (agent work, no code) ·
#339 revisions: S
