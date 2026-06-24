# Projektstatus: kb.ai / ForgeKan

> **Stand: 24. Juni 2026**  
> **Version: 0.1.1 (Code-Review & Aufräumarbeiten)**

Dieses Dokument beschreibt den aktuellen Entwicklungsstand des `kb.ai`-Projekts.

---

## Projekt-Ziel

**kb.ai ist primär ein MCP-Server** mit PostgreSQL als Backend. Das Tool exponiert Kanban-Operationen als MCP-Tools für Agentic AI Workflows.

---

## Zusammenfassung

| Kategorie | Status | Fortschritt |
|-----------|--------|-------------|
| **Projekt-Infrastruktur** | ✅ Fertig | 100% |
| **Datenbank-Schema** | ✅ Fertig | 100% |
| **C-Kern-Implementierung** | 🟡 Teilweise | ~85% |
| **Build-System (Nix Flake)** | ✅ Fertig | 100% |
| **MCP-Server** | ✅ Fertig | 100% |
| **Statisch gelinkte Binaries** | ❌ Nicht begonnen | 0% |

---

## Implementierte Features

### ✅ Vollständig umgesetzt

#### 1. Projekt-Infrastruktur
- [x] Git-Repository initialisiert
- [x] Verzeichnisstruktur gemäß AGENTS.md
- [x] `.gitignore` für C-Projekte
- [x] `README.md` mit Projektbeschreibung
- [x] `AGENTS.md` mit Workflow-Regeln (vorhanden)
- [x] `docs/PROJECT_STATUS.md` mit aktuellem Stand

#### 2. Datenbank-Schema (PostgreSQL)
- [x] **Projects-Tabelle**: Projektverwaltung mit slug, name, description
- [x] **Board_Statuses-Tabelle**: Status-Spalten pro Projekt mit Position und Agent-Rollen
- [x] **Status_Transitions-Tabelle**: Workflow-Graph (erlaubte Übergänge)
- [x] **Tickets-Tabelle**: Ticket-Daten mit Projekt- und Status-Referenz
- [x] **Ticket_Tasks-Tabelle**: Akzeptanzkriterien/Subtasks
- [x] **Ticket_Documents-Tabelle**: Externe Referenzen (Markdown, URLs)
- [x] **Ticket_Comments-Tabelle**: Kollaborations-Logs
- [x] **Ticket_Dependencies-Tabelle**: Blocker-Beziehungen
- [x] **Trigger `enforce_kanban_workflow_integrity`**: Validiert alle Regeln:
  - Erzwingt erlaubte Status-Übergänge (Workflow-Graph)
  - Blockiert "done" bei unvollständigen Tasks

#### 3. Build-System (Nix Flake)
- [x] Flake-Konfiguration mit Abhängigkeiten
- [x] `gcc` als C-Compiler
- [x] `postgresql` (libpq) als Datenbank-Client
- [x] `cjson` für JSON-Parsing
- [x] Entwicklungsshell (`nix develop`)
- [x] Build-Pipeline (`nix build`)
- [x] Static build package (`.#static`) - **Build funktioniert, Static Linken noch nicht vollständig**
- [x] Run-Konfiguration (`nix run .`)
- [x] flake.lock generiert

#### 4. C-Kern-Implementierung

**Datenbank-Anbindung (`src/db/`, `include/db/`)**
- [x] `connection.h/c`: Verbindungshandling mit libpq
- [x] `db_connect()`: Verbindung zur PostgreSQL-DB
- [x] `db_disconnect()`: Verbindungsschließung
- [x] `db_query()`: SQL-Abfragen ausführen
- [x] `db_is_connected()`: Statusprüfung
- [x] **`transaction.h/c`**: Transaktionsmanagement
  - `db_begin_transaction()` / `db_commit_transaction()` / `db_rollback_transaction()`
  - `db_execute_transaction()` für atomare Operationen
  - `db_in_transaction()` für Statusprüfung

**Kanban-Logik (`src/kanban/`, `include/kanban/`)**
- [x] **Projects-Modul**:
  - `project_create()`: Projekt anlegen
  - `project_get_by_id()`: Projekt nach ID
  - `project_get_by_slug()`: Projekt nach Slug
  - `project_list_all()`: Alle Projekte auflisten
  - `project_free()`: Speicherfreigabe
