# kabai knowledge base (zettelkasten) — binding rules

The `kabai_docs_*` tools are the project's knowledge archive: atomic notes
in PostgreSQL, connected by typed links. It replaces grepping markdown
files. **Tickets are the record of work; notes are the record of
knowledge.** Anything a future agent should know without replaying old
tickets belongs in a note.

## 0. Use the MCP tools — nothing else

Notes live only in the database behind the kabai MCP server. You MUST NOT
read or write them via direct SQL, and you MUST NOT grep the filesystem
for knowledge-base content — it is not stored as files. Tool names here
use the form `kabai_docs_*`, exactly as the server registers them; your
client may show a server-prefixed variant (e.g. `kabai__kabai_docs_*`) —
match by name
suffix against your available tool list. No match means the MCP server is
not connected: report it, do not improvise.

## 1. Search first — always

Before creating a note, and before reading code to answer a "how does X
work" question:

```json
kabai_docs_search {"query": "delta compression", "project_id": 4}
```

- Hits are ranked, carry a snippet, `match_type` (`fts`, or
  `title_similarity` as typo-tolerant fallback) and `body_chars`.
- If a note on the topic exists: **update it** (`kabai_docs_update_note`)
  instead of creating a near-duplicate. NEVER create a second note for the
  same concept.
- An empty result means the topic is genuinely undocumented — that is your
  cue to write the note once you know the answer.

## 2. Atomicity

One note = one concept. If a note needs chapter headings for distinct
topics, split it into several notes and connect them with links. Split
criterion: could another ticket need only one of the parts? Then it is a
separate note. (Reference implementation: a 300-line protocol document
becomes one note per message type plus one hub.)

## 3. Creating notes

```json
kabai_docs_create_note {
  "slug": "concept-delta-compression",
  "title": "Delta compression of entity snapshots",
  "kind": "note",
  "body": "…markdown…",
  "tags": ["networking", "performance"],
  "project_ids": [4]
}
```

- **slug** — kebab-case, globally unique, **PERMANENT** (it cannot be
  renamed; choose carefully). Recommended prefixes: `adr-NNN-*` for
  decisions, `arch-*` for architecture/system descriptions, `concept-*`
  for design/domain concepts, `*-hub` for hubs. These prefixes are a
  convention, not enforced — but keep them consistent within a project.
- **kind** — `adr` for a decision with context/alternatives/consequences;
  `hub` for an overview page; `note` for everything else. No other values
  exist.
- **tags** — lowercase topic words for filtering (they are also
  search-indexed). Reuse existing tags (`kabai_docs_list_notes` shows
  them) instead of inventing synonyms.
- **project_ids** — 0..n projects; the zettelkasten is global. Knowledge
  shared by several projects (process rules, shared infrastructure) gets
  several projects; truly global notes get none.

## 4. Linking notes (`kabai_docs_link_notes`)

Links ARE the structure — a note without links is invisible to navigation
("orphan"). After creating a note, link it:

| link_type | Use |
|-----------|-----|
| `contains` | hub → member. A hub with its contains links IS a table of contents. Every new note SHOULD be added to the matching hub. |
| `references` | generic "related/see also" between notes. |
| `supersedes` | new decision replaces an old one: create the NEW note, link new → old, keep the old note (do not rewrite history). |
| `contradicts` | you found two notes (or note vs. reality) in conflict and cannot resolve it now — mark it for review instead of ignoring it. |

Wrong link? `kabai_docs_unlink_notes` with the exact same triple.

## 5. Linking tickets (`kabai_docs_link_ticket`)

**Rule: every note you create or substantially change while working a
ticket gets linked to that ticket.** That is how later agents find the
knowledge from the ticket side (`kabai_get_ticket_detailed` returns
`linked_notes`).

| relation | Use |
|----------|-----|
| `created_by` | the ticket produced this note. |
| `documents` | the note documents what the ticket built/changed. |
| `verified_by` | the ticket checked the note against reality (see §7). |
| `references` | loose relation (note was consulted, background). |

## 6. Hubs and discovery

- Entry-point discovery: `kabai_docs_list_notes {"kind": "hub",
  "summary": true}` — cheap, lists the tables of contents.
- Navigation: `kabai_docs_get_note` on a hub returns its `contains` links
  as metadata (slug/title/kind, no bodies); follow with a second
  `get_note` for exactly the member you need. Two calls, no wasted
  context.
