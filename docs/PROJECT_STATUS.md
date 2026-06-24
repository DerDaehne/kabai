# Projektstatus: kb.ai / ForgeKan

> **Stand: 24. Juni 2026**  
> **Version: 0.1.0 (Initial Setup)**

Dieses Dokument beschreibt den aktuellen Entwicklungsstand des `kb.ai`-Projekts.

---

## Zusammenfassung

| Kategorie | Status | Fortschritt |
|-----------|--------|-------------|
| **Projekt-Infrastruktur** | ✅ Fertig | 100% |
| **Datenbank-Schema** | ✅ Fertig | 100% |
| **C-Kern-Implementierung** | 🟡 Teilweise | ~60% |
| **Build-System (Nix Flake)** | ✅ Fertig | 100% |
| **MCP-Server** | ❌ Nicht begonnen | 0% |
| **API/CLI-Schnittstelle** | ❌ Nicht begonnen | 0% |

---

## Implementierte Features

### ✅ Vollständig umgesetzt

#### 1. Projekt-Infrastruktur
- [x] Git-Repository initialisiert
- [x] Verzeichnisstruktur gemäß AGENTS.md
- [x] `.gitignore` für C-Projekte
- [x] `README.md` mit Projektbeschreibung
- [x] `AGENTS.md` mit Workflow-Regeln (vorhanden)

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
- [x] Entwicklungsshell (`nix develop`)
- [x] Build-Pipeline (`nix build`)
- [x] Run-Konfiguration (`nix run . -- start`)

#### 4. C-Kern-Implementierung

**Datenbank-Anbindung (`src/db/`, `include/db/`)**
- [x] `connection.h/c`: Verbindungshandling mit libpq
- [x] `db_connect()`: Verbindung zur PostgreSQL-DB
- [x] `db_disconnect()`: Verbindungsschließung
- [x] `db_query()`: SQL-Abfragen ausführen
- [x] `db_is_connected()`: Statusprüfung

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

---

## Teilweise umgesetzt / In Arbeit

### 🟡 C-Kern-Implementierung (60%)

**Fehlend:**
- [ ] Status-Transitions abfragen/validieren
- [ ] Board_Statuses CRUD-Operationen
- [ ] Ticket_Dependencies (Blocker) Logik
- [ ] Ticket_Documents CRUD
- [ ] Ticket_Comments CRUD
- [ ] Fehlerbehandlung für SQL-Exceptions (z.B. "Illegaler Kanban-Move")
- [ ] Transaktionsmanagement

### 🟡 Command-Line-Interface (0%)

**Geplant in `src/main.c`:**
- [ ] `init-db`: Datenbank-Schema anlegen
- [ ] `list-projects`: Projekte auflisten
- [ ] `create-project`: Projekt anlegen
- [ ] `list-tickets`: Tickets auflisten
- [ ] `show-ticket`: Ticket-Details anzeigen
- [ ] `create-ticket`: Ticket anlegen
- [ ] `move-ticket`: Ticket-Status ändern
- [ ] `assign-ticket`: Ticket zuweisen

---

## Nicht umgesetzt

### ❌ MCP-Server (0%)

**Fehlend:**
- [ ] MCP-Protokoll-Implementierung (JSON-RPC über STDIO)
- [ ] Tool-Definitionen für MCP
- [ ] MCP-Server-Loop
- [ ] integration mit Datenbank-Logik

**Geplant:**
- Separates Modul in `src/mcp/`
- Tools für: Projects, Tickets, Tasks, Status, Transitions

### ❌ HTTP-API / Web-Interface (0%)

**Nicht geplant für v0.1**, aber zukünftig möglich:
- REST-API mit libmicrohttpd oder ähnlich
- WebSocket für Echtzeit-Updates

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

### Phase 1: Kernfunktionalität (Priorität: Hoch)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| CLI-Befehle | Grundlegende CLI-Operationen | - |
| Error Handling | Robuste Fehlerbehandlung für DB-Operationen | CLI |
| Transaktionsmanagement | ACID-konforme Operationen | DB-Logik |
| Status-Validierung | Client-seitige Prüfung vor DB-Operationen | DB-Logik |

### Phase 2: MCP-Integration (Priorität: Hoch)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| MCP-Server | JSON-RPC STDIO-Server | Kern-Logik |
| MCP-Tools | Alle Kanban-Operationen als MCP-Tools | MCP-Server |
| MCP-Tool Wrappers | Helfer für häufige Operationen | MCP-Tools |

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
| Integrationstests | End-to-End Tests | CLI, MCP |
| CI/CD Pipeline | Automatisierte Tests | Tests |

### Phase 5: Performance & Skalierung (Priorität: Niedrig)

| Feature | Beschreibung | Abhängigkeiten |
|---------|--------------|----------------|
| Connection Pooling | Wiederverwendung von DB-Verbindungen | Kern-Logik |
| Prepared Statements | Performance-Optimierung | Kern-Logik |
| Async I/O | Nicht-blockierende Operationen | Kern-Logik |

---

## Aktuelle Einschränkungen

### ⚠️ Bekannte Probleme

1. **Keine Fehlerbehandlung für SQL-Exceptions**
   - Der Trigger wirft Exceptions (z.B. "Illegaler Kanban-Move")
   - Die C-Implementierung fängt diese nicht ab
   - **Lösung:** PQresult Error-Messages parsen

2. **Keine Validierung vor DB-Operationen**
   - Client prüft nicht, ob Status-Übergang erlaubt ist
   - **Lösung:** Status-Transitions vor Update abfragen

3. **Kein Connection Pooling**
   - Jede Operation öffnet neue Verbindung
   - **Lösung:** Connection-Pool implementieren

4. **Keine Memory-Safety Checks**
   - Keine NULL-Checks in allen Pfaden
   - **Lösung:** Defensive Programmierung

5. **Keine Transaktionen**
   - Einzelne Operationen sind nicht atomar
   - **Lösung:** BEGIN/COMMIT/ROLLBACK

### ⚠️ Build-Issues

1. **Nix Flake Test nicht durchgeführt**
   - Flake wurde erstellt, aber nicht getestet
   - **Aktion:** `nix build` ausführen

2. **PostgreSQL Entwicklungsheader**
   - Flake nutzt `${pkgs.postgresql.dev}/include`
   - Muss verifiziert werden

---

## Entwicklungs-Prioritäten

### 🔥 Sofort (für erste Nutzung)

1. **CLI-Befehle implementieren** (`src/main.c`)
2. **Fehlerbehandlung für DB-Operationen**
3. **Build mit Nix testen**

### 📅 Nächstes Sprint (1-2 Wochen)

1. **MCP-Server implementieren**
2. **MCP-Tools definieren**
3. **Transaktionsmanagement**

### 🎯 Zukunft

1. **Tests schreiben**
2. **Performance-Optimierungen**
3. **Erweiterte Features** (Dokumente, Kommentare, Dependencies)

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

---

## Offene Fragen

1. **Soll der MCP-Server als separater Prozess laufen oder in den CLI integriert werden?**
2. **Welche MCP-Tool-Namen sollen verwendet werden?**
3. **Sollen binäre Releases erstellt werden oder nur Source?**
4. **Soll eine Docker-Container-Option angeboten werden?**

---

*Dokument generiert am 24. Juni 2026*  
*Letzte Aktualisierung: Initialer Stand*