- [x] **Tickets-Modul**:
  - `ticket_create()`: Ticket anlegen
  - `ticket_get_by_id()`: Ticket nach ID
  - `ticket_list_by_project()`: Tickets nach Projekt
  - `ticket_update_status()`: Status ändern
  - `ticket_assign()`: Ticket zuweisen
  - `ticket_add_task()`: Task hinzufügen
  - `ticket_complete_task()`: Task abschließen
  - `ticket_get_tasks()`: Tasks eines Tickets
  - Speicherfreigabe-Funktionen

**MCP-Server (`src/main.c`)**
- [x] MCP-Protokoll Grundgerüst (STDIO)
- [x] JSON-Escape-Funktionen
- [x] Tool-Dispatcher für kb.ai_* Tools
- [x] **14 MCP-Tools** implementiert:
  - `kb.ai_create_project`, `kb.ai_list_projects`, `kb.ai_get_project`
  - `kb.ai_create_ticket`, `kb.ai_list_tickets`, `kb.ai_get_ticket`
  - `kb.ai_move_ticket`, `kb.ai_assign_ticket`
  - `kb.ai_add_task`, `kb.ai_complete_task`
  - **NEU:** `kb.ai_update_ticket` (Titel & Beschreibung editieren)
  - **NEU:** `kb.ai_get_ticket_detailed` (mit Tasks & Kommentaren)
  - **NEU:** `kb.ai_add_comment` (Work-Log Eintrag hinzufügen)
  - **NEU:** `kb.ai_list_comments` (Work-Log anzeigen)
- [x] Umgebungsvariablen für DB-Konfiguration
- [x] Server-Info Ausgabe bei Start

**Kommentare/Work-Log (`src/kanban/comments.c`, `include/kanban/comments.h`)**
- [x] `comment_add()`: Kommentar hinzufügen
- [x] `comment_list_by_ticket()`: Alle Kommentare eines Tickets
- [x] `comment_get_by_id()`: Einzelner Kommentar
- [x] `comment_update()`: Kommentar bearbeiten
- [x] `comment_delete()`: Kommentar löschen
- [x] Speicherfreigabe-Funktionen

**Ticket-Editing (`src/kanban/tickets.c`)**
- [x] `ticket_update_title()`: Ticket-Titel ändern
- [x] `ticket_update_description()`: Ticket-Beschreibung ändern
- [x] `ticket_get_detailed()`: Ticket mit Tasks + Kommentaren
- [x] `TicketDetailed` Struct für comprehensive Antworten

---

## Teilweise umgesetzt / In Arbeit

### 🟡 C-Kern-Implementierung (~85%)

**Fehlend:**
- [ ] Status-Transitions abfragen/validieren
- [ ] Board_Statuses CRUD-Operationen
- [ ] Ticket_Dependencies (Blocker) Logik
- [ ] Ticket_Documents CRUD
- [ ] Proper JSON-Parser (cJSON/jansson) - **✅ cJSON integriert**

**Implementiert:**
- [x] Ticket Editing (Titel, Beschreibung)
- [x] Kommentare/Work-Log CRUD
- [x] Detailed Ticket View (mit Tasks + Kommentaren)
- [x] Transaktionsmanagement (BEGIN/COMMIT/ROLLBACK)
- [x] Robuste Fehlerbehandlung für DB-Operationen

### ✅ MCP-Server (~95%)

**Implementiert in `src/main.c`:**
- [x] MCP-Protokoll-Parser (JSON-RPC über STDIN/STDOUT)
- [x] Tool-Dispatcher für kb.ai-Tools
- [x] **Serialisierung/Deserialisierung mit cJSON**
- [x] Connection-Lifecycle-Management
- [x] Server-Info Ausgabe
- [x] **Robustes Error-Handling mit PQerrorMessage**
- [x] **Input-Validierung mit cJSON Type-Checking**

**Fehlend:**
- [ ] Nachrichtensignatur für Sicherheitscheck (optional)
- [ ] Rate Limiting (optional)

---

## Nicht umgesetzt

### ❌ Statisch gelinkte Binaries (0%)

**Fehlend:**
- [ ] Nix Flake für statisches Linken konfigurieren
- [ ] Cross-Compilation für Linux (x86_64, aarch64)
- [ ] Cross-Compilation für macOS (x86_64, arm64)
- [ ] Cross-Compilation für Windows (mingw)
- [ ] Release-Pipeline für statische Binaries

**Hinweis:** Docker-Container sind **ausdrücklich ausgeschlossen** per Projektvorgabe.

### ❌ HTTP-API / Web-Interface (0%)

**Nicht geplant** - Das Projekt ist ein reiner MCP-Server. Keine HTTP-Schnittstelle notwendig.

### ❌ Persistenz-Schicht Optimierungen (0%)