- When a topic area accumulates ~5+ notes without a hub, create one
  (`kind: "hub"`, short body describing the area) and add contains links.

## 7. Lifecycle: verify, update, archive

- **Verify:** when you read a note while working a ticket and confirmed it
  still matches the code/system, call
  `kabai_docs_verify_note {"note_id": N, "ticket_id": T}`. Verification
  age is shown everywhere; old or missing verification tells agents to
  double-check before trusting.
- **Fixed verification triggers — part of the workflow, not a favor:**
  (a) on ticket pickup, after reading the ticket's `linked_notes`, verify
  each note you confirmed against reality; (b) when reviewing a ticket
  (review column), check its linked notes and verify them. A note nobody
  ever verifies is a rumor, not knowledge.
- **Staleness review:** `kabai_docs_list_notes {"project_id": P,
  "unverified_since_days": 30, "summary": true}` lists notes nobody has
  confirmed recently.
- **Update:** `kabai_docs_update_note` changes only the fields you pass
  (title/body/kind/tags). Always pass `ticket_id` — it records provenance.
  If the old content is *superseded* rather than corrected, write a new
  note plus a `supersedes` link instead (see §4).
- **Archive:** `kabai_docs_archive_note {"note_id": N, "reason": "…"}` —
  soft delete with mandatory reason; archived notes keep links and stay
  resolvable but leave search/listings. There is no hard delete. NEVER
  archive a note just because it is old — only when it is wrong or
  irrelevant AND nothing supersedes it (else use supersedes).

## 8. docs_required gating

Tickets can carry `docs_required: true`. Setting it is MANDATORY for
architecture, design-decision, and schema work (new subsystems, ADR-worthy
decisions, schema changes) — and for EVERY epic: an epic MUST NOT close
without at least one note created or substantially updated and linked
during its lifetime. Reviewers MUST reject tickets that should have
carried the flag. Such a ticket **cannot move to done** until a note is
linked.

**Document early, update often.** Create the note (or a draft skeleton)
when you pick the ticket up or at the first design decision, link it
immediately (`created_by`), and keep it current as the work evolves.
Writing the note as a closing chore right before `done` produces
summary-of-work notes instead of living knowledge. If you believe no docs
are needed after all, unset the flag via `kabai_update_ticket` and justify
it in a work-log comment — this escape hatch does NOT apply to epics.

## 9. When to write a note — decision path

While working a ticket you learned or decided something. Walk this list:

1. Will it matter after this ticket is closed? **No** → work-log comment
   on the ticket is enough. **Yes** → continue.
2. `kabai_docs_search` for the concept. **Found?** → does your knowledge
   correct or extend it? Update the note (with `ticket_id`), or create a
   new note + `supersedes` if the old approach is replaced. Then
   `verify_note` if you confirmed the rest still holds.
3. **Not found** → `create_note` (atomic! split if needed), then:
   `link_ticket(created_by)`, `contains`-link from the matching hub,
   `references`-links to related notes, project assignment.
4. Decision with alternatives and consequences? Use `kind: "adr"` and an
   `adr-*` slug.

## 10. Reading efficiently

- Check `body_chars` (in search hits and listings) before `get_note` —
  you know the retrieval cost in advance.
- `summary: true` for all overviews; paginate with limit/offset.
- On ticket pickup, call `kabai_docs_suggest_for_ticket {"ticket_id": N}`:
  it combines the ticket-relation graph with a full-text match and states
  a reason per suggestion. Also read the ticket's `linked_notes` from
  `kabai_get_ticket_detailed`.
- `kabai_docs_assign_project`/`unassign_project` fix project scoping when
  a note turns out to be relevant to more (or fewer) projects.

## 11. Anti-patterns

- Knowledge buried in ticket comments only — later agents replay whole
  ticket histories to find it.
- Orphan notes (no links at all) — invisible to hub navigation.
- Hub-less collections — five `concept-*` notes and no `concept-hub`.
- Slug improvisation — three notes about one topic under three naming
  schemes.
- Overwriting decision history — editing an ADR into its successor
  instead of `supersedes`.
- Duplicate notes because search was skipped.
- Archiving instead of superseding, or deleting knowledge that was merely
  outdated.
- Epic closed without a new or substantially updated, linked note.
- Documentation written only as a closing chore right before done.
- Notes that are never verified — verification age is the trust signal.
