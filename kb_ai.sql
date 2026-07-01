--
-- PostgreSQL database dump
--

\restrict 0fcVMrfSSkfUPPdYPPNBJTejOvOS67hu9WLW1LrAS3kCXbIQoIKLDnH7cuenN4i

-- Dumped from database version 17.10
-- Dumped by pg_dump version 17.10

SET statement_timeout = 0;
SET lock_timeout = 0;
SET idle_in_transaction_session_timeout = 0;
SET transaction_timeout = 0;
SET client_encoding = 'SQL_ASCII';
SET standard_conforming_strings = on;
SELECT pg_catalog.set_config('search_path', '', false);
SET check_function_bodies = false;
SET xmloption = content;
SET client_min_messages = warning;
SET row_security = off;

--
-- Name: notify_ticket_change(); Type: FUNCTION; Schema: public; Owner: david
--

CREATE FUNCTION public.notify_ticket_change() RETURNS trigger
    LANGUAGE plpgsql
    AS $$
DECLARE
    rec RECORD;
BEGIN
    IF TG_OP = 'DELETE' THEN rec := OLD; ELSE rec := NEW; END IF;
    PERFORM pg_notify(
        'tickets_' || rec.project_id::text,
        json_build_object(
            'op',         TG_OP,
            'ticket_id',  rec.id,
            'status_id',  rec.status_id,
            'project_id', rec.project_id
        )::text
    );
    IF TG_OP = 'DELETE' THEN RETURN OLD; ELSE RETURN NEW; END IF;
END;
$$;


ALTER FUNCTION public.notify_ticket_change() OWNER TO david;

--
-- Name: verify_kanban_rules_and_transitions(); Type: FUNCTION; Schema: public; Owner: david
--

CREATE FUNCTION public.verify_kanban_rules_and_transitions() RETURNS trigger
    LANGUAGE plpgsql
    AS $$
DECLARE
    target_status_name VARCHAR(50);
    open_tasks_count INT;
    is_initial_insert BOOLEAN := (TG_OP = 'INSERT');
BEGIN
    -- Context Check: If it's an update and the status hasn't changed, bypass transition rules
    IF NOT is_initial_insert AND OLD.status_id = NEW.status_id THEN
        RETURN NEW;
    END IF;

    -- Rule A: If it's an update, enforce the explicit Workflow Graph state machine
    IF NOT is_initial_insert THEN
        IF NOT EXISTS (
            SELECT 1 FROM status_transitions 
            WHERE project_id = NEW.project_id 
              AND from_status_id = OLD.status_id 
              AND to_status_id = NEW.status_id
        ) THEN
            RAISE EXCEPTION 'Illegaler Kanban-Move (Projekt %): Ein direkter Uebergang von Status-ID % zu Status-ID % ist laut Workflow-Definition nicht erlaubt.', 
                NEW.project_id, OLD.status_id, NEW.status_id;
        END IF;
    END IF;

    -- Rule B: Enforce Acceptance Criteria check when a ticket heads towards any 'done' column
    SELECT name INTO target_status_name 
    FROM board_statuses 
    WHERE id = NEW.status_id;
    
    IF target_status_name = 'done' THEN
        SELECT COUNT(*) INTO open_tasks_count 
        FROM ticket_tasks 
        WHERE ticket_id = NEW.id AND is_completed = FALSE;
        
        IF open_tasks_count > 0 THEN
            RAISE EXCEPTION 'Kanban-Validierungsfehler: Ticket #% kann nicht geschlossen werden, da noch % Akzeptanzkriterium/Kriterien ungeloest sind.', 
                NEW.id, open_tasks_count;
        END IF;
    END IF;

    -- Timestamp management
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$;


ALTER FUNCTION public.verify_kanban_rules_and_transitions() OWNER TO david;

SET default_tablespace = '';

SET default_table_access_method = heap;

--
-- Name: board_statuses; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.board_statuses (
    id integer NOT NULL,
    project_id integer NOT NULL,
    name character varying(50) NOT NULL,
    display_name character varying(100) NOT NULL,
    "position" integer NOT NULL,
    agent_role_instruction text,
    created_at timestamp without time zone DEFAULT now()
);


ALTER TABLE public.board_statuses OWNER TO david;

--
-- Name: board_statuses_id_seq; Type: SEQUENCE; Schema: public; Owner: david
--

CREATE SEQUENCE public.board_statuses_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


ALTER SEQUENCE public.board_statuses_id_seq OWNER TO david;

--
-- Name: board_statuses_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: david
--

ALTER SEQUENCE public.board_statuses_id_seq OWNED BY public.board_statuses.id;


--
-- Name: projects; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.projects (
    id integer NOT NULL,
    slug character varying(50) NOT NULL,
    name character varying(100) NOT NULL,
    description text,
    created_at timestamp without time zone DEFAULT now()
);


ALTER TABLE public.projects OWNER TO david;

--
-- Name: projects_id_seq; Type: SEQUENCE; Schema: public; Owner: david
--

CREATE SEQUENCE public.projects_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


ALTER SEQUENCE public.projects_id_seq OWNER TO david;

--
-- Name: projects_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: david
--

ALTER SEQUENCE public.projects_id_seq OWNED BY public.projects.id;


--
-- Name: status_transitions; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.status_transitions (
    project_id integer NOT NULL,
    from_status_id integer NOT NULL,
    to_status_id integer NOT NULL
);


ALTER TABLE public.status_transitions OWNER TO david;

--
-- Name: ticket_comments; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.ticket_comments (
    id integer NOT NULL,
    ticket_id integer NOT NULL,
    author character varying(100) NOT NULL,
    comment_text text NOT NULL,
    created_at timestamp without time zone DEFAULT now()
);


ALTER TABLE public.ticket_comments OWNER TO david;

--
-- Name: ticket_comments_id_seq; Type: SEQUENCE; Schema: public; Owner: david
--

CREATE SEQUENCE public.ticket_comments_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


ALTER SEQUENCE public.ticket_comments_id_seq OWNER TO david;

--
-- Name: ticket_comments_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: david
--

ALTER SEQUENCE public.ticket_comments_id_seq OWNED BY public.ticket_comments.id;


--
-- Name: ticket_dependencies; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.ticket_dependencies (
    ticket_id integer NOT NULL,
    blocked_by_ticket_id integer NOT NULL,
    CONSTRAINT check_not_self_blocking CHECK ((ticket_id <> blocked_by_ticket_id))
);


ALTER TABLE public.ticket_dependencies OWNER TO david;

--
-- Name: ticket_documents; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.ticket_documents (
    id integer NOT NULL,
    ticket_id integer NOT NULL,
    file_path_or_url text NOT NULL,
    description character varying(255),
    created_at timestamp without time zone DEFAULT now()
);


ALTER TABLE public.ticket_documents OWNER TO david;

--
-- Name: ticket_documents_id_seq; Type: SEQUENCE; Schema: public; Owner: david
--

CREATE SEQUENCE public.ticket_documents_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


ALTER SEQUENCE public.ticket_documents_id_seq OWNER TO david;

--
-- Name: ticket_documents_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: david
--

ALTER SEQUENCE public.ticket_documents_id_seq OWNED BY public.ticket_documents.id;


--
-- Name: ticket_tasks; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.ticket_tasks (
    id integer NOT NULL,
    ticket_id integer NOT NULL,
    title character varying(255) NOT NULL,
    is_completed boolean DEFAULT false,
    created_at timestamp without time zone DEFAULT now()
);


ALTER TABLE public.ticket_tasks OWNER TO david;

--
-- Name: ticket_tasks_id_seq; Type: SEQUENCE; Schema: public; Owner: david
--

CREATE SEQUENCE public.ticket_tasks_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


ALTER SEQUENCE public.ticket_tasks_id_seq OWNER TO david;

--
-- Name: ticket_tasks_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: david
--

ALTER SEQUENCE public.ticket_tasks_id_seq OWNED BY public.ticket_tasks.id;


--
-- Name: tickets; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.tickets (
    id integer NOT NULL,
    project_id integer NOT NULL,
    title character varying(255) NOT NULL,
    description text,
    status_id integer NOT NULL,
    assignee character varying(100),
    created_at timestamp without time zone DEFAULT now(),
    updated_at timestamp without time zone DEFAULT now(),
    model character varying(100)
);


ALTER TABLE public.tickets OWNER TO david;

--
-- Name: tickets_id_seq; Type: SEQUENCE; Schema: public; Owner: david
--

CREATE SEQUENCE public.tickets_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


ALTER SEQUENCE public.tickets_id_seq OWNER TO david;

--
-- Name: tickets_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: david
--

ALTER SEQUENCE public.tickets_id_seq OWNED BY public.tickets.id;


--
-- Name: board_statuses id; Type: DEFAULT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.board_statuses ALTER COLUMN id SET DEFAULT nextval('public.board_statuses_id_seq'::regclass);


--
-- Name: projects id; Type: DEFAULT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.projects ALTER COLUMN id SET DEFAULT nextval('public.projects_id_seq'::regclass);


--
-- Name: ticket_comments id; Type: DEFAULT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_comments ALTER COLUMN id SET DEFAULT nextval('public.ticket_comments_id_seq'::regclass);


--
-- Name: ticket_documents id; Type: DEFAULT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_documents ALTER COLUMN id SET DEFAULT nextval('public.ticket_documents_id_seq'::regclass);


--
-- Name: ticket_tasks id; Type: DEFAULT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_tasks ALTER COLUMN id SET DEFAULT nextval('public.ticket_tasks_id_seq'::regclass);


--
-- Name: tickets id; Type: DEFAULT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.tickets ALTER COLUMN id SET DEFAULT nextval('public.tickets_id_seq'::regclass);


