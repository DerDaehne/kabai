# kb.ai / ForgeKan

> **Database-Driven Kanban/Project Board Engine for Agentic AI Workflows**

`kb.ai` ist ein leichtgewichtiges, datenbankbasiertes Kanban-System, das speziell für agentische AI-Workflows entwickelt wurde. Das System nutzt **C** mit **PostgreSQL** (libpq) und wird über **Nix Flakes** gebaut.

## Architektur

- **MCP Server (C)**: Stateful translation layer, die Datenbankoperationen als MCP-Tools verfügbar macht
- **PostgreSQL**: Beherbergt das Multi-Project-Schema, verwaltet Workflow-Graphen, erzwingt Spaltenregeln und blockiert illegale Zustandsübergänge durch strenge Datenbank-Trigger

## Voraussetzungen

- **Nix** mit Flakes Support (`nix --experimental-features 'nix-command flakes'`)
- PostgreSQL 14+

## schnelleinstieg

### Entwicklungsumgebung aufbauen

```bash
# Entwicklungsshell mit allen Abhängigkeiten
nix develop

# Oder direkt bauen
nix build
```

### Datenbank-Schema anlegen

```bash
psql -U postgres -f migrations/V1__Initial_Multi_Project_Kanban_Schema.sql
```

### Server starten

```bash
nix run . -- start
```

## Projektstruktur

```
kb.ai/
├── src/                    # C-Quellcode
│   ├── main.c              # Haupteinstiegspunkt
│   ├── db/                 # Datenbank-Anbindung (libpq)
│   │   ├── connection.c
│   │   └── connection.h
│   ├── kanban/             # Kanban-Logik
│   │   ├── tickets.c
│   │   ├── tickets.h
│   │   ├── projects.c
│   │   └── projects.h
│   └── mcp/                # MCP-Server-Implementierung
│       ├── server.c
│       └── server.h
├── include/                # Public Header
├── migrations/             # Datenbank-Migrationen
│   └── V1__Initial_Multi_Project_Kanban_Schema.sql
├── flake.nix               # Nix Flake Build-Konfiguration
├── AGENTS.md               # Agent-Anweisungen
├── README.md
└── .gitignore
```

## Workflow

siehe [AGENTS.md](AGENTS.md) für detaillierte Anweisungen zur Zusammenarbeit mit diesem Repository.
