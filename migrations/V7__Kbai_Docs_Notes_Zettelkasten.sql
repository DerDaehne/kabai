-- V7__Kbai_Docs_Notes_Zettelkasten.sql
-- kbai-docs: Zettelkasten-Wissensbasis (Postgres als Source of Truth).
-- Entscheidungen: docs/adr/001-kbai-docs-postgres-zettelkasten.md (Ticket kbai-docs #330/#331)

-- 0. pg_trgm für Substring-/Tippfehler-Suche auf Titeln (ADR D3)
CREATE EXTENSION IF NOT EXISTS pg_trgm;

-- array_to_string ist nur STABLE — für die generierte tsvector-Spalte
-- braucht es eine IMMUTABLE-Hülle (Tags ändern das Ergebnis deterministisch).
CREATE OR REPLACE FUNCTION immutable_array_to_string(text[], text)
RETURNS text AS $$ SELECT array_to_string($1, $2) $$ LANGUAGE sql IMMUTABLE;

-- 1. Notes: atomare Wissenseinheiten (ADR D1, D3, D4)
--    Der Slug ist der stabile, global eindeutige Schlüssel (nicht änderbar);
--    kind ist reines Filterkriterium ohne Sonderlogik (note | adr | hub).
--    updated_by_ticket_id bereitet die Revisions-Historie vor (#339);
--    last_verified_* trägt die Verifikations-Metadaten (#326).
CREATE TABLE IF NOT EXISTS notes (
    id                      SERIAL PRIMARY KEY,
    slug                    TEXT NOT NULL UNIQUE,
    title                   TEXT NOT NULL,
    kind                    VARCHAR(20) NOT NULL DEFAULT 'note'
        CHECK (kind IN ('note', 'adr', 'hub')),
    body                    TEXT NOT NULL DEFAULT '',
    tags                    TEXT[] NOT NULL DEFAULT '{}',
    archived                BOOLEAN NOT NULL DEFAULT FALSE,
    created_at              TIMESTAMP DEFAULT NOW(),
    updated_at              TIMESTAMP DEFAULT NOW(),
    updated_by_ticket_id    INT REFERENCES tickets(id) ON DELETE SET NULL,
    last_verified_ticket_id INT REFERENCES tickets(id) ON DELETE SET NULL,
    last_verified_at        TIMESTAMP,
    search_tsv              tsvector GENERATED ALWAYS AS (
        setweight(to_tsvector('simple', coalesce(title, '')), 'A') ||
        setweight(to_tsvector('simple', immutable_array_to_string(tags, ' ')), 'B') ||
        setweight(to_tsvector('simple', coalesce(body, '')), 'C')
    ) STORED
);

CREATE INDEX IF NOT EXISTS idx_notes_search_tsv  ON notes USING GIN (search_tsv);
CREATE INDEX IF NOT EXISTS idx_notes_title_trgm  ON notes USING GIN (title gin_trgm_ops);
CREATE INDEX IF NOT EXISTS idx_notes_tags        ON notes USING GIN (tags);
CREATE INDEX IF NOT EXISTS idx_notes_kind        ON notes (kind);

-- updated_at automatisch pflegen (analog zum tickets-Trigger)
CREATE OR REPLACE FUNCTION touch_note_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS notes_touch_updated_at ON notes;
CREATE TRIGGER notes_touch_updated_at
BEFORE UPDATE ON notes
FOR EACH ROW EXECUTE FUNCTION touch_note_updated_at();

-- 2. Note↔Note-Links: gerichteter, typisierter Graph (ADR D2).
--    Hubs/Übersichtsseiten sind normale Notes, deren 'contains'-Links die
--    Sammlung definieren — keine eigene Strukturtabelle.
CREATE TABLE IF NOT EXISTS note_links (
    id           SERIAL PRIMARY KEY,
    from_note_id INT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    to_note_id   INT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
    link_type    VARCHAR(20) NOT NULL
        CHECK (link_type IN ('references', 'contains', 'supersedes', 'contradicts')),
    created_at   TIMESTAMP DEFAULT NOW(),
    UNIQUE (from_note_id, to_note_id, link_type),
    CONSTRAINT check_note_not_self_linked CHECK (from_note_id <> to_note_id)
);

CREATE INDEX IF NOT EXISTS idx_note_links_from ON note_links (from_note_id);
CREATE INDEX IF NOT EXISTS idx_note_links_to   ON note_links (to_note_id);

-- 3. Note↔Projekt: n:m — der Zettelkasten ist global, Notes gehören
--    0..n Projekten (ADR Grundsatzentscheidung 3).
CREATE TABLE IF NOT EXISTS note_projects (
    note_id    INT NOT NULL REFERENCES notes(id)    ON DELETE CASCADE,
    project_id INT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    PRIMARY KEY (note_id, project_id)
);

CREATE INDEX IF NOT EXISTS idx_note_projects_project ON note_projects (project_id);

-- 4. Note↔Ticket: strukturierte, bidirektional abfragbare Verknüpfung (#325);
--    Grundlage für Vorschläge (#327) und Dokupflicht-Guard (#328).
CREATE TABLE IF NOT EXISTS note_ticket_links (
    id         SERIAL PRIMARY KEY,
    note_id    INT NOT NULL REFERENCES notes(id)   ON DELETE CASCADE,
    ticket_id  INT NOT NULL REFERENCES tickets(id) ON DELETE CASCADE,
    relation   VARCHAR(20) NOT NULL
        CHECK (relation IN ('documents', 'created_by', 'verified_by', 'references')),
    created_at TIMESTAMP DEFAULT NOW(),
    UNIQUE (note_id, ticket_id, relation)
);

CREATE INDEX IF NOT EXISTS idx_note_ticket_links_ticket ON note_ticket_links (ticket_id);

-- 5. Legacy-Aufräumen (ADR D7): beide Tabellen sind leer und von keinem
--    Code referenziert. ticket_documents (Datei-Pfad-Links) wird durch
--    note_ticket_links ersetzt; ticket_dependencies wurde von
--    ticket_relations (V4) abgelöst.
DROP TABLE IF EXISTS ticket_documents;
DROP TABLE IF EXISTS ticket_dependencies;