--
-- Data for Name: board_statuses; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.board_statuses (id, project_id, name, display_name, "position", agent_role_instruction, created_at) FROM stdin;
1	1	backlog	Backlog	0	\N	2026-06-24 21:40:48.962551
2	1	ready	Bereit	1	Du bist ein Planungs-Agent. Lies das Ticket sorgfältig und zerlege es in konkrete, abarbeitbare Tasks (kb.ai_add_task). Kläre offene Fragen per Kommentar, bevor du das Ticket weiterschiebst.	2026-06-24 21:40:50.98115
3	1	in_progress	In Bearbeitung	2	Du bist ein Implementierungs-Agent. Weise dir das Ticket zu (kb.ai_assign_ticket), arbeite die Tasks der Reihe nach ab (kb.ai_complete_task) und dokumentiere jeden relevanten Schritt per Work-Log-Eintrag (kb.ai_add_comment). Wenn alle Tasks erledigt sind, schiebe das Ticket nach in_review.	2026-06-24 21:40:51.791957
4	1	in_review	Im Review	3	Du bist ein Review-Agent. Prüfe ob alle Tasks abgeschlossen sind, die Implementierung dem Ticket-Ziel entspricht und keine offenen Fragen mehr bestehen. Bei Bestehen: Ticket nach done schieben. Bei Mängeln: konkretes Feedback per Kommentar hinterlassen und Ticket zurück nach in_progress.	2026-06-24 21:40:52.387908
5	1	done	Fertig	4	\N	2026-06-24 21:40:53.502255
6	2	blg	backlog	0	bla bal	2026-06-25 00:20:23.758919
7	2	todo	zu Bearbeiten	0	hier sollst du tickets heraus nehmen und bearbeiten :P	2026-06-25 00:21:10.830471
8	3	backlog	Backlog	0	\N	2026-06-25 08:51:24.233009
9	3	todo	Todo	1	\N	2026-06-25 08:51:27.775862
10	3	in_progress	In Arbeit	2	\N	2026-06-25 08:51:31.231842
11	3	review	Review	3	\N	2026-06-25 08:51:36.334855
12	3	done	Done	4	\N	2026-06-25 08:51:40.071774
13	4	backlog	Backlog	0	**Rolle: Story & Content Writer**\n\nScope: Narrative, game mechanics, world-building, high-level concepts. Kein Code, keine Architektur-Entscheidungen.\n\nWorkflow:\n- Iterative Deepening: "What if?"-Szenarien proaktiv vorschlagen, um Tiefe eines Mechanics zu erkunden.\n- Backlog Entry: Neue User Stories in project/backlog/ anlegen, um Konzepte zu formalisieren.\n\nOutput: Dokumentation in docs/concept/, Stories in project/backlog/.\n\nDefinition of Done — Story Writer:\n- [ ] Story-Datei mit Description + Success Criteria + Agent Log.\n- [ ] Cross-References zu bestehenden Concept-Docs (oder neues Concept-Doc erstellt).\n- [ ] Kein Code und keine Architektur-Änderungen (out of scope).	2026-06-25 09:22:09.490285
14	4	planning	Planning	1	**Rolle: Requirement Manager**\n\nScope: Refinement von Backlog-Items zu technischen Anforderungen.\n\nWorkflow:\n- Items aus project/backlog/ aufnehmen.\n- Technische Tiefe, Edge Cases und Sub-Systeme ausarbeiten.\n- Tickets nach project/planning/ verschieben.\n\nOutput: Detaillierte Stories in project/planning/.\n\nDefinition of Done — Requirement Manager:\n- [ ] Offene Fragen aufgelistet und entweder beantwortet oder für Researcher markiert.\n- [ ] Sub-Stories vorgeschlagen, wenn Story zu groß für ein Ticket ist.\n- [ ] Story via git mv backlog/ → planning/ verschoben.	2026-06-25 09:22:13.11806
15	4	todo	Todo	2	**Rolle: Architect**\n\nScope: Technisches Design, Systemstruktur, Implementierungsstrategien.\n\nWorkflow:\n- Items aus project/planning/ aufnehmen.\n- Implementierung designen (ADRs, Specs).\n- Decomposition: Planning-Tickets in granulare Implementierungsaufgaben zerlegen → project/todo/.\n\nOutput: ADRs in adr/, technische Specs in architecture/, verfeinerte Stories in project/todo/.\n\nDefinition of Done — Architect:\n- [ ] Jede nicht-offensichtliche Entscheidung hat ein ADR in adr/.\n- [ ] Sub-Tickets in todo/ sind Developer-pickup-ready (konkrete Schritte, Dateipfade, Verification-Pfad).\n- [ ] Jedes Sub-Ticket enthält Effort-Estimate (XS/S/M/L/XL).\n- [ ] Story via git mv planning/ → todo/ verschoben (Parent bleibt in planning bis alle Sub-Tickets done sind).\n\nEffort Estimate Scale: XS < 30min | S 30min–2h | M 2–5h | L 5–10h | XL > 10h (muss vom Architect zerlegt werden)	2026-06-25 09:22:20.301044
16	4	in_progress	In Progress	3	**Rolle: Developer**\n\nScope: Implementierung und Validierung von Features. Besitzt NICHT die Test-Suite (das ist Tester-Domäne).\n\nWorkflow:\n- Items aus project/todo/ aufnehmen.\n- Ticket nach project/in-progress/ verschieben während der Arbeit.\n- Ticket nach project/done/ verschieben nach `odin check` und Tester-Sign-off.\n\nHard Negatives (NIEMALS):\n- odin check / odin test als grün melden ohne das echte Kommando-Output im Verification Block zu zitieren.\n- Ticket zu done/ verschieben wenn ein Verification-Schritt übersprungen wurde.\n- Unrelated Code innerhalb eines Bugfix-Tickets refactorn.\n- Amend eines publizierten Commits, Force-Push auf master, --no-verify / --no-gpg-sign.\n- Zwei Tickets in einem Commit mischen.\n\nVerification Block (PFLICHT am Ende jedes Agent-Log-Eintrags):\n  odin check src/server: GREEN (oder: SKIPPED — <Grund>)\n  odin check src/client: GREEN (oder: SKIPPED — <Grund>)\n  odin test  src/server: <N> passed (oder: SKIPPED — <Grund>)\n\nDefinition of Done — Developer:\n- [ ] Verification Block zeigt GREEN für odin check src/server und odin check src/client.\n- [ ] Wenn neue Logik worth testing: korrespondierendes Tester-Ticket existiert.\n- [ ] Commit-Message referenziert Ticket-Dateipfad; enthält Co-Authored-By: Trailer.\n- [ ] Ticket via git mv in-progress/ → done/ verschoben.\n- [ ] Branch auf Remote gepusht.	2026-06-25 09:22:30.323476
17	4	done	Done	4	**Abgeschlossen.** Tickets in diesem Status sind fertiggestellt und verifiziert. Kein Agent greift aktiv auf Done-Tickets zu — sie dienen als historisches Archiv und Referenz.\n\nTester-Aktivierung: Ein Tester-Ticket kann hier landen nachdem der Developer done/ signalisiert hat. Der Tester fügt dann Testergebnisse zum Agent Log hinzu und filed Bug-Tickets in backlog/ falls nötig.\n\nDefinition of Done (alle Rollen):\n- Verification Block vorhanden und GREEN (oder explizit N/A).\n- Agent Log vollständig — jeder Schritt dokumentiert, was getan wurde und was gefunden wurde.\n- Kein offenes TODO im Code (// TODO: continue later ist ein Hard Negative).	2026-06-25 09:22:35.497039
18	5	todo_list	To Do	0	\N	2026-06-29 09:58:22.451251
19	5	in_progress_list	In Progress	1	\N	2026-06-29 09:58:22.483744
20	5	done_list	Done	2	\N	2026-06-29 09:58:22.484197
\.


--
-- Data for Name: projects; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.projects (id, slug, name, description, created_at) FROM stdin;
1	kanban-ai	Kanban AI	Entwicklung des kb.ai MCP-Servers — Kanban-Backend für Agentic AI Workflows	2026-06-24 21:37:50.554287
2	dingo	dingodingo	das ist ein test projekt	2026-06-25 00:19:49.703476
3	kbai-ui	kbai-ui	SvelteKit Kanban-Frontend für das kb.ai PostgreSQL-Backend. Dark-Neon-Design, Modal-basierte Navigation, Drag-and-Drop Board.	2026-06-25 08:51:16.633647
4	kernel-panic	Kernel Panic	Survival/Automation-Game in Odin. Spieler programmieren Worker-Entities via Lua, um in einer "Data World" Root Access zu erlangen. Workflow: AGENTS.md-Kanban mit Rollen Story Writer → Requirement Manager → Architect → Developer → Tester.	2026-06-25 09:21:59.069675
5	projects/tum-monitor	TUM - Process Monitor Development Plan	Umsetzungsplan für das TUM Task/User Monitor Projekt: Prozessdaten vervollständigen, Display überarbeiten und Navigation hinzufügen.	2026-06-29 09:58:10.867973
\.


--
-- Data for Name: status_transitions; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.status_transitions (project_id, from_status_id, to_status_id) FROM stdin;
1	1	2
1	2	3
1	3	4
1	4	5
1	4	3
2	6	7
1	3	2
3	8	9
3	9	10
3	10	11
3	11	10
3	11	12
3	10	9
4	13	14
4	14	15
4	15	16
4	16	17
4	16	15
4	15	14
4	14	13
5	18	19
5	19	20
5	19	18
\.


--
-- Data for Name: ticket_comments; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.ticket_comments (id, ticket_id, author, comment_text, created_at) FROM stdin;
1	5	claude-sonnet-4-6	Planung abgeschlossen. 7 Tasks angelegt. Offene Frage vor Implementierung: Codeberg bietet aktuell nur limitierte hosted Runner — falls kein hosted Runner verfügbar ist, muss ein self-hosted Runner eingerichtet werden (Task 1). Nix-Cache via cachix bevorzugt, falls kein cachix-Account vorhanden ist, fällt man auf den langsameren GitHub Actions Cache zurück.	2026-06-24 21:56:50.205677
2	5	claude-sonnet-4-6	Workflow überarbeitet. Bestehende build.yml hatte 5 Probleme: (1) branch 'master' statt 'main', (2) runs-on: 'ci' (self-hosted Label) statt 'ubuntu-latest', (3) kein Nix-Cache, (4) github.ref statt gitea.ref im Release-Job, (5) kein Test-Step. Fixes: branch auf 'main', Runner auf ubuntu-latest, magic-nix-cache-action ergänzt, Build und Release in separate Jobs aufgeteilt (Release läuft nur bei v*-Tags und hängt von erfolgreichem Build ab), gitea.ref korrekt gesetzt. Test-Step als Placeholder vorhanden. Noch offen: README-Badge (Task 7).	2026-06-24 21:59:58.139695
3	5	claude-sonnet-4-6	Alle 7 Tasks erledigt. Commit d0dbe2e auf main. Bereit für Review.	2026-06-24 22:07:14.41048
4	5	claude-sonnet-4-6	Fix: gitea.ref → github.ref (Commit 6f7193b). Forgejo Actions ist GitHub Actions-kompatibel und kennt den gitea-Kontext nicht — github.ref ist korrekt.	2026-06-24 22:09:36.833346
5	5	claude-sonnet-4-6	Fix (cef1c6e): Em-Dash im Test-Step verursachte "unexpected EOF" im Shell-Parser des Runners. Ersetzt durch ASCII-Bindestrich mit single-quotes.	2026-06-24 22:18:24.845691
6	5	claude-sonnet-4-6	Pipeline laeuft durch (Commit 9f13c72). Zwei weitere Fixes noetig gewesen: Test-Stage entfernt (keine Tests vorhanden), upload-artifact@v4 auf @v3 downgegradet (v4+ wird auf Forgejo/GHES nicht unterstuetzt).	2026-06-24 22:29:04.295385
7	35	claude-opus-4-7	**Cleanup-Pass:** Ticket-Description listet alle Success Criteria als erfüllt (Token-Read OK, Tools für Projects/Columns/Cards/Comments vorhanden, `uv run` startbar, httpx async-Requests). Verification N/A (Python-Script, kein `odin check`). Move-Sequenz: todo → in_progress → done.	2026-06-28 21:09:03.320092
8	34	claude-opus-4-7	**Cleanup-Pass:** Description-Note: „114a (Debug JSON API), 114b (Lockstep), 114c (Headless Mock Client) sind bereits done". Damit sind alle aufgeschlüsselten Sub-Tickets fertig. Move-Sequenz: planning → todo → in_progress → done.	2026-06-28 21:09:08.625817
9	27	claude-opus-4-7	**Cleanup-Pass:** Alle 5 Sub-Tickets (099b core data model, 099c resource migration, 099d fragment migration, 099e crafting registry, 099f persistence/networking/lua, 099g smoke-test Transistor) sind im done-Archiv. Parent kann schließen. Move-Sequenz: planning → todo → in_progress → done.	2026-06-28 21:09:14.466463
10	36	claude-sonnet-4-6	**098b abgeschlossen (2026-06-29)**\n\nLetztes Sub-Ticket implementiert:\n- `MSG_UPGRADE_WORKER_LOC` (0x1D) in protocol.odin\n- Server-Handler `_apply_upgrade_worker_loc`: Terminal-Check + 5 RAM-Kosten + loC_limit+10 + Ack\n- Client: `[U]` im Terminal-Overlay, Ack aktualisiert Worker-State\n- Terminal-Overlay zeigt Upgrade-Option mit aktuellem loC_limit\n\nAlle drei Sub-Tickets (098a, 098b, 098c) done. Roof-Ticket 098 kann jetzt auf done verschoben werden.	2026-06-29 09:37:15.782263
11	24	claude-sonnet-4-6	**Architect-Decomposition (2026-06-29)**\n\n.Wall Tile-Typ existiert bereits in world.odin + tile_is_solid. Fehlende Teile:\n- 006a (kbai #189): Wall bauen — MSG_BUILD_WALL, W-Taste, 20 Bytes Kosten\n- 006b (kbai #190): GC-Kollision — advance_gcs Greedy-Step + tile_is_solid Check\n\nBeide Sub-Tickets in todo/ bereit für Developer.	2026-06-29 09:41:54.987346
12	28	claude-sonnet-4-6	**Architect-Decomposition (2026-06-29)**\n\nSub-Tickets erstellt:\n- 100a (kbai #191): Worldgen Placement — .Cave_Entrance Tile, 5-8 Tile Raum, 1 pro 8×8-Chunk-Region\n- 100b (kbai #192): Entry Mechanic — Pointer_Key Item, Interact öffnet Eingang\n- 100c (kbai #193): Loot Tables — 60% Spawn-Rate, 10% Crystals\n- 100d (kbai #194): Visual — Cave_Entrance Orange/#, Ambient-Tint\n\nAlle in todo/ bereit für Developer. 100b depends on 100a.	2026-06-29 09:41:57.081085
13	189	claude-sonnet-4-6	**Developer — 2026-06-30 — DONE**\n\nImplementiert in commit 5d33ead.\n\n- `MSG_BUILD_WALL :: u8(0x1F)` + `WALL_BUILD_COST :: u32(20)` in `src/shared/protocol.odin`\n- `_apply_build_wall` in `src/server/simulation.odin`: prüft Facing-Tile == .Empty + Bytes >= 20, mutiert Tile via `get_tile` Pointer (`t.type = .Wall`)\n- W-Taste in `_handle_normal_keys` (`src/client/input.odin`)\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — kein Test für Build-Wall vorgesehen	2026-06-30 10:35:31.345934
14	184	claude-sonnet-4-6	**Developer — 2026-06-30 — DONE**\n\nImplementiert in commit 7ffe7cc.\n\n- Hot-Zone: Amber-Overlay `{255,160,0}` ±15α bei 3Hz via `math.sin(GetTime()*3π)` nach bestehendem `{70,25,0}`-Layer\n- Kernel-Rand: Magenta `{200,0,255}` α20–50 bei 0.5Hz auf dist 18–20 (outer 2 tiles der Cold Zone)\n- Nur `src/client/render.odin` — kein Server-State\n\nVerification:\n  odin check src/server: SKIPPED — keine Server-Änderungen\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — rein visuell	2026-06-30 10:35:38.002215
15	185	claude-sonnet-4-6	**Developer — 2026-06-30 — DONE**\n\nImplementiert in commit a0aa6a6.\n\n- Zweiter Chunk-Pass am Ende von `draw_world` (nach Resource-Icon-Loop)\n- Variante A: `h % 100 < 15` → 2 diagonale DrawLine α8 `{160,160,160}`\n- Variante B: `h % 100 < 5 && near_boundary` → DrawRectangle `{255,255,255}` α0–20 animiert\n- Variante C (<> auf Fragment-Tiles) war bereits implementiert (Zeile 618–623)\n- Hash: `u32(twx*374761393) ~ u32(twy*668265263) ~ 0xC0FFEE42`\n\nVerification:\n  odin check src/server: SKIPPED — keine Server-Änderungen\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — rein visuell	2026-06-30 10:35:41.507304
16	186	claude-sonnet-4-6	**Developer — 2026-06-30 — DONE**\n\nImplementiert in commit febd287.\n\n- Sekundärer Halo: Desaturierung via `grey=(r+g+b)/3`, 75% grey Mischung → DrawCircleV bei base_r+28, α40\n- L1 Pulsring: `DrawCircleLines` bei base_r+20 ±4px, α25–60, 2Hz; nur wenn `gc.tier == 1`\n- L2 Arc-Sweep: `DrawCircleSector` 45°-Bogen statt DrawLine; `sweep_angle` (rad) → degrees; für `gc.tier >= 2`\n\nVerification:\n  odin check src/server: SKIPPED — keine Server-Änderungen\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — rein visuell	2026-06-30 10:35:47.815683
17	187	claude-sonnet-4-6	**Developer — 2026-06-30 — DONE**\n\nImplementiert in commit a77c458.\n\n- L1 `sweep_len`: `_gc_params` case 1 von `0.75*TILE_SIZE` auf `1.5*TILE_SIZE` erhöht\n- L2 konzentrische Ringe: 3x `DrawCircleLines` bei 0.5T/1.0T/1.5T, α80/50/25; zusätzlich zum 110d-Arc-Sektor\n- Koordiniert mit 110d (beide in draw_gcs, kein Konflikt)\n\nVerification:\n  odin check src/server: SKIPPED — keine Server-Änderungen\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — rein visuell	2026-06-30 10:35:54.20629
18	188	claude-sonnet-4-6	**Developer — 2026-06-30 — DONE**\n\nImplementiert in commit 158df1f.\n\n- Panel-BG + Border: bereits via `HUD_PANEL {5,34,26,200}` + `hud_frame_color()` vorhanden\n- Colored Dots: bereits als 6×6 DrawRectangle in Ressource-Farben vorhanden\n- Cyan Worker-Rand: bereits als 2px DrawRectangle pulsierend vorhanden (1Hz)\n- NEU: SYS-Status-String unter Worker-Panel — `fmt.ctprintf("SYS: entropy %d%%  GC: %d", i32(cs.entropy), len(cs.gcs))`\n\nVerification:\n  odin check src/server: SKIPPED — keine Server-Änderungen\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — rein visuell	2026-06-30 10:35:55.513825
19	37	claude-sonnet-4-6	**Status-Update — 2026-06-30**\n\n103 ist ein Roof-Ticket. Sub-Tickets 103gc (GC-Tier-Visualisierung) und 103e→107 (HUD-Polish) sind abgeschlossen.\n\nVerbleibende Sub-Tickets blockieren auf externe Faktoren:\n- 104a/104f: warten auf Sprite-Assets vom Mensch\n- 105: wartet auf Asset-Pipeline-Entscheidung und 099\n- 106: Isometrie-Entscheidung getroffen, Architect-Phase steht aus\n- 103f (Beleuchtung): explizit auf Backlog gesetzt laut Datei — "kann später als 109 aufgemacht werden falls nötig"\n\nTicket bleibt in_progress als Umbrella. Aktive Arbeit läuft über die Sub-Tickets 104/105/106.	2026-06-30 10:36:59.056653
20	191	claude-sonnet-4-6	**Developer — 2026-06-30 — DONE** (commit bbdf746)\n\n- `TileType.Cave_Entrance` in `src/shared/world.odin` + `tile_is_solid` erweitert (solid bis 100b öffnet)\n- `CaveRegion :: struct { entrance_x, entrance_y: i32; id: u32 }` in `world_gen.odin`\n- `caves: [dynamic]CaveRegion` in `ServerState` (main.odin)\n- `_place_cave_if_host`: 1 Höhle pro 8×8-Chunk-Region via `_world_hash(rcx, rcy, seed ~ 0xCAFEB0BA)`. Room-Half 2–4 Tiles, Wall-Ring außen, leeres Inneres, Cave_Entrance an Nordwand-Mitte.\n- Client `render.odin`: Cave_Entrance → `draw_structural_tile` (Fallback bis 100d)\n- `delete(s.caves)` in allen 4 Test-Teardown-Procs (Memory-Leak-Fix)\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin test  src/server: 83 passed (keine Leaks)	2026-06-30 10:42:51.138672
21	193	claude-sonnet-4-6	## Developer Log — 100c Cave Loot Tables\n\n### Problem\nDer CA-Pass (`_place_walls_ca`) behandelt Empty-Tiles als Kandidaten und hätte das Höhleninnere nach dem Clear-Pass in `_place_cave_if_host` zerstört.\n\n### Lösung\n1. `CaveRegion` um `center_x, center_y, half: i32` erweitert (nötig für Post-CA-Restore).\n2. `_restore_and_loot_cave_if_host` proc hinzugefügt — wird in `generate_chunk` nach `_place_walls_ca` aufgerufen.\n3. Der Proc iteriert alle Caves deren Zentrum im aktuellen Chunk liegt, setzt alle Interior-Tiles zurück und befüllt sie deterministisch per `_world_hash(wx, wy, seed ~ 0xDEADB0B5)`:\n   - 60 % Spawn-Chance (`lh % 100 < 60`)\n   - Davon 10 % Crystals (`(lh>>8)%10 == 0`): Crystal_Bytes / Crystal_CPU / Crystal_RAM gleichverteilt\n   - Davon 90 % Pickups: Byte 30 % / CPU_Cycle 35 % / RAM_Chip 35 % (`(lh>>16)%20`)\n\n### Verification Block\n  odin check src/server: GREEN\n  odin check src/client: SKIPPED — keine Client-Änderungen\n  odin test  src/server: 83 passed	2026-06-30 10:49:57.220694
22	194	claude-sonnet-4-6	## Developer Log — 100d Cave Visual Differentiation\n\n### Änderungen\n\n**`draw_structural_tile` (render.odin)**\n- `case .Cave_Entrance:` in den TileType-Switch eingehängt: base `{160,60,10}` + accent `{240,140,40}` (orange Schemen wie Wall, aber warm)\n- Nach dem Crack-Block: orange `#`-Glyph (Größe 18, Gold `{255,200,0}`) + gold `[X]`-Lock-Marker (Größe 8) über dem Tile\n\n**Ambient Tint Pass in `draw_world`**\n- Neuer Pass VOR dem Resource-Icon-Loop — iteriert über sichtbare Chunks, sucht `Cave_Entrance`-Tiles\n- Für jede Entrance: kreisförmiger rust-brauner Tint (`{140,45,10}`) mit Radius 6 Tiles, alpha fade 0→50 vom Rand zum Zentrum\n- Chunks außerhalb Frustum + Tint-Radius werden übersprungen (kein FPS-Impact)\n\n### Verification Block\n  odin check src/server: SKIPPED — keine Server-Änderungen\n  odin check src/client: GREEN\n  odin test  src/server: 83 passed	2026-06-30 10:54:34.0135
23	200	claude-sonnet-4-6	## Developer Log — 099h Inventar-Overlay Terminal-Listview\n\n### Änderungen\n\n**`src/client/main.odin`**\n- `inventory_selected: int` zu ClientState hinzugefügt (Cursor-Index)\n\n**`src/client/input.odin`** (`_handle_inventory_keys`)\n- ESC schließt Inventar (zusätzlich zu I)\n- ↑↓ (mit Repeat) bewegt `cs.inventory_selected` durch aktive Slots; wrapped am Ende\n\n**`src/client/inventory.odin`** (Komplett-Rewrite)\n- `/proc/player/inventory` Header\n- Volume-Fortschrittsbalken (farbkodiert: grün→orange bei Füllstand)\n- Dynamische Slot-Iteration über `p.inventory.slots[0..slot_count]`\n- Cursor-Zeile mit `>` und COL-Highlight auf `inventory_selected`\n- KB-Volumen pro Zeile (`def.volume * count`)\n- `[e] use`-Hint nur für Items mit `.consumable` Capability\n- Crafting-Sektion: gleiche Logik, Formatierung bereinigt\n- Fragments-Sektion: alle 5 Fragmente (echo.h neu hinzugefügt)\n- Footer: `[↑↓] select  [e] use  [i/ESC] close`\n\n### Verification Block\n  odin check src/server: SKIPPED — keine Server-Änderungen\n  odin check src/client: GREEN\n  odin test  src/server: 83 passed	2026-06-30 11:08:59.984507
\.


--
-- Data for Name: ticket_dependencies; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.ticket_dependencies (ticket_id, blocked_by_ticket_id) FROM stdin;
\.


--
-- Data for Name: ticket_documents; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.ticket_documents (id, ticket_id, file_path_or_url, description, created_at) FROM stdin;
\.


--
-- Data for Name: ticket_tasks; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.ticket_tasks (id, ticket_id, title, is_completed, created_at) FROM stdin;
1	5	Codeberg Actions Runner-Verfügbarkeit prüfen (hosted vs. self-hosted)	t	2026-06-24 21:56:33.914293
2	5	.forgejo/workflows/ci.yml anlegen mit Push-Trigger auf alle Branches	t	2026-06-24 21:56:35.222562
3	5	Nix im CI-Job installieren (DeterminateSystems/nix-installer-action)	t	2026-06-24 21:56:36.561536
4	5	nix build im Workflow ausführen und Fehler bei Build-Fehler propagieren	t	2026-06-24 21:56:39.000753
5	5	Nix-Store-Cache zwischen Runs konfigurieren (cachix oder GitHub Actions Cache)	t	2026-06-24 21:56:40.552199
6	5	Separaten Release-Job für v*-Tags anlegen (nix build → Binary als Artefakt hochladen)	t	2026-06-24 21:56:41.885418
7	5	Workflow in der README dokumentieren (Badge + Setup-Hinweise)	t	2026-06-24 21:56:44.243196
8	189	MSG_BUILD_WALL :: u8(0x1F) + WALL_BUILD_COST :: u32(20) in protocol.odin	t	2026-06-30 10:33:30.910088
9	184	Hot-Zone-Tiles zeigen subtilen Amber-Flicker (±15α bei 3Hz)	t	2026-06-30 10:33:33.716394
10	185	Scratch-Pattern auf ~15% der Empty-Tiles (deterministisch, α8)	t	2026-06-30 10:33:36.274037
11	186	Sekundärer desaturierter Halo auf allen GC-Tier (base_r+28, α40)	t	2026-06-30 10:33:38.655079
12	187	L1-Sweep reicht 1.5× Tile-Weite (sweep_len 0.75T→1.5T)	t	2026-06-30 10:33:40.504612
13	188	HUD-Panel hat dunkelgrünen Hintergrund + 1px Border	t	2026-06-30 10:33:43.00577
14	189	Server-Handler _apply_build_wall: Facing-Tile wird .Wall, kostet 20 Bytes	t	2026-06-30 10:33:49.307964
15	189	Client: W-Taste in _handle_normal_keys sendet MSG_BUILD_WALL	t	2026-06-30 10:33:53.468759
16	189	odin check ./src/server grün	t	2026-06-30 10:33:54.720022
17	189	odin check ./src/client grün	t	2026-06-30 10:33:56.205437
18	184	Kernel-Zone-Rand zeigt langsamen Magenta-Puls (20–50α bei 0.5Hz, dist 18–20)	t	2026-06-30 10:33:58.798424
19	184	odin check ./src/client grün	t	2026-06-30 10:33:59.931082
20	185	Shimmer auf ~5% der Empty-Tiles nahe Zonengrenzen (α0–20, animiert)	t	2026-06-30 10:34:05.864343
21	185	Fragment-Floor <> Symbol auf .Variable/.Pointer-Tiles (war bereits implementiert)	t	2026-06-30 10:34:06.849502
22	185	odin check ./src/client grün	t	2026-06-30 10:34:08.748269
23	186	L1-GCs zeigen pulsierenden Ring (DrawCircleLines, 2Hz, α25–60)	t	2026-06-30 10:34:10.160307
24	186	L2-GCs haben 45°-Arc (DrawCircleSector) statt Sweep-Linie	t	2026-06-30 10:34:12.027849
25	186	odin check ./src/client grün	t	2026-06-30 10:34:13.286166
26	187	L2-Sweep zeigt 3 konzentrische Ringe (DrawCircleLines α80/50/25 bei 0.5/1.0/1.5T)	t	2026-06-30 10:34:19.455282
27	187	odin check ./src/client grün	t	2026-06-30 10:34:20.81498
28	188	Colored Dots vor Resource-Counts (Bytes blau, CPU gold, RAM violett)	t	2026-06-30 10:34:22.092904
29	188	Aktive Worker erhalten pulsierenden Cyan-Rand (2px, 1Hz)	t	2026-06-30 10:34:23.127737
30	188	SYS-Status-String unter Worker-Panel: entropy X% + GC count	t	2026-06-30 10:34:24.501536
31	188	odin check ./src/client grün	t	2026-06-30 10:34:27.173183
34	37	103a → 104a: Player-Sprites — wartet auf Asset-Lieferung (Mensch-Task)	f	2026-06-30 10:36:42.167534
35	37	103c → 104f: Boss-Spider-Sprite — wartet auf Asset-Lieferung	f	2026-06-30 10:36:44.470772
36	37	103d → 106: Isometrische Weltdarstellung — Architect-Phase offen	f	2026-06-30 10:36:45.596438
37	37	103b → 105: V2-Expansion-Entities — wartet auf 099 + Asset-Pipeline	f	2026-06-30 10:36:47.625077
38	37	103f: Beleuchtung — explizit auf Backlog gesetzt, ggf. als separates Ticket	f	2026-06-30 10:36:49.210365
32	37	103gc: GC-Tier-Visualisierung (Skull/Spider/Boss) — implementiert 2026-06-24	t	2026-06-30 10:36:38.684537
33	37	103e → 107: HUD-Polish (Entropy + Threat-Level) — implementiert	t	2026-06-30 10:36:40.156811
39	191	TileType.Cave_Entrance in shared/world.odin + tile_is_solid erweitert	t	2026-06-30 10:41:53.570158
40	191	CaveRegion struct + state.caves [dynamic]CaveRegion in ServerState	t	2026-06-30 10:41:57.272553
41	191	_place_cave_if_host: 1 Höhle pro 8×8-Region, deterministisch, Wall-Ring + leeres Inneres	t	2026-06-30 10:42:08.487694
42	191	Worldgen-Tests bleiben grün (83/83, keine Memory-Leaks)	t	2026-06-30 10:42:15.502699
43	191	odin check ./src/server + ./src/client grün	t	2026-06-30 10:42:20.131864
44	193	Höhlenraum-Tiles haben ~60 % Ressourcen-Spawn-Rate	t	2026-06-30 10:49:03.930146
45	193	Alle drei Ressourcen-Typen vertreten (Byte 30%, CPU 35%, RAM 35%)	t	2026-06-30 10:49:10.154519
46	193	Crystal-Deposits vorhanden (~10% der Spawns)	t	2026-06-30 10:49:14.988981
47	193	Post-CA-Restore verhindert CA-Zerstörung des Höhleninneren	t	2026-06-30 10:49:19.826425
48	193	odin check ./src/server grün	t	2026-06-30 10:49:24.882143
49	193	odin test ./src/server — alle Tests grün (keine Regressionen)	t	2026-06-30 10:49:30.752013
50	194	Cave_Entrance rendert als Orange-# mit Gold-[X]-Lock-Symbol	t	2026-06-30 10:54:01.877875
51	194	Ambient-Tint (rust-braun, Radius 6 Tiles) um Cave_Entrance	t	2026-06-30 10:54:08.516457
52	194	odin check ./src/client grün	t	2026-06-30 10:54:12.980299
53	200	Alle Inventory-Slots dynamisch gerendert (nicht hardcodiert)	t	2026-06-30 11:08:20.94996
54	200	Volume-Balken zeigt used_volume / INVENTORY_VOLUME_CAP	t	2026-06-30 11:08:23.627786
55	200	↑↓ Cursor-Navigation durch Item-Slots	t	2026-06-30 11:08:26.154124
56	200	odin check ./src/client grün	t	2026-06-30 11:08:31.151436
\.


--
-- Data for Name: tickets; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.tickets (id, project_id, title, description, status_id, assignee, created_at, updated_at, model) FROM stdin;
1	1	Unit- und Integrationstests implementieren	Aktuell 0% Testabdeckung. Ziel: Unit-Tests für alle kanban/-Module (projects, tickets, comments, board_statuses) mit einer echten Test-Datenbank. Integrationstests für den MCP-Protokoll-Layer (JSON-RPC 2.0 initialize → tools/list → tools/call Roundtrip). Framework: cmocka oder criterion.	1	\N	2026-06-24 21:42:40.759076	2026-06-24 21:42:40.759076	\N
2	1	Statisch gelinkte Binary bauen (nix build .#static)	Nix-Flake um ein statisches Build-Target erweitern. libpq und cjson statisch linken, sodass die Binary ohne installierte Systemabhängigkeiten läuft. Nützlich für Container-Deployments und direkte Distribution.	1	\N	2026-06-24 21:42:43.258418	2026-06-24 21:42:43.258418	\N
3	1	Pagination für kb.ai_list_tickets	list_tickets gibt aktuell alle Tickets ohne Limit zurück. Bei großen Projekten kann das zu sehr großen MCP-Responses führen, die den Kontext eines LLMs belasten. Lösung: optionale Parameter limit und offset (oder cursor-basiert) im Tool-Schema ergänzen und in der SQL-Query als LIMIT/OFFSET umsetzen.	1	\N	2026-06-24 21:42:45.774268	2026-06-24 21:42:45.774268	\N
4	1	Connection Pooling (pgBouncer oder interne Pool-Schicht)	Aktuell hält der Server eine einzige persistente libpq-Verbindung. Bei mehreren parallelen Agenten oder Reconnect-Szenarien ist das ein Engpass. Optionen: (a) pgBouncer vorschalten, (b) minimalen internen Pool mit libpq-Verbindungen im Server implementieren.	1	\N	2026-06-24 21:42:52.187047	2026-06-24 21:42:52.187047	\N
6	1	Docker-Image und docker-compose Setup	Dockerfile für den kb.ai MCP-Server erstellen (auf Basis der statischen Binary). docker-compose.yml mit kb.ai + PostgreSQL + Migrations-Init-Container, sodass das gesamte Setup mit einem einzigen `docker compose up` startet.	1	\N	2026-06-24 21:42:56.684568	2026-06-24 21:42:56.684568	\N
7	1	Ticket-Filterung nach Status in list_tickets	Agenten arbeiten typischerweise nur mit Tickets eines bestimmten Status (z.B. alle "ready"-Tickets holen). list_tickets um optionalen status_id-Filter erweitern, damit Agenten gezielt nur relevante Tickets laden und nicht alles durch den Kontext jagen.	1	\N	2026-06-24 21:42:59.120605	2026-06-24 21:42:59.120605	\N
5	1	CI/CD Pipeline auf Codeberg Actions einrichten	Codeberg Actions Workflow anlegen, der bei jedem Push: (1) nix build ausführt, (2) Tests laufen lässt (sobald vorhanden), (3) bei Tags einen Release-Build erzeugt. Nix-Cache zwischen Runs nutzen um Build-Zeiten zu reduzieren.	5	claude-sonnet-4-6	2026-06-24 21:42:54.25993	2026-06-24 22:29:08.501643	claude-sonnet-4-6
8	2	testen	teterete	7	-	2026-06-25 00:21:39.71538	2026-06-30 12:08:49.954424	\N
9	3	E2E-Test: Modal-Interaktionen verifizieren	Alle drei Modals im Live-Browser testen: Ticket-Klick → TicketModal, Statuses-Button → StatusesModal, Workflow-Button → WorkflowModal. Sonderfälle: Escape-Taste schließt Modal, Backdrop-Klick schließt Modal, nach Schließen von StatusesModal werden Statuses + Tickets neu geladen.	8	\N	2026-06-25 08:52:14.056868	2026-06-25 08:52:14.056868	\N
10	3	E2E-Test: Drag-and-Drop mit Workflow-Validierung	Testen ob das Board beim Ziehen eines Tickets in einen nicht erlaubten Status korrekt reagiert: API gibt 409 zurück, Karte springt optimistisch zurück, Fehlermeldung erscheint. Auch valide Transitionen testen und sicherstellen dass bind:tickets die reaktive Aktualisierung korrekt durchreicht.	8	\N	2026-06-25 08:52:21.023606	2026-06-25 08:52:21.023606	\N
11	3	E2E-Test: Workflow-Transition löschen via UI	In WorkflowModal die Transition-Liste unterhalb des Graphen prüfen: Trash-Button löscht Transition per DELETE-API, Edge verschwindet aus dem Graphen, Listeneintrag entfernt sich. Auch Keyboard-Shortcut (Delete/Backspace auf selektierter Edge) testen.	8	\N	2026-06-25 08:52:30.705301	2026-06-25 08:52:30.705301	\N
12	3	E2E-Test: Ticket anlegen und bearbeiten	Neues Ticket ohne Assignee anlegen (darf nicht fehlschlagen). Danach Ticket per Klick im Board öffnen → TicketModal. Bearbeiten-Button im Modal aktiviert Inline-Edit-Formular (kein Seitennavigation). Änderungen speichern und im Board prüfen.	8	\N	2026-06-25 08:54:02.545842	2026-06-25 08:54:02.545842	\N
13	3	Spalten-Edit-Icon öffnet StatusesModal statt Seite	Das Bleistift-Icon im Spalten-Header und der "Neue Spalte"-Button rufen jetzt onOpenStatuses() auf statt goto(). Wurde implementiert, muss noch live getestet werden: Klick öffnet StatusesModal, Modal schließen aktualisiert das Board.	8	\N	2026-06-25 08:54:08.01524	2026-06-25 08:54:08.01524	\N
14	3	Ticket löschen aus TicketModal	Nach dem Löschen eines Tickets im TicketModal soll die Karte sofort aus dem Board verschwinden. onDeleted-Callback filtert tickets-Array im Board. Prüfen ob der reaktive Update korrekt funktioniert und das Modal sich schließt.	8	\N	2026-06-25 08:54:22.899503	2026-06-25 08:54:22.899503	\N
15	3	Ticket-Status-Änderung im Modal ans Board zurückmelden	Wenn ein Ticket im TicketModal in einen anderen Status verschoben wird, soll die Karte im Board direkt in die richtige Spalte wandern ohne Seiten-Reload. Aktuell wird status_id nach dem Speichern nicht an den Board-State zurückgemeldet — onUpdated-Callback oder Store-Aktualisierung nötig.	8	\N	2026-06-25 08:54:30.877712	2026-06-25 08:54:30.877712	\N
16	3	Drag-and-Drop auf Touch-Geräten prüfen	HTML5 drag-and-drop funktioniert auf Mobile-Browsern nicht. Evaluieren ob pointer-events-basiertes DnD (z.B. mit svelte-dnd-action) benötigt wird oder ob Mobile vorerst out-of-scope ist.	8	\N	2026-06-25 08:54:36.83538	2026-06-25 08:54:36.83538	\N
17	3	Status-Sortierung per Drag-and-Drop	In StatusesModal und der Statuses-Standalone-Seite gibt es noch keinen Drag-and-Drop-Mechanismus zur Umsortierung. Aktuell nur manuell über das Position-Feld. Fußnote "Drag & Drop-Sortierung folgt" ist bereits im UI.	8	\N	2026-06-25 08:54:42.380521	2026-06-25 08:54:42.380521	\N
18	3	Leere Board-Spalten: Höhe bei langen Ticket-Listen angleichen	KanbanColumn hat min-h-[160px] auf der Drop-Zone. Bei sehr langen Spalten können benachbarte leere Spalten visuell klein wirken. Prüfen ob flex-grow oder eine dynamische Mindesthöhe sinnvoller ist.	8	\N	2026-06-25 08:54:56.989031	2026-06-25 08:54:56.989031	\N
19	3	Kommentar-Funktion im TicketModal	Das Backend hat bereits eine /api/tickets/:id/comments Endpoint-Struktur. Im TicketModal fehlt noch ein Kommentar-Abschnitt: Liste bestehender Kommentare + Eingabefeld für neue. Design analog zu den bestehenden Inline-Edit-Bereichen.	8	\N	2026-06-25 08:55:02.47213	2026-06-25 08:55:02.47213	\N
20	3	Task-Checkliste im TicketModal	Tickets haben Tasks (Subtasks) im Backend. Im TicketModal ist bereits ein Fortschrittsbalken angedeutet, aber keine interaktive Task-Liste. Checkboxen zum Abhaken, neue Tasks hinzufügen, Tasks löschen.	8	\N	2026-06-25 08:55:06.906555	2026-06-25 08:55:06.906555	\N
21	3	Keyboard-Navigation und Accessibility-Grundlagen	Modals sind per Escape schließbar, aber focus-trap fehlt noch (Tab wandert durch den Hintergrund). Aria-Attribute für Modal (role=dialog, aria-modal, aria-labelledby) und Kanban-Karten (role=article) ergänzen. Focus-Rückgabe beim Schließen des Modals.	8	\N	2026-06-25 08:55:11.296416	2026-06-25 08:55:11.296416	\N
22	4	097: Codeberg-Pages — Doku- und WASM-Hosting	**Datei:** project/backlog/097-codeberg-pages-docs-wasm.md\n**Aufwand:** M | **Status:** Deferred (2026-06-23: Mensch priorisiert anders)\n\nStatisches Hosting via Codeberg Pages:\n1. Spielbare WASM-Demo (client.wasm existiert bereits)\n2. Projekt-Doku (docs/, adr/, project/)\n3. Release-Galerie (Windows-EXE-Downloads)\n\n**Deferred weil:**\n- Pages-Setup hat Repo-Politik-Implikationen (pages-Branch vs Sub-Repo)\n- WASM-Client braucht WebSocket-Layer (heute nur TCP)\n- Doku-Toolchain (mkdocs/zola/mdBook) nicht entschieden\n\n**Sub-Themen:** 097a Pages-Setup, 097b Doku-Toolchain, 097c WASM-Client, 097d Release-Galerie\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Mensch (2026-06-23): auf Eis gelegt\n- [ ] Reaktivieren wenn Kapazität oder strategischer Bedarf entsteht	13	\N	2026-06-25 09:23:51.893011	2026-06-25 09:23:51.893011	\N
23	4	105: Sprite Sheet V2.0 Expansion — 10 neue Entities	**Datei:** project/backlog/105-sprite-sheet-v2-expansion.md\n**Aufwand:** XL | **Refs:** docs/concept/visual/konzept-sprites-v2-expansion.png, crops-v2/\n\n10 neue Entities aus V2-Sheet:\n- Debugger Bot, Buffer Bot, Ping Bot (Worker-Varianten)\n- Firewall Segment, Antivirus Turret (Defense)\n- Power Cell (Resource/Crafting)\n- Logic Bomb (Placeable Trap)\n- Corrupted Data, Data Leak (Hazard Tiles)\n- Network Worm (Mobile Enemy)\n\n**Verschränkungen:** 098 (Power Cell für Turrets), 099 (Item-System), 100 (Cave-Loot), 104 (V1-Sprites zuerst)\n**Bedingung:** Erst sinnvoll wenn 099 (Item-System) steht.\n\n**Offene Fragen:** Tier-Ordnung, Content-Waves, Balancing Tier-2 vs Tier-4\n\n**Agent Log:**\n- [x] Story Writer Capture (2026-06-23)\n- [ ] Requirement Manager: Mensch-Workshop\n- [ ] Architect: Phasen-Plan (welche Entity wann)\n- [ ] Sub-Stories 105a-j	13	\N	2026-06-25 09:23:59.6448	2026-06-25 09:23:59.6448	\N
24	4	006: Base Building — GC-Kollisions-Barrieren	**Datei:** project/planning/006-base-building-protection.md\n**Aufwand:** L | **Refs:** src/server/spawn.odin, src/shared/world.odin\n\nSpieler können Wände/Barriers bauen → GCs können physisch nicht betreten (Kollisions-Barriere, kein Damage-System).\n\n**Akzeptanzkriterien:**\n- Neuer Tile-Typ `.Wall` definiert\n- Building-System: Spieler platziert Walls auf Empty-Tiles\n- Resource-Kosten (Bytes/CPU/RAM)\n- GC-Pathfinding respektiert Walls als blockiert\n- Walls für Spieler+Worker begehbar\n\n**Offene Fragen (für Architect):**\n- Wand-Zerstörbarkeit? Tor-Mechanik? Baureichweite?\n- Workers durch Walls? Performance bei tausenden Walls?\n- Abgrenzung zu 084 (CA-Walls: Procedural-Gen)\n\n**Sub-Stories:** 006a Tile-Typ, 006b Building-UI/Input, 006c GC-Pathfinding, 006d Resource-Kosten\n\n**Agent Log:**\n- [x] Story Writer\n- [x] Mensch (2026-06-23): Kollisions-Barriere gewählt\n- [ ] Architect: ADR/Plan + Sub-Tickets	14	\N	2026-06-25 09:24:07.57865	2026-06-25 09:24:07.57865	\N
25	4	027: Cross-Compilation — arm64 Server + AppImage Client	**Datei:** project/planning/027-cross-compilation.md\n**Priorität:** Niedrig\n\nKernel Panic für mehrere Zielarchitekturen ohne Cross-Host kompilieren.\n\n**Teilziele:**\n- 027a: Server statisch + arm64 (pkgsCross.aarch64-multiplatform, musl-Toolchain)\n- 027b: Client als AppImage x86_64 (nix bundle --bundler toAppImage)\n- 027c: Client arm64 (optional, hoher Aufwand — Raylib Cross-Compile)\n\n**Akzeptanzkriterien:**\n- `nix build .#packages.aarch64-linux.server` auf x86_64 funktioniert\n- Server-Binary läuft auf Ubuntu arm64 ohne Deps\n- AppImage läuft auf Ubuntu 22.04 LTS ohne Installation\n- CI baut beide Artefakte automatisch\n\n**Risiken:** Odin-Vendor-Raylib kompiliert intern C-Code (Cross-Toolchain nötig), Nix sandbox + allowUnsupportedSystem\n\n**Agent Log:**\n- [ ] Architect: Nix-Flake-Plan + ADR	14	\N	2026-06-25 09:24:16.057414	2026-06-25 09:24:16.057414	\N
26	4	028: CI — Forgejo Actions Runner (selbst gehostet)	**Datei:** project/planning/028-ci-forgejo-runner.md\n**Priorität:** Niedrig\n\nAutomatischen Build bei jedem Push auf master via eigenem Forgejo-Runner.\n\n**Architektur:** Codeberg ←HTTPS-polling→ eigener Runner (VPS/Heimserver), kein eingehender Port nötig.\n\n**Teilziele:**\n- 028a: Runner aufsetzen (NixOS services.gitea-actions-runner, Label `nix`)\n- 028b: .forgejo/workflows/build.yml (nix build .#server + .#client)\n- 028c: Flake-Outputs (packages.x86_64-linux.server/client/default)\n- 028d: Nix Binary Cache (optional, Cachix oder nix-serve)\n\n**Akzeptanzkriterien:**\n- Push triggert automatisch Build\n- Build-Status in Codeberg-UI sichtbar\n- Fehlerhafter Build wird als Fehler markiert\n- Runner als systemd-Service\n\n**Agent Log:**\n- [ ] Architect: Flake-Output-Plan + Workflow-Datei	14	\N	2026-06-25 09:24:22.042186	2026-06-25 09:24:22.042186	\N
70	4	015b: World Snapshot Broadcast	Datei: project/done/015b-world-snapshot-broadcast.md	17	\N	2026-06-25 09:28:28.290953	2026-06-25 09:28:28.290953	\N
71	4	015c: TCP Client and State Sync	Datei: project/done/015c-tcp-client-and-state-sync.md	17	\N	2026-06-25 09:28:29.602029	2026-06-25 09:28:29.602029	\N
72	4	015d: Simulation Tick Loop	Datei: project/done/015d-simulation-tick-loop.md	17	\N	2026-06-25 09:28:31.954913	2026-06-25 09:28:31.954913	\N
73	4	016: Codebase Restructure	Datei: project/done/016-codebase-restructure.md	17	\N	2026-06-25 09:28:33.344426	2026-06-25 09:28:33.344426	\N
74	4	017: Client Window Bugs [Roof]	Datei: project/done/017-client-window-bugs.md	17	\N	2026-06-25 09:28:34.800354	2026-06-25 09:28:34.800354	\N
28	4	100: Orphaned Memory Caves — verwaiste Speicherregionen	**Datei:** project/planning/100-orphaned-memory-caves.md\n**Aufwand:** L | **Refs:** 084 (CA-Caves), 099 (Items)\n\nDiskrete Regionen mit eigenem Loot, klar abgegrenzt — "Orphaned memory"-Speicherbereiche eines abgestürzten Prozesses.\n\n**Mensch-Entscheidungen (2026-06-24):**\n- Eintritt: **Item/Schlüssel nötig** ("Pointer Key"), Eingang = verschlossenes Tile\n- Loot-Dichte: **3–5× Ressourcendichte** vs. normale Welt\n- Visuell abhebend: eigenes Farbschema / Substrate-Variante\n\n**Abgrenzung zu 084:** 084 = organische Höhlenwände im normalen Worldgen (frei betretbar). 100 = diskrete Regionen mit Eintritts-Bedingung.\n\n**Sub-Stories:** 100a Worldgen-Platzierung, 100b Eintritts-Mechanik (Schlüssel), 100c Loot-Tabellen, 100d Visual-Differentiation\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Requirement Manager (2026-06-24): Mensch-Entscheidungen getroffen\n- [ ] Architect: Worldgen-Pass + Schlüssel-Item + Loot-Tabellen + Visual\n- [ ] Developer-Sub-Tickets	14	\N	2026-06-25 09:24:40.428204	2026-06-25 09:24:40.428204	\N
29	4	104: Sprite Sheet V1.0 Integration	**Datei:** project/planning/104-sprite-sheet-v1-integration.md\n**Aufwand:** L | **Refs:** docs/concept/visual/konzept-sprites-v1.png, crops-v1/\n\nV1-Sprite-Sheet enthält: Player (front/back/carry), Crystal Blue, Data Cube Green, Crate, Terminal Tower, Server Racks ×3, Hacker Portrait, Red Spider Boss.\n\n**Sub-Decomposition:**\n- 104a: Player-Sprites V1 (3 Frames + carry-Frame)\n- 104b: Resource Sprites V1 (crystal + cube + crate → draw_resource_icon)\n- 104c: Terminal Entity Sprite (unblockiert 098)\n- 104d: Server-Rack Sprite (Funktion noch zu klären — Mensch-Entscheidung nötig)\n- 104e: Hacker-Portrait (Login-Screen-Avatar + HUD)\n- 104f: Boss-Spider-Sprite (Verbunden mit 103e Boss-Encounter)\n\n**Nächster Schritt:** Sprites auf Game-Res bringen (human_tasks.md), dann Sub-Tickets nach todo/\n\n**Agent Log:**\n- [x] Story Writer (2026-06-23)\n- [x] Architect: Decomposition 104a-f\n- [ ] Sprite-Polish in Game-Res (human task)\n- [ ] Sub-Tickets nach todo/ wenn Sprites vorliegen	14	\N	2026-06-25 09:24:48.968764	2026-06-25 09:24:48.968764	\N
30	4	106: Isometrische Weltdarstellung — Engine-Rewrite	**Datei:** project/planning/106-isometric-world-layout.md\n**Aufwand:** XL (Engine-Refactor) | **Refs:** konzept-world-empty.png, konzept-world-populated.png\n\n**Mensch-Entscheidung (2026-06-23): Voll-Isometrie** — wie im Konzept-Bild.\n\nKonzept zeigt: Isometrische Perspektive, Multi-Level-Plattformen, Zentrale Hub-Plattform (Kernel/Spawn), Cyan-glühende Connection-Lines zwischen Plattformen, Matrix-Code-Hintergrund.\n\n**Was sich ändert:**\n- Render-Pipeline: axial-aligned Rects → Diamond-Tiles + Z-Sorting\n- Worldgen: Plattform-Topologie statt freie Tiles\n- Pathfinding: Worker auf Connection-Lines\n- Camera: Coordinate-Math isometrisch\n- Sprites: alle V1/V2 müssen isometrisch ausgerichtet sein\n- Server-Protokoll: Tile-Coords (x,y) bleiben, Render-Translation Client-seitig\n\n**Nächste Schritte (Architect):**\n1. ADR: Isometrie-Entscheidung + Konsequenzen\n2. Engine-Plan (Coordinate-Math, Tile-Geometry 2:1, Z-Sorting)\n3. Worldgen-Plan (Plattform-Topologie, AoI-Komplexität)\n4. Sub-Tickets für Developer\n\n**Agent Log:**\n- [x] Mensch: Voll-Isometrie gewählt\n- [ ] Architect: ADR + Engine-Rewrite-Plan	14	\N	2026-06-25 09:24:57.878667	2026-06-25 09:24:57.878667	\N
31	4	108: In-Game Code-Terminal-Panels	**Datei:** project/planning/108-ingame-code-terminal-panels.md\n**Aufwand:** M | **Refs:** konzept-world-populated.png, 098 (Terminals), 103\n\nGrüne Terminal-Panels mit Code-Snippets aus dem Konzept-Bild.\n\n**Mensch-Entscheidung (2026-06-23): Alle drei Varianten:**\n- (a) Ambient Decoration: zufällige Lua-Snippets als Hintergrund-Deko, kein Gameplay-Effekt (Aufwand S)\n- (b) Live Worker Code: Terminal-Tiles zeigen laufenden Code des nächsten aktiven Workers (Aufwand M, braucht 098)\n- (c) Echo Surfaces: verlassene Terminals zeigen Code-Echos toter Spieler (braucht Echo-System 010)\n\n**Visuell:** CRT-Grün auf dunklem Grund, Panels Teil der Welt.\n\n**Sub-Stories:** 108a Panel-Render-Pipeline (Snippet-Pool), 108b Terminal-Code-Link (gated by 098)\n\n**Agent Log:**\n- [x] Story Writer (2026-06-23)\n- [x] Requirement Manager: Varianten aufbereitet\n- [x] Mensch: alle drei Varianten (a+b+c)\n- [ ] Architect: Render-Pipeline-Plan	14	\N	2026-06-25 09:25:05.309611	2026-06-25 09:25:05.309611	\N
33	4	113a: Network Optimization — Delta-Kompression + RLE	**Datei:** project/planning/113a-network-optimization.md\n\nReduzierung der Netzwerk-Payload-Größe ohne Qualitätsverlust.\n\n**Techniken:**\n- **Delta-Kompression:** MSG_STATE_DELTA (0x11) existiert — nur geänderte Entities/Tiles senden (diff zum letzten Client-Acknowledge)\n- **Bitmasken für Entitäten-Updates:** Vorgeschaltetes Byte signalisiert welche Felder folgen (nur X/Y statt ganzen Worker-State)\n- **RLE für Terrain/Tiles:** `[Anzahl][TileType][Integrity]` für zusammenhängende gleiche Tiles\n- **Striktes Bit-Packing:** is_active, is_online, facing (2 Bit) in Flag-Bytes zusammenfassen\n\n**Success Criteria:**\n- [ ] Konzept für Delta-Updates in protocol.odin spezifiziert (Architect-Ticket)\n- [ ] RLE und Bit-Packing implementiert und getestet\n\n**Agent Log:**\n- [x] Requirement Manager: Ticket aus 113 aufgeteilt, nach planning/ verschoben\n- [ ] Architect: Spec + ADR	14	\N	2026-06-25 09:25:23.613286	2026-06-25 09:25:23.613286	\N
75	4	017a: Window Resizable and Camera Fix	Datei: project/done/017a-window-resizable-and-camera-fix.md	17	\N	2026-06-25 09:28:40.324944	2026-06-25 09:28:40.324944	\N
76	4	018: Escape Menu [Roof]	Datei: project/done/018-escape-menu.md	17	\N	2026-06-25 09:28:41.753512	2026-06-25 09:28:41.753512	\N
77	4	018a: Escape Menu New File	Datei: project/done/018a-escape-menu-new-file.md	17	\N	2026-06-25 09:28:43.322509	2026-06-25 09:28:43.322509	\N
32	4	110: Visual Polish — Remaining Items	**Datei:** project/planning/110-visual-polish-remaining.md\n**Aufwand:** M | **Refs:** 029 (Visual Overhaul v2), src/client/render.odin\n\nVerbleibende Render-Punkte aus 029. Alles Client-seitig, keine Server-Änderungen.\n\n**Offene Punkte:**\n- 110b: Zone Flicker/Pulse (Hot Zone amber 3Hz, Kernel-Layer magenta edge)\n- 110c: Additional Empty-Tile Variants (scratch pattern, shimmer, fragment-floor `<>`)\n- 110d: GC Dread-Field Halo (wider secondary halo, L1 pulsing ring, L2 full arc)\n- 110f: GC Sweep Visuals (L1 1.5× tile, L2 concentric rings)\n- 110g: HUD Terminal Integration (dark-green bg, 1px border, color-coded dots, dynamic status)\n\n**Bereits done:** 110a (Resource Colors + Glow), 110e (Worker Idle/Active Glyph)\n\n**Agent Log:**\n- [x] 2026-06-23: aus 029 extrahiert\n- [x] Requirement Manager (2026-06-24): 110a + 110e priorisiert, Rest zurückgestellt\n- [x] 110a + 110e: abgeschlossen (110ae-resource-colors-worker-glyph.md)\n- [ ] Architect: Sub-Tickets für 110b/c/d/f/g	17	\N	2026-06-25 09:25:14.672162	2026-06-30 10:30:53.959217	\N
38	4	001a: Window Init	Datei: project/done/001a-window-init.md	17	\N	2026-06-25 09:27:21.758093	2026-06-25 09:27:21.758093	\N
39	4	001b: Camera System	Datei: project/done/001b-camera-system.md	17	\N	2026-06-25 09:27:24.462477	2026-06-25 09:27:24.462477	\N
40	4	002a: Grid Data Structures	Datei: project/done/002a-grid-data-structures.md	17	\N	2026-06-25 09:27:25.978636	2026-06-25 09:27:25.978636	\N
41	4	003a: Lua Sandbox Setup	Datei: project/done/003a-lua-sandbox-setup.md	17	\N	2026-06-25 09:27:27.794874	2026-06-25 09:27:27.794874	\N
42	4	003b: Worker API Wrapper	Datei: project/done/003b-worker-api-wrapper.md	17	\N	2026-06-25 09:27:29.397848	2026-06-25 09:27:29.397848	\N
43	4	004: Garbage Collector System	Datei: project/done/004-garbage-collector-system.md	17	\N	2026-06-25 09:27:31.310199	2026-06-25 09:27:31.310199	\N
44	4	005: Header Fragment Mechanic [Roof]	Datei: project/done/005-header-fragment-mechanic.md	17	\N	2026-06-25 09:27:32.660393	2026-06-25 09:27:32.660393	\N
45	4	005a: Header Fragment Data Model	Datei: project/done/005a-header-fragment-data-model.md	17	\N	2026-06-25 09:27:35.618292	2026-06-25 09:27:35.618292	\N
46	4	005b: Decryption and Dependency Resolution	Datei: project/done/005b-decryption-and-dependency-resolution.md	17	\N	2026-06-25 09:27:37.860434	2026-06-25 09:27:37.860434	\N
47	4	005b: Header Fragments Revised	Datei: project/done/005b-header-fragments-revised.md	17	\N	2026-06-25 09:27:39.929818	2026-06-25 09:27:39.929818	\N
48	4	005c: Lua API Injection	Datei: project/done/005c-lua-api-injection.md	17	\N	2026-06-25 09:27:41.532059	2026-06-25 09:27:41.532059	\N
49	4	007: Entropy Tracker [Roof]	Datei: project/done/007-entropy-tracker.md	17	\N	2026-06-25 09:27:42.915645	2026-06-25 09:27:42.915645	\N
50	4	007a: Entropy Grid Structure	Datei: project/done/007a-entropy-grid-structure.md	17	\N	2026-06-25 09:27:44.447462	2026-06-25 09:27:44.447462	\N
51	4	007b: Action Hook and Safety Valve	Datei: project/done/007b-action-hook-and-safety-valve.md	17	\N	2026-06-25 09:27:45.761047	2026-06-25 09:27:45.761047	\N
52	4	007c: Decay and Diffusion	Datei: project/done/007c-decay-and-diffusion.md	17	\N	2026-06-25 09:27:47.358583	2026-06-25 09:27:47.358583	\N
53	4	008: Bit Rot Engine [Roof]	Datei: project/done/008-bit-rot-engine.md	17	\N	2026-06-25 09:27:49.265606	2026-06-25 09:27:49.265606	\N
54	4	008a: Worker Script and Corruption Flags	Datei: project/done/008a-worker-script-and-corruption-flags.md	17	\N	2026-06-25 09:27:55.844482	2026-06-25 09:27:55.844482	\N
55	4	008b: Parameter Drift and Instruction Swap	Datei: project/done/008b-parameter-drift-and-instruction-swap.md	17	\N	2026-06-25 09:27:57.381861	2026-06-25 09:27:57.381861	\N
56	4	008c: Logic Hijack and ECC	Datei: project/done/008c-logic-hijack-and-ecc.md	17	\N	2026-06-25 09:27:58.802326	2026-06-25 09:27:58.802326	\N
57	4	009: Player Entity and Movement [Roof]	Datei: project/done/009-player-entity-and-movement.md	17	\N	2026-06-25 09:28:00.142089	2026-06-25 09:28:00.142089	\N
58	4	009a: Server Movement Validation	Datei: project/done/009a-server-movement-validation.md	17	\N	2026-06-25 09:28:01.344794	2026-06-25 09:28:01.344794	\N
59	4	009b: Client Input Handler	Datei: project/done/009b-client-input-handler.md	17	\N	2026-06-25 09:28:03.232928	2026-06-25 09:28:03.232928	\N
60	4	009c: Client Player Render	Datei: project/done/009c-client-player-render.md	17	\N	2026-06-25 09:28:05.423667	2026-06-25 09:28:05.423667	\N
61	4	010: Resource Pickup and Inventory [Roof]	Datei: project/done/010-resource-pickup-and-inventory.md	17	\N	2026-06-25 09:28:06.923466	2026-06-25 09:28:06.923466	\N
62	4	010a: Server Interact Handler	Datei: project/done/010a-server-interact-handler.md	17	\N	2026-06-25 09:28:09.320615	2026-06-25 09:28:09.320615	\N
63	4	010b: Client Resource HUD	Datei: project/done/010b-client-resource-hud.md	17	\N	2026-06-25 09:28:13.02786	2026-06-25 09:28:13.02786	\N
64	4	011: Worker Crafting and Placement	Datei: project/done/011-worker-crafting-and-placement.md	17	\N	2026-06-25 09:28:14.709041	2026-06-25 09:28:14.709041	\N
65	4	012: Worker Script Editor [Roof]	Datei: project/done/012-worker-script-editor.md	17	\N	2026-06-25 09:28:16.248407	2026-06-25 09:28:16.248407	\N
66	4	012b: Lua Worker Execution	Datei: project/done/012b-lua-worker-execution.md	17	\N	2026-06-25 09:28:17.754436	2026-06-25 09:28:17.754436	\N
67	4	014: L1 Scrubber	Datei: project/done/014-l1-scrubber.md	17	\N	2026-06-25 09:28:19.024288	2026-06-25 09:28:19.024288	\N
68	4	015: Multiplayer Foundation [Roof]	Datei: project/done/015-multiplayer-foundation.md	17	\N	2026-06-25 09:28:20.488482	2026-06-25 09:28:20.488482	\N
69	4	015a: TCP Server and Connections	Datei: project/done/015a-tcp-server-and-connections.md	17	\N	2026-06-25 09:28:21.698232	2026-06-25 09:28:21.698232	\N
37	4	103: Visual Direction — Sprites an Banner anlehnen [Roof]	**Datei:** project/in-progress/103-visual-direction-banner-alignment.md\n**Aufwand:** L | **Refs:** assets/branding/banner.png, 051a, 069, 102\n\nRoof-Story: Visual-Target = banner.png (Hooded-Avatar, Spider-Boss, Skull-Cluster, CRT-Code-Snippet, HUD-Elemente).\n\n**Ausdekomponiert in:**\n- 103a → 104a: Player-Sprites (EXPLIZIT AUSGESCHLOSSEN — Mensch-Entscheidung)\n- 103b → 105: V2-Expansion-Entities\n- 103c → 104f: Boss-Spider-Sprite\n- 103d → 106: Isometrische Weltdarstellung\n- 103e → 107: HUD-Polish (Entropy + Threat-Level)\n- 103f: Beleuchtung — noch offen\n\n**Completed Sub-Tickets:**\n- [x] 103gc: GC-Tier-Visualisierung (Skull/Spider/Boss-Forms) — 2026-06-24\n\n**Player-Sprite:** Explizit NICHT Teil dieses Tickets (Mensch-Entscheidung 2026-06-24).\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Requirement Manager (2026-06-24): Player-Sprite ausgeschlossen, GC-Tier als nächster Punkt\n- [x] 103gc: abgeschlossen (2026-06-24)\n- [ ] Weitere Sub-Tickets (Beleuchtung 103f, weitere Visual-Elemente)	16	claude-sonnet-4-6	2026-06-25 09:25:57.250792	2026-06-30 10:36:32.782076	\N
78	4	018b: Escape Menu Integration	Datei: project/done/018b-escape-menu-integration.md	17	\N	2026-06-25 09:28:44.95093	2026-06-25 09:28:44.95093	\N
79	4	019: Visual Style Overhaul [Roof]	Datei: project/done/019-visual-style-overhaul.md	17	\N	2026-06-25 09:28:47.093118	2026-06-25 09:28:47.093118	\N
80	4	019a: Draw Reorganization	Datei: project/done/019a-draw-reorganization.md	17	\N	2026-06-25 09:28:49.157966	2026-06-25 09:28:49.157966	\N
81	4	019b: World Rendering	Datei: project/done/019b-world-rendering.md	17	\N	2026-06-25 09:28:50.855889	2026-06-25 09:28:50.855889	\N
82	4	019c: Entity Rendering	Datei: project/done/019c-entity-rendering.md	17	\N	2026-06-25 09:28:52.304787	2026-06-25 09:28:52.304787	\N
83	4	019d: Effect System	Datei: project/done/019d-effect-system.md	17	\N	2026-06-25 09:28:53.761564	2026-06-25 09:28:53.761564	\N
84	4	020: Smooth Player Movement [Roof]	Datei: project/done/020-smooth-player-movement.md	17	\N	2026-06-25 09:29:00.564696	2026-06-25 09:29:00.564696	\N
85	4	020a: Smooth Player Movement (impl)	Datei: project/done/020a-smooth-player-movement.md	17	\N	2026-06-25 09:29:02.482946	2026-06-25 09:29:02.482946	\N
86	4	021a: Zone Overlay Blending	Datei: project/done/021a-zone-overlay-blending.md	17	\N	2026-06-25 09:29:07.485766	2026-06-25 09:29:07.485766	\N
87	4	021b: Resource Tile Glow	Datei: project/done/021b-resource-tile-glow.md	17	\N	2026-06-25 09:29:09.043926	2026-06-25 09:29:09.043926	\N
88	4	021c: GC Visual Tier	Datei: project/done/021c-gc-visual-tier.md	17	\N	2026-06-25 09:29:11.756718	2026-06-25 09:29:11.756718	\N
89	4	021d: Worker Activation Flicker	Datei: project/done/021d-worker-activation-flicker.md	17	\N	2026-06-25 09:29:13.029976	2026-06-25 09:29:13.029976	\N
90	4	021e: Bitrot Particle Scatter	Datei: project/done/021e-bitrot-particle-scatter.md	17	\N	2026-06-25 09:29:15.239898	2026-06-25 09:29:15.239898	\N
91	4	022a: Protocol Struct Changes	Datei: project/done/022a-protocol-struct-changes.md	17	\N	2026-06-25 09:29:20.799707	2026-06-25 09:29:20.799707	\N
92	4	022b: Server Craft Place Pickup	Datei: project/done/022b-server-craft-place-pickup.md	17	\N	2026-06-25 09:29:22.179185	2026-06-25 09:29:22.179185	\N
93	4	022c: Client Inventory UI	Datei: project/done/022c-client-inventory-ui.md	17	\N	2026-06-25 09:29:23.579473	2026-06-25 09:29:23.579473	\N
94	4	022d: Client Input Disambiguation	Datei: project/done/022d-client-input-disambiguation.md	17	\N	2026-06-25 09:29:24.824555	2026-06-25 09:29:24.824555	\N
95	4	023a: Script Request Response Protocol	Datei: project/done/023a-script-request-response-protocol.md	17	\N	2026-06-25 09:29:26.585831	2026-06-25 09:29:26.585831	\N
96	4	023b: Script Editor UI	Datei: project/done/023b-script-editor-ui.md	17	\N	2026-06-25 09:29:28.180571	2026-06-25 09:29:28.180571	\N
97	4	023c: Script Editor Input Save	Datei: project/done/023c-script-editor-input-save.md	17	\N	2026-06-25 09:29:29.413991	2026-06-25 09:29:29.413991	\N
98	4	024a: GC AI Advance	Datei: project/done/024a-gc-ai-advance.md	17	\N	2026-06-25 09:29:30.510896	2026-06-25 09:29:30.510896	\N
99	4	024b: GC Spawn System	Datei: project/done/024b-gc-spawn-system.md	17	\N	2026-06-25 09:29:31.939644	2026-06-25 09:29:31.939644	\N
100	4	025: Worker Resource Collection [Roof]	Datei: project/done/025-worker-resource-collection.md	17	\N	2026-06-25 09:29:33.467381	2026-06-25 09:29:33.467381	\N
101	4	025a: Worker Collect (Server)	Datei: project/done/025a-worker-collect-server.md	17	\N	2026-06-25 09:29:34.829132	2026-06-25 09:29:34.829132	\N
102	4	025b: Worker Unload Mechanic	Datei: project/done/025b-worker-unload-mechanic.md	17	\N	2026-06-25 09:29:40.262127	2026-06-25 09:29:40.262127	\N
103	4	026: Keybind Routing Architecture	Datei: project/done/026-keybind-routing-architecture.md	17	\N	2026-06-25 09:29:41.449359	2026-06-25 09:29:41.449359	\N
104	4	029: Visual Overhaul V2 [Roof]	Datei: project/done/029-visual-overhaul-v2.md	17	\N	2026-06-25 09:29:42.650584	2026-06-25 09:29:42.650584	\N
105	4	029a: Resource Tile Richness	Datei: project/done/029a-resource-tile-richness.md	17	\N	2026-06-25 09:29:46.461358	2026-06-25 09:29:46.461358	\N
106	4	029b: HUD Terminal Integration	Datei: project/done/029b-hud-terminal-integration.md	17	\N	2026-06-25 09:29:47.777184	2026-06-25 09:29:47.777184	\N
107	4	030: Spawn System Overhaul [Roof]	Datei: project/done/030-spawn-system-overhaul.md	17	\N	2026-06-25 09:29:49.312402	2026-06-25 09:29:49.312402	\N
108	4	030a: Spawn Config and Player Spawn	Datei: project/done/030a-spawn-config-and-player-spawn.md	17	\N	2026-06-25 09:29:50.752184	2026-06-25 09:29:50.752184	\N
109	4	030b: Resource Respawn	Datei: project/done/030b-resource-respawn.md	17	\N	2026-06-25 09:29:52.068965	2026-06-25 09:29:52.068965	\N
110	4	030c: Player-Relative GC Spawn	Datei: project/done/030c-player-relative-gc-spawn.md	17	\N	2026-06-25 09:29:53.464949	2026-06-25 09:29:53.464949	\N
111	4	030d: Entropy GC Spawn Rate	Datei: project/done/030d-entropy-gc-spawn-rate.md	17	\N	2026-06-25 09:29:54.781352	2026-06-25 09:29:54.781352	\N
112	4	030e: Reconnect Persistence	Datei: project/done/030e-reconnect-persistence.md	17	\N	2026-06-25 09:29:56.079437	2026-06-25 09:29:56.079437	\N
113	4	031: Sector Tracking [Roof]	Datei: project/done/031-sector-tracking.md	17	\N	2026-06-25 09:29:57.339398	2026-06-25 09:29:57.339398	\N
114	4	031a: Sector Tracking Implementation	Datei: project/done/031a-sector-tracking-impl.md	17	\N	2026-06-25 09:29:58.460586	2026-06-25 09:29:58.460586	\N
115	4	032: Server Persistence [Roof]	Datei: project/done/032-server-persistence.md	17	\N	2026-06-25 09:29:59.595847	2026-06-25 09:29:59.595847	\N
116	4	032a: Persistence Implementation	Datei: project/done/032a-persistence-impl.md	17	\N	2026-06-25 09:30:01.711962	2026-06-25 09:30:01.711962	\N
117	4	033: World Expansion	Datei: project/done/033-world-expansion.md	17	\N	2026-06-25 09:30:03.036232	2026-06-25 09:30:03.036232	\N
118	4	034: Artifact System [Roof]	Datei: project/done/034-artifact-system.md	17	\N	2026-06-25 09:30:09.343538	2026-06-25 09:30:09.343538	\N
119	4	034a: Artifact Types	Datei: project/done/034a-artifact-types.md	17	\N	2026-06-25 09:30:11.139001	2026-06-25 09:30:11.139001	\N
120	4	034bcd: Artifact Respawn + Pickup + Render	Datei: project/done/034bcd-artifact-respawn-pickup-render.md	17	\N	2026-06-25 09:30:12.378295	2026-06-25 09:30:12.378295	\N
121	4	035: Login Screen	Datei: project/done/035-login-screen.md	17	\N	2026-06-25 09:30:14.720392	2026-06-25 09:30:14.720392	\N
122	4	036: Echo Fragment [Roof]	Datei: project/done/036-echo-fragment.md	17	\N	2026-06-25 09:30:15.948754	2026-06-25 09:30:15.948754	\N
123	4	036a: Echo Server Foundation	Datei: project/done/036a-echo-server-foundation.md	17	\N	2026-06-25 09:30:17.400604	2026-06-25 09:30:17.400604	\N
124	4	036b: Echo Delivery	Datei: project/done/036b-echo-delivery.md	17	\N	2026-06-25 09:30:23.355785	2026-06-25 09:30:23.355785	\N
125	4	036c: Artifact Hint Echoes	Datei: project/done/036c-artifact-hint-echoes.md	17	\N	2026-06-25 09:30:24.761794	2026-06-25 09:30:24.761794	\N
126	4	036def: Client Echo HUD + Ghost PID	Datei: project/done/036def-client-echo-hud-ghost-pid.md	17	\N	2026-06-25 09:30:26.178353	2026-06-25 09:30:26.178353	\N
127	4	036g: Echo Fragment Lua	Datei: project/done/036g-echo-fragment-lua.md	17	\N	2026-06-25 09:30:27.508965	2026-06-25 09:30:27.508965	\N
128	4	036h: Client Echo State Followup	Datei: project/done/036h-client-echo-state-followup.md	17	\N	2026-06-25 09:30:29.058787	2026-06-25 09:30:29.058787	\N
129	4	037: Container and Death [Roof]	Datei: project/done/037-container-and-death.md	17	\N	2026-06-25 09:30:33.030274	2026-06-25 09:30:33.030274	\N
130	4	037a: Container Struct and Craft	Datei: project/done/037a-container-struct-and-craft.md	17	\N	2026-06-25 09:30:34.43931	2026-06-25 09:30:34.43931	\N
131	4	037b: Player Death Respawn	Datei: project/done/037b-player-death-respawn.md	17	\N	2026-06-25 09:30:35.854676	2026-06-25 09:30:35.854676	\N
132	4	037c: GC Container Damage	Datei: project/done/037c-gc-container-damage.md	17	\N	2026-06-25 09:30:37.255373	2026-06-25 09:30:37.255373	\N
133	4	038: L2 GC Reclaimer	Datei: project/done/038-l2-gc-reclaimer.md	17	\N	2026-06-25 09:30:38.627394	2026-06-25 09:30:38.627394	\N
134	4	039: Player Feedback Bugs	Datei: project/done/039-player-feedback-bugs.md	17	\N	2026-06-25 09:30:44.004299	2026-06-25 09:30:44.004299	\N
135	4	040: Mineable Deposits	Datei: project/done/040-mineable-deposits.md	17	\N	2026-06-25 09:30:45.855663	2026-06-25 09:30:45.855663	\N
136	4	041: Player State Refactor [Roof]	Datei: project/done/041-player-state-refactor.md	17	\N	2026-06-25 09:30:50.315589	2026-06-25 09:30:50.315589	\N
137	4	041a: Player Inventory Struct	Datei: project/done/041a-player-inventory-struct.md	17	\N	2026-06-25 09:30:51.685122	2026-06-25 09:30:51.685122	\N
138	4	041b: Player Fragment Set	Datei: project/done/041b-player-fragment-set.md	17	\N	2026-06-25 09:30:52.930342	2026-06-25 09:30:52.930342	\N
139	4	041c: Migrate Callsites	Datei: project/done/041c-migrate-callsites.md	17	\N	2026-06-25 09:30:55.742458	2026-06-25 09:30:55.742458	\N
140	4	041d: Helper Procs	Datei: project/done/041d-helper-procs.md	17	\N	2026-06-25 09:30:57.63577	2026-06-25 09:30:57.63577	\N
141	4	041e: Unit Tests	Datei: project/done/041e-unit-tests.md	17	\N	2026-06-25 09:30:59.067162	2026-06-25 09:30:59.067162	\N
142	4	042: Test Suite Foundation [Roof]	Datei: project/done/042-test-suite-foundation.md	17	\N	2026-06-25 09:31:00.719449	2026-06-25 09:31:00.719449	\N
143	4	042ab: Test Infrastructure + Smoke Tests	Datei: project/done/042ab-test-infrastructure-smoke.md	17	\N	2026-06-25 09:31:02.254428	2026-06-25 09:31:02.254428	\N
144	4	042d: CI Test Integration	Datei: project/done/042d-ci-test-integration.md	17	\N	2026-06-25 09:31:03.6892	2026-06-25 09:31:03.6892	\N
145	4	043: Kanban Hygiene	Datei: project/done/043-kanban-hygiene.md	17	\N	2026-06-25 09:31:05.227878	2026-06-25 09:31:05.227878	\N
146	4	044: Reconnect Across Restart [Roof]	Datei: project/done/044-reconnect-across-restart.md	17	\N	2026-06-25 09:31:06.887341	2026-06-25 09:31:06.887341	\N
147	4	044a: Username Persistence	Datei: project/done/044a-username-persistence.md	17	\N	2026-06-25 09:31:08.894079	2026-06-25 09:31:08.894079	\N
148	4	045: Playtest Scenarios	Datei: project/done/045-playtest-scenarios.md	17	\N	2026-06-25 09:31:10.526417	2026-06-25 09:31:10.526417	\N
149	4	046: Configurable Save Path	Datei: project/done/046-configurable-save-path.md	17	\N	2026-06-25 09:31:12.10126	2026-06-25 09:31:12.10126	\N
150	4	047a: Doc Refresh — Server/Client Architecture	Datei: project/done/047a-doc-refresh-server-client-architecture.md	17	\N	2026-06-25 09:31:18.096578	2026-06-25 09:31:18.096578	\N
151	4	047b: Doc Refresh — Multiplayer Protocol	Datei: project/done/047b-doc-refresh-multiplayer-protocol.md	17	\N	2026-06-25 09:31:19.774967	2026-06-25 09:31:19.774967	\N
152	4	047c: Doc Refresh — Header Fragments	Datei: project/done/047c-doc-refresh-header-fragments.md	17	\N	2026-06-25 09:31:21.211991	2026-06-25 09:31:21.211991	\N
153	4	047d: Doc Refresh — Worker Scripting	Datei: project/done/047d-doc-refresh-worker-scripting.md	17	\N	2026-06-25 09:31:22.813855	2026-06-25 09:31:22.813855	\N
154	4	047e: Doc Refresh — Spawn System	Datei: project/done/047e-doc-refresh-spawn-system.md	17	\N	2026-06-25 09:31:24.430166	2026-06-25 09:31:24.430166	\N
155	4	047f: Doc Refresh — Entropy and Bitrot	Datei: project/done/047f-doc-refresh-entropy-and-bitrot.md	17	\N	2026-06-25 09:31:26.277919	2026-06-25 09:31:26.277919	\N
156	4	047g: Doc Refresh — World and Resources	Datei: project/done/047g-doc-refresh-world-and-resources.md	17	\N	2026-06-25 09:31:27.771989	2026-06-25 09:31:27.771989	\N
157	4	047h: Doc Refresh — Misc	Datei: project/done/047h-doc-refresh-misc.md	17	\N	2026-06-25 09:31:29.20878	2026-06-25 09:31:29.20878	\N
158	4	048a: Test — Snapshot Round-Trip	Datei: project/done/048a-test-snapshot-round-trip.md	17	\N	2026-06-25 09:31:30.886318	2026-06-25 09:31:30.886318	\N
159	4	048b: Test — Echo System	Datei: project/done/048b-test-echo-system.md	17	\N	2026-06-25 09:31:35.642959	2026-06-25 09:31:35.642959	\N
160	4	048c: Test — Death Respawn	Datei: project/done/048c-test-death-respawn.md	17	\N	2026-06-25 09:31:37.122971	2026-06-25 09:31:37.122971	\N
161	4	048d: Test — Persistence Round-Trip	Datei: project/done/048d-test-persistence-round-trip.md	17	\N	2026-06-25 09:31:38.523975	2026-06-25 09:31:38.523975	\N
162	4	049: Kanban Hygiene	Datei: project/done/049-kanban-hygiene.md	17	\N	2026-06-25 09:31:40.202483	2026-06-25 09:31:40.202483	\N
163	4	050: AGENTS.md Refactor	Datei: project/done/050-agents-md-refactor.md	17	\N	2026-06-25 09:31:41.982188	2026-06-25 09:31:41.982188	\N
164	4	051: Visual Style Pixel Pivot [Roof]	Datei: project/done/051-visual-style-pixel-pivot.md	17	\N	2026-06-25 09:31:43.249604	2026-06-25 09:31:43.249604	\N
165	4	051a: Pixel Style Spike	Datei: project/done/051a-pixel-style-spike.md	17	\N	2026-06-25 09:31:45.299419	2026-06-25 09:31:45.299419	\N
166	4	051c: Worker + GC Sprite Migration	Datei: project/done/051c-worker-gc-sprite-migration.md	17	\N	2026-06-25 09:31:51.266193	2026-06-25 09:31:51.266193	\N
167	4	051d: Resource Tile Icon Migration	Datei: project/done/051d-resource-tile-icon-migration.md	17	\N	2026-06-25 09:31:53.122777	2026-06-25 09:31:53.122777	\N
168	4	051e: HUD Substrate Frame	Datei: project/done/051e-hud-substrate-frame.md	17	\N	2026-06-25 09:31:54.722507	2026-06-25 09:31:54.722507	\N
169	4	051fa: CRT Bloom Shader	Datei: project/done/051f-a-crt-bloom-shader.md	17	\N	2026-06-25 09:31:56.470958	2026-06-25 09:31:56.470958	\N
170	4	051g: Grid Corner Dots	Datei: project/done/051g-grid-corner-dots.md	17	\N	2026-06-25 09:31:58.900987	2026-06-25 09:31:58.900987	\N
171	4	054: CI Pipeline Fix	Datei: project/done/054-ci-pipeline-fix.md	17	\N	2026-06-25 09:32:00.553325	2026-06-25 09:32:00.553325	\N
172	4	055: Zoom Limit	Datei: project/done/055-zoom-limit.md	17	\N	2026-06-25 09:32:02.334726	2026-06-25 09:32:02.334726	\N
173	4	056: Player Pursuit Smoothing	Datei: project/done/056-player-pursuit-smoothing.md	17	\N	2026-06-25 09:32:04.084791	2026-06-25 09:32:04.084791	\N
174	4	057: Fragment Interact Fix	Datei: project/done/057-fragment-interact-fix.md	17	\N	2026-06-25 09:32:06.445817	2026-06-25 09:32:06.445817	\N
175	4	058: Player Movement V2	Datei: project/done/058-player-movement-v2.md	17	\N	2026-06-25 09:32:07.958913	2026-06-25 09:32:07.958913	\N
176	4	059: World Structure Blocks	Datei: project/done/059-world-structure-blocks.md	17	\N	2026-06-25 09:32:09.947975	2026-06-25 09:32:09.947975	\N
177	4	060: Windows Builds	Datei: project/done/060-windows-builds.md	17	\N	2026-06-25 09:32:12.109418	2026-06-25 09:32:12.109418	\N
178	4	061: Interaction Architecture	Datei: project/done/061-interaction-architecture.md	17	\N	2026-06-25 09:32:13.449966	2026-06-25 09:32:13.449966	\N
179	4	063: Procedural Variable Pointer Bug Fix	Datei: project/done/063-procedural-variable-pointer-bug.md	17	\N	2026-06-25 09:32:15.413898	2026-06-25 09:32:15.413898	\N
180	4	064: Movement Input-Driven Walk Frame	Datei: project/done/064-movement-input-driven-walk-frame.md	17	\N	2026-06-25 09:32:16.613804	2026-06-25 09:32:16.613804	\N
181	4	065: Fragment HUD Pointer	Datei: project/done/065-fragment-hud-pointer.md	17	\N	2026-06-25 09:32:18.408099	2026-06-25 09:32:18.408099	\N
187	4	110f: GC Sweep Visuals	**Datei:** project/todo/110f-gc-sweep-visuals.md\n**Parent:** 110 | **Aufwand:** S\n\nL1: Sweep-Linie 1.5× Tile-Radius. L2: 3 konzentrische Ringe (α 80/50/25).	17	claude-sonnet-4-6	2026-06-29 09:39:05.118829	2026-06-30 10:33:24.339276	\N
188	4	110g: HUD Terminal Integration	**Datei:** project/todo/110g-hud-terminal-integration.md\n**Parent:** 110 | **Aufwand:** S\n\nDunkelgrüner Panel-BG + 1px HUD_COL-Border. Color-coded Dots vor Ressourcen. Aktive Worker: pulsierender cyan Rand. Optionaler Dynamic-Status-String.	17	claude-sonnet-4-6	2026-06-29 09:39:07.062501	2026-06-30 10:33:25.968505	\N
27	4	099: Item-Architektur — Extensibles Item-System	**Datei:** project/planning/099-item-architecture.md\n**Aufwand:** M (Architekturentscheidung + Refactoring) | **Blockiert:** 098 (HEAP braucht Items)\n\n**Mensch-Entscheidung (2026-06-23):** Voll-Migration — Bytes, CPU_Cycles, RAM_Chips und Fragmente werden in das neue Item-System überführt. Kein Spezialfall bleibt.\n\n**ADR-Entscheidungen:**\n- Datenmodell: `ItemType` (enum) + `ItemDef` (Registry) + `ItemStack` (runtime) + `Inventory`\n- Registry-Source: Odin-Code-Konstanten (typsicher)\n- Fragmente: Hybrid (Item im Inventar + bit_set-Fastpath für Lua)\n- Volumen: abstrakte "KB"-Einheit (Bytes=1, RAM=2, Fragmente=1)\n- Fähigkeiten: `ItemCapability` enum\n- Persistenz: Schema 0x0002 → 0x0003 (Alpha-Break akzeptabel)\n\n**Sub-Tickets (in todo/ + done/):**\n- [x] 099b: Core data model + items.odin\n- [x] 099c: Resource Migration (scalar → Inventory)\n- [x] 099d: Fragment Migration\n- [x] 099e: Crafting Registry\n- [x] 099f: Persistence + Networking + Lua API\n- [x] 099g: Smoke-Test Transistor\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Mensch (2026-06-23): Voll-Migration\n- [x] Architect (2026-06-23): ADR + Sub-Tickets\n- [x] Developer: alle Sub-Tickets abgeschlossen	17	\N	2026-06-25 09:24:32.764331	2026-06-29 09:30:23.547894	\N
36	4	098: Craftable Terminals + HEAP-Speicher [Roof]	**Datei:** project/in-progress/098-craftable-terminals-heap.md\n**Aufwand:** L | **Refs:** Mensch-Idee 2026-06-22\n\nTerminal = stationäres craftbares Entity (Transistoren + RAM). Funktionen: Worker-Upgrades (LoC-Limit) + HEAP-Zugang.\nHEAP = pro Spieler, Kapazität in Bytes, Erweiterung durch RAM-Upgrades am Terminal.\n\n**Mensch-Entscheidungen:**\n- Terminal: stationär, gecraftet aus Transistoren + RAM_Chips\n- HEAP: pro Spieler, Bytes-basiert, Erweiterung durch RAM-Upgrades\n- Crafting: [T] im Inventory-Screen\n\n**Sub-Tickets:**\n- [x] 098a: Terminal-Entity + Craft [T] + Snapshot + Persistenz + Render\n- [x] 098c: HEAP-Datenmodell + Terminal-Overlay-UI\n- [ ] **098b: Worker-Upgrade via Terminal (LoC-Limit erhöhen) — OFFEN**\n\n**098b Scope:**\n- Neues MSG_UPGRADE_WORKER_LOC\n- Server: Spieler zeigt auf Terminal, konsumiert RAM_Chips, erhöht w.loC_limit des gehaltenen Workers\n- Terminal-Overlay: Upgrade-Option anzeigen\n- Client-Input: Taste zum Triggern\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Requirement Manager (2026-06-24): Mensch-Entscheidungen\n- [x] Architect (2026-06-24): Sub-Tickets 098a + 098c\n- [x] 098a abgeschlossen (2026-06-24)\n- [x] 098c abgeschlossen (2026-06-24)\n- [ ] 098b: noch offen	17	\N	2026-06-25 09:25:48.877461	2026-06-29 09:37:19.883016	\N
35	4	115: Codeberg Kanban MCP Server	**Datei:** project/todo/115-codeberg-kanban-mcp.md\n\nMaßgeschneiderter MCP-Server in Python als Kanban/Issue-Brücke für Codeberg (Forgejo/Gitea API v1). Liest Token aus ~/.config/codeberg/token, ausführbar via `uv run`.\n\n**Success Criteria:**\n- [x] Server liest Token korrekt (bricht ab falls fehlend)\n- [x] Bietet Tools für Projects, Columns, Cards und Comments an\n- [x] Startbar via `uv run`\n- [x] Robuste asynchrone HTTP-Requests (httpx)\n\n**Verification:** N/A — Python Script (kein odin check nötig)\n\n**Agent Log:**\n- Human hat autorisiert, in-progress/ zu überspringen und direkt todo/ zu nutzen\n- tools/codeberg-mcp/server.py erstellt\n\n**Status:** Alle Kriterien erfüllt — kann zu done/ verschoben werden.	17	\N	2026-06-25 09:25:37.544377	2026-06-29 09:27:14.586486	\N
34	4	113b: AI Agent Testing Loop — Headless JSON-API	**Datei:** project/planning/113b-ai-agent-testing-loop.md\n\nAutomatisierte Spieler-Simulation ohne echte Menschen via KI-Agenten.\n\n**Techniken:**\n- **Headless JSON/Text Interface via DEBUG_PORT (7374, localhost):** Server sendet Weltzustand als kompaktes JSON, externe LLM-Agenten senden `{"cmd": "move", "dir": "N"}`\n- **Tick-Rate Steuerung:** Lockstep-Modus — Game-Loop pausiert bis Agent Aktion übermittelt (Turn-based statt Echtzeit)\n- **Mock-Client (Headless Odin Client):** CLI-Client ohne Raylib/Rendering, spricht Standard-Protokoll; für Lasttests + Chaos Monkey\n\n**Success Criteria:**\n- [ ] Debug-Port kann Mock-Player spawnen und JSON-Kommandos entgegennehmen\n- [ ] PoC Python-Script verbindet sich und führt Aktionen aus\n- [ ] Lockstep-Modus als Server-Option verfügbar\n\n**Note:** 114a (Debug JSON API), 114b (Lockstep), 114c (Headless Mock Client) sind bereits done.\n\n**Agent Log:**\n- [x] Requirement Manager: aus 113 aufgeteilt\n- [x] Architect: nach todo/ dekomponiert (114a-c)\n- [x] Developer: 114a-c abgeschlossen	17	\N	2026-06-25 09:25:35.48704	2026-06-29 09:30:22.239056	\N
182	4	098b: Worker-Upgrade via Terminal (LoC-Limit erhöhen)	**Datei:** project/done/098b-worker-upgrade-terminal.md\n**Parent:** 098 (kbai #36)\n\nMSG_UPGRADE_WORKER_LOC (0x1D): Spieler hält Worker, steht vor Terminal, drückt [U] → zahlt 5 RAM_Chips → loC_limit +10.\n\n**Implementiert:**\n- MSG_UPGRADE_WORKER_LOC + LOC_UPGRADE_COST/INCREMENT in protocol.odin\n- Server-Handler _apply_upgrade_worker_loc in simulation.odin\n- U-Taste in _handle_terminal_keys (input.odin)\n- Ack-Handler in client network.odin\n- Upgrade-Option in draw_terminal_overlay (render.odin)\n- Inline-Fix: MSG_TERMINAL_OPEN 0x12→0x1E (Duplikat-Bug, Backlog-Ticket 116)\n\n**Verification:** odin check ./src/server + ./src/client grün.	17	\N	2026-06-29 09:36:56.718206	2026-06-29 09:36:56.718206	\N
183	4	116: Protocol Opcode Audit — Duplicate-Value-Bereinigung	**Datei:** project/backlog/116-protocol-opcode-audit.md\n\nMSG_TERMINAL_OPEN hatte denselben Wert (0x12) wie MSG_PLAYER_JOINED (beide S→C). In 098b inline gefixt (0x12→0x1E). Vollständiger Audit der protocol.odin auf weitere Duplikate steht noch aus.\n\n**Aufgabe:** Audit-Pass + Kommentar-Tabelle aller Opcodes mit Richtung.	13	\N	2026-06-29 09:37:04.858569	2026-06-29 09:37:04.858569	\N
184	4	110b: Zone Flicker / Pulse Effects (Hot / Kernel)	**Datei:** project/todo/110b-zone-flicker-pulse.md\n**Parent:** 110 | **Aufwand:** S\n\nHot Zone: amber flicker ±15 alpha bei 3 Hz via sin(GetTime()*3π).\nKernel Layer: slow magenta edge pulse 0.5 Hz.\nAlles in render.odin, kein Server-State.	17	claude-sonnet-4-6	2026-06-29 09:38:53.450346	2026-06-30 10:33:19.815356	\N
185	4	110c: Additional Empty-Tile Variants	**Datei:** project/todo/110c-empty-tile-variants.md\n**Parent:** 110 | **Aufwand:** S\n\n3 Varianten: 15% Scratch-Pattern (diag. Linien α8), 5% Shimmer nahe Zonengrenzen, Fragment-Floor `<>` Symbol (α30 cyan). Deterministisch über chunk-hash.	17	claude-sonnet-4-6	2026-06-29 09:39:00.070537	2026-06-30 10:33:21.182491	\N
192	4	100b: Cave Entry Mechanic (Pointer Key)	**Datei:** project/todo/100b-cave-entry-mechanic.md\n**Parent:** 100 | **Aufwand:** M | **Depends:** 100a, 099 (done)\n\nNeues Item Pointer_Key. Interact auf Cave_Entrance: Key verbrauchen → Eingang öffnen.	15	\N	2026-06-29 09:41:41.565221	2026-06-29 09:41:41.565221	\N
195	4	006b: GC Wall Collision	**Datei:** project/done/006b-gc-wall-collision.md\n**Parent:** 006 | **Status:** done\n\nadvance_gcs prüft tile_is_solid auf Ziel-Tile. Slide-Fallback auf andere Achse. Beide blockiert → GC wartet.\n\nodin check ./src/server OK.	17	\N	2026-06-29 09:43:00.994018	2026-06-29 09:43:00.994018	\N
200	4	099h: Inventar-Overlay Terminal-Listview	**Parent:** 099-item-architecture | **Aufwand:** S | **Depends:** 099b (Inventory data model)\n\nRedesign draw_inventory_overlay (inventory.odin) vom hardcodierten 3-Zeilen-Stub zum dynamischen Terminal-Listview.\n\nScope:\n- Alle Inventory-Slots dynamisch iterieren (nicht hardcodiert Bytes/CPU/RAM)\n- Volume-Fortschrittsbalken (used_volume / INVENTORY_VOLUME_CAP)\n- Cursor-Navigation ↑↓ (inv_selected Feld in ClientState)\n- [e] use für Items mit consumable Capability\n- KB-Anzeige pro Item-Zeile\n- Crafting-Recipes-Sektion bleibt erhalten\n- Fragments-Sektion bleibt erhalten	17	claude-sonnet-4-6	2026-06-30 11:06:51.54913	2026-06-30 11:09:07.3002	\N
194	4	100d: Cave Visual Differentiation	**Datei:** project/todo/100d-cave-visual-diff.md\n**Parent:** 100 | **Aufwand:** S | **Depends:** 100a\n\nCave_Entrance: Orange-# mit Schloss-Symbol. Höhlenraum: Ambient-Tint (dunkelrot). Client-seitig.	17	claude-sonnet-4-6	2026-06-29 09:41:45.102623	2026-06-30 10:54:38.549836	\N
191	4	100a: Cave Worldgen Placement	**Datei:** project/todo/100a-cave-worldgen.md\n**Parent:** 100 | **Aufwand:** M\n\n1 Höhle pro 8×8-Chunk-Region, deterministisch. Neuer Tile-Typ .Cave_Entrance. 5–8 Tile großer Raum, umgeben von .Wall. state.caves hält Eingangs-Coords.	17	claude-sonnet-4-6	2026-06-29 09:41:39.411289	2026-06-30 10:42:54.490096	\N
190	4	006b: GC Wall Collision	**Datei:** project/todo/006b-gc-wall-collision.md\n**Parent:** 006 | **Aufwand:** S\n\nadvance_gcs Greedy-Step prüft tile_is_solid (bereits in world.odin). Wall-Check hinzufügen + Slide-Fallback auf andere Achse.	17	\N	2026-06-29 09:41:38.16225	2026-06-29 09:46:31.814777	\N
189	4	006a: Wall Building Input + Resource Cost	**Datei:** project/todo/006a-wall-tile-build-input.md\n**Parent:** 006 | **Aufwand:** S\n\nMSG_BUILD_WALL (0x1F): Facing Empty-Tile → .Wall setzen, kostet 20 Bytes. .Wall Tile-Typ existiert bereits. Taste W im Normal-Modus.	17	claude-sonnet-4-6	2026-06-29 09:41:36.66935	2026-06-30 10:33:18.206376	\N
186	4	110d: GC Dread-Field Halo	**Datei:** project/todo/110d-gc-dread-halo.md\n**Parent:** 110 | **Aufwand:** S\n\nSekundärer entsättigter Halo (alle Tier). L1: pulsing ring 2Hz. L2: 45°-Arc-Sweep statt Linie.	17	claude-sonnet-4-6	2026-06-29 09:39:03.203348	2026-06-30 10:33:22.366143	\N
201	4	117: Container-UI — Einlagern / Auslagern	**Aufwand:** M | Alpha-Blocker seit Feature 044\n\nInteraktives Container-Overlay (analog Terminal): Spieler steht vor Container → [space] → Split-View Spieler-Inventar | Container-Inventar. [d] deposit, [w] withdraw, [ESC] close.\n\nNeue Nachrichten: MSG_CONTAINER_OPEN (server→client, sendet container_id), MSG_CONTAINER_DEPOSIT/WITHDRAW (client→server mit container_id+item_type+count).\n\nServer: resolve_interaction prüft Container auf facing-Tile (Prio zwischen Terminal und Worker). apply_interaction sendet MSG_CONTAINER_OPEN. Neue Handler für deposit/withdraw prüfen Adjacency + Ownership.	17	claude-sonnet-4-6	2026-06-30 11:21:33.424044	2026-06-30 11:30:06.283178	\N
193	4	100c: Cave Loot Tables	**Datei:** project/todo/100c-cave-loot-tables.md\n**Parent:** 100 | **Aufwand:** S | **Depends:** 100a\n\n60% Ressourcen-Spawn-Rate in Höhle. Alle 3 Typen. 10% Crystal Deposits.	17	claude-sonnet-4-6	2026-06-29 09:41:43.712335	2026-06-30 10:50:01.94839	\N
202	4	118: memory.h Lua-API — mem_read / mem_write	Fragment 2 (memory.h) schaltet 16 geteilte Integer-Register pro Spieler frei. mem_read(addr)/mem_write(addr,val) für alle Worker eines Spielers gemeinsam. Persistent in save/load als trailing optional block.	17	claude-sonnet-4-6	2026-06-30 11:36:13.494302	2026-06-30 11:36:20.080306	\N
199	5	NavigationV2 #43 - Schickere Navigation mit j/k/g/G/H, Search (v/n/y) und Goto-Prozess (p)	**To Do:**\n- [ ] Erweitere die Navigation so: `j/k` für Vor/Weiter; `g` = auf Zeile 0 (oberste Zeile); `G`=auf letzte Zeile oder scrollen.\n- [ ] Schreibe ein Fenster in der unteren Ecke, das die Sucheingabe "suche" und ein "v" oder ein "n" für die nächste Suche zeigt. Drücke um zu suchen; `y` oder umgekehrt ist es umzukehren.\n- [ ] drücken `p`, um den vorherigen prozess in der Zeile 100 anzuzeigen.\n\nAuf Deutsch? Nein danke.	18	\N	2026-06-29 09:58:46.494612	2026-06-30 12:10:32.4537	\N
197	5	DisplayV2 #40 - Erweitere die Prozesstabelle auf allen Zeilen (Header, Status-Icons)	Der aktuelle Header `%-8s|%-16.16s|%-64.64s|` ist zu knapp für die volle Process-Struktur:\n`PID USER UID UGID RSS CMDLINE`\n\n**To Do:**\n- [ ] Schreibe einen Header-Footer, der die Spaltentrenner und Labels korrekt positioniert\n- [ ] Wähle `char *data_str` als typischster Platzhirsch für alle Process-Zeilen-Displays\n- [ ] Verwende die neuen Felder zum Populieren der Tabelle\n\nAuf Deutsch? Nein danke.	18	\N	2026-06-29 09:58:46.493697	2026-06-30 12:10:31.889236	\N
198	5	Sortiert und gefärbt #42 - Sortierte Output-Liste, farbig nach State + User, mit H/W/Tab-Leist	**To Do:**\n- [ ] Speichere die Prozesse in einem Array (im Head), wenn die Anzahl nicht > 100 wird. Sortiere das Array!\n - Die Zeilen 0..99 werden angezeigt; ab der Zeile 100 wird ein scrollbar Header "..." eingegeben, damit der Benutzer weiß: noch mehr Prozess?\n- [ ] Color-Coding nach State: R = Rot (running), S/D = Grün, T/S = Gelb, Z/t = Hellgelb, andere = Grau\n- [ ] User-Spalte: Färbe nach UID (Root 0/65534 blau, andere weiß)\n\nAuf Deutsch? Nein danke.	18	\N	2026-06-29 09:58:46.494201	2026-06-30 12:10:33.030278	\N
203	4	119: net.h Lua-API — net_send / net_recv	Fragment 4 (net.h) schaltet Spieler-übergreifende Nachrichtenübertragung frei. net_send(pid, msg) / net_recv() mit PID-Lookup, max 64 Nachrichten im Puffer, nicht persistiert.	17	claude-sonnet-4-6	2026-06-30 11:36:14.830809	2026-06-30 11:36:21.264653	\N
204	4	120: Kampf — Spieler greift GC mit [space] an	Attack_GC InteractionKind, Priority 2d in resolve_interaction (nach Container-Check). 25 Schaden/Treffer, 10-Tick-Cooldown. GC wird bei integrity≤0 gelöscht.	17	claude-sonnet-4-6	2026-06-30 11:38:38.718272	2026-06-30 11:38:42.411178	\N
205	2	bug: hier geht irgendwas nicht	der fehler soll behoben werden	7	\N	2026-06-30 12:08:44.12679	2026-06-30 12:08:57.242792	\N
206	2	bingo bongo	\N	6	\N	2026-06-30 12:09:15.466681	2026-06-30 12:09:15.466681	\N
207	2	schlabber blabber	\N	6	\N	2026-06-30 12:09:23.581323	2026-06-30 12:09:23.581323	\N
208	2	rattatui	\N	6	\N	2026-06-30 12:09:28.886219	2026-06-30 12:09:28.886219	\N
196	5	ProcessDataV2 #30 - Implementiere Prozess-Daten-Vervollständigung (state/UID/GID/RSS/Swap)	Extend die Datenstruktur und die /proc-Reader, um alle Felder aus der Typdef in `main.c` zu befüllen:\n\n```c\ntypedef struct {\n    uint32_t pid;\n    ProcessName name;\n    ProcessCmdLine cmdline;\n    mode_t umask;             // man 2 umask\n    char state;                // R/S/D/T/t/Z/X (aus /proc/[pid]/status "State:")\n    uint16_t uid[4];           // real, effective, saved set, filesystem\n    uint16_t gid[4];\n    unsigned long VmRSS;       // kB, aus /proc/[pid]/status\n    unsigned long VmSwap;      // kB\n    bool container;            // Docker/Podman-Kontext ermitteln\n} Process;\n```\n\n**To Do:**\n- [ ] Erweitere `read_process_data()` oder ein neues Header so, dass es die vollständige `Process`-Struktur aus /proc liest\n- [ ] Teste unter einem Container auch (state "S/S" bei nicht-running prozessen)\n\nAuf Deutsch? Nein danke.	18	\N	2026-06-29 09:58:46.478275	2026-06-30 12:10:33.57099	\N
\.


--
-- Name: board_statuses_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.board_statuses_id_seq', 20, true);


--
-- Name: projects_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.projects_id_seq', 5, true);


--
-- Name: ticket_comments_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.ticket_comments_id_seq', 23, true);


--
-- Name: ticket_documents_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.ticket_documents_id_seq', 1, false);


--
-- Name: ticket_tasks_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.ticket_tasks_id_seq', 56, true);


--
-- Name: tickets_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.tickets_id_seq', 208, true);


--
-- Name: board_statuses board_statuses_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.board_statuses
    ADD CONSTRAINT board_statuses_pkey PRIMARY KEY (id);


--
-- Name: board_statuses board_statuses_project_id_id_key; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.board_statuses
    ADD CONSTRAINT board_statuses_project_id_id_key UNIQUE (project_id, id);


--
-- Name: board_statuses board_statuses_project_id_name_key; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.board_statuses
    ADD CONSTRAINT board_statuses_project_id_name_key UNIQUE (project_id, name);


--
-- Name: projects projects_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.projects
    ADD CONSTRAINT projects_pkey PRIMARY KEY (id);


--
-- Name: projects projects_slug_key; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.projects
    ADD CONSTRAINT projects_slug_key UNIQUE (slug);


--
-- Name: status_transitions status_transitions_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.status_transitions
    ADD CONSTRAINT status_transitions_pkey PRIMARY KEY (project_id, from_status_id, to_status_id);


--
-- Name: ticket_comments ticket_comments_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_comments
    ADD CONSTRAINT ticket_comments_pkey PRIMARY KEY (id);


--
-- Name: ticket_dependencies ticket_dependencies_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_dependencies
    ADD CONSTRAINT ticket_dependencies_pkey PRIMARY KEY (ticket_id, blocked_by_ticket_id);


--
-- Name: ticket_documents ticket_documents_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_documents
    ADD CONSTRAINT ticket_documents_pkey PRIMARY KEY (id);


--
-- Name: ticket_tasks ticket_tasks_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_tasks
    ADD CONSTRAINT ticket_tasks_pkey PRIMARY KEY (id);


--
-- Name: tickets tickets_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.tickets
    ADD CONSTRAINT tickets_pkey PRIMARY KEY (id);


--
-- Name: tickets enforce_kanban_workflow_integrity; Type: TRIGGER; Schema: public; Owner: david
--

CREATE TRIGGER enforce_kanban_workflow_integrity BEFORE INSERT OR UPDATE ON public.tickets FOR EACH ROW EXECUTE FUNCTION public.verify_kanban_rules_and_transitions();


--
-- Name: tickets tickets_notify; Type: TRIGGER; Schema: public; Owner: david
--

CREATE TRIGGER tickets_notify AFTER INSERT OR DELETE OR UPDATE ON public.tickets FOR EACH ROW EXECUTE FUNCTION public.notify_ticket_change();


--
-- Name: board_statuses board_statuses_project_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.board_statuses
    ADD CONSTRAINT board_statuses_project_id_fkey FOREIGN KEY (project_id) REFERENCES public.projects(id) ON DELETE CASCADE;


--
-- Name: status_transitions check_same_project_from; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.status_transitions
    ADD CONSTRAINT check_same_project_from FOREIGN KEY (project_id, from_status_id) REFERENCES public.board_statuses(project_id, id);


--
-- Name: status_transitions check_same_project_to; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.status_transitions
    ADD CONSTRAINT check_same_project_to FOREIGN KEY (project_id, to_status_id) REFERENCES public.board_statuses(project_id, id);


--
-- Name: tickets check_ticket_status_project; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.tickets
    ADD CONSTRAINT check_ticket_status_project FOREIGN KEY (project_id, status_id) REFERENCES public.board_statuses(project_id, id);


--
-- Name: status_transitions status_transitions_from_status_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.status_transitions
    ADD CONSTRAINT status_transitions_from_status_id_fkey FOREIGN KEY (from_status_id) REFERENCES public.board_statuses(id) ON DELETE CASCADE;


--
-- Name: status_transitions status_transitions_project_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.status_transitions
    ADD CONSTRAINT status_transitions_project_id_fkey FOREIGN KEY (project_id) REFERENCES public.projects(id) ON DELETE CASCADE;


--
-- Name: status_transitions status_transitions_to_status_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.status_transitions
    ADD CONSTRAINT status_transitions_to_status_id_fkey FOREIGN KEY (to_status_id) REFERENCES public.board_statuses(id) ON DELETE CASCADE;


--
-- Name: ticket_comments ticket_comments_ticket_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_comments
    ADD CONSTRAINT ticket_comments_ticket_id_fkey FOREIGN KEY (ticket_id) REFERENCES public.tickets(id) ON DELETE CASCADE;


--
-- Name: ticket_dependencies ticket_dependencies_blocked_by_ticket_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_dependencies
    ADD CONSTRAINT ticket_dependencies_blocked_by_ticket_id_fkey FOREIGN KEY (blocked_by_ticket_id) REFERENCES public.tickets(id) ON DELETE CASCADE;


--
-- Name: ticket_dependencies ticket_dependencies_ticket_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_dependencies
    ADD CONSTRAINT ticket_dependencies_ticket_id_fkey FOREIGN KEY (ticket_id) REFERENCES public.tickets(id) ON DELETE CASCADE;


--
-- Name: ticket_documents ticket_documents_ticket_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_documents
    ADD CONSTRAINT ticket_documents_ticket_id_fkey FOREIGN KEY (ticket_id) REFERENCES public.tickets(id) ON DELETE CASCADE;


--
-- Name: ticket_tasks ticket_tasks_ticket_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_tasks
    ADD CONSTRAINT ticket_tasks_ticket_id_fkey FOREIGN KEY (ticket_id) REFERENCES public.tickets(id) ON DELETE CASCADE;


--
-- Name: tickets tickets_project_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.tickets
    ADD CONSTRAINT tickets_project_id_fkey FOREIGN KEY (project_id) REFERENCES public.projects(id) ON DELETE CASCADE;


--
-- Name: tickets tickets_status_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.tickets
    ADD CONSTRAINT tickets_status_id_fkey FOREIGN KEY (status_id) REFERENCES public.board_statuses(id);


--
-- PostgreSQL database dump complete
--

\unrestrict 0fcVMrfSSkfUPPdYPPNBJTejOvOS67hu9WLW1LrAS3kCXbIQoIKLDnH7cuenN4i

