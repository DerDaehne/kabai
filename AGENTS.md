# AI Agents Development Guide: `kb.ai` / ForgeKan

Welcome, Agent. This repository contains the core logic for `kb.ai`, a lightweight, database-driven Kanban/Project Board engine designed specifically for Agentic AI workflows. 

Your mission is to help develop, maintain, and expand this tool. Because the system is **Database-Driven**, the majority of the business logic, state machines, and constraints live directly inside PostgreSQL. This keeps the MCP (Model Context Protocol) server lightweight, fast, and secure.

---

## Architecture Overview

The system architecture minimizes the intelligence required by the MCP layer by utilizing PostgreSQL's native capabilities (Foreign Keys, Check Constraints, and Triggers):

1. **MCP Server (The Link):** A stateless, minimal translation layer that exposes database operations as standard MCP tools.
2. **PostgreSQL (The Brain):** Houses the multi-project schema, handles the workflow graphs, enforces column rules, and blocks illegal state transitions using strict database triggers.

---

## Core Schema Structure

The database supports **multiple boards/projects** simultaneously. Every project defines its own customized columns, workflow paths, and specific instructions per status.

### 1. Projects & Statuses (`projects`, `board_statuses`)
- A `project` acts as an isolated workspace (e.g., your robot game, a backend service, etc.).
- Each project has multiple `board_statuses` (columns) ordered by `position`.
- **`agent_role_instruction`:** This column contains a dynamic prompt injection. When you inspect a ticket, this field tells you exactly what role you must adopt (e.g., Coder, Architect, Reviewer) and how to act while the ticket is in this column.

### 2. Workflows (`status_transitions`)
- Defines a directed graph of allowed column movements per project.
- If you attempt to update a ticket status through a path not defined here, the database will reject the operation with a SQL exception.

### 3. Tickets, Tasks & Artifacts (`tickets`, `ticket_tasks`, `ticket_documents`, `ticket_comments`)
- **Tickets:** Contain titles, descriptions, status tracking, and assignees (`assignee`).
- **Tasks (Acceptance Criteria):** Actionable atomic subtasks. **Crucial rule:** A ticket cannot move to a status named `done` if there are any uncompleted tasks linked to it.
- **Documents:** External references (Markdown specs, Codeberg URLs, design asset paths).
- **Comments:** Collaboration logs for agent-to-agent or agent-to-human communication.

---

## How You (The Agent) Should Work with this Repository

When you are assigned to implement features or fix bugs within this codebase, always utilize the following multi-step workflow loops:

### 1. The Alignment Loop (Planning Phase)
- **Action:** Query the board to view open tickets.
- **Context:** Fetch ticket details along with its current `agent_role_instruction`.
- **Execution:** Adopt the requested persona immediately. Read any referenced files listed in `ticket_documents`.

### 2. The Implementation Loop (Coding Phase)
- **Action:** Assign the ticket to yourself by updating the `assignee` field.
- **Execution:** Work on your local workspace files. Write tests and clean up implementations.
- **Feedback:** As you clear checkpoints, mark individual subtasks (`ticket_tasks`) as completed.

### 3. The Transition Loop (Handoff Phase)
- **Action:** Attempt to move the ticket to the next status (e.g., from `in_progress` to `review`).
- **Error Handling:** If PostgreSQL returns an error (e.g., *"Illegal Kanban-Move"* or *"Open acceptance criteria"*), do not ignore it. It means you missed a validation rule. Read the exception text, correct your work, complete the tasks, and try again.
- **Communication:** Always document your changes by adding a professional, concise comment to the ticket before handing it off to the next agent or human.

---

## Database Schema Inspections
Before generating code or writing queries, make sure you conform strictly to the migration schemas specified in the `migrations/` directory. Never attempt to bypass database triggers; use the provided tool wrappers instead.
