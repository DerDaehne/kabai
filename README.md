# kb.ai - MCP Server

> **Database-Driven Kanban/Project Board Engine for Agentic AI Workflows**

`kb.ai` ist ein **MCP-Server** in C, der Kanban-Operationen als MCP-Tools für Agentic AI Workflows bereitstellt. Das System nutzt **PostgreSQL** (libpq) als Backend und wird über **Nix Flakes** gebaut.

## Architektur

- **MCP Server (C)**: Exponiert Datenbankoperationen als MCP-Tools (`kb.ai_*` Präfix)
- **PostgreSQL**: Beherbergt das Multi-Project-Schema, verwaltet Workflow-Graphen, erzwingt Regeln durch Trigger
- **Statisch gelinkte Binaries**: Für Cross-Plattform Releases

## Voraussetzungen

- **Nix** mit Flakes Support (`nix --experimental-features 'nix-command flakes'`)
- PostgreSQL 14+

## schnelleinstieg

### Entwicklungsumgebung aufbauen

```bash
# Entwicklungsshell mit allen Abhängigkeiten
nix develop

# Standard Build (dynamisch gelinkt)
nix build

# Statisch gelinktes Binary für Releases
nix build .#static
```

### Datenbank-Schema anlegen

```bash
# Datenbank erstellen
createdb kb_ai

# Schema migrieren
psql -U postgres -d kb_ai -f migrations/V1__Initial_Multi_Project_Kanban_Schema.sql
```

### MCP-Server starten

```bash
# Umgebungsvariablen für DB-Verbindung
KB_AI_DB_HOST=localhost \
KB_AI_DB_PORT=5432 \
KB_AI_DB_NAME=kb_ai \
KB_AI_DB_USER=postgres \
KB_AI_DB_PASSWORD=yourpassword \
nix run .
```

Der Server liest von STDIN und schreibt nach STDOUT (MCP-Protokoll über STDIO).

## Projektstruktur

```
kb.ai/
├── src/                    # C-Quellcode
│   ├── main.c              # MCP-Server Haupteinstiegspunkt
│   ├── db/                 # Datenbank-Anbindung (libpq)
│   │   ├── connection.c
│   │   └── connection.h
│   └── kanban/             # Kanban-Logik
│       ├── tickets.c       # Tickets + Editing
│       ├── tickets.h
│       ├── projects.c
│       ├── projects.h
│       ├── comments.c     # Work-Log / Kommentare
│       └── comments.h
├── include/                # Public Header
│   ├── db/
│   │   └── connection.h
│   └── kanban/
│       ├── tickets.h
│       ├── projects.h
│       └── comments.h
├── migrations/             # Datenbank-Migrationen
│   └── V1__Initial_Multi_Project_Kanban_Schema.sql
├── flake.nix               # Nix Flake Build-Konfiguration
├── docs/                   # Dokumentation
│   └── PROJECT_STATUS.md   # Aktueller Projektstatus
├── AGENTS.md               # Agent-Anweisungen
├── README.md
└── .gitignore
```

## MCP-Tools

Der Server exponiert folgende Tools mit dem Präfix `kb.ai_`:

### Projektverwaltung
| Tool | Beschreibung |
|------|--------------|
| `kb.ai_create_project` | Projekt anlegen |
| `kb.ai_list_projects` | Alle Projekte auflisten |
| `kb.ai_get_project` | Projekt-Details abrufen |

### Ticketverwaltung
| Tool | Beschreibung |
|------|--------------|
| `kb.ai_create_ticket` | Ticket anlegen |
| `kb.ai_list_tickets` | Tickets eines Projekts auflisten |
| `kb.ai_get_ticket` | Ticket-Details abrufen |
| `kb.ai_get_ticket_detailed` | **Ticket mit Tasks, Status UND Work-Log** |
| `kb.ai_move_ticket` | Ticket-Status ändern |
| `kb.ai_assign_ticket` | Ticket zuweisen |
| `kb.ai_update_ticket` | **Ticket bearbeiten (Titel/Beschreibung)** |

### Tasks (Akzeptanzkriterien)
| Tool | Beschreibung |
|------|--------------|
| `kb.ai_add_task` | Task zu Ticket hinzufügen |
| `kb.ai_complete_task` | Task abschließen |

### Work-Log / Kommentare
| Tool | Beschreibung |
|------|--------------|
| `kb.ai_add_comment` | **Work-Log Eintrag hinzufügen** |
| `kb.ai_list_comments` | **Alle Work-Log Einträge eines Tickets** |

## Workflow

siehe [AGENTS.md](AGENTS.md) für detaillierte Anweisungen zur Zusammenarbeit mit diesem Repository.

## Releases

Statisch gelinkte Binaries werden für folgende Plattformen gebaut:
- Linux x86_64
- Linux aarch64
- macOS x86_64
- macOS arm64
- Windows (über MinGW)

Build mit: `nix build .#static`