- [ ] Connection Pooling
- [ ] Prepared Statements für Performance
- [ ] Result-Caching

### ❌ Tests (0%)

- [ ] Unit-Tests für Datenbank-Operationen
- [ ] Integrationstests für Workflow-Validierung
- [ ] Test-Datenbank-Setup

---

## Geplante Features (Roadmap)

### Phase 1: MCP-Server Kern (Priorität: Hoch)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| MCP-Server Implementierung | JSON-RPC über STDIO | Kern-Logik |
| Error Handling | Robuste Fehlerbehandlung für DB-Operationen | Kern-Logik |
| Transaktionsmanagement | ACID-konforme Operationen | DB-Logik |
| Status-Validierung | Client-seitige Prüfung vor DB-Operationen | DB-Logik |

### Phase 2: MCP-Tools (Priorität: Hoch)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| Alle Kanban-Tools | MCP-Tools für Projects, Tickets, Tasks, Status | MCP-Server |
| Tool: kb.ai_create_project | Projekt anlegen | MCP-Server |
| Tool: kb.ai_list_projects | Projekte auflisten | MCP-Server |
| Tool: kb.ai_get_project | Projekt abrufen | MCP-Server |
| Tool: kb.ai_create_ticket | Ticket anlegen | MCP-Server |
| Tool: kb.ai_list_tickets | Tickets auflisten | MCP-Server |
| Tool: kb.ai_get_ticket | Ticket-Details | MCP-Server |
| Tool: kb.ai_get_ticket_detailed | Ticket mit Tasks & Work-Log | MCP-Server |
| Tool: kb.ai_move_ticket | Ticket-Status ändern | MCP-Server |
| Tool: kb.ai_assign_ticket | Ticket zuweisen | MCP-Server |
| Tool: kb.ai_update_ticket | Ticket bearbeiten (Titel/Beschreibung) | MCP-Server |
| Tool: kb.ai_add_task | Task hinzufügen | MCP-Server |
| Tool: kb.ai_complete_task | Task abschließen | MCP-Server |
| Tool: kb.ai_add_comment | Work-Log Eintrag hinzufügen | MCP-Server |
| Tool: kb.ai_list_comments | Work-Log anzeigen | MCP-Server |

### Phase 3: Erweiterte Features (Priorität: Mittel)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| Ticket Dependencies | Blocker-Beziehungen verwalten | Kern-Logik |
| Dokumente/Artifacts | Datei-URLs verknüpfen | Kern-Logik |
| Kommentare | Kollaborations-Logs | Kern-Logik |
| Projektspezifische Regeln | Dynamische Agent-Rollen pro Status | Kern-Logik |

### Phase 4: Qualitätssicherung (Priorität: Mittel)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| Unit-Tests | C-Tests mit Check oder ähnlich | Kern-Logik |
| Integrationstests | End-to-End Tests | MCP-Server |
| CI/CD Pipeline | Automatisierte Tests | Tests |

### Phase 5: Release Engineering (Priorität: Mittel)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| Statisches Linken | Vollständig statisch gelinkte Binaries | Build-System |
| Cross-Compilation | Binaries für verschiedene Plattformen | Build-System |
| Connection Pooling | Wiederverwendung von DB-Verbindungen | Kern-Logik |
| Prepared Statements | Performance-Optimierung | Kern-Logik |

### Phase 6: Performance & Skalierung (Priorität: Niedrig)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| Async I/O | Nicht-blockierende Operationen | Kern-Logik |

---

## Aktuelle Einschränkungen

### ⚠️ Bekannte Probleme

1. **Kein Connection Pooling**
   - Jede Nutzung öffnet eine neue Verbindung
   - **Lösung:** Connection-Pool implementieren (Phase 5)

2. **Keine Tests**
   - Unit- und Integrationstests fehlen vollständig
   - **Lösung:** Test-Framework einrichten (Phase 4)

### ✅ Behobene Probleme (v0.1.1)

1. **SQL Schema FOREIGN KEY Typkonflikt** — `status_transitions` referenzierte `board_statuses(project_id, name)` statt `(project_id, id)` (INT vs VARCHAR) → Migration fehlgeschlagen
2. **Use-After-Free in `db_check_result`** — `PQclear(res)` gefolgt von `return PQresultErrorMessage(res)` (dangling pointer)
3. **Double-Free in `db_query_checked`** — Gab gefreeten `PGresult*` an Aufrufer zurück
4. **`db_in_transaction` benötigte Superuser** — Query auf `pg_stat_activity` ersetzt durch `PQtransactionStatus()`
5. **Inkonsistente Include-Pfade** — relativer `"../include/"` vs `-I.`-basierte Pfade vereinheitlicht
6. **Redundanter Dateiname** — `migrations/migrations_V1__...sql` → `migrations/V1__...sql`
7. **Fehlerbehandlung `mcp_move_ticket`** — `PQerrorMessage` wurde durch `SELECT 1` überschrieben
8. **Fehlende Validierung** — `ticket_create` prüft jetzt Existenz von project_id/status_id vor INSERT
9. **Fehler-Logging** — `db_query` logged jetzt Fehler auf stderr

