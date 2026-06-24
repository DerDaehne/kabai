# Projektstatus: kb.ai

> **Stand: 24. Juni 2026**
> **Version: 0.2.0 — Einsatzbereit**

---

## Übersicht

`kb.ai` ist ein **MCP-Server** in C, der Kanban-Operationen als MCP-Tools für Agentic AI Workflows bereitstellt. PostgreSQL als Backend. Kommunikation über STDIO (JSON-RPC 2.0).

---

## Status

| Kategorie | Status | Fortschritt |
|---|---|---|
| Projekt-Infrastruktur | ✅ Fertig | 100% |
| Datenbank-Schema | ✅ Fertig | 100% |
| MCP-Protokoll (JSON-RPC 2.0) | ✅ Fertig | 100% |
| C-Kern-Implementierung | ✅ Fertig | 100% |
| Build-System (Nix Flake) | ✅ Fertig | 100% |
| Statisch gelinkte Binaries | ❌ Nicht begonnen | 0% |
| Tests | ❌ Nicht begonnen | 0% |

---

## MCP-Tools (18 gesamt)

### Projektverwaltung
| Tool | Beschreibung |
|---|---|
| `kb.ai_create_project` | Projekt anlegen |
| `kb.ai_list_projects` | Alle Projekte auflisten |
| `kb.ai_get_project` | Projekt-Details abrufen |

### Board-Konfiguration (neu in 0.2.0)
| Tool | Beschreibung |
|---|---|
| `kb.ai_list_board_statuses` | Spalten + `agent_role_instruction` auflisten |
| `kb.ai_create_board_status` | Neue Spalte mit Persona-Prompt anlegen |
| `kb.ai_create_status_transition` | Erlaubten Workflow-Übergang definieren |
| `kb.ai_list_status_transitions` | Workflow-Graph anzeigen |

### Ticketverwaltung
| Tool | Beschreibung |
|---|---|
| `kb.ai_create_ticket` | Ticket anlegen |
| `kb.ai_list_tickets` | Tickets eines Projekts auflisten |
| `kb.ai_get_ticket` | Ticket-Details inkl. `status_name` + `agent_role_instruction` |
| `kb.ai_get_ticket_detailed` | Ticket mit Tasks und Work-Log |
| `kb.ai_move_ticket` | Ticket-Status ändern (Workflow-Trigger greift) |
| `kb.ai_assign_ticket` | Ticket zuweisen |
| `kb.ai_update_ticket` | Titel/Beschreibung bearbeiten |

### Tasks (Akzeptanzkriterien)
| Tool | Beschreibung |
|---|---|
| `kb.ai_add_task` | Task zu Ticket hinzufügen |
| `kb.ai_complete_task` | Task abschließen |

### Work-Log
| Tool | Beschreibung |
|---|---|
| `kb.ai_add_comment` | Work-Log-Eintrag hinzufügen |
| `kb.ai_list_comments` | Alle Einträge eines Tickets |

---

## Protokoll

Der Server implementiert **MCP (Model Context Protocol)** über STDIO:

- **JSON-RPC 2.0** — `jsonrpc: "2.0"` in allen Responses
- **`initialize`** — Handshake mit `protocolVersion: "2024-11-05"`
- **`notifications/initialized`** — korrekt ignoriert (kein Response)
- **`tools/list`** — vollständige `inputSchema` (JSON Schema) für alle 18 Tools
- **`tools/call`** — Results in `content: [{type: "text", text: "..."}]` + `isError`
- **ID-Typ-Preservation** — string/number aus dem Request wird exakt gespiegelt

---

## Behobene Probleme (0.1.x → 0.2.0)

| Problem | Fix |
|---|---|
| Kein MCP-Protokoll (kein initialize, kein tools/list) | Vollständige JSON-RPC 2.0 Implementierung |
| Fester 16-KiB STDIN-Puffer | `getline()` — unbegrenzte Requestgröße |
| `PQescapeStringConn` + feste 2048-Byte Query-Buffer | `PQexecParams` — kein Buffer-Limit für TEXT-Felder |
| TOCTOU-Race in `ticket_create` | Entfernt — DB-FKs fangen das atomar ab |
| Doppeltes `updated_at` in `ticket_update_status` | Entfernt — Trigger setzt es bereits |
| `project_id=0` → leeres Array | Gibt jetzt Fehler zurück |
| `agent_role_instruction` nie zurückgegeben | `get_ticket` und `get_ticket_detailed` JOINen board_statuses |
| Keine board_statuses-API | 4 neue Tools für Board-Setup und Workflow-Konfiguration |
| Nicht-standardisierter `%%[server_info]`-Output | Entfernt |

---

## Bekannte Einschränkungen

- **Keine Tests** — Unit- und Integrationstests fehlen
- **Kein Connection Pooling** — eine persistente Verbindung pro Serverinstanz
- **Statisch gelinkte Binaries fehlen** — `nix build .#static` noch nicht konfiguriert
- **Keine Pagination** — `list_tickets` gibt alle Tickets zurück (bei großen Projekten ggf. langsam)

---

## Schnellstart

```bash
# Schema anlegen
createdb kb_ai
psql -d kb_ai -f migrations/V1__Initial_Multi_Project_Kanban_Schema.sql

# Server starten
KB_AI_DB_HOST=localhost \
KB_AI_DB_PORT=5432 \
KB_AI_DB_NAME=kb_ai \
KB_AI_DB_USER=postgres \
KB_AI_DB_PASSWORD=secret \
nix run .
```

### Claude Desktop Konfiguration

```json
{
  "mcpServers": {
    "kb.ai": {
      "command": "/path/to/kbai",
      "env": {
        "KB_AI_DB_HOST": "localhost",
        "KB_AI_DB_NAME": "kb_ai",
        "KB_AI_DB_USER": "postgres",
        "KB_AI_DB_PASSWORD": "secret"
      }
    }
  }
}
```

### Empfohlener Agent-Workflow

1. `kb.ai_list_board_statuses` — Spalten-IDs und Persona-Prompts lesen
2. `kb.ai_list_tickets` — offene Tickets sehen
3. `kb.ai_get_ticket_detailed` — Ticket mit `agent_role_instruction` und Tasks laden
4. `kb.ai_assign_ticket` — Ticket sich selbst zuweisen
5. Arbeit erledigen, Tasks per `kb.ai_complete_task` abhaken
6. `kb.ai_add_comment` — Fortschritt dokumentieren
7. `kb.ai_move_ticket` — zur nächsten Spalte (Trigger prüft Workflow + offene Tasks)

---

## Changelog

| Datum | Version | Änderungen |
|---|---|---|
| 24.06.2026 | 0.1.0 | Initial: C-Struktur, Nix Flake, DB-Schema, 14 MCP-Tools |
| 24.06.2026 | 0.1.1 | 9 Bugs gefixt (Use-After-Free, UNIQUE-Constraint, Include-Pfade, …) |
| 24.06.2026 | 0.2.0 | MCP JSON-RPC 2.0, PQexecParams, board_statuses-API, agent_role_instruction |
