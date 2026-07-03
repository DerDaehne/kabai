# ADR 001: kbai-docs — Postgres-Zettelkasten als docs-Modul

Status: accepted · 2026-07-03 · Autor: claude-fable-5 · Ticket: kbai-docs #330

## Kontext

kbai deckt Projekt-Management ab (Tickets, Rollen, Workflow). Die zweite
Hälfte des Agenten-Kontexts — "was ist über dieses System bereits bekannt,
und wie hängt es mit dem aktuellen Ticket zusammen" — lief bisher über rohes
Lesen von Markdown-Dateien: teuer, blind (grep-Raten), ohne strukturelle
Verbindung zu Tickets, driftanfällig (belegt in der Kernel-Panic-Session
2026-07-03, siehe Epic kbai-docs #322).

## Grundsatzentscheidungen (David, 2026-07-03 — hier festgeschrieben)

1. **PostgreSQL ist Source of Truth.** Keine Markdown-Dateien im Repo, kein
   Git-Sync. Markdown-**Export** aus der DB ist Aufgabe eines anderen Tools
   (out of scope). Verworfen: Git-Markdown als Quelle mit DB-Index — hätte
   permanente Sync-/Drift-Maschinerie gebraucht (das gestrichene Ticket #334).
2. **Zettelkasten-Modell.** Atomare Notes (Titel + Markdown-Body), typisierte
   gerichtete Note↔Note-Links. Keine Datei-/Kapitel-Baumstruktur;
   Übersichtsseiten ("Hubs") sind normale Notes, deren `contains`-Links die
   Sammlung definieren. Verworfen: doc/section-Hierarchie — löst das
   Abschnitts-Abruf-Problem nur halb und bricht bei Umstrukturierung.
3. **Global mit n:m-Projektzuordnung.** Ein Zettelkasten für alle Projekte;
   Notes gehören 0..n Projekten. Verworfen: strikt projekt-scoped — dupliziert
   projektübergreifendes Wissen (Rollen-/Prozess-Notes).
4. **`kind`-Feld ohne Sonderlogik** (note | adr | hub) + freie Tags. Keine
   typspezifischen Workflows im MVP; ADR-Ablösung via `supersedes`-Link.
5. **Ein Binary.** docs ist ein Modul `src/docs/` im kbai-Server, registriert
   über das MCP-Framework (docs/MCP_FRAMEWORK_DESIGN.md, umgesetzt in
   Kanban AI #335). Gleiche DB → note_ticket_link mit echten FKs.
6. **Revisions-Historie später**, Schema vorbereitet (kbai-docs #339).

## Detailentscheidungen

### D1: Note-Adressierung — id + globaler Slug

`note.id` (serial) plus `note.slug` (TEXT, UNIQUE, global). Der Slug ist
Pflichtparameter bei `create_note` — Agenten liefern gute Slugs, und
server-seitige Transliteration (Umlaute etc.) in C lohnt nicht. Slug ist
stabiler Schlüssel und nicht änderbar; der Titel ist frei editierbar.
`get_note` akzeptiert id oder Slug.

### D2: Link-Typen für note_link

Minimalset als CHECK-Constraint (analog ticket_relations):

| Typ          | Bedeutung |
|--------------|-----------|
| `references` | generischer Verweis (Default) |
| `contains`   | Hub → Mitglied; definiert Übersichtsseiten |
| `supersedes` | Neue Fassung löst alte ab (ADR-Konvention) |
| `contradicts`| markierter Widerspruch — Review-Signal für #326 |

Erweiterung per Migration (CHECK anpassen), nicht per freiem String —
Tippfehler in Link-Typen wären sonst stille Datenfehler.

### D3: Volltextsuche — 'simple' + pg_trgm

Inhalte sind gemischt deutsch/englisch; ein Sprach-Stemmer ('german' oder
'english') verstümmelt jeweils die andere Sprache. Daher:

- Generierte Spalte `search_tsv tsvector` über title (Gewicht A), tags (B),
  body (C) mit Konfiguration **'simple'**, GIN-Index. Abfrage per
  `websearch_to_tsquery('simple', $1)`, Ranking per `ts_rank`.
- Zusätzlich **pg_trgm**-GIN auf title für Substring-/Tippfehler-Matching
  als Fallback, wenn die tsquery keine Treffer liefert.
- Verworfen: Embeddings/pgvector — erst wenn FTS+Tags nachweislich nicht
  reichen (eigenes Ticket, nicht MVP).

### D4: Tag-Modell — text[]

`note.tags TEXT[] NOT NULL DEFAULT '{}'` mit GIN-Index; Tags werden
lowercase normalisiert. Verworfen: note_tag-Tabelle — mehr Joins ohne
Mehrwert, solange Tags reine Such-/Filterkriterien sind.

### D5: Tool-Namespace

Server-intern Präfix `kb.ai_docs_` (konsistent zu `kb.ai_...`; MCP-Clients
normalisieren zu `kb_ai_docs_...`). Geplante Tools:

| Tool | Ticket | Zweck |
|------|--------|-------|
| `kb.ai_docs_create_note(slug, title, kind, body, tags?, project_ids?)` | #332 | Note anlegen |
| `kb.ai_docs_update_note(note_id, title?, body?, kind?, tags?)` | #332 | Felder gezielt ändern |
| `kb.ai_docs_archive_note(note_id, reason)` | #332 | Soft-Delete |
| `kb.ai_docs_link_notes(from_note_id, to_note_id, link_type)` / `unlink_notes` | #332 | Note↔Note |
| `kb.ai_docs_assign_project(note_id, project_id)` / `unassign_project` | #332 | n:m Projekt |
| `kb.ai_docs_get_note(note_id \| slug)` | #324 | Body + Link-Nachbarschaft (Metadaten) |
| `kb.ai_docs_list_notes(project_id?, kind?, tag?, summary?, limit?, offset?)` | #324 | Übersicht |
| `kb.ai_docs_search(query, project_id?, kind?, tag?)` | #323 | FTS, Treffer mit body_chars |
| `kb.ai_docs_link_ticket(note_id, ticket_id, relation)` / `unlink_ticket` | #325 | Note↔Ticket |
| `kb.ai_docs_verify_note(note_id, ticket_id)` | #326 | last_verified setzen |
| `kb.ai_docs_suggest_for_ticket(ticket_id)` | #327 | Vorschläge |
| `kb.ai_docs_get_note_history(note_id)` | #339 | später (Revisionen) |

note_ticket_link-Relationen: `documents | created_by | verified_by |
references` (CHECK-Constraint, analog D2).

### D6: Ablageort dieses ADRs (Bootstrapping)

Bis kbai-docs läuft: Markdown unter docs/adr/ im kb.ai-Repo. Nach dem
Alt-Import (#333) wird dieses ADR als eine der ersten Notes in den
Zettelkasten übernommen; ab dann entstehen kbai-Design-Docs direkt dort.

### D7 (Zusatzbefund): Legacy-Tabellen droppen

`ticket_documents` (Datei-Pfad-Links an Tickets — exakt das Konzept, das
note_ticket_link sauber ersetzt) und `ticket_dependencies` (von
ticket_relations abgelöst) sind beide leer und von keinem Code referenziert.
Die V7-Migration (#331) droppt beide, damit kein zweites, totes
"Dokumente"-Konzept neben kbai-docs existiert.

## Schema-Ableitung für #331 (Gegenprobe)

| Anforderung | Schema-Element |
|-------------|----------------|
| #323 Suche | note.search_tsv (generiert, GIN) + pg_trgm auf title |
| #324 Read/Hubs | note, note_link (contains) — Nachbarschaft per Join |
| #325 Ticket-Links | note_ticket_link (note_id FK, ticket_id FK, relation CHECK) |
| #326 Verifikation | note.last_verified_ticket_id FK, note.last_verified_at |
| #327 Vorschläge | keine eigenen Objekte (nutzt #323 + #325) |
| #328 Dokupflicht | tickets.docs_required BOOL + Prüfung in Move-Pfad (kbai-Core) |
| #339 Revisionen | note.updated_by_ticket_id jetzt; note_revision + Trigger später |
| global n:m | note_project (note_id, project_id, PK beides) |

Keine offene Frage blockiert #331.

## Effort-Einschätzung

#331 Schema: S · #332 Modul+Write-API: M · #323 Suche: S · #324 Read-API: S ·
#325 Ticket-Links: S (inkl. get_ticket_detailed-Erweiterung) · #326 Verify: XS ·
#327 Suggest: M · #328 Guard: S · #333 Import: M (Agent-Arbeit, kein Code) ·
#339 Revisionen: S
