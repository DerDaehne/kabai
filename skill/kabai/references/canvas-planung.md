# kabai Canvas — binding planning rules

Canvas is the planning layer **above epics**: a cross-project whiteboard
for rough planning (SvelteFlow-based in kbai-ui) that AI agents co-edit
with the human through the `kabai_canvas_*` tools. **Canvas is the record
of planning; tickets are the record of work; notes are the record of
knowledge.** Frames replace milestones — there is no separate milestone
entity.

## 0. Use the MCP tools — nothing else

Canvases, elements, and edges live only in the database behind the kabai
MCP server. You MUST NOT query or modify them via direct SQL, and you
MUST NOT search the filesystem or repo for canvas content — it is not
stored as files. Tool names here use the form `kabai_canvas_*` /
`kabai_*_canvas_element` / `kabai_*_canvas_edge`, exactly as the server
registers them; your client may show a server-prefixed variant. Match by
name suffix against your available tool list; no match means the MCP
server is not connected — report it, do not improvise.

## 1. When to use Canvas — vs. a ticket, vs. a note

A canvas is for **rough, cross-project planning that has not yet become
committed work**: sketching how a feature might fit together, collecting
related tickets/epics/notes from several projects side by side, drafting
a rollout before it is broken into epics. Once planning firms up into
actual work, it becomes tickets (an epic + children) — the canvas stays
as the planning record that produced them, linked via `ref` elements, not
replaced or deleted.

- Committed, scoped work → a ticket (or epic), not a canvas element.
- Something a future agent should know regardless of any one project
  → a zettelkasten note (see [references/docs-zettelkasten.md](docs-zettelkasten.md)), not a canvas text block.
- Cross-project rough planning, sketches, "how do these ideas relate" → a
  canvas.

## 2. Discovery before creating

A canvas is a standalone entity (belongs to no project; linked n:m via
`project_ids`/`kabai_canvas_assign_project`). Before creating a new one:

```json
kabai_list_canvases {"project_id": 4}
```

If an existing canvas already covers the planning area, add to it
(`kabai_add_canvas_element`) instead of starting a second one for the
same topic — an orphan duplicate canvas is as bad as a duplicate ticket.

## 3. Elements

`kabai_add_canvas_element {"canvas_id": N, "type": "...", "content": {...}, ...}`.
Five types, each with its own `content` shape:

| type | content | Notes |
|------|---------|-------|
| `text` | `{"text": "..."}` | Free markdown block. |
| `frame` | `{"title": "..."}` | Grouping container — **replaces milestones** (§5). |
| `ref` | `{"target_type": "ticket"\|"note", "target_id": N}` | Reference card to a ticket/epic/note in **any** project (existence checked on write). |
| `image` | `{"attachment_id": N}` | Requires `description` (§4). |
| `sketch` | `{"strokes": [[[x,y,pressure], ...], ...]}` | Requires `description` (§4). |

`parent_frame_id` groups an element under a `frame` element on the SAME
canvas. `kabai_get_canvas(canvas_id)` returns every element and edge in
one call — use it, not repeated single-element reads.

## 4. description (alt-text) is MANDATORY for image and sketch

`kabai_add_canvas_element` REJECTS an `image` or `sketch` element without
a non-empty `description`, and `kabai_update_canvas_element` REJECTS
clearing it. This is not a UI nicety — it is how agents that cannot see
images at all still understand what a screenshot or sketch shows. Write
a description that stands on its own (what does the image/sketch convey,
not just "screenshot"). If you are a multimodal agent and encounter an
element with a missing or thin description while reading a canvas, NEVER
skip fixing it — call `kabai_update_canvas_element` to add or improve it,
the same way you would fix a stale note.

## 5. Frames replace milestones; deriving an epic from a frame

There is no milestone entity. A `frame` element groups the rough plan for
what will become one epic. To turn planning into committed work, there is
NO dedicated tool — the sequence is:

1. `kabai_create_ticket {"project_id": P, "status_id": S, "title": "...",
   "type": "epic", "docs_required": true}` — build the epic from the
   frame's content.
2. `kabai_add_canvas_element {"canvas_id": N, "type": "ref",
   "content": {"target_type": "ticket", "target_id": <new epic id>}}` —
   add a reference card back to the new epic (near or inside the frame),
   so the canvas keeps showing what it produced.
3. Cut the epic's children as usual (`kabai_link_tickets(parent_of)`,
   §"Epics" in [references/ticket-workflow.md](ticket-workflow.md)).

The frame is NOT deleted or converted — it stays as the planning context
next to the reference card. An epic derived this way still follows every
normal epic rule (docs_required, `parent_of` children, etc.).

## 6. Edges

`kabai_add_canvas_edge {"canvas_id": N, "from_element_id": A,
"to_element_id": B, "label": "..."}`. Labels are free text — there is
DELIBERATELY no fixed edge taxonomy (rough planning should not force
early categorization). Both elements MUST be on the same canvas; the
server rejects cross-canvas edges and self-edges with a speaking error.

## 7. Canvas ↔ zettelkasten

Planning that lives only on a canvas is fragile (elements move, get
deleted, get rewritten). When a piece of planning becomes a **stable,
reusable insight** — an architecture decision, a pattern worth reusing
across projects, a concept other agents should find later — write it as
a proper note ([references/docs-zettelkasten.md](docs-zettelkasten.md)),
not just a canvas text block, and add a `ref` element pointing at the new
note so the canvas keeps its context. The canvas is where ideas are
tried out; the zettelkasten is where they are kept once they hold up.

## 8. Efficiency

- `kabai_list_canvases` is a cheap overview (id, name, timestamps,
  element_count — no elements/edges/project_ids). Use it for discovery
  (§2); use `kabai_get_canvas` only when you actually need the content.
- `kabai_get_canvas` returns everything in one call — do not loop
  individual element reads.

## 9. Anti-patterns

- Creating a new canvas without checking `kabai_list_canvases` first —
  orphan duplicate planning surfaces.
- `image`/`sketch` elements with a missing, empty, or thin description —
  invisible to non-multimodal agents; NEVER skip the alt-text.
- Treating a canvas as a dumping ground for committed work — scoped,
  actionable work belongs in tickets, not text blocks.
- Deriving an epic from a frame without adding the `ref` element back —
  the canvas loses track of what it produced.
- Stable, reusable insight left only as a canvas text block instead of
  becoming a zettelkasten note.
- Cross-canvas edges or edges spanning two frames on different canvases —
  rejected by the server; do not retry blindly, re-check which canvas
  each element actually lives on.