### ⚠️ Build-Issues

1. **Nix Flake Test nicht durchgeführt**
   - Flake wurde erstellt, aber nicht getestet
   - **Aktion:** `nix build` ausführen

2. **PostgreSQL Entwicklungsheader**
   - Flake nutzt `${pkgs.postgresql.dev}/include`
   - Muss verifiziert werden

---

## Entwicklungs-Prioritäten

### ✅ Abgeschlossen (für erste Nutzung)

1. **✅ MCP-Server implementieren** (`src/main.c`) 
   - Grundgerüst mit cJSON
   - Tool-Dispatcher implementiert
   - Alle 14 kb.ai_* Tools funktionell
2. **✅ Proper JSON-Parser integrieren** (cJSON)
3. **✅ Build mit Nix testen** - `nix build` funktioniert
4. **✅ Fehlerbehandlung für DB-Operationen verbessern**
5. **✅ Transaktionsmanagement implementiert**

### 📅 Nächstes Sprint (1-2 Wochen)

1. **Static Build komplett konfigurieren** (.#static)
2. **Status-Transitions client-seitig validieren**
3. **Board_Statuses CRUD implementieren**
4. **Unit-Tests schreiben**

### 📅 Nächstes Sprint (1-2 Wochen)

1. **Alle MCP-Tools implementieren** (kb.ai_*)
2. **Transaktionsmanagement**
3. **Statisches Linken in Nix Flake konfigurieren**

### 🎯 Zukunft

1. **Tests schreiben**
2. **Performance-Optimierungen**
3. **Erweiterte Features** (Dokumente, Kommentare, Dependencies)
4. **Cross-Compilation für alle Zielplattformen**

---

## Test-Anleitung

### Manuelles Testen

```bash
# 1. Datenbank vorbereiten
psql -U postgres -c "CREATE DATABASE kb_ai;"
psql -U postgres -d kb_ai -f migrations/V1__Initial_Multi_Project_Kanban_Schema.sql

# 2. Programm bauen (nach Nix Setup)
nix build

# 3. Test-Daten anlegen (manuell über psql)
psql -U postgres -d kb_ai

# 4. Programm testen
./result/bin/kbai start
```

### Automatisiertes Testen

- [ ] Unit-Tests: Nicht verfügbar
- [ ] Integrationstests: Nicht verfügbar
- [ ] CI/CD: Nicht konfiguriert

---

## Kontakte & Verantwortlichkeiten

| Bereich | Verantwortlich | Status |
|---------|----------------|--------|
| Projekt-Architektur | David | ✅ Aktiv |
| C-Implementierung | Vibe | ✅ Aktiv |
| Datenbank-Design | Vibe | ✅ Fertig |
| Build-System | Vibe | ✅ Fertig |

---

## Changelog

| Datum | Version | Änderungen |
|-------|---------|------------|
| 24.06.2026 | 0.1.0 | Initial Setup: C-Projektstruktur, Nix Flake, DB-Schema |
| 24.06.2026 | 0.1.1 | Code-Review: 9 Bugs/Issues gefixt, Struktur aufgeräumt, .editorconfig |

---

## Offene Fragen

1. **Test-Framework** — Noch kein Framework für C-Unit-Tests festgelegt

## Entscheidungen

| Entscheidung | Wert | Begründung |
|--------------|------|------------|
| **Primärer Fokus** | MCP-Server | Projektvorgabe |
| **Backend** | PostgreSQL | Vorhandenes Schema |
| **MCP-Tool-Name** | `kb.ai` | Projektvorgabe |
| **Release-Format** | Statisch gelinkte Binaries | Cross-Plattform Kompatibilität |
| **Docker-Container** | ❌ Ausgeschlossen | Projektvorgabe |
| **Kommunikation** | STDIO (JSON-RPC) | MCP-Standard |
| **Umgebungsvariablen** | `KB_AI_DB_*` | Konfigurierbare DB-Verbindung |

---

*Dokument generiert am 24. Juni 2026*  
*Letzte Aktualisierung: Initialer Stand*
