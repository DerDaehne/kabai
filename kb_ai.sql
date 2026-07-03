--
-- PostgreSQL database dump
--

\restrict NmpJstSgE1WyqTriZiynuXQ4sYCXCkYcgnBfcC2l4rTCSacIocx1lj7N3gFVcBK

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
-- Name: create_human_intervention_statuses(); Type: FUNCTION; Schema: public; Owner: david
--

CREATE FUNCTION public.create_human_intervention_statuses() RETURNS trigger
    LANGUAGE plpgsql
    AS $$
DECLARE
    hi_id INT;
    ha_id INT;
BEGIN
    INSERT INTO board_statuses
        (project_id, name, display_name, position, special_type, agent_role_instruction)
    VALUES (
        NEW.id,
        'human_intervention',
        'Human Intervention',
        98,
        'human_intervention',
        'Dieses Ticket wartet auf menschliche Intervention. '
        'Lies alle Kommentare, beantworte die Frage des Agenten und '
        'verschiebe das Ticket danach nach "human_answered".'
    )
    RETURNING id INTO hi_id;

    INSERT INTO board_statuses
        (project_id, name, display_name, position, special_type, agent_role_instruction)
    VALUES (
        NEW.id,
        'human_answered',
        'Human Answered',
        99,
        'human_answered',
        'Der Mensch hat geantwortet. Lies die neuesten Kommentare und '
        'fahre mit der Arbeit fort. Verschiebe das Ticket in den passenden '
        'Folgestatus.'
    )
    RETURNING id INTO ha_id;

    INSERT INTO status_transitions (project_id, from_status_id, to_status_id)
    VALUES (NEW.id, hi_id, ha_id)
    ON CONFLICT DO NOTHING;

    RETURN NEW;
END;
$$;


ALTER FUNCTION public.create_human_intervention_statuses() OWNER TO david;

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
-- Name: notify_ticket_child_change(); Type: FUNCTION; Schema: public; Owner: david
--

CREATE FUNCTION public.notify_ticket_child_change() RETURNS trigger
    LANGUAGE plpgsql
    AS $$
DECLARE
    rec RECORD;
    v_ticket_id INT;
    v_project_id INT;
    v_status_id INT;
BEGIN
    IF TG_OP = 'DELETE' THEN rec := OLD; ELSE rec := NEW; END IF;
    v_ticket_id := rec.ticket_id;

    SELECT project_id, status_id INTO v_project_id, v_status_id
      FROM tickets WHERE id = v_ticket_id;

    IF v_project_id IS NOT NULL THEN
        PERFORM pg_notify(
            'tickets_' || v_project_id::text,
            json_build_object(
                'op',         'UPDATE',
                'ticket_id',  v_ticket_id,
                'status_id',  v_status_id,
                'project_id', v_project_id
            )::text
        );
    END IF;

    IF TG_OP = 'DELETE' THEN RETURN OLD; ELSE RETURN NEW; END IF;
END;
$$;


ALTER FUNCTION public.notify_ticket_child_change() OWNER TO david;

--
-- Name: verify_kanban_rules_and_transitions(); Type: FUNCTION; Schema: public; Owner: david
--

CREATE FUNCTION public.verify_kanban_rules_and_transitions() RETURNS trigger
    LANGUAGE plpgsql
    AS $$
DECLARE
    target_status_name  VARCHAR(50);
    target_special_type VARCHAR(50);
    source_special_type VARCHAR(50);
    open_tasks_count    INT;
    is_initial_insert   BOOLEAN := (TG_OP = 'INSERT');
BEGIN
    IF NOT is_initial_insert AND OLD.status_id = NEW.status_id THEN
        RETURN NEW;
    END IF;

    IF NOT is_initial_insert THEN
        SELECT special_type INTO target_special_type
          FROM board_statuses WHERE id = NEW.status_id;
        SELECT special_type INTO source_special_type
          FROM board_statuses WHERE id = OLD.status_id;

        -- Bypass graph check when escalating to human_intervention
        -- or when human_answered resumes normal flow
        IF (target_special_type IS DISTINCT FROM 'human_intervention') AND
           (source_special_type IS DISTINCT FROM 'human_answered') THEN
            IF NOT EXISTS (
                SELECT 1 FROM status_transitions
                WHERE project_id    = NEW.project_id
                  AND from_status_id = OLD.status_id
                  AND to_status_id   = NEW.status_id
            ) THEN
                RAISE EXCEPTION
                    'Illegaler Kanban-Move (Projekt %): Ein direkter Uebergang von '
                    'Status-ID % zu Status-ID % ist laut Workflow-Definition nicht erlaubt.',
                    NEW.project_id, OLD.status_id, NEW.status_id;
            END IF;
        END IF;
    END IF;

    -- Acceptance-criteria check when closing a ticket
    SELECT name INTO target_status_name
      FROM board_statuses WHERE id = NEW.status_id;

    IF target_status_name = 'done' THEN
        SELECT COUNT(*) INTO open_tasks_count
          FROM ticket_tasks
         WHERE ticket_id = NEW.id AND is_completed = FALSE;

        IF open_tasks_count > 0 THEN
            RAISE EXCEPTION
                'Kanban-Validierungsfehler: Ticket #% kann nicht geschlossen werden, '
                'da noch % Akzeptanzkriterium/Kriterien ungeloest sind.',
                NEW.id, open_tasks_count;
        END IF;
    END IF;

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
    created_at timestamp without time zone DEFAULT now(),
    special_type character varying(50)
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
-- Name: ticket_relations; Type: TABLE; Schema: public; Owner: david
--

CREATE TABLE public.ticket_relations (
    id integer NOT NULL,
    from_ticket_id integer NOT NULL,
    to_ticket_id integer NOT NULL,
    relation_type character varying(50) NOT NULL,
    created_at timestamp without time zone DEFAULT now(),
    CONSTRAINT check_not_self_related CHECK ((from_ticket_id <> to_ticket_id)),
    CONSTRAINT ticket_relations_relation_type_check CHECK (((relation_type)::text = ANY ((ARRAY['parent_of'::character varying, 'blocks'::character varying, 'duplicate_of'::character varying, 'relates_to'::character varying])::text[])))
);


ALTER TABLE public.ticket_relations OWNER TO david;

--
-- Name: ticket_relations_id_seq; Type: SEQUENCE; Schema: public; Owner: david
--

CREATE SEQUENCE public.ticket_relations_id_seq
    AS integer
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;


ALTER SEQUENCE public.ticket_relations_id_seq OWNER TO david;

--
-- Name: ticket_relations_id_seq; Type: SEQUENCE OWNED BY; Schema: public; Owner: david
--

ALTER SEQUENCE public.ticket_relations_id_seq OWNED BY public.ticket_relations.id;


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
    model character varying(100),
    type character varying(50) DEFAULT 'ticket'::character varying NOT NULL
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
-- Name: ticket_relations id; Type: DEFAULT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_relations ALTER COLUMN id SET DEFAULT nextval('public.ticket_relations_id_seq'::regclass);


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

COPY public.board_statuses (id, project_id, name, display_name, "position", agent_role_instruction, created_at, special_type) FROM stdin;
1	1	backlog	Backlog	0	\N	2026-06-24 21:40:48.962551	\N
2	1	ready	Bereit	1	Du bist ein Planungs-Agent. Lies das Ticket sorgfältig und zerlege es in konkrete, abarbeitbare Tasks (kb.ai_add_task). Kläre offene Fragen per Kommentar, bevor du das Ticket weiterschiebst.	2026-06-24 21:40:50.98115	\N
3	1	in_progress	In Bearbeitung	2	Du bist ein Implementierungs-Agent. Weise dir das Ticket zu (kb.ai_assign_ticket), arbeite die Tasks der Reihe nach ab (kb.ai_complete_task) und dokumentiere jeden relevanten Schritt per Work-Log-Eintrag (kb.ai_add_comment). Wenn alle Tasks erledigt sind, schiebe das Ticket nach in_review.	2026-06-24 21:40:51.791957	\N
4	1	in_review	Im Review	3	Du bist ein Review-Agent. Prüfe ob alle Tasks abgeschlossen sind, die Implementierung dem Ticket-Ziel entspricht und keine offenen Fragen mehr bestehen. Bei Bestehen: Ticket nach done schieben. Bei Mängeln: konkretes Feedback per Kommentar hinterlassen und Ticket zurück nach in_progress.	2026-06-24 21:40:52.387908	\N
5	1	done	Fertig	4	\N	2026-06-24 21:40:53.502255	\N
6	2	blg	backlog	0	bla bal	2026-06-25 00:20:23.758919	\N
7	2	todo	zu Bearbeiten	0	hier sollst du tickets heraus nehmen und bearbeiten :P	2026-06-25 00:21:10.830471	\N
8	3	backlog	Backlog	0	\N	2026-06-25 08:51:24.233009	\N
10	3	in_progress	In Arbeit	2	\N	2026-06-25 08:51:31.231842	\N
11	3	review	Review	3	\N	2026-06-25 08:51:36.334855	\N
12	3	done	Done	4	\N	2026-06-25 08:51:40.071774	\N
13	4	backlog	Backlog	0	**Rolle: Story & Content Writer**\n\nScope: Narrative, game mechanics, world-building, high-level concepts. Kein Code, keine Architektur-Entscheidungen.\n\nWorkflow:\n- Iterative Deepening: "What if?"-Szenarien proaktiv vorschlagen, um Tiefe eines Mechanics zu erkunden.\n- Backlog Entry: Neue User Stories in project/backlog/ anlegen, um Konzepte zu formalisieren.\n\nOutput: Dokumentation in docs/concept/, Stories in project/backlog/.\n\nDefinition of Done — Story Writer:\n- [ ] Story-Datei mit Description + Success Criteria + Agent Log.\n- [ ] Cross-References zu bestehenden Concept-Docs (oder neues Concept-Doc erstellt).\n- [ ] Kein Code und keine Architektur-Änderungen (out of scope).	2026-06-25 09:22:09.490285	\N
14	4	planning	Planning	1	**Rolle: Requirement Manager**\n\nScope: Refinement von Backlog-Items zu technischen Anforderungen.\n\nWorkflow:\n- Items aus project/backlog/ aufnehmen.\n- Technische Tiefe, Edge Cases und Sub-Systeme ausarbeiten.\n- Tickets nach project/planning/ verschieben.\n\nOutput: Detaillierte Stories in project/planning/.\n\nDefinition of Done — Requirement Manager:\n- [ ] Offene Fragen aufgelistet und entweder beantwortet oder für Researcher markiert.\n- [ ] Sub-Stories vorgeschlagen, wenn Story zu groß für ein Ticket ist.\n- [ ] Story via git mv backlog/ → planning/ verschoben.	2026-06-25 09:22:13.11806	\N
15	4	todo	Todo	2	**Rolle: Architect**\n\nScope: Technisches Design, Systemstruktur, Implementierungsstrategien.\n\nWorkflow:\n- Items aus project/planning/ aufnehmen.\n- Implementierung designen (ADRs, Specs).\n- Decomposition: Planning-Tickets in granulare Implementierungsaufgaben zerlegen → project/todo/.\n\nOutput: ADRs in adr/, technische Specs in architecture/, verfeinerte Stories in project/todo/.\n\nDefinition of Done — Architect:\n- [ ] Jede nicht-offensichtliche Entscheidung hat ein ADR in adr/.\n- [ ] Sub-Tickets in todo/ sind Developer-pickup-ready (konkrete Schritte, Dateipfade, Verification-Pfad).\n- [ ] Jedes Sub-Ticket enthält Effort-Estimate (XS/S/M/L/XL).\n- [ ] Story via git mv planning/ → todo/ verschoben (Parent bleibt in planning bis alle Sub-Tickets done sind).\n\nEffort Estimate Scale: XS < 30min | S 30min–2h | M 2–5h | L 5–10h | XL > 10h (muss vom Architect zerlegt werden)	2026-06-25 09:22:20.301044	\N
16	4	in_progress	In Progress	3	**Rolle: Developer**\n\nScope: Implementierung und Validierung von Features. Besitzt NICHT die Test-Suite (das ist Tester-Domäne).\n\nWorkflow:\n- Items aus project/todo/ aufnehmen.\n- Ticket nach project/in-progress/ verschieben während der Arbeit.\n- Ticket nach project/done/ verschieben nach `odin check` und Tester-Sign-off.\n\nHard Negatives (NIEMALS):\n- odin check / odin test als grün melden ohne das echte Kommando-Output im Verification Block zu zitieren.\n- Ticket zu done/ verschieben wenn ein Verification-Schritt übersprungen wurde.\n- Unrelated Code innerhalb eines Bugfix-Tickets refactorn.\n- Amend eines publizierten Commits, Force-Push auf master, --no-verify / --no-gpg-sign.\n- Zwei Tickets in einem Commit mischen.\n\nVerification Block (PFLICHT am Ende jedes Agent-Log-Eintrags):\n  odin check src/server: GREEN (oder: SKIPPED — <Grund>)\n  odin check src/client: GREEN (oder: SKIPPED — <Grund>)\n  odin test  src/server: <N> passed (oder: SKIPPED — <Grund>)\n\nDefinition of Done — Developer:\n- [ ] Verification Block zeigt GREEN für odin check src/server und odin check src/client.\n- [ ] Wenn neue Logik worth testing: korrespondierendes Tester-Ticket existiert.\n- [ ] Commit-Message referenziert Ticket-Dateipfad; enthält Co-Authored-By: Trailer.\n- [ ] Ticket via git mv in-progress/ → done/ verschoben.\n- [ ] Branch auf Remote gepusht.	2026-06-25 09:22:30.323476	\N
17	4	done	Done	4	**Abgeschlossen.** Tickets in diesem Status sind fertiggestellt und verifiziert. Kein Agent greift aktiv auf Done-Tickets zu — sie dienen als historisches Archiv und Referenz.\n\nTester-Aktivierung: Ein Tester-Ticket kann hier landen nachdem der Developer done/ signalisiert hat. Der Tester fügt dann Testergebnisse zum Agent Log hinzu und filed Bug-Tickets in backlog/ falls nötig.\n\nDefinition of Done (alle Rollen):\n- Verification Block vorhanden und GREEN (oder explizit N/A).\n- Agent Log vollständig — jeder Schritt dokumentiert, was getan wurde und was gefunden wurde.\n- Kein offenes TODO im Code (// TODO: continue later ist ein Hard Negative).	2026-06-25 09:22:35.497039	\N
18	5	todo_list	To Do	0	\N	2026-06-29 09:58:22.451251	\N
19	5	in_progress_list	In Progress	1	\N	2026-06-29 09:58:22.483744	\N
20	5	done_list	Done	2	\N	2026-06-29 09:58:22.484197	\N
21	1	human_intervention	Human Intervention	98	Dieses Ticket wartet auf menschliche Intervention. Lies alle Kommentare, beantworte die Frage des Agenten und verschiebe das Ticket danach nach "human_answered".	2026-07-01 15:25:52.310271	human_intervention
22	1	human_answered	Human Answered	99	Der Mensch hat geantwortet. Lies die neuesten Kommentare und fahre mit der Arbeit fort. Verschiebe das Ticket in den passenden Folgestatus.	2026-07-01 15:25:52.310271	human_answered
23	2	human_intervention	Human Intervention	98	Dieses Ticket wartet auf menschliche Intervention. Lies alle Kommentare, beantworte die Frage des Agenten und verschiebe das Ticket danach nach "human_answered".	2026-07-01 15:25:52.310271	human_intervention
24	2	human_answered	Human Answered	99	Der Mensch hat geantwortet. Lies die neuesten Kommentare und fahre mit der Arbeit fort. Verschiebe das Ticket in den passenden Folgestatus.	2026-07-01 15:25:52.310271	human_answered
25	3	human_intervention	Human Intervention	98	Dieses Ticket wartet auf menschliche Intervention. Lies alle Kommentare, beantworte die Frage des Agenten und verschiebe das Ticket danach nach "human_answered".	2026-07-01 15:25:52.310271	human_intervention
26	3	human_answered	Human Answered	99	Der Mensch hat geantwortet. Lies die neuesten Kommentare und fahre mit der Arbeit fort. Verschiebe das Ticket in den passenden Folgestatus.	2026-07-01 15:25:52.310271	human_answered
9	3	todo	Todo	1	tickets die hier liegen duerfen erst in den naechsten Status verschoben werden, wenn sie refined wurden.	2026-06-25 08:51:27.775862	\N
27	4	human_intervention	Human Intervention	98	Dieses Ticket wartet auf menschliche Intervention. Lies alle Kommentare, beantworte die Frage des Agenten und verschiebe das Ticket danach nach "human_answered".	2026-07-01 15:25:52.310271	human_intervention
28	4	human_answered	Human Answered	99	Der Mensch hat geantwortet. Lies die neuesten Kommentare und fahre mit der Arbeit fort. Verschiebe das Ticket in den passenden Folgestatus.	2026-07-01 15:25:52.310271	human_answered
29	5	human_intervention	Human Intervention	98	Dieses Ticket wartet auf menschliche Intervention. Lies alle Kommentare, beantworte die Frage des Agenten und verschiebe das Ticket danach nach "human_answered".	2026-07-01 15:25:52.310271	human_intervention
30	5	human_answered	Human Answered	99	Der Mensch hat geantwortet. Lies die neuesten Kommentare und fahre mit der Arbeit fort. Verschiebe das Ticket in den passenden Folgestatus.	2026-07-01 15:25:52.310271	human_answered
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
1	21	22
2	23	24
3	25	26
4	27	28
5	29	30
2	6	23
2	7	23
2	24	6
2	24	7
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
24	209	claude-sonnet-4-6	Fix in render.odin:draw_players — DrawRectangleLines(tx*TILE_SIZE,...) durch tile_to_screen(ftx,fty) + draw_iso_diamond ersetzt. odin check ./src/client GREEN. Commit de04a2e.	2026-06-30 18:08:07.782205
25	210	claude-sonnet-4-6	7 neue Tests: 6 in interaction_test.odin (resolve+apply Container, resolve+apply GC-Attack inkl. Cooldown-Guard/Kill/Set), 1 in persistence_test.odin (mem_slots 16-Slot Roundtrip). Gesamtcount: 83→90. Alle grün. Commit de04a2e.	2026-06-30 18:08:09.420124
26	211	claude-sonnet-4-6	Architect-Phase gestartet. Lokale Backlog-Datei project/backlog/123-iso-controls-redesign.md angelegt (commit 16753cd). Empfehlung: Option 3 — 8-Richtungen (W+A=screen-up). ADR + Sub-Tickets folgen.	2026-06-30 18:11:42.33006
27	212	claude-sonnet-4-6	Lokale Backlog-Datei project/backlog/124-resource-spawn-overhaul.md angelegt (commit 16753cd). Warte auf User-Entscheidung zu: (1) Plattform-Topologie Island vs. Free-Form, (2) Ressourcen-Dichte-Modell. Dann ADR + Sub-Tickets.	2026-06-30 18:11:45.261388
28	211	claude-sonnet-4-6	ADR entschieden: Option 3 — 8-Richtungen via InputVector{dx, dy: i8}. Sub-Tickets angelegt: #213 (123b Shared), #214 (123c Server), #215 (123d Client), #216 (123e Tests). Backlog-Datei mit Bewegungsmatrix aktualisiert.	2026-06-30 18:14:51.560078
29	212	claude-sonnet-4-6	ADR entschieden: Biom-Zonen auf offener Welt (HEAP/STACK/CACHE/REGISTERS). Sub-Tickets angelegt: #217 (124a Shared), #218 (124b world_gen), #219 (124c Respawn), #220 (124e Tests). 124d Client-Tint über 106e koordiniert.	2026-06-30 18:14:53.432181
30	214	claude-sonnet-4-6	_apply_move auf 2-Byte InputVector [dx,dy:i8] umgestellt. Backward-Compat für 1-Byte Legacy-Format (bis 123d). Facing-Regel: dy≠0→N/S, sonst dx→E/W. 90/90 Tests grün. Commit 1ca57fc.	2026-06-30 18:20:26.553454
31	215	claude-sonnet-4-6	WASD+HJKL Multi-Key implementiert. W+A→(-1,-1) screen-up, W+D→(+1,-1) screen-right. Diagonalen bewegen direkt, Einzeltasten behalten turn-before-move (058). [B] = Build-Wall. odin check GREEN. Commit 91c5fcc.	2026-06-30 18:22:52.660586
32	216	claude-sonnet-4-6	9 Tests in movement_test.odin: W+A→(-1,-1), W+D→(+1,-1), S+A→(-1,+1), S+D→(+1,+1), West-Facing, Invalid|dx|>1 ignoriert, Zero-Vector ignoriert, Diagonal-Block, Legacy-1-Byte. 90→99 Tests, alle grün. Commit 6230763.	2026-06-30 18:25:18.79771
33	211	claude-sonnet-4-6	Vollständig implementiert. ADR: 8-Richtungen via InputVector{dx,dy:i8}. Sub-Tickets 213–216 alle done. W+A=screen-up, Diagonalen ohne Turn-Phase, [B]=Build-Wall. 99 Server-Tests grün.	2026-06-30 18:25:57.703721
34	217	claude-sonnet-4-6	Implementiert: Biome::enum u8{None,Heap,Stack,Cache,Registers} in world.odin, Tile.biome:Biome Feld. Persistence: SAVE_VERSION 3→4, save_chunk schreibt biome-Byte, load_chunk liest es mit Backward-Compat (ver<4→biome=.None). 99/99 Tests grün, client check GREEN. Commit 5396a7c.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin test  src/server: 99 passed	2026-06-30 18:35:28.302878
35	218	claude-sonnet-4-6	_biome_for_chunk: 3-Chunk-Zonen (~96×96 Tiles), hash-basiert deterministisch. _tile_type_for_biome: 50% Biom-Forced (HEAP→RAM, STACK→CPU, CACHE→Byte, REGISTERS→dist-Tabelle). generate_chunk: Biom-Stempel-Pass vor Vein-Placement, pickup-Veins nutzen _tile_type_for_biome. 99/99 Tests grün. Commit 9b4ce0f.\n\nVerification:\n  odin check src/server: GREEN\n  odin test  src/server: 99 passed\n  odin check src/client: SKIPPED — server-only change	2026-06-30 18:37:58.939382
36	219	claude-sonnet-4-6	RESOURCE_RESPAWN_TICKS=6000 (5min@20Hz). harvested_tiles: map[[2]i32]u64 in ServerState. apply_interaction:.Collect_* trägt in Map ein. tick_resource_respawn: Threshold-Pass (biome-aware via _tile_type_for_biome) + Background-Pass (biome-aware wenn tile.biome!=None, Legacy-Tabelle sonst). 99/99 Tests grün. Commit 0aee4d8.\n\nVerification:\n  odin check src/server: GREEN\n  odin test  src/server: 99 passed\n  odin check src/client: SKIPPED — server-only change	2026-06-30 18:41:39.666181
37	220	claude-sonnet-4-6	10 Tests in biome_test.odin: Biom-Determinismus, alle 4 Biome in 10×10-Grid, Chunk-Stamp (jedes Tile trägt Chunk-Biom), HEAP/STACK/CACHE-Skew, Skew-Fallthrough, Respawn-Trigger (nach Threshold), Respawn-noch-nicht (vor Threshold), staler Eintrag bereinigt. 99→109 Tests grün. Commit 477ea19.\n\nVerification:\n  odin test src/server: 109 passed\n  odin check src/client: SKIPPED — server-only change	2026-06-30 18:42:44.29898
38	212	claude-sonnet-4-6	Vollständig implementiert. ADR: Biom-Zonen (HEAP/STACK/CACHE/REGISTERS) auf offener Welt. Sub-Tickets #217–#220 alle done. 109 Server-Tests grün.	2026-06-30 18:43:10.797728
39	221	claude-sonnet-4-6	**Requirement Manager** — Analyse abgeschlossen, Ticket nach planning verschoben.\n\nRoot cause identifiziert: Ticket 123 hat 8-directional WASD eingeführt, aber single keys bleiben auf den alten iso-diagonalen (W=(0,-1), A=(-1,0) etc.). Kombinationen wie S+A erzeugen 64px Screen-Horizontalbewegung statt 32px — geometrisch korrekt für 2:1-Iso, aber für den Spieler inkonsistent und intuitiv falsch.\n\nFix-Ansatz: Single keys auf die 4 screen-aligned iso-Diagonalen remappen. Keine Kombinationen mehr nötig. Geschwindigkeits-Diskrepanz durch Kombinationen entfällt damit automatisch.\n\nVerification: N/A — requirement-only.	2026-06-30 18:52:44.119282
40	221	claude-sonnet-4-6	**Architect** — Design festgelegt.\n\n**Fix-Strategie:** Nur `src/client/input.odin` ändern (Zeilen 290–301). Server-Protokoll und Server-Logik bleiben unverändert.\n\n**Neues Key→InputVector-Mapping:**\n- W → dx=-1, dy=-1 → screen (0,-32) = screen-up ✓\n- S → dx=+1, dy=+1 → screen (0,+32) = screen-down ✓\n- A → dx=-1, dy=+1 → screen (-64,0) = screen-left\n- D → dx=+1, dy=-1 → screen (+64,0) = screen-right\n\n**Kombinationen (nach Clamp auf [-1,1]):**\n- W+A → (-1,0) → screen (-32,-16) = obere-linke Ecke, 35.8px\n- W+D → (0,-1) → screen (+32,-16) = obere-rechte Ecke, 35.8px\n- S+A → (0,+1) → screen (-32,+16) = untere-linke Ecke, 35.8px\n- S+D → (+1,0) → screen (+32,+16) = untere-rechte Ecke, 35.8px\n\n**Ergebnis:** Kombinationen sind nicht mehr schneller als Einzeltasten (35.8px < 64px). Bug behoben.\n\n**is_diagonal-Mechanik (058 turn-before-move):**\nAlle 4 Einzeltasten produzieren jetzt `dx≠0 && dy≠0` → `is_diagonal=true` → immer direktes Bewegen ohne Turn-Phase. Kombinationen produzieren `dx=0||dy=0` → is_diagonal=false → Turn-Phase aktiv. Seiteneffekt positiv: Einzeltasten fühlen sich responsiver an.\n\n**movement_test.odin:** Keine Änderungen nötig — testet Server-Verhalten (InputVector-Verarbeitung), nicht Client-Keymapping.\n\nVerification: N/A — design-only.	2026-06-30 18:53:48.233867
41	221	claude-sonnet-4-6	**Developer** — Implementierung abgeschlossen. Commit: 42af508 (fix/221-iso-wasd-remap).\n\nGeänderte Datei: src/client/input.odin (Zeilen 285–308).\nAltes Mapping: W→(0,-1), A→(-1,0), S→(0,+1), D→(+1,0) + separate Kombinationslogik.\nNeues Mapping: W→(-1,-1), S→(+1,+1), A→(-1,+1), D→(+1,-1) mit sum+clamp(-1,1) für Kombinationen.\nErgebnis: Keine Geschwindigkeits-Inkonsistenz mehr; W geht direkt screen-up.\n\nVerification:\n  odin check src/client: GREEN (EXIT:0)\n  odin check src/server: GREEN (SRVEX:0)\n  odin test  src/server: 109 passed (TESTEX:0)	2026-06-30 18:55:16.851307
42	222	claude-sonnet-4-6	**Requirement Manager** — Subsystem-Inventar erstellt, Lücken identifiziert, nach planning verschoben.\n\n9 Subsysteme dokumentiert, 8 Lücken gefunden. Größte Lücke: render.odin (~1200 Zeilen, keine architecture/-Datei). Zweite Priorität: interaction-system (zentrale Spielmechanik mit Prioritäts-Stack). Übersichts-README fehlt völlig.\n\nVerification: N/A — requirement-only.	2026-06-30 18:56:46.357819
43	222	claude-sonnet-4-6	**Architect** — Dekomposition abgeschlossen. Sub-Tickets in todo:\n\n- #223 (XS): architecture/README.md — Subsystem-Index\n- #224 (S): architecture/interaction-system.md\n- #225 (M): architecture/render-pipeline.md\n- #226 (S): architecture/item-system.md\n- #227 (S): architecture/biome-system.md + gc-ai.md\n\nPriorisierung: 223 → 224 → 226 → 227 → 225 (Render-Pipeline letzte, da größter Aufwand und System noch in Entwicklung).\n\nVerification: N/A — doc-only.	2026-06-30 18:59:11.516234
44	222	claude-sonnet-4-6	**Architect/Doc Writer** — Alle 6 Dateien geschrieben und committed (4e07720, branch docs/222-subsystem-architecture).\n\nErstellt:\n- architecture/README.md — Subsystem-Index, alle 17 Subsysteme tabellarisch\n- architecture/interaction-system.md — Prioritäts-Stack, alle InteractionKind-Werte, apply-Effekte\n- architecture/item-system.md — ItemType-Registry, Inventory-Ops, Crafting-Rezepte, Fragment-Hybrid\n- architecture/biome-system.md — Chunk-Seeding, tile_type_for_biome, Respawn-Passes\n- architecture/gc-ai.md — Tier-Tabelle L1/L2, Greedy-Step+Slide, Spawn-Logik\n- architecture/render-pipeline.md — Koordinatensystem, Frame-Loop, CRT-Shader-Pipeline\n\nVerification:\n  odin check src/server: GREEN (SRVEX:0)\n  odin check src/client: GREEN (CLIEX:0)\n  odin test  src/server: N/A — doc-only	2026-06-30 19:04:34.547469
45	183	claude-sonnet-4-6	## Verification Block\n\n**Branch:** fix/183-opcode-audit → merged to master (65f7d02)\n\n**Changed:** `src/shared/protocol.odin`\n- Added full opcode table comment (0x01–0x24, all directions, payload summaries)\n- `MSG_HEAP_DEPOSIT`: 0x10 → 0x23\n- `MSG_HEAP_WITHDRAW`: 0x11 → 0x24\n- Removed duplicate MSG_FRAGMENT_TABLE comment block\n\n**Checks:**\n- `odin check src/server` → exit 0\n- `odin check src/client` → exit 0\n\n**Runtime impact:** None. MSG_HEAP_DEPOSIT/WITHDRAW are not yet sent by the client (heap terminal overlay not wired). Server handler in `simulation.odin:190-192` updated implicitly via the shared constants.	2026-06-30 19:25:51.613112
46	229	claude-sonnet-4-6	## Root cause\n\n`draw_entities` in `src/client/render.odin` called `draw_terminals`, `draw_workers`, `draw_artifacts`, `draw_gcs`, `draw_players`, and `draw_ghost_echoes` — but had no `draw_containers` call. The proc didn't exist.\n\nServer-side: containers are created correctly in `_apply_craft_container` (simulation.odin:226), stored in `state.containers`, encoded in `encode_world_snapshot`, and decoded into `cs.containers` on the client. The resource deduction and placement logic were sound.\n\n## Fix\n\nAdded `draw_containers` proc to `src/client/render.odin` (amber "C" glyph, integrity-reactive colour, halo circle) and wired it into `draw_entities` before `draw_terminals`.\n\n**Branch:** fix/229-container-render → merged master (5d38bc7)\n\n**Checks:**\n- `odin check src/client` → exit 0	2026-06-30 19:30:16.425602
98	262	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält bereits vollständige Belege. Keine offenen Fragen. Moving to todo.	2026-07-01 14:31:43.172604
47	228	claude-sonnet-4-6	## Verification Block\n\n**Branch:** feat/228-inventory-subsystem → merged master (88f0503)\n\n**Changes:**\n\n1. `src/client/inventory.odin`\n   - `[✓]` → `[*]`: the checkmark U+2713 was outside the ASCII 32-126 range that Raylib loads by default; rendered as "?" at runtime\n   - `RECIPE_KEYS`: `[4]string` fixed array → `[]string` slice + bounds guard `if i < len(RECIPE_KEYS) else "?"` — prevents index panic when new crafting recipes are added to `CRAFTING_RECIPES` without updating the key list\n\n2. `architecture/inventory-system.md` (new): full subsystem spec covering data model, shared ops, all three overlays (player/container/heap), font constraint note, protocol integration\n\n3. `architecture/README.md`: linked new inventory-system.md in the Player & Interaction table\n\n**Checks:**\n- `odin check src/client` → exit 0	2026-06-30 19:34:14.508569
48	230	claude-sonnet-4-6	## Verification Block\n\n**Branch:** feat/230-ui-subsystem → merged master (4380105)\n\n**New file: `src/client/ui.odin`**\n- `UILayer` enum: World < HUD < Overlay < Modal < Popup — documents intended draw order\n- Palette constants: `UI_COL`, `UI_DIM`, `UI_HEADER`, `UI_CYAN`, `UI_WARN`, `UI_ERR`\n- Primitives: `ui_dim_screen()`, `ui_panel()`, `ui_divider()`, `ui_list_row()`\n\n**Refactored: `src/client/inventory.odin`** (reference implementation)\n- All local `COL/DIM/HEADER/CYAN` replaced with `UI_*`\n- `rl.DrawRectangle(0,0,sw,sh,...)` → `ui_dim_screen()`\n- `"─────..."` box-drawing chars (U+2500, renders as `?`) → `ui_divider()`\n- `"↑↓"` arrow chars (non-ASCII) → `"up/dn"` in footer hints\n\n**New file: `architecture/ui-system.md`** — layer model, palette table, primitive docs, ASCII font constraint table, overlay migration status\n\n**TODO (follow-up):** Migrate `draw_terminal_overlay`, `draw_fragment_overlay`, `draw_help_overlay`, `draw_escape_menu` to use `UI_*` palette and `ui_dim_screen()`.\n\n**Checks:**\n- `odin check src/client` → exit 0	2026-06-30 19:38:17.77632
49	231	claude-sonnet-5	**[Story Writer]** Cross-checked against kbai #229 (done, commit 5d38bc7, merged to master). #229 fixed the exact symptom described here: server-side container creation/encoding was correct, but src/client/render.odin had no draw_containers call in draw_entities. That proc now exists and is wired in.\n\nThis ticket was very likely filed against a client build predating that fix, or is a duplicate report. Moving to planning for Requirement Manager to confirm via live verification rather than closing outright — code inspection alone is not sufficient grounds to close; a live-client check should confirm the fix is visible before this is marked resolved.	2026-06-30 22:21:33.387996
50	231	claude-sonnet-5	**[Requirement Manager]** Re-read src/client/render.odin directly: draw_entities (called every frame from the World layer) invokes draw_containers(cs) before draw_terminals/draw_workers/etc. draw_containers iterates cs.containers and draws a colored rect + integrity-tinted circle + "C" glyph per entry — this is exactly the #229 fix, present on current master.\n\nNo open technical question remains at the code level. Remaining risk is purely "did the reporter's client build include this commit" — that's a live-session check, not a requirements question, so I'm not decomposing this further.\n\nOpen question for Tester: spawn a container in a live client session post-#229 and confirm it renders. If it still doesn't render, the bug is environment-specific (stale build, render-disabled flag, etc.) and needs new repro steps — re-open as a new bug ticket rather than reusing this one (per Hard Negative #3 — don't fold unrelated investigation into a closed bug).\n\nMoving to todo. Effort: XS (verification only, no code expected).	2026-06-30 22:22:03.449617
51	232	claude-sonnet-5	**[Story Writer]** Confirmed root cause by reading src/client/input.odin:179-227 and src/client/inventory.odin:156-209. Real bug, not a duplicate of any closed ticket. Noted that this overlay is also on the architecture/ui-system.md migration TODO list (#230 follow-up) — Architect should decide whether to bundle the UI-palette migration of draw_container_overlay into the same ticket as the focus-fix, since both touch the same proc and bundling avoids touching it twice. Moving to planning.	2026-06-30 22:23:03.1899
52	232	claude-sonnet-5	**[Requirement Manager]** Technical depth:\n\n- Add `container_focus: enum{Player, Container}` to ClientState (or reuse an existing bool — check player_state.odin/network.odin for ClientState definition before adding a field; avoid duplicating state).\n- `_handle_container_keys`: gate the Up/Down block on `container_focus`; route to `cs.inventory_selected` vs `cs.container_inv_selected` accordingly. Add `[Tab]` to flip `container_focus`. Reset `cs.inventory_selected` to 0 when the container overlay opens (currently unset — likely source of "stale index" half of the bug).\n- `draw_container_overlay`: pass focus state into `ui_panel`/border draw so the focused side is visually distinct (UI_CYAN border on focused pane, UI_DIM on the other) — natural fit for #230's `ui_panel`.\n- Edge case: container or player inventory has 0 items while the other has items — Tab should still toggle, Up/Down on the empty pane should no-op (guard already exists via `slot_count > 0` checks, mirror for player-inventory pane).\n- No sub-tickets needed — single cohesive change, one file pair (`input.odin` + `inventory.odin`), estimated effort S (30min-2h).\n\nNo open questions remain. Moving to todo.	2026-06-30 22:23:27.586218
53	233	claude-sonnet-5	**[Story Writer]** Root cause located precisely via code read: src/server/simulation.odin _apply_move (1099-1138) only checks the destination tile for diagonal moves, never the two axis-aligned intermediate tiles. Standard tile-grid corner-cutting bug. Confirmed not covered by existing test_move_diagonal_blocked_by_solid (src/server/movement_test.odin:96-106), which only tests a solid destination, not solid intermediates with an open destination. Moving to planning.	2026-06-30 22:24:52.081381
54	233	claude-sonnet-5	**[Requirement Manager]** Technical depth:\n\n- Fix is a 4-line guard inside _apply_move, gated on `dx != 0 && dy != 0`, checking shared.get_tile(&state.world, nx, p.y) and shared.get_tile(&state.world, p.x, ny) for solidity before committing the diagonal move.\n- Single edge case to verify: orthogonal (non-diagonal) movement must be completely unaffected — guard only applies when both dx and dy are non-zero.\n- Interacts with #235 (diagonal speed) and #123 (iso movement input) conceptually but not at the code level — no shared state, no ordering dependency. Can be done independently.\n- No architecture doc currently owns "movement collision rules" as a concept — recommend Architect add a short paragraph to architecture/player-state.md (closest existing home, owns src/server/player_state.odin) rather than a new file, since this is a single invariant not a subsystem.\n- Effort: XS (<30min) — single guarded branch + one test case.\n\nNo open questions. Moving to todo.	2026-06-30 22:25:14.914403
55	234	claude-sonnet-5	**[Story Writer]** Verified this is distinct from kbai #209 (done) — #209 fixed coordinate-space drift, this is about the Direction enum only covering 4 of 8 possible movement vectors. Confirmed via src/shared/entities.odin:3, src/client/input.odin:323-329, src/server/simulation.odin:1122-1128 — all three need updating together since facing must stay client/server-consistent. Moving to planning.	2026-06-30 22:31:24.620791
56	231	claude-sonnet-5	**[Process correction]** The two prior comments on this ticket were mislabeled — per AGENTS.md, backlog→planning is Requirement Manager's job (not Story Writer, which is for drafting net-new concepts), and planning→todo is Architect's job. Read as: Requirement Manager pass (root-cause cross-check, backlog→planning) then Architect pass (todo placement, no decomposition needed — single verification step). Content of both passes stands as written.	2026-06-30 22:34:39.255722
57	232	claude-sonnet-5	**[Process correction]** Same role-label correction as #231: first pass = Requirement Manager (backlog→planning), second pass = Architect (planning→todo). Content stands.	2026-06-30 22:34:42.79868
58	233	claude-sonnet-5	**[Process correction]** Same role-label correction as #231/#232: first pass = Requirement Manager (backlog→planning), second pass = Architect (planning→todo). Content stands.	2026-06-30 22:34:45.351982
59	234	claude-sonnet-5	**[Process correction]** Previous comment mislabeled "Story Writer" — backlog→planning is Requirement Manager's job. Correcting: that pass was Requirement Manager. This pass is Architect (planning→todo).\n\n**[Architect]** Investigated scope further (sub-agent code search across protocol.odin, render.odin, simulation.odin). Findings change the plan:\n\n1. Direction is plain u8 over the wire (protocol.odin:150-154, 350, 387, 543-544) — no bit-packing constraint, safe to extend to 8 values.\n2. direction_delta (entities.odin:12-20) is the single chokepoint used by ALL interaction targeting (container/terminal/wall-building/worker-pickup facing tiles in simulation.odin:233-238, 267-272, 817, 958-963) plus the client facing-indicator (render.odin:238-243). Extending it to 8 cases covers all call sites in one change — good, no fan-out risk.\n3. BLOCKER for full scope: player sprite sheet (render.odin:298-331, draw_player_sprite) has only 4 directional frames (×2 for idle/walk = 8 total frames). No diagonal-facing art exists. Confirms reporter's ask is bigger than "fix a bug" — it's a content gap too.\n4. MSG_FRAGMENT_HINT already encodes 8 compass directions (protocol.odin:112-114) — precedent exists, this is not a novel pattern for the codebase.\n\n**Decomposition:**\n- **234a** (this ticket, scope-narrowed): Direction enum → 8 values; direction_delta + all interaction-targeting call sites + facing-indicator updated for diagonals; client/server facing-calc (input.odin:323-329, simulation.odin:1122-1128) updated to preserve dx alongside dy instead of discarding it. Sprite selection falls back to nearest cardinal frame for diagonal facings (e.g. NE→North or NE→East, Developer's call, document the choice) — no new art required. Effort: M.\n- **234b** (new, backlog): 8-directional player sprite art + frame wiring once 234a's fallback is in place. This is an art/content ticket, not urgent — current 4-frame fallback is functionally correct, just not visually distinct per diagonal. Story Writer/Architect can scope this later.\n\nADR needed: this changes a shared enum consumed by protocol, server sim, and client render simultaneously — non-obvious to a future agent why direction_delta has 8 cases when sprites only have 4. Developer should add adr/ entry recording the "fallback to nearest cardinal for sprite, full precision for targeting" decision.\n\nMoving 234 (rescoped to the 234a description above) to todo. Effort: M.	2026-06-30 22:35:38.338877
60	236	claude-sonnet-5	**[Architect]** Filed as a decomposition follow-up from #234 per that ticket's success-criterion 7. Blocked on #234 landing first (needs the 8-value Direction enum to exist). Left in backlog — not urgent, current #234 fallback is functionally complete.	2026-06-30 22:37:10.818644
61	235	claude-sonnet-5	**[Requirement Manager]** Confirmed real bug (not just perspective) via src/server/simulation.odin:1119-1120 and movement_test.odin:13-34 — diagonal tile-steps cover √2× the distance of cardinal steps every tick, with existing tests pinning this as current expected behavior. Moving to planning — implementation strategy needs an Architect decision (two non-trivial, non-equivalent approaches, see description) before this can be sized for todo.	2026-06-30 22:38:30.665674
62	195	claude-sonnet-5	**[Hygiene check]** This ticket duplicates #190 (same title "006b: GC Wall Collision", same fix, both done). Likely a relic of the pre-kbai file-based workflow (project/todo/ vs project/done/ paths in the two descriptions). No delete_ticket tool is currently exposed via MCP, so left both standing with this cross-reference rather than silently picking one. Human: safe to delete either #190 or #195 directly if your kbai client supports it.	2026-06-30 22:39:29.416769
63	190	claude-sonnet-5	**[Hygiene check]** Duplicate of #195 (same title, same fix). See #195 comment for detail.	2026-06-30 22:39:33.749996
64	232	claude-sonnet-5	**[Developer]** Implemented on fix/232-container-cursor-focus (commit ab7c303):\n- Added `cs.container_focus: ContainerFocus` (Player/Container) to ClientState.\n- `[Tab]` toggles focus; `_handle_container_keys` gates Up/Down on the focused pane.\n- `draw_container_overlay` highlights the focused pane's header/rows in UI_CYAN.\n- network.odin: cursor/focus reset only on first open (`!cs.container_open` check), not on post-deposit/withdraw refresh — preserves cursor position across actions.\n- architecture/inventory-system.md updated.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — no server-side change, client-only input/render fix\n\nMoving to done.	2026-06-30 22:50:41.423782
65	233	claude-sonnet-5	**[Developer]** Implemented on fix/233-diagonal-corner-cut (commits 4721fe3, 251f0ff):\n- `_apply_move`: diagonal step now also checks both axis-aligned intermediate tiles, not just the destination.\n- New test `test_move_diagonal_corner_cut_blocked` (movement_test.odin) covers the corner-cut case directly.\n- architecture/player-state.md: new "Movement Collision" section documenting both invariants, per Requirement Manager's suggestion.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: SKIPPED — no client change\n  odin test  src/server: 110 passed (full suite, all green; pre-existing unrelated memory-leak warnings on 3 tests not touched by this change)\n\nMoving to done.	2026-06-30 22:52:07.909214
66	234	claude-sonnet-5	**[Developer]** Implemented on feat/234-eight-directional-facing (commit cf306c1):\n- `Direction` extended to 8 values (diagonals appended for wire-compat).\n- `direction_delta` extended to all 8; converted simulation.odin (container/terminal craft, worker pickup), render.odin (facing-indicator, HUD unload-hint), input.odin (editor-open, unload) from hand-rolled cardinal switches to call the chokepoint — all targeting now diagonal-capable.\n- `_apply_move` facing-calc now produces real diagonal facing for diagonal input (was dy-dominant collapse). Updated movement_test.odin assertions accordingly (W+A etc. now assert .NorthWest/.NorthEast/.SouthWest/.SouthEast).\n- Worker Lua-API turning (scripting.odin: lua_turn_left/right/move_forward/mine) converted to `#partial switch` — explicit scope exclusion, Workers stay cardinal-only, verified via compiler (exhaustive-switch errors caught every call site that needed a decision).\n- draw_player_sprite: diagonal facings fall back to nearest cardinal frame (NE/NW→North, SE/SW→South), documented in adr/234-eight-directional-facing.md.\n- New test: entities_test.odin::test_direction_delta_all_eight (src/shared package).\n- architecture/player-state.md: new "Facing & Targeting" section. architecture/README.md updated.\n- Filed kbai #236 (234b, diagonal sprite art) per success-criterion 7 — already done earlier this session.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin test  src/server: 110 passed\n  odin test  src/shared: 31 passed (includes new test_direction_delta_all_eight)\n\nMoving to done.	2026-06-30 22:57:43.11408
67	235	claude-sonnet-5	**[Architect]** Revising the plan in this ticket's description — both previously listed options (server-side movement-budget-accumulator, continuous movement) assumed the fix had to live server-side. Checked: there is no server-side rate limiting on MSG_INPUT_MOVE at all (grepped src/server/network.odin). Movement speed is purely client-paced — src/client/input.odin:18 `MOVE_REPEAT_INTERVAL :: f32(0.10)` (100ms/10 tiles per sec) gates how often the client sends a move message via `cs.move_timer`. The server just executes whatever message it receives, 1 tile, no independent throttle.\n\n**Decision: Option 3 (not previously listed) — scale the client's send interval by √2 for diagonal moves.** `is_diagonal` is already computed at the send site (input.odin:329). Setting `cs.move_timer = MOVE_REPEAT_INTERVAL * (1.41421356 if is_diagonal else 1)` on a successful send makes diagonal tiles arrive proportionally less often, equalizing average Euclidean speed — without touching the server, collision (#233), GC pursuit, or protocol encoding at all.\n\nThis supersedes both previously-listed options. Effort revised: XS (single line + a named constant), not M. Moving to todo.	2026-06-30 22:58:29.499108
68	235	claude-sonnet-5	**[Developer]** Implemented on fix/235-diagonal-move-speed (commit 93c7a9f):\n- New `MOVE_DIAGONAL_FACTOR :: f32(1.41421356)` constant next to `MOVE_REPEAT_INTERVAL`.\n- Post-send `cs.move_timer` scaled by this factor when `is_diagonal`.\n- architecture/input-routing.md updated (also fixed a stale "125ms/8 tiles per sec" comment that no longer matched the actual 100ms/10-tiles constant while I was in that section).\n\nVerification:\n  odin check src/server: GREEN (no server change, sanity-checked anyway)\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — client-only timing change, no automatable unit test (move_timer is real-time-driven via rl.GetFrameTime(), not a pure function); verified by code inspection that the multiply is correctly gated on is_diagonal and applies to both mv_fresh and mv_repeat paths.\n\nMoving to done.	2026-06-30 23:00:13.063376
69	245	claude-sonnet-5	**[Requirement Manager]** Investigated far deeper than the original #229 fix — this is a coordinate-system mismatch, not a missing draw call (that part was #229's fix and is fine). draw_containers computes screen position via raw tile*TILE_SIZE while the camera/terrain/players/workers all use tile_to_screen (isometric, since #106). Confirmed via direct code read of render.odin (camera target derivation, tile_to_screen formula, worker lerp system as a working comparison case). Filed kbai #247 for the same bug in draw_terminals/draw_artifacts/draw_gcs (sibling entities, same root cause, out of this ticket's literal "container" scope per Ticket-First Mandate). Moving to planning.	2026-06-30 23:10:56.176176
70	247	claude-sonnet-5	**[Architect-adjacent, filed during #245 Requirement Manager pass]** Left in backlog — same root cause as #245, lower urgency since no tester has yet reported terminals/artifacts/GCs specifically as invisible (possibly because they're encountered less often, or the offset happens to land near-enough to be visible in some play patterns). Pick up after #245 lands as the reference fix.	2026-06-30 23:11:20.443229
71	245	claude-sonnet-5	**[Architect]** Single-line fix in a single proc, no decomposition needed. No ADR required — this isn't a new design decision, it's restoring consistency with the existing tile_to_screen convention established by ADR/spec for #106. Will note the fix in architecture/render-pipeline.md instead (which already owns coordinate transforms). Effort: XS. Moving to todo.	2026-06-30 23:11:26.689862
72	245	claude-sonnet-5	**[Developer]** Implemented on fix/245-container-iso-coords (commit be122e3):\n- `draw_containers`: `cx, cy := tile_to_screen(c.x, c.y)` replaces raw `tile * TILE_SIZE`. Visual layout (square highlight + circle + glyph) unchanged, just correctly positioned now.\n- architecture/render-pipeline.md: added missing `draw_containers` row to the entity draw-order table (was never documented after #229), plus a new "Coordinate-system pitfall" callout explaining the bug class and pointing at #247 for the remaining affected procs.\n\nNo automated test — this is a pure rendering/visual fix (Raylib draw calls), consistent with AGENTS.md Tester scope ("What NOT to test: Rendering, Raylib calls"). Visual correctness requires a live client session to confirm; I cannot run the graphical client headlessly in this environment, so this needs a human or Tester playtest to close the loop on "is it now actually visible in-game."\n\nVerification:\n  odin check src/server: GREEN (sanity, no server change)\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — no server-side change, pure client rendering fix, not testable per Tester scope (no Raylib calls in test suite)\n\nMoving to done. Flagging: closing this without a live-game visual confirmation is a process gap — same limitation noted on #231 earlier this session. Recommend a human playtest pass to confirm visually before fully trusting this fix in production.	2026-06-30 23:12:53.924188
73	246	claude-sonnet-5	**[Requirement Manager]** Checked tile_to_screen, camera init/zoom/rotation, screen_to_tile — all mathematically consistent, standard 2:1 dimetric projection, no shear/skew bug found in the core transform. This may be a subjective "doesn't match my expectation of isometric" report rather than a regression, OR a texture/sprite aspect-ratio mismatch I haven't checked yet (ground tile textures vs. the diamond footprint). Per AGENTS.md Decision Protocol ("error is ambiguous with multiple likely root causes, fixes not symmetric in risk") — not implementing a guess. Need either a screenshot from the reporter or a side-by-side comparison against adr/106's intended look before this can be sized. Leaving in backlog rather than advancing to planning with an unresolved ambiguity — re-pick-up once reporter clarifies or a screenshot is attached.	2026-06-30 23:23:49.13707
74	247	claude-sonnet-5	**[Requirement Manager]** Checked advance_gcs (simulation.odin:1332-1398) — GCs move every server tick (20/sec), same cadence as players/workers, not event-driven. Without a lerp, GC movement would snap tile-to-tile every 50ms — visually choppy compared to the smoothed workers/players. Recommend mirroring `_update_worker_lerps` with a `gc_lerps` map for consistency, not just a bare `tile_to_screen` call. Moving to planning.	2026-06-30 23:24:19.174896
75	247	claude-sonnet-5	**[Architect]** Confirmed ClientState already has the `player_lerps`/`worker_lerps: map[u64]PlayerLerp` pattern (main.odin:100-101, init at 211-212, deleted at 303-304, updated each frame at 478-479). Plan:\n- New `gc_lerps: map[u64]PlayerLerp` field, mirroring worker_lerps exactly (init/delete/update call sites).\n- New `_update_gc_lerps` proc, near-identical to `_update_worker_lerps`, keyed by GC id, source of truth `cs.gcs`.\n- `draw_terminals`/`draw_artifacts`: no lerp needed — these are static once placed (terminals/artifacts don't move tile-to-tile after creation), a direct `tile_to_screen` call per frame is sufficient, same as the resource-icon pattern (render.odin:630).\n- No ADR needed — this is restoring consistency with the existing #106 convention, not a new decision (same reasoning as #245).\nNo decomposition needed, single cohesive change across one new lerp system + 3 position-calc swaps. Effort: S (30min-2h, slightly more than #245's XS due to the GC lerp addition). Moving to todo.	2026-06-30 23:24:33.478246
76	247	claude-sonnet-5	**[Developer]** Implemented on fix/247-terminal-artifact-gc-iso-coords (commit e804184):\n- `draw_terminals`, `draw_artifacts`: direct `tile_to_screen` call replaces raw `tile*TILE_SIZE`.\n- `draw_gcs`: new `gc_lerps: map[u64]PlayerLerp` field on ClientState (init/delete alongside player_lerps/worker_lerps), new `_update_gc_lerps` proc mirroring `_update_worker_lerps` (same `GC_PURSUIT_RATE = 8.0`), wired into the per-frame update chain. `draw_gcs` reads from the lerp instead of computing raw position directly.\n- architecture/render-pipeline.md updated — coordinate-pitfall note now says "both fixed" and documents the lerp-vs-direct-call split (static entities: direct call; per-tick-moving entities: lerp map).\n\nSame caveat as #245: pure rendering fix, no automated test (Tester scope excludes Raylib calls), and I cannot get a reliable headless/visual confirmation in this sandboxed environment (Wayland-only raylib build, attempted Xvfb workaround in this session and it didn't pan out — documented for the record, not retrying here). Recommend a human playtest pass covering #245 and #247 together.\n\nVerification:\n  odin check src/server: GREEN (sanity, no server change)\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — no server-side change, pure client rendering fix\n\nMoving to done.	2026-06-30 23:26:50.287731
77	248	claude-sonnet-5	**[Requirement Manager]** Found and confirmed root cause via code read: simulation.odin:692-695 uses Manhattan distance ≤1 for the deposit/withdraw adjacency check, which rejects diagonal neighbors. Since #234 (this session) made container crafting diagonal-facing-capable, and the iso WASD scheme makes single-key presses diagonal by default, most containers end up diagonally placed — making this nearly universally broken, matching the blunt severity of the report. High priority. Moving to planning.	2026-06-30 23:28:46.132127
78	248	claude-sonnet-5	**[Architect]** Verified success-criterion 4 — grepped all Manhattan-distance usages in simulation.odin. Lines 1154-1216 are GC AI targeting/damage radius checks (much larger radii, different semantics — not "must be exactly adjacent," a design choice not a bug). resolve_interaction (line 469, used by terminal-open/worker-pickup/wall-building) uses exact-tile match via direction_delta, not a distance check — already diagonal-correct since #234. _apply_container_transfer is the only adjacency-gated interaction with this bug. Single-line fix, no decomposition, no ADR (restoring intended behavior, not a new decision). Effort: XS. Moving to todo.	2026-06-30 23:29:06.472176
79	248	claude-sonnet-5	**[Developer]** Implemented on fix/248-container-diagonal-adjacency (commit ea33a41):\n- `_apply_container_transfer`: adjacency check changed from `dx+dy > 1` (Manhattan) to `max(dx,dy) > 1` (Chebyshev).\n- Changed proc privacy from `@(private="file")` to `@(private="package")` to make it directly unit-testable, matching the established `_apply_move` pattern.\n- New `container_transfer_test.odin`: 3 tests — diagonal deposit succeeds, diagonal withdraw succeeds, genuinely out-of-range (2 tiles) still rejected.\n- architecture/inventory-system.md updated with the adjacency rule and why it broke.\n\nThis one IS testable (unlike #245/#247) since it's pure server logic, no Raylib involved — full regression coverage.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin test  src/server: 113 passed (110 + 3 new)\n\nMoving to done.	2026-06-30 23:31:17.223349
80	250	claude-sonnet-5	**[Story Writer]** Aufgenommen als Fortsetzung von #230 (TODO-Liste in architecture/ui-system.md). Titel-Nummer von 249 auf 237 korrigiert — 249 hätte die NNN-Sequenz dieses Projekts übersprungen (letzte verwendete Nummer war 234/236).	2026-06-30 23:33:14.913726
81	250	claude-sonnet-5	**[Requirement Manager]** Read all 5 target procs (render.odin: draw_fragment_overlay:1575, draw_help_overlay:1609, draw_terminal_overlay:1660, draw_hud:1375; escape_menu.odin: draw_escape_menu:41). All use plain ASCII text already (no font-constraint violations to fix here, unlike #228/#230's original scope). draw_escape_menu uses raylib built-ins (rl.GREEN/RED/DARKGREEN) rather than local consts — same migration principle applies. Moving to planning.\n\n**[Architect]** Checked draw_hud's HUD_COL/HUD_DIM/HUD_CYAN (render.odin:1283-1286) against UI_COL/UI_DIM/UI_CYAN (ui.odin) — exact value matches ({51,255,139,255}, {31,128,80,255}, {0,200,200,255} respectively). This is pure duplicate-constant removal, not a redesign — low risk. HUD_PANEL (background tint, {5,34,26,200}) has no UI_* equivalent and isn't conceptually the same as ui_panel (modal panel vs. always-visible HUD strip) — leaving it as its own HUD-specific constant, not folding into ui_panel.\n\nNo decomposition needed — single cohesive mechanical change across 2 files, low risk per-overlay. No ADR (restoring consistency with #230's established convention). Effort: M confirmed. Moving to todo.	2026-06-30 23:34:07.213649
82	250	claude-sonnet-5	**[Developer]** Implemented on feat/237-ui-overlay-migration (commit f4d5020):\n- draw_fragment_overlay, draw_escape_menu: migrated to ui_panel + ui_dim_screen.\n- draw_terminal_overlay, draw_help_overlay: local consts → UI_COL/UI_DIM/UI_HEADER, ui_dim_screen.\n- draw_hud: HUD_COL/HUD_DIM/HUD_CYAN were exact-value duplicates of UI_COL/UI_DIM/UI_CYAN — removed, all call sites (status bar, worker panel, notifications, etc.) repointed at UI_*. HUD_PANEL/HUD_FRAME/HUD_SHADOW kept as HUD-specific (no UI_* equivalent, conceptually distinct from a dimmable modal).\n- architecture/ui-system.md table fully updated to "uses UI system."\n\nAll 5 overlays from #230's TODO list are now migrated — architecture/ui-system.md has no remaining TODOs in the overlay-inventory table.\n\nPure rendering/palette change, no logic touched — same caveat as #245/#247 on visual confirmation (cannot reliably run the graphical client headlessly in this session's environment). Visual change is low-risk: same colors (HUD dedup) or near-identical colors (overlay migrations used equivalent UI_* values to the originals), so regression risk is low even without a live screenshot.\n\nVerification:\n  odin check src/server: GREEN (sanity, no server change)\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — pure client rendering/palette change, no server-side logic\n\nMoving to done.	2026-06-30 23:37:33.463446
83	231	claude-sonnet-5	**[Architect]** Closing with real evidence, not just code inspection: kbai #248 (filed by a tester after #245 landed) explicitly states "the container is now visible as 'C' glyphe" — direct live-session confirmation that the container renders. #245 (this session, commit be122e3) was a deeper fix than #229 alone — the actual bug was a coordinate-system mismatch (top-down vs isometric), not just a missing draw call. That fix is what made the container actually visible. Moving to done.	2026-06-30 23:39:20.253305
126	272	claude-sonnet-4-6	Beide Punkte behoben:\n\n1. Relation-Label-Bug: getrennte relationLabels (outgoing) / incomingRelationLabels-Map in TicketModal.svelte statt fehlerhaftem String-Replace-Hack. Eingehende parent_of-Relation zeigt jetzt korrekt "ist Kind von" statt "ist Parent von-Ziel von".\n\n2. Epic-Erstellung: "Neues Ticket"-Formular hat jetzt ein Typ-Select (Ticket/Epic) und ein optionales "Parent-Epic"-Dropdown (lädt alle Tickets mit type=epic im Projekt). Bei Auswahl wird nach dem Erstellen automatisch eine parent_of-Relation vom Epic zum neuen Ticket angelegt (POST /api/tickets/{epicId}/relations). Damit ist der komplette Flow — Epic anlegen, Ticket direkt als Kind eines Epics erstellen — über die UI ohne Umweg erreichbar.	2026-07-01 20:50:23.446034
84	30	claude-sonnet-5	**[Architect — Kanban-Hygiene-Check]** Dieses Ticket ist seit 2026-06-23/-30 in planning, beschreibt aber den vollen Scope von ADR-106 (adr/106-isometric-rendering.md). Re-Lese der ADR zeigt: Phase 1 (Sub-Tickets 106a-g — Konstanten, draw_world, Entity-Rendering, Camera, Atmosphäre, Input-Vorbereitung, Sprite-Review) ist explizit der aktuelle Scope; Multi-Level-Plattformen + Connection-Lines (aus dem Konzept-Bild, in diesem Ticket-Text erwähnt) sind in der ADR explizit als "Phase 2" vertagt, nicht Teil von 106.\n\nStatus von Phase 1, Stand heute: 106c (Entity-Rendering auf iso) war für Container/Terminals/Artifacts/GCs bis heute NICHT vollständig — das waren genau die Bugs aus kbai #245 und #247 (diese Session, beide done), die draw_containers/draw_terminals/draw_artifacts/draw_gcs von Top-Down-Koordinaten auf tile_to_screen umgestellt haben. Mit #245/#247 ist 106c jetzt komplett. Player/Worker-Rendering (106c-Teil) war schon vorher korrekt.\n\nEmpfehlung: Phase 1 (= dieses Ticket #30) als done markieren — der beschriebene Engine-Rewrite ist abgeschlossen. Phase 2 (Elevation/Plattform-Topologie/Connection-Lines) ist separates, noch nicht angelegtes Zukunfts-Ticket — sollte bei Bedarf neu als Story aufgenommen werden, nicht als Fortsetzung dieses Tickets, da Scope/Aufwand grundlegend anders ist (Engine-Erweiterung, nicht -Migration).	2026-06-30 23:40:44.690194
85	192	claude-sonnet-5	**[Architect]** Ticket-Description war minimal (1 Zeile, Pre-kbai-Relikt). Mensch hat Key-Quelle entschieden: World-Drop, analog zu Header-Fragment-Artifacts.\n\n**Design:**\n1. `shared/items.odin`: neuer `ItemType.Pointer_Key` (angehängt, wire-kompatibel). `item_def`: Equipment-Kategorie, keine `consumable`-Capability (wird nicht über Inventar-`[e]` benutzt, sondern automatisch beim Cave-Interact verbraucht — sonst irreführender "[e] use"-Hint im Inventar-Overlay der nichts tut).\n2. `shared/entities.odin`: `ArtifactType` um `.Pointer_Key` erweitert (4. Wert nach `Kernel_Module`, wire-kompatibel).\n3. `simulation.odin` `_handle_artifact_pickup`: neuer Case `.Pointer_Key` — pusht 1× `ItemType.Pointer_Key` in Spieler-Inventar (bisher hatte die Funktion keinen Spieler-Inventar-Zugriff für die Kernel_Log/Module-Cases — muss `state.players[player_id]` jetzt lesen+schreiben).\n4. `simulation.odin` `resolve_interaction`: Priority-4-Switch um `.Cave_Entrance` erweitert — nur wenn Spieler `Pointer_Key` im Inventar hat, sonst bleibt `t.kind = .None` (stiller No-Op, gleiches Muster wie `Toggle_Worker`s Ownership-Check).\n5. `simulation.odin` `apply_interaction`: neuer Case `.Open_Cave_Entrance` — verbraucht 1× `Pointer_Key`, setzt Tile auf `.Empty` (Eingang dauerhaft offen).\n6. `server/spawn.odin`: neue `tick_pointer_key_respawn`, Kopie von `tick_artifact_respawn`s Cold-Sector-Mechanik, aber: niedrigere Rarity (Keys sollen wertvoller wirken als Fragments), und Ausschluss von Tiles innerhalb eines bereits generierten (aber noch nicht geöffneten) `state.caves`-Interior — sonst Henne-Ei-Problem (Key spawnt in einer Höhle, die man ohne Key nicht betreten kann).\n\nTests: `resolve_interaction` mit/ohne Key im Inventar (pure function, einfach testbar wie bisherige Interaction-Tests). `apply_interaction` Key-Verbrauch + Tile-Öffnung.\n\nKein ADR nötig — folgt 1:1 dem etablierten Artifact/Interaction-Muster, keine neue Architektur-Entscheidung. Effort: M bestätigt. Bleibt in todo (war schon dort), kein Decomposition nötig — eine zusammenhängende Änderung über mehrere Dateien.	2026-06-30 23:43:43.208674
86	192	claude-sonnet-5	**[Developer]** Implemented on feat/192-cave-entry-pointer-key (commit 98192ee):\n- ItemType.Pointer_Key, ArtifactType.Pointer_Key (world-drop via tick_pointer_key_respawn, mirrors tick_artifact_respawn with lower rarity + chicken-egg guard against spawning inside unopened caves).\n- resolve_interaction/apply_interaction: Open_Cave_Entrance interaction kind, key-gated, permanent/global unlock.\n- _handle_artifact_pickup: new Pointer_Key case pushes the item into player inventory.\n- draw_artifacts: distinct "P" glyph for Pointer_Key (was falling into the generic "K" bucket).\n- New ADR 192 (key-source decision: world-drop, chosen by human this session; permanent-unlock scope cut documented).\n- New architecture/cave-system.md — consolidates the previously spec-less Cave System (worldgen #191, loot #193, now entry #192) into the subsystem index; architecture/README.md updated.\n- 4 new tests in interaction_test.odin: resolve with/without key, apply consumes key + opens tile, apply without key is a no-op.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin test  src/server: 117 passed (113 + 4 new)\n  odin test  src/shared: 31 passed (protocol_test.odin's exhaustive ArtifactType switch needed #partial after the enum grew)\n\nMoving to done.	2026-06-30 23:49:10.734209
87	24	claude-sonnet-5	**[Architect — Kanban-Hygiene]** Beide Sub-Tickets bestätigt done (verifiziert via get_ticket, nicht nur Freitext): #189 (006a, status_id=17) und #190 (006b, status_id=17). Parent-Ticket-Scope (Wall-Building + GC-Kollision) ist vollständig erledigt. Moving to done.	2026-06-30 23:53:03.590078
88	28	claude-sonnet-5	**[Architect — Kanban-Hygiene]** Alle 4 Sub-Tickets bestätigt done (verifiziert via get_ticket): #191 (100a, status_id=17), #192 (100b, status_id=17, diese Session), #193 (100c, status_id=17), #194 (100d, status_id=17). Parent-Ticket-Scope (Orphaned Memory Caves komplett: Worldgen + Entry + Loot + Visual) ist vollständig erledigt. Siehe auch architecture/cave-system.md (neu, diese Session) für die konsolidierte Spec. Moving to done.	2026-06-30 23:53:08.143288
89	31	claude-sonnet-5	**[Architect]** Untersucht alle 3 vom Menschen gewählten Varianten technisch:\n\n- **(a) Ambient Decoration:** rein client-seitig, statischer Snippet-Pool, deterministisch per Terminal-ID ausgewählt (stabil über Frames). Kein Protokoll-Change. Effort S — sofort umsetzbar.\n- **(b) Live Worker Code:** braucht server-seitiges Tracking "welche Zeile führt Worker X gerade aus" — existiert nicht (grep über scripting.odin: kein current_line/active_line Feld). Braucht neue Server-Buchhaltung + Broadcast-Mechanismus (vermutlich Snapshot-Erweiterung oder neue MSG_*). Deutlich größerer Scope als ursprünglich angenommen.\n- **(c) Echo Surfaces:** Echo-System (#036) liefert aktuell nur proximity-getriggerte Toast-Nachrichten (MSG_ECHO_DELIVERY → nc.pending_echoes, transient), keine persistente Orts-Bindung an Terminals. Bräuchte neue Sektor↔Terminal-Echo-Assoziation + Anzeige-Logik. Auch größerer Scope.\n\n**Entscheidung:** Dekomposition in 3 separate Tickets statt 2 (Original-Ticket hatte nur 108a/108b vorgesehen, deckt aber nicht alle 3 Varianten ab):\n- **108a — Ambient Decoration** (dieses Ticket, umbenannt/reduziert auf Variante a): Effort S, todo-ready, wird jetzt implementiert.\n- **108b — Live Worker Code:** neues Ticket, backlog, Effort M-L (braucht Server-Line-Tracking-Design zuerst).\n- **108c — Echo Surfaces:** neues Ticket, backlog, Effort M (braucht Sektor-Terminal-Assoziations-Design zuerst).\n\nKein ADR für 108a nötig (rein dekorativ, keine Architektur-Entscheidung). 108b/108c brauchen eigene Architect-Pässe bevor Developer-Pickup möglich ist — als separate Tickets sauberer als ein XL-Ticket, das alles auf einmal versucht.	2026-06-30 23:56:54.06384
90	31	claude-sonnet-5	**[Developer]** Implemented on feat/108a-terminal-ambient-code-deco (commit a3b34de):\n- AMBIENT_CODE_SNIPPETS: 5 small Lua-flavored snippets, file-private constant.\n- draw_terminals: each terminal selects `AMBIENT_CODE_SNIPPETS[tm.id % len(...)]` (deterministic), renders 2-3 lines in UI_DIM above the terminal glyph via tile_to_screen-derived coordinates.\n- architecture/render-pipeline.md draw_entities table updated.\n\nPure rendering/decoration, no automated test (Tester scope excludes Raylib calls), same visual-confirmation caveat as #245/#247/#237 this session.\n\nVerification:\n  odin check src/server: GREEN (sanity, no server change)\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — pure client decoration, no server logic\n\nMoving to done. 108b and 108c remain in backlog awaiting their own Architect design pass (server-side line-tracking and sector-terminal association respectively — both genuinely larger than originally scoped).	2026-06-30 23:59:05.120509
91	257	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält bereits vollständige Datei:Zeile-Belege und Success Criteria aus dem Code-Review. Keine offenen Fragen, kein ADR nötig (reine Doku-Korrektur, keine neue Entscheidung). Moving to todo.	2026-07-01 14:31:24.889914
92	259	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält bereits vollständige Belege. Keine offenen Fragen. Moving to todo.	2026-07-01 14:31:27.096061
93	263	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält bereits vollständige Belege. Keine offenen Fragen. Moving to todo.	2026-07-01 14:31:29.232305
94	264	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält bereits vollständige Belege. Keine offenen Fragen. Moving to todo.	2026-07-01 14:31:32.584966
95	256	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält bereits vollständige Belege. Keine offenen Fragen. Moving to todo.	2026-07-01 14:31:35.926479
96	260	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält bereits vollständige Belege, alle 5 Punkte verifiziert. Keine offenen Fragen. Moving to todo.	2026-07-01 14:31:38.912404
97	261	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält bereits vollständige Belege. Keine offenen Fragen. Moving to todo.	2026-07-01 14:31:41.213568
99	256	claude-sonnet-5	**[Developer]** Doku-Fix umgesetzt, keine Code-Änderungen.\n\narchitecture/multiplayer-protocol.md:\n- 12 fehlende Message-Types ergänzt (C→S: MSG_CRAFT_TRANSISTOR 0x0E, MSG_CRAFT_TERMINAL 0x0F, MSG_BUILD_WALL 0x1F, MSG_CONTAINER_DEPOSIT 0x21, MSG_CONTAINER_WITHDRAW 0x22, MSG_HEAP_DEPOSIT 0x23, MSG_HEAP_WITHDRAW 0x24; S→C: MSG_FRAGMENT_TABLE 0x1B, MSG_FRAGMENT_HINT 0x1C, MSG_TERMINAL_OPEN 0x1E, MSG_CONTAINER_OPEN 0x20; neue "Bidirektional"-Sektion für MSG_UPGRADE_WORKER_LOC 0x1D), Payload-Formate direkt aus protocol.odin übernommen (keine Annahmen).\n- World-Snapshot-Encoding-Sektion ergänzt: Terminal-Block (nach Artifacts, u16 count, [id,owner_id,x,y]), trailing avg_entropy (f32), trailing Memory-Leak-Block (u16 count + [x,y]×count), inkl. Prosa-Absätze zu jedem neuen Block (Reihenfolge exakt wie in encode_world_snapshot, protocol.odin:290-478).\n\narchitecture/server_client_architecture.md:\n- ServerState-Struct (Zeile ~157) um 5 fehlende Felder ergänzt: terminals, memory_leaks, caves, net_inboxes, harvested_tiles — je mit Typ + Ein-Zeiler-Beschreibung, cross-referenziert auf cave-system.md und biome-system.md.\n\nNur die zwei genannten Dateien angefasst. Kein Code geändert.\n\nVerification:\n  odin check src/server: N/A — doc-only\n  odin check src/client: N/A — doc-only\n  odin test  src/server: N/A — doc-only	2026-07-01 14:35:01.416284
100	260	claude-sonnet-5	**[Developer]** Fixed all 5 factual errors in architecture/gc-ai.md, doc-only, verified against live src/server/simulation.odin.\n\n1. L1 target priority (was "Containers first, nearest fallback"): corrected to Players (preferred, code comment "Scan Players first (preferred)") → active placed Workers → Containers as last resort, matching `_gc_find_target_l1` (simulation.odin:1174-1210) exactly.\n2. L1 Worker damage (was "—"): `_gc_apply_damage` uses `damage := GC_DAMAGE_WORKER` (25.0) as the default path for ALL tiers; only L2 overrides via `GC_L2_DAMAGE_WORKER` (also 25.0 currently). Table now shows 25.0/contact for both tiers.\n3. Cold zone radius (was "~5 tiles"): `GC_COLD_ZONE_RADIUS_SQ :: i32(400)` is squared radius → actual radius = sqrt(400) = 20 tiles. Doc corrected to 20, noted it's the squared constant.\n4. L1 player damage (was "~5.0/contact"): `GC_DAMAGE_PLAYER :: f32(10.0)`. Corrected to 10.0/contact (same as L2's `GC_L2_DAMAGE_PLAYER`).\n5. L1 detection radius (was undocumented, "pursues nearest target"): `GC_DETECTION_RADIUS :: i32(16)`. Added as 16 tiles. Verified L2's existing doc value of 24 tiles still matches `GC_L2_DETECTION_RADIUS :: i32(24)` — no change needed there.\n\nNo source files touched (src/, adr/ untouched) — pure documentation edit to architecture/gc-ai.md.\n\nVerification:\n  odin check src/server: GREEN (nix develop --command odin check src/server — no errors, only pre-existing "Git tree is dirty" warning)\n  odin check src/client: GREEN (nix develop --command odin check src/client — no errors, only pre-existing "Git tree is dirty" warning)\n  odin test  src/server: SKIPPED — doc-only change, no logic touched, no new tests warranted	2026-07-01 14:35:14.073282
101	257	claude-sonnet-5	**[Developer]** Sub-Agent hat beide ADRs aktualisiert (adr/101, adr/032), commit a74b7ca. lockstep-Feld dokumentiert, v1-v4 Versionshistorie mit Breaking-Changes ergänzt.\n\nVerification:\n  odin check src/server: N/A — doc-only\n  odin check src/client: N/A — doc-only\n  odin test  src/server: N/A — doc-only\n\nMoving to done.	2026-07-01 14:36:43.259163
102	259	claude-sonnet-5	**[Developer]** Sub-Agent hat world_and_resources.md um alle 5 fehlenden TileTypes ergänzt (Wall, Crystal_Bytes/CPU/RAM, Cave_Entrance), commit b0c64f1.\n\nVerification: N/A — doc-only (alle drei Zeilen).\n\nMoving to done.	2026-07-01 14:36:54.26057
103	263	claude-sonnet-5	**[Developer]** Sub-Agent hat header-fragments.md (fragment_decrypt-Signatur) und item-system.md (Pointer_Key) aktualisiert, commit 19c0113.\n\nVerification: N/A — doc-only.\n\nMoving to done.	2026-07-01 14:37:11.551826
104	264	claude-sonnet-5	**[Developer]** Sub-Agent hat input-routing.md um alle 3 fehlenden InputContext-Werte + Kontext-Details-Abschnitte ergänzt, commit cdd4838.\n\nVerification: N/A — doc-only.\n\nMoving to done.	2026-07-01 14:37:20.120828
105	260	claude-sonnet-5	**[Developer]** Sub-Agent hat gc-ai.md korrigiert (Priorität, Worker-Damage, Cold-Zone-Radius, Player-Damage, Detection-Radius), commit d75f8e1. odin check bereits vom Sub-Agent bestätigt GREEN (kein Code geändert, nur Sanity-Check).\n\nVerification: N/A — doc-only.\n\nMoving to done.	2026-07-01 14:37:36.128004
106	261	claude-sonnet-5	**[Developer]** Sub-Agent hat player-state.md + worker-scripting.md auf das aktuelle Inventory-Slot-System umgeschrieben, commit 451422d.\n\nVerification: N/A — doc-only.\n\nMoving to done.	2026-07-01 14:37:44.967886
107	262	claude-sonnet-5	**[Developer]** Sub-Agent hat interaction-system.md korrigiert (direction_delta-Beispiel, Open_Cave_Entrance), commit 93efe10.\n\nVerification: N/A — doc-only.\n\nMoving to done.	2026-07-01 14:37:53.114431
108	265	claude-sonnet-5	**[Requirement Manager → Architect]** Ticket-Beschreibung enthält vollständige Belege und ist mechanisch identisch zu #237s Muster (lokale Farbkonstanten → UI_*-Palette). Kein ADR nötig. Moving to todo.	2026-07-01 14:38:44.722189
109	265	claude-sonnet-5	**[Developer]** Sub-Agent hat draw_man_pages_overlay auf UI_COL/UI_DIM/UI_HEADER/UI_WARN + ui_dim_screen() migriert (SIG→UI_WARN als nächstliegende Palette-Farbe), render-pipeline.md korrigiert (Proc-Namen, draw_worker_commands entfernt, HUD-Tabelle ergänzt), ui-system.md Overlay-Tabelle ergänzt. Commit 793b08f.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin test  src/server: SKIPPED — pure client rendering/palette change\n\nMoving to done.	2026-07-01 14:41:04.040246
110	258	claude-sonnet-5	**[Requirement Manager]** Mensch-Entscheidung eingeholt: mehr Echoes in kalten Sektoren, Code wird an ADR-031 angepasst. Moving to planning.	2026-07-01 14:44:47.106909
111	258	claude-sonnet-5	**[Architect]** Design bereits vollständig in der Description (2-Datei-Fix, zusammenhängend). Kein ADR-Update nötig (ADR-031 war korrekt). Effort XS bestätigt. Moving to todo.	2026-07-01 14:44:56.461669
112	258	claude-sonnet-5	**[Developer]** Implementiert auf fix/258-sector-activity-score-semantics (commit 9d61cf3):\n- sector_activity_score: Formel gedreht (0.0=aktiv, 1.0=kalt, ADR-031-konform).\n- echo.odin: Kompensierende Invertierung entfernt (`0.25*(1-activity)` → `0.25*activity`), No-Record-Fallback von 0→1 gedreht (kein Eintrag = nie besucht = maximal kalt).\n- Neuer sector_test.odin: 3 Tests (gerade besucht, kalt, Mittelwert).\n\nSpielverhalten unverändert (mehr Echoes in kalten Sektoren bleibt erhalten), nur die interne Semantik ist jetzt korrekt und ADR-konform.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN (Sanity, keine Client-Änderung)\n  odin test  src/server: 120 passed (117 + 3 neue)\n\nMoving to done.	2026-07-01 14:46:29.885691
113	266	claude-sonnet-5	**[Requirement Manager]** Ticket-Beschreibung enthält bereits vollständigen Split-Vorschlag mit Datei-Zuordnung. Keine offenen Fragen. Moving to planning.	2026-07-01 14:54:52.834413
114	266	claude-sonnet-5	**[Architect]** Exakten Split-Schnitt festgelegt (siehe adr/266-render-file-split.md, bereits geschrieben): 5 Dateien — render.odin (Orchestrator: Font/Text-Utils + draw_entities-Dispatcher), render_world.odin (Koordinaten + Terrain), render_entities.odin (Entity-Draws + Lerp + Effects), render_hud.odin (immer-sichtbares HUD), render_overlays.odin (Modal/Overlay-Draws).\n\nVor Umsetzung geprüft: 9 `@(private="file")`-Deklarationen im Original — nur 2 davon (`draw_iso_diamond`, `g_font`) haben Caller, die nach dem Split in unterschiedlichen Dateien landen und brauchen deshalb Package-Sichtbarkeit. Die anderen 7 (`_player_trail`, `_worker_trail`, `_worker_flicker`, `substrate_hash`, `AMBIENT_CODE_SNIPPETS`, etc.) bleiben file-private, da ihre einzigen Caller mit ihnen in dieselbe neue Datei wandern.\n\nKein Sub-Ticket nötig — mechanische Extraktion, kein Design-Risiko mehr nach diesem Schnitt. Effort L bestätigt. Moving to todo.	2026-07-01 14:55:12.399584
115	266	claude-sonnet-5	**[Developer]** Implementiert auf refactor/266-render-file-split (commit 6cf6adc). Extraktion per präzisen sed-Line-Ranges (nicht manuelles Retippen, um Copy-Paste-Fehler zu vermeiden), danach iterativ gegen odin check korrigiert (2 fehlende Imports, 2 Sichtbarkeits-Anpassungen).\n\n- render.odin: 1861 → 68 Zeilen (Orchestrator)\n- render_world.odin: 445 Zeilen\n- render_entities.odin: 734 Zeilen (größte neue Datei, aber deutlich unter dem alten 1861-Zeilen-Monolithen)\n- render_hud.odin: 401 Zeilen\n- render_overlays.odin: 272 Zeilen\n\nVerifiziert: Proc-Anzahl vor/nach identisch (45), kein Proc verloren oder dupliziert. draw_iso_diamond + g_font von file-private auf package-visible relaxed (einzige nötigen Sichtbarkeits-Änderungen). Neue ADR 266 dokumentiert den Schnitt. architecture/render-pipeline.md Source-Zeile aktualisiert.\n\nVerification:\n  odin check src/server: GREEN\n  odin check src/client: GREEN\n  odin build src/client: erfolgreich (Binary verlinkt, danach gelöscht)\n  odin test  src/server: SKIPPED — reiner Client-Datei-Split, keine Server-Änderung\n\nMoving to done.	2026-07-01 14:55:59.227028
116	241	claude-sonnet-4-6	Triage: Gutes Feature, aber erfordert neue DB-Tabelle (ticket_relations) + Migration + mind. 2 neue Tools (link_tickets, get_ticket_relations). Scope zu gross für aktuellen Sprint. Bleibt in backlog als eigenständiges Architektur-Ticket. Umsetzbar sobald Ticket-Relations als priorisiertes Feature beschlossen werden.	2026-07-01 15:12:00.32813
117	252	claude-sonnet-4-6	Triage: EPIC-Typ benötigt entweder ein neues ticket_type-Feld (einfach) oder eine vollständig neue epics-Tabelle mit Parent/Child-Beziehungen (komplex). Ticket #241 (strukturierte Relations) ist ein verwandtes Vorhaben das zuerst gelöst werden sollte. Bleibt in backlog bis Relations-Feature entschieden.	2026-07-01 15:12:03.549042
118	255	claude-sonnet-4-6	Triage: Wertvolles Feature (human-in-the-loop Eskalations-Workflow). Erfordert zwei neue board_statuses (human_intervention + human_answered) mit spezieller Übergangslogik im Workflow-Trigger. Umsetzbar als Konfiguration auf Projektebene, nicht hardcodiert — braucht aber Abstimmung über Setup-Flow (wer legt diese Statuses an und wann?). Bleibt in backlog für Design-Entscheidung.	2026-07-01 15:12:06.121081
119	271	claude-sonnet-4-6	Implementiert: Header-Button "Inbox" mit Badge-Counter zeigt alle Tickets im Status human_intervention. Kanban-Board blendet human_intervention/human_answered-Spalten aus (statuses.filter(s => !s.special_type)). Im TicketModal erscheint bei wartenden Tickets ein Banner mit Button "An die KI zurückgeben", der das Ticket per PATCH nach human_answered verschiebt. Neue Datei: src/lib/components/inbox/InboxModal.svelte.	2026-07-01 20:33:31.90618
120	270	claude-sonnet-4-6	V4 UI-Integration abgeschlossen: BoardStatus.special_type + Ticket.type/TicketRelation in types.ts. Neue Endpoints GET/POST /api/tickets/[id]/relations, DELETE /api/tickets/[id]/relations/[rid]. TicketModal: Epic-Badge, Typ-Select (ticket/epic) im Edit-Formular, neue "Verknüpfungen"-Sektion mit Hinzufügen/Entfernen von Relationen (parent_of/blocks/duplicate_of/relates_to). TicketCard zeigt Epic-Flag + goldenen Rand. Human-Intervention/Human-Answered separat in Ticket 271 behandelt (Inbox-Feature).	2026-07-01 20:36:05.773844
121	249	claude-sonnet-4-6	Implementiert: KanbanColumn zeigt max. 25 Tickets (visibleCount State, PAGE_SIZE=25). Bei mehr Tickets erscheint unten ein "+N weitere laden"-Stapel-Button, Klick lädt die nächsten 25 nach (visibleCount += 25). Kein zusätzlicher API-Call nötig, da Board bereits alle Tickets im Client hält — rein clientseitiges Pagination-Slicing.	2026-07-01 20:37:15.208512
122	251	claude-sonnet-4-6	Kommentar-Eingabeformular in TicketModal und der Standalone-Ticketseite ergänzt (Textarea + Button, POST an bereits existierenden /api/tickets/[id]/comments Endpoint). Autor wird automatisch aus der eingeloggten Session (PostgreSQL-Username) gezogen.	2026-07-01 20:39:00.448294
123	267	claude-sonnet-4-6	Markdown-Rendering implementiert via marked + isomorphic-dompurify (SSR-sicher, da normales dompurify ein window-Objekt braucht). Neue Datei src/lib/markdown.ts mit renderMarkdown(text) -> sanitized HTML. Description-Feld in TicketModal und Standalone-Ticketseite nutzt jetzt {@html renderMarkdown(...)} statt whitespace-pre-wrap. Eigene .markdown-body CSS-Klasse in app.css für Headings/Listen/Code/Blockquotes im Dark-Theme.	2026-07-01 20:41:52.033928
124	268	claude-sonnet-4-6	Zwei Teile: 1) Board-Page highlightet jetzt bei JEDEM SSE-UPDATE (Cyan-Glow, 2s), nicht mehr nur bei Statuswechsel — also auch wenn KI nur description/assignee/model ändert. 2) Neue Trigger notify_ticket_child_change() auf ticket_comments und ticket_tasks (V5__Notify_On_Comments_And_Tasks.sql + in init-db/V1 für Fresh-Installs): pg_notify feuert jetzt auch wenn ein Agent nur kommentiert oder eine Task abhakt, ohne die tickets-Zeile selbst anzufassen. Migration muss auf Bestandsinstallationen manuell eingespielt werden (siehe INSTALL.md Abschnitt 7).	2026-07-01 20:44:03.0706
125	269	claude-sonnet-4-6	Spike-Ergebnis: OIDC-Auth direkt gegen PostgreSQL ist nicht trivial, aber machbar — mit Kompromissen.\n\n1) PostgreSQL nativ: Erst ab PG 18 gibt es einen nativen OAUTHBEARER-SASL-Mechanismus (RFC 7628, Supabase/Timescale-Beitrag). Für ältere PG-Versionen (kbai-ui empfiehlt aktuell PG 16) existiert kein natives OIDC-Auth-Verfahren im Server selbst.\n\n2) Für PG < 18 gängige Workarounds:\n   - LDAP-Bridge: IdP (Keycloak/Authentik) exposed LDAP-Interface, pg_hba.conf authentifiziert via LDAP. Indirekt, zusätzliche Infra-Komponente.\n   - GSSAPI/Kerberos-Bridge: enterprise-lastig, hoher Ops-Aufwand.\n   - Proxy-Pattern (am gängigsten in der Praxis, z.B. pgbouncer mit auth_query): App authentifiziert Nutzer per OIDC, verbindet intern aber mit einer festen Service-Rolle zur DB.\n\n3) Kompatibel mit unserem aktuellen Architekturprinzip (Nutzer gibt PG-Credentials direkt im Browser ein, keine Persistierung, PG-Rechte steuern Zugriff) wäre folgendes Muster: App authentifiziert per OIDC, mapped die verifizierte Identität auf einen bestehenden PG-Rollennamen, verbindet mit einer privilegierten Service-Rolle und nutzt `SET ROLE <mapped_user>` um die Rechte des Zielnutzers zu übernehmen (kein Passwort nötig, nur GRANT-Mitgliedschaft). Das bewahrt granulare PG-Rechte pro Nutzer, führt aber ein neues hochprivilegiertes Service-Credential ein, das besonders geschützt werden muss — ein Bruch mit dem aktuellen "keine gespeicherten Credentials"-Prinzip.\n\nEmpfehlung: Aktuell nicht umsetzen. Aufwand/Nutzen lohnt sich für die Zielgruppe (kleine Teams, Self-Hosted) nicht. Falls später gebraucht: SET-ROLE-Proxy-Pattern (Punkt 3) ist der sauberste Weg, PG 18 natives OAuth als Langfrist-Option im Auge behalten.	2026-07-01 20:45:07.231977
127	276	claude-sonnet-4-6	Root-Route `/` (worauf "Dashboard" im Navbar verlinkt) hatte tatsächlich keine +page.svelte — der Link führte ins Leere/404. Statt den Menüpunkt zu entfernen, funktionales Dashboard gebaut:\n\n- Neuer Endpoint GET /api/dashboard: eine aggregierte SQL-Query (COUNT...FILTER, GROUP BY project) für Ticket-Zahlen pro Projekt (gesamt, erledigt, wartend in Inbox) statt N+1-Fetches pro Projekt, plus eine zweite Query für die 10 zuletzt aktualisierten Tickets projektübergreifend. Beide Queries laufen parallel via Promise.all.\n- Neue src/routes/+page.svelte: 4 Kennzahlen-Karten oben (Projekte, Tickets, Erledigt, Wartet auf dich/Inbox-Summe), darunter pro-Projekt-Karten mit Fortschrittsbalken (klickbar → Board) und eine "Zuletzt aktualisiert"-Liste (klickbar → Ticket).\n- Neue Types: ProjectStats, RecentTicket, DashboardData in types.ts.\n\nBuild + svelte-check sauber.	2026-07-01 21:21:58.701153
128	277	claude-sonnet-4-6	Root Cause gefunden: isomorphic-dompurify bündelt jsdom, dessen Abhängigkeit html-encoding-sniffer wiederum @exodus/bytes per require() lädt — ein reines ESM-Package. Unter Vite dev's SSR-Modul-Transformation (CommonJS-Interop) führt das zu ERR_REQUIRE_ESM. Intermittierend, weil es nur bei den ersten SSR-Aufruf der Module (Cold-Start/Re-Optimierung) auftrat.\n\nFix: isomorphic-dompurify entfernt, stattdessen normales dompurify (kein jsdom, kein SSR-Dependency-Ballast). dompurify braucht ein echtes window-Objekt zum Sanitizen — das gibt's im Browser automatisch (Auto-Detect beim Import), im SSR-Kontext bleibt sanitize() ein No-Op. Das ist unkritisch, weil Ticket-Inhalte in TicketModal/Standalone-Ticketseite ausschließlich clientseitig per onMount-Fetch geladen werden, also nie während SSR mit echtem Content gerendert werden — src/lib/markdown.ts prüft jetzt `browser` aus $app/environment und sanitized nur dort.\n\nVerifiziert nicht nur mit `npm run build` (Rollup, anderer Codepfad), sondern mit Vite's echtem SSR-Modul-Loader direkt (server.ssrLoadModule('.../TicketModal.svelte')) — genau der Mechanismus, der den ursprünglichen Fehler geworfen hat. Läuft jetzt sauber durch.	2026-07-01 21:40:06.031965
129	279	claude-sonnet-4-6	Implementiert:\n- .forgejo/workflows/release.yml: Trigger bei Tag-Push (vX.Y.Z), baut Dockerfile-Image, published nach codeberg.org/danszek/kbai-ui:<version> und :latest via docker/build-push-action.\n- RELEASING.md: Dokumentiert Tag-Konvention (SemVer) und die Kriterien, nach denen ein KI-Agent eigenständig entscheiden kann, ob ein Release gerechtfertigt ist (sichtbare Änderung seit letztem Tag, sauberer Build, keine offenen kritischen Tickets, konsistente Migrationen). Bei Unsicherheit: Ticket nach human_intervention statt zu raten.\n- README.md: Verweis auf RELEASING.md ergänzt.\n\nWichtig — manueller Schritt nötig, kann ich nicht selbst erledigen: Repo-Secret CODEBERG_TOKEN muss in den Codeberg-Repo-Settings gesetzt werden (Personal-Access-Token mit write:package-Scope). Ohne dieses Secret schlägt der Login-Schritt der Pipeline fehl.\n\nHinweis: Die Autonomie-Passage in RELEASING.md (Agenten dürfen eigenständig taggen/pushen) wurde vor dem Commit explizit vom User bestätigt, da der Permission-Classifier das zunächst blockiert hatte — automatisches Publizieren auf eine externe Registry plus Selbstermächtigung künftiger Agent-Läufe ist eine weitreichende Befugnis.	2026-07-01 21:49:18.511351
130	280	david	danke, das hat funktioniert!	2026-07-01 21:53:15.33769
131	281	claude-sonnet-4-6	Zwei echte Lücken gefixt (Board-Highlighting selbst war schon korrekt, siehe Ticket 268):\n\n1. InboxModal zeigte gar kein Highlight/Reorder — Kommentare an einem Ticket in human_intervention aktualisierten die Liste zwar (Props reaktiv), aber ohne visuellen Hinweis. movedTicketIds jetzt von der Board-Seite durchgereicht, gleicher Glow + animate:flip wie im Kanban-Board.\n\n2. TicketModal und die Standalone-Ticketseite haben sich gar nicht aktualisiert, solange sie offen waren. Kommentiert ein Agent, während ein Mensch das Ticket gerade offen hat, änderte sich nichts bis zum Schließen/Neuöffnen. Board-Seite reicht jetzt ein liveUpdateSignal ans offene TicketModal durch; die Standalone-Seite öffnet eine eigene EventSource. Beide machen einen leisen Refetch (kein Lade-Spinner) mit kurzem "Gerade aktualisiert"-Puls — pausiert während der Nutzer aktiv editiert, um keine ungespeicherten Änderungen zu überschreiben.\n\nWichtiger Hinweis, den ich nicht selbst prüfen kann: Falls das Problem konkret bei reinen Kommentar-/Task-Updates (ohne sonstige Feldänderung) auftrat, könnte auch die V5-Migration (V5__Notify_On_Comments_And_Tasks.sql) fehlen — die legt die pg_notify-Trigger auf ticket_comments/ticket_tasks an. Ohne sie feuert bei reinen Kommentaren gar kein SSE-Event. Bitte prüfen/einspielen falls das Verhalten weiterhin fehlt: docker compose exec -T postgres psql -U kb_user -d kb_ai < V5__Notify_On_Comments_And_Tasks.sql	2026-07-01 21:57:35.438897
132	249	claude-sonnet-4-6	Live-Test: Kommentar ohne Statuswechsel, um zu prüfen ob das Board dafür ein Highlight zeigt (siehe Ticket 281 / Frage vom User).	2026-07-01 22:02:01.965051
133	281	claude-sonnet-4-6	Live-Test #2: schau jetzt aufs Board (Review-Spalte).	2026-07-01 22:02:48.363943
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
-- Data for Name: ticket_relations; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.ticket_relations (id, from_ticket_id, to_ticket_id, relation_type, created_at) FROM stdin;
1	274	275	parent_of	2026-07-01 21:13:28.555803
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
57	221	Client input.odin: W/S/A/D auf Iso-Diagonal-Richtungen remappen (single-key, keine Kombinationen)	t	2026-06-30 18:52:19.840901
58	221	movement_test.odin: Test-Erwartungen an neues Mapping anpassen	t	2026-06-30 18:52:23.6099
59	221	odin check src/client + src/server grün; odin test src/server grün	t	2026-06-30 18:52:27.279359
67	236	draw_player_sprite covers all 8 Direction values directly	f	2026-06-30 22:36:58.663333
68	234	File 234b backlog ticket for diagonal sprite art (done — see #236)	t	2026-06-30 22:37:13.508184
62	232	Up/Down navigates whichever pane currently has focus	t	2026-06-30 22:28:57.103411
63	232	Focused pane visually indicated (UI_CYAN border vs UI_DIM)	t	2026-06-30 22:29:17.896515
64	233	Diagonal move rejected when either axis-aligned intermediate tile is solid	t	2026-06-30 22:29:33.684442
65	233	New unit test covers corner-cut case; existing test_move_diagonal_blocked_by_solid still passes	t	2026-06-30 22:30:09.576192
66	236	Sprite sheet extended with 4 diagonal frames	t	2026-06-30 22:36:54.147115
69	235	Architect/human picks movement-budget-accumulator vs continuous-movement strategy	t	2026-06-30 22:38:36.146671
70	235	Diagonal and cardinal average speed verified equal via test (budget-accumulator) or velocity math (continuous)	t	2026-06-30 22:38:37.213235
60	231	Tester confirms container renders visibly in a live client session post-#229	t	2026-06-30 22:28:40.003859
71	237	list_tickets supports status_id filter + limit/offset pagination	t	2026-06-30 22:47:13.157186
61	232	Add Tab (or Left/Right) key to toggle focus between player-inventory and container panes	t	2026-06-30 22:28:45.266961
81	245	draw_containers uses tile_to_screen instead of raw TILE_SIZE multiply	t	2026-06-30 23:11:30.835466
82	245	architecture/render-pipeline.md documents the fix and flags #247 as outstanding for sibling entities	t	2026-06-30 23:11:32.241763
83	246	Get screenshot or reporter clarification before sizing — blocked, do not guess-implement	f	2026-06-30 23:23:56.754077
79	247	draw_terminals and draw_artifacts use tile_to_screen	t	2026-06-30 23:11:17.707946
80	247	draw_gcs uses tile_to_screen (with or without lerp, per GC movement cadence check)	t	2026-06-30 23:11:19.27412
84	248	Adjacency check uses Chebyshev distance (max(|dx|,|dy|) <= 1)	t	2026-06-30 23:29:09.243102
85	248	New test: diagonal-adjacent deposit/withdraw succeeds	t	2026-06-30 23:29:11.12035
86	250	5 Overlays nutzen UI_* Palette statt lokaler Konstanten	t	2026-06-30 23:33:10.244528
87	250	Modal-Overlays (fragment, escape-menu) nutzen ui_panel	t	2026-06-30 23:33:12.00584
88	250	architecture/ui-system.md Tabelle aktualisiert	t	2026-06-30 23:33:13.671482
89	253	Architect-Design für Server-Line-Tracking + Protokoll-Erweiterung	f	2026-06-30 23:57:26.574157
90	254	Architect-Design für Sektor-Terminal-Assoziation + Client-State	f	2026-06-30 23:57:27.900653
91	31	Snippet-Pool + deterministische Auswahl per Terminal-ID	t	2026-06-30 23:57:35.774869
92	31	draw_terminals zeigt 2-3 Zeilen via tile_to_screen	t	2026-06-30 23:57:37.691599
93	256	Alle 12 fehlenden Message-Types + Snapshot-Felder + ServerState-Felder dokumentiert	t	2026-07-01 00:07:22.443772
94	257	lockstep-Feld + Persistence-Versionshistorie v1-v4 dokumentiert	t	2026-07-01 00:07:24.292337
96	259	Alle 11 TileTypes in world_and_resources.md dokumentiert	t	2026-07-01 00:07:28.175312
100	263	fragment_decrypt-Signatur + Pointer_Key in beiden Docs aktualisiert	t	2026-07-01 00:07:36.595253
101	264	3 fehlende InputContext-Werte in Tabelle + Details ergänzt	t	2026-07-01 00:07:38.09404
97	260	Alle 5 Punkte (Priorität/Worker-Damage/Cold-Zone/Player-Damage/Detection-Radius) korrigiert	t	2026-07-01 00:07:29.878137
98	261	Beide Docs auf aktuelles Inventory-Slot-System umgeschrieben	t	2026-07-01 00:07:31.87741
99	262	direction_delta-Beispiel korrigiert + Open_Cave_Entrance ergänzt	t	2026-07-01 00:07:33.703737
102	265	Doku korrigiert + draw_man_pages_overlay auf UI-System migriert	t	2026-07-01 00:07:40.175096
104	265	draw_man_pages_overlay UI-migriert + Doku korrigiert	t	2026-07-01 14:40:59.843827
105	258	sector_activity_score liefert 0.0=aktiv, 1.0=kalt (ADR-konform)	t	2026-07-01 14:45:02.045699
106	258	echo.odin-Formel angepasst, Spielverhalten unverändert (mehr Echoes in kalten Sektoren)	t	2026-07-01 14:45:04.197584
95	258	Entscheidung getroffen: ADR korrigieren oder Code-Vorzeichen drehen	t	2026-07-01 00:07:26.669952
107	266	5-Datei-Split durchgeführt, Proc-Anzahl vor/nach identisch (45)	t	2026-07-01 14:55:20.837002
108	266	odin check + odin build src/client erfolgreich	t	2026-07-01 14:55:24.050731
103	266	Architect legt Split-Schnitt fest, ggf. Sub-Tickets erstellt	t	2026-07-01 00:07:42.01592
109	3	Add limit/offset params to list_tickets SQL query and tool schema	t	2026-07-01 15:09:17.526068
110	3	Add summary mode (omit description)	t	2026-07-01 15:09:18.950763
72	238	search_tickets(project_id, query) tool exists and returns title matches	t	2026-06-30 22:47:14.636922
73	239	delete_ticket or merge_tickets tool exists and is exposed via MCP	t	2026-06-30 22:47:16.618304
74	240	Error message names the missing env var explicitly when assignee is omitted	t	2026-06-30 22:47:18.821362
76	242	Batch move/assign tool exists for multiple ticket_ids in one call	t	2026-06-30 22:47:22.252027
77	243	created_at/updated_at fields present in ticket and comment responses	t	2026-06-30 22:47:23.421774
78	244	get_ticket_detailed gains include_role_instruction flag, or separate get_role_instructions tool exists	t	2026-06-30 22:47:24.624822
111	241	ticket_relations table with parent_of/blocks/duplicate_of/relates_to types	t	2026-07-01 15:27:26.643904
112	241	link_tickets and unlink_tickets MCP tools	t	2026-07-01 15:27:28.084215
113	241	get_ticket_detailed includes relations array with direction + other_ticket_title	t	2026-07-01 15:27:29.565834
114	252	type column on tickets ('ticket'/'epic'), exposed in all responses	t	2026-07-01 15:27:32.222677
115	252	create_ticket accepts type param; list_tickets accepts type filter	t	2026-07-01 15:27:33.594809
116	255	special_type column on board_statuses	t	2026-07-01 15:27:36.028508
117	255	human_intervention + human_answered auto-created on project creation	t	2026-07-01 15:27:37.811582
118	255	Workflow trigger allows any→human_intervention and human_answered→any	t	2026-07-01 15:27:40.038777
75	241	link_tickets tool + relations array on get_ticket_detailed exist	t	2026-06-30 22:47:20.267457
\.


--
-- Data for Name: tickets; Type: TABLE DATA; Schema: public; Owner: david
--

COPY public.tickets (id, project_id, title, description, status_id, assignee, created_at, updated_at, model, type) FROM stdin;
1	1	Unit- und Integrationstests implementieren	Aktuell 0% Testabdeckung. Ziel: Unit-Tests für alle kanban/-Module (projects, tickets, comments, board_statuses) mit einer echten Test-Datenbank. Integrationstests für den MCP-Protokoll-Layer (JSON-RPC 2.0 initialize → tools/list → tools/call Roundtrip). Framework: cmocka oder criterion.	1	\N	2026-06-24 21:42:40.759076	2026-06-24 21:42:40.759076	\N	ticket
2	1	Statisch gelinkte Binary bauen (nix build .#static)	Nix-Flake um ein statisches Build-Target erweitern. libpq und cjson statisch linken, sodass die Binary ohne installierte Systemabhängigkeiten läuft. Nützlich für Container-Deployments und direkte Distribution.	1	\N	2026-06-24 21:42:43.258418	2026-06-24 21:42:43.258418	\N	ticket
4	1	Connection Pooling (pgBouncer oder interne Pool-Schicht)	Aktuell hält der Server eine einzige persistente libpq-Verbindung. Bei mehreren parallelen Agenten oder Reconnect-Szenarien ist das ein Engpass. Optionen: (a) pgBouncer vorschalten, (b) minimalen internen Pool mit libpq-Verbindungen im Server implementieren.	1	\N	2026-06-24 21:42:52.187047	2026-06-24 21:42:52.187047	\N	ticket
5	1	CI/CD Pipeline auf Codeberg Actions einrichten	Codeberg Actions Workflow anlegen, der bei jedem Push: (1) nix build ausführt, (2) Tests laufen lässt (sobald vorhanden), (3) bei Tags einen Release-Build erzeugt. Nix-Cache zwischen Runs nutzen um Build-Zeiten zu reduzieren.	5	claude-sonnet-4-6	2026-06-24 21:42:54.25993	2026-06-24 22:29:08.501643	claude-sonnet-4-6	ticket
8	2	testen	teterete	7	-	2026-06-25 00:21:39.71538	2026-06-30 12:08:49.954424	\N	ticket
3	1	Pagination für kb.ai_list_tickets	list_tickets gibt aktuell alle Tickets ohne Limit zurück. Bei großen Projekten kann das zu sehr großen MCP-Responses führen, die den Kontext eines LLMs belasten. Lösung: optionale Parameter limit und offset (oder cursor-basiert) im Tool-Schema ergänzen und in der SQL-Query als LIMIT/OFFSET umsetzen.	5	\N	2026-06-24 21:42:45.774268	2026-07-01 15:09:58.568169	\N	ticket
7	1	Ticket-Filterung nach Status in list_tickets	Agenten arbeiten typischerweise nur mit Tickets eines bestimmten Status (z.B. alle "ready"-Tickets holen). list_tickets um optionalen status_id-Filter erweitern, damit Agenten gezielt nur relevante Tickets laden und nicht alles durch den Kontext jagen.	5	\N	2026-06-24 21:42:59.120605	2026-07-01 15:09:59.944677	\N	ticket
9	3	E2E-Test: Modal-Interaktionen verifizieren	Alle drei Modals im Live-Browser testen: Ticket-Klick → TicketModal, Statuses-Button → StatusesModal, Workflow-Button → WorkflowModal. Sonderfälle: Escape-Taste schließt Modal, Backdrop-Klick schließt Modal, nach Schließen von StatusesModal werden Statuses + Tickets neu geladen.	8	\N	2026-06-25 08:52:14.056868	2026-06-25 08:52:14.056868	\N	ticket
10	3	E2E-Test: Drag-and-Drop mit Workflow-Validierung	Testen ob das Board beim Ziehen eines Tickets in einen nicht erlaubten Status korrekt reagiert: API gibt 409 zurück, Karte springt optimistisch zurück, Fehlermeldung erscheint. Auch valide Transitionen testen und sicherstellen dass bind:tickets die reaktive Aktualisierung korrekt durchreicht.	8	\N	2026-06-25 08:52:21.023606	2026-06-25 08:52:21.023606	\N	ticket
11	3	E2E-Test: Workflow-Transition löschen via UI	In WorkflowModal die Transition-Liste unterhalb des Graphen prüfen: Trash-Button löscht Transition per DELETE-API, Edge verschwindet aus dem Graphen, Listeneintrag entfernt sich. Auch Keyboard-Shortcut (Delete/Backspace auf selektierter Edge) testen.	8	\N	2026-06-25 08:52:30.705301	2026-06-25 08:52:30.705301	\N	ticket
12	3	E2E-Test: Ticket anlegen und bearbeiten	Neues Ticket ohne Assignee anlegen (darf nicht fehlschlagen). Danach Ticket per Klick im Board öffnen → TicketModal. Bearbeiten-Button im Modal aktiviert Inline-Edit-Formular (kein Seitennavigation). Änderungen speichern und im Board prüfen.	8	\N	2026-06-25 08:54:02.545842	2026-06-25 08:54:02.545842	\N	ticket
13	3	Spalten-Edit-Icon öffnet StatusesModal statt Seite	Das Bleistift-Icon im Spalten-Header und der "Neue Spalte"-Button rufen jetzt onOpenStatuses() auf statt goto(). Wurde implementiert, muss noch live getestet werden: Klick öffnet StatusesModal, Modal schließen aktualisiert das Board.	8	\N	2026-06-25 08:54:08.01524	2026-06-25 08:54:08.01524	\N	ticket
14	3	Ticket löschen aus TicketModal	Nach dem Löschen eines Tickets im TicketModal soll die Karte sofort aus dem Board verschwinden. onDeleted-Callback filtert tickets-Array im Board. Prüfen ob der reaktive Update korrekt funktioniert und das Modal sich schließt.	8	\N	2026-06-25 08:54:22.899503	2026-06-25 08:54:22.899503	\N	ticket
15	3	Ticket-Status-Änderung im Modal ans Board zurückmelden	Wenn ein Ticket im TicketModal in einen anderen Status verschoben wird, soll die Karte im Board direkt in die richtige Spalte wandern ohne Seiten-Reload. Aktuell wird status_id nach dem Speichern nicht an den Board-State zurückgemeldet — onUpdated-Callback oder Store-Aktualisierung nötig.	8	\N	2026-06-25 08:54:30.877712	2026-06-25 08:54:30.877712	\N	ticket
16	3	Drag-and-Drop auf Touch-Geräten prüfen	HTML5 drag-and-drop funktioniert auf Mobile-Browsern nicht. Evaluieren ob pointer-events-basiertes DnD (z.B. mit svelte-dnd-action) benötigt wird oder ob Mobile vorerst out-of-scope ist.	8	\N	2026-06-25 08:54:36.83538	2026-06-25 08:54:36.83538	\N	ticket
17	3	Status-Sortierung per Drag-and-Drop	In StatusesModal und der Statuses-Standalone-Seite gibt es noch keinen Drag-and-Drop-Mechanismus zur Umsortierung. Aktuell nur manuell über das Position-Feld. Fußnote "Drag & Drop-Sortierung folgt" ist bereits im UI.	8	\N	2026-06-25 08:54:42.380521	2026-06-25 08:54:42.380521	\N	ticket
18	3	Leere Board-Spalten: Höhe bei langen Ticket-Listen angleichen	KanbanColumn hat min-h-[160px] auf der Drop-Zone. Bei sehr langen Spalten können benachbarte leere Spalten visuell klein wirken. Prüfen ob flex-grow oder eine dynamische Mindesthöhe sinnvoller ist.	8	\N	2026-06-25 08:54:56.989031	2026-06-25 08:54:56.989031	\N	ticket
19	3	Kommentar-Funktion im TicketModal	Das Backend hat bereits eine /api/tickets/:id/comments Endpoint-Struktur. Im TicketModal fehlt noch ein Kommentar-Abschnitt: Liste bestehender Kommentare + Eingabefeld für neue. Design analog zu den bestehenden Inline-Edit-Bereichen.	8	\N	2026-06-25 08:55:02.47213	2026-06-25 08:55:02.47213	\N	ticket
20	3	Task-Checkliste im TicketModal	Tickets haben Tasks (Subtasks) im Backend. Im TicketModal ist bereits ein Fortschrittsbalken angedeutet, aber keine interaktive Task-Liste. Checkboxen zum Abhaken, neue Tasks hinzufügen, Tasks löschen.	8	\N	2026-06-25 08:55:06.906555	2026-06-25 08:55:06.906555	\N	ticket
21	3	Keyboard-Navigation und Accessibility-Grundlagen	Modals sind per Escape schließbar, aber focus-trap fehlt noch (Tab wandert durch den Hintergrund). Aria-Attribute für Modal (role=dialog, aria-modal, aria-labelledby) und Kanban-Karten (role=article) ergänzen. Focus-Rückgabe beim Schließen des Modals.	8	\N	2026-06-25 08:55:11.296416	2026-06-25 08:55:11.296416	\N	ticket
22	4	097: Codeberg-Pages — Doku- und WASM-Hosting	**Datei:** project/backlog/097-codeberg-pages-docs-wasm.md\n**Aufwand:** M | **Status:** Deferred (2026-06-23: Mensch priorisiert anders)\n\nStatisches Hosting via Codeberg Pages:\n1. Spielbare WASM-Demo (client.wasm existiert bereits)\n2. Projekt-Doku (docs/, adr/, project/)\n3. Release-Galerie (Windows-EXE-Downloads)\n\n**Deferred weil:**\n- Pages-Setup hat Repo-Politik-Implikationen (pages-Branch vs Sub-Repo)\n- WASM-Client braucht WebSocket-Layer (heute nur TCP)\n- Doku-Toolchain (mkdocs/zola/mdBook) nicht entschieden\n\n**Sub-Themen:** 097a Pages-Setup, 097b Doku-Toolchain, 097c WASM-Client, 097d Release-Galerie\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Mensch (2026-06-23): auf Eis gelegt\n- [ ] Reaktivieren wenn Kapazität oder strategischer Bedarf entsteht	13	\N	2026-06-25 09:23:51.893011	2026-06-25 09:23:51.893011	\N	ticket
23	4	105: Sprite Sheet V2.0 Expansion — 10 neue Entities	**Datei:** project/backlog/105-sprite-sheet-v2-expansion.md\n**Aufwand:** XL | **Refs:** docs/concept/visual/konzept-sprites-v2-expansion.png, crops-v2/\n\n10 neue Entities aus V2-Sheet:\n- Debugger Bot, Buffer Bot, Ping Bot (Worker-Varianten)\n- Firewall Segment, Antivirus Turret (Defense)\n- Power Cell (Resource/Crafting)\n- Logic Bomb (Placeable Trap)\n- Corrupted Data, Data Leak (Hazard Tiles)\n- Network Worm (Mobile Enemy)\n\n**Verschränkungen:** 098 (Power Cell für Turrets), 099 (Item-System), 100 (Cave-Loot), 104 (V1-Sprites zuerst)\n**Bedingung:** Erst sinnvoll wenn 099 (Item-System) steht.\n\n**Offene Fragen:** Tier-Ordnung, Content-Waves, Balancing Tier-2 vs Tier-4\n\n**Agent Log:**\n- [x] Story Writer Capture (2026-06-23)\n- [ ] Requirement Manager: Mensch-Workshop\n- [ ] Architect: Phasen-Plan (welche Entity wann)\n- [ ] Sub-Stories 105a-j	13	\N	2026-06-25 09:23:59.6448	2026-06-25 09:23:59.6448	\N	ticket
24	4	006: Base Building — GC-Kollisions-Barrieren	**Datei:** project/planning/006-base-building-protection.md\n**Aufwand:** L | **Refs:** src/server/spawn.odin, src/shared/world.odin\n\nSpieler können Wände/Barriers bauen → GCs können physisch nicht betreten (Kollisions-Barriere, kein Damage-System).\n\n**Akzeptanzkriterien:**\n- Neuer Tile-Typ `.Wall` definiert\n- Building-System: Spieler platziert Walls auf Empty-Tiles\n- Resource-Kosten (Bytes/CPU/RAM)\n- GC-Pathfinding respektiert Walls als blockiert\n- Walls für Spieler+Worker begehbar\n\n**Offene Fragen (für Architect):**\n- Wand-Zerstörbarkeit? Tor-Mechanik? Baureichweite?\n- Workers durch Walls? Performance bei tausenden Walls?\n- Abgrenzung zu 084 (CA-Walls: Procedural-Gen)\n\n**Sub-Stories:** 006a Tile-Typ, 006b Building-UI/Input, 006c GC-Pathfinding, 006d Resource-Kosten\n\n**Agent Log:**\n- [x] Story Writer\n- [x] Mensch (2026-06-23): Kollisions-Barriere gewählt\n- [ ] Architect: ADR/Plan + Sub-Tickets	14	\N	2026-06-25 09:24:07.57865	2026-06-25 09:24:07.57865	\N	ticket
25	4	027: Cross-Compilation — arm64 Server + AppImage Client	**Datei:** project/planning/027-cross-compilation.md\n**Priorität:** Niedrig\n\nKernel Panic für mehrere Zielarchitekturen ohne Cross-Host kompilieren.\n\n**Teilziele:**\n- 027a: Server statisch + arm64 (pkgsCross.aarch64-multiplatform, musl-Toolchain)\n- 027b: Client als AppImage x86_64 (nix bundle --bundler toAppImage)\n- 027c: Client arm64 (optional, hoher Aufwand — Raylib Cross-Compile)\n\n**Akzeptanzkriterien:**\n- `nix build .#packages.aarch64-linux.server` auf x86_64 funktioniert\n- Server-Binary läuft auf Ubuntu arm64 ohne Deps\n- AppImage läuft auf Ubuntu 22.04 LTS ohne Installation\n- CI baut beide Artefakte automatisch\n\n**Risiken:** Odin-Vendor-Raylib kompiliert intern C-Code (Cross-Toolchain nötig), Nix sandbox + allowUnsupportedSystem\n\n**Agent Log:**\n- [ ] Architect: Nix-Flake-Plan + ADR	14	\N	2026-06-25 09:24:16.057414	2026-06-25 09:24:16.057414	\N	ticket
26	4	028: CI — Forgejo Actions Runner (selbst gehostet)	**Datei:** project/planning/028-ci-forgejo-runner.md\n**Priorität:** Niedrig\n\nAutomatischen Build bei jedem Push auf master via eigenem Forgejo-Runner.\n\n**Architektur:** Codeberg ←HTTPS-polling→ eigener Runner (VPS/Heimserver), kein eingehender Port nötig.\n\n**Teilziele:**\n- 028a: Runner aufsetzen (NixOS services.gitea-actions-runner, Label `nix`)\n- 028b: .forgejo/workflows/build.yml (nix build .#server + .#client)\n- 028c: Flake-Outputs (packages.x86_64-linux.server/client/default)\n- 028d: Nix Binary Cache (optional, Cachix oder nix-serve)\n\n**Akzeptanzkriterien:**\n- Push triggert automatisch Build\n- Build-Status in Codeberg-UI sichtbar\n- Fehlerhafter Build wird als Fehler markiert\n- Runner als systemd-Service\n\n**Agent Log:**\n- [ ] Architect: Flake-Output-Plan + Workflow-Datei	14	\N	2026-06-25 09:24:22.042186	2026-06-25 09:24:22.042186	\N	ticket
70	4	015b: World Snapshot Broadcast	Datei: project/done/015b-world-snapshot-broadcast.md	17	\N	2026-06-25 09:28:28.290953	2026-06-25 09:28:28.290953	\N	ticket
71	4	015c: TCP Client and State Sync	Datei: project/done/015c-tcp-client-and-state-sync.md	17	\N	2026-06-25 09:28:29.602029	2026-06-25 09:28:29.602029	\N	ticket
72	4	015d: Simulation Tick Loop	Datei: project/done/015d-simulation-tick-loop.md	17	\N	2026-06-25 09:28:31.954913	2026-06-25 09:28:31.954913	\N	ticket
73	4	016: Codebase Restructure	Datei: project/done/016-codebase-restructure.md	17	\N	2026-06-25 09:28:33.344426	2026-06-25 09:28:33.344426	\N	ticket
74	4	017: Client Window Bugs [Roof]	Datei: project/done/017-client-window-bugs.md	17	\N	2026-06-25 09:28:34.800354	2026-06-25 09:28:34.800354	\N	ticket
28	4	100: Orphaned Memory Caves — verwaiste Speicherregionen	**Datei:** project/planning/100-orphaned-memory-caves.md\n**Aufwand:** L | **Refs:** 084 (CA-Caves), 099 (Items)\n\nDiskrete Regionen mit eigenem Loot, klar abgegrenzt — "Orphaned memory"-Speicherbereiche eines abgestürzten Prozesses.\n\n**Mensch-Entscheidungen (2026-06-24):**\n- Eintritt: **Item/Schlüssel nötig** ("Pointer Key"), Eingang = verschlossenes Tile\n- Loot-Dichte: **3–5× Ressourcendichte** vs. normale Welt\n- Visuell abhebend: eigenes Farbschema / Substrate-Variante\n\n**Abgrenzung zu 084:** 084 = organische Höhlenwände im normalen Worldgen (frei betretbar). 100 = diskrete Regionen mit Eintritts-Bedingung.\n\n**Sub-Stories:** 100a Worldgen-Platzierung, 100b Eintritts-Mechanik (Schlüssel), 100c Loot-Tabellen, 100d Visual-Differentiation\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Requirement Manager (2026-06-24): Mensch-Entscheidungen getroffen\n- [ ] Architect: Worldgen-Pass + Schlüssel-Item + Loot-Tabellen + Visual\n- [ ] Developer-Sub-Tickets	14	\N	2026-06-25 09:24:40.428204	2026-06-25 09:24:40.428204	\N	ticket
29	4	104: Sprite Sheet V1.0 Integration	**Datei:** project/planning/104-sprite-sheet-v1-integration.md\n**Aufwand:** L | **Refs:** docs/concept/visual/konzept-sprites-v1.png, crops-v1/\n\nV1-Sprite-Sheet enthält: Player (front/back/carry), Crystal Blue, Data Cube Green, Crate, Terminal Tower, Server Racks ×3, Hacker Portrait, Red Spider Boss.\n\n**Sub-Decomposition:**\n- 104a: Player-Sprites V1 (3 Frames + carry-Frame)\n- 104b: Resource Sprites V1 (crystal + cube + crate → draw_resource_icon)\n- 104c: Terminal Entity Sprite (unblockiert 098)\n- 104d: Server-Rack Sprite (Funktion noch zu klären — Mensch-Entscheidung nötig)\n- 104e: Hacker-Portrait (Login-Screen-Avatar + HUD)\n- 104f: Boss-Spider-Sprite (Verbunden mit 103e Boss-Encounter)\n\n**Nächster Schritt:** Sprites auf Game-Res bringen (human_tasks.md), dann Sub-Tickets nach todo/\n\n**Agent Log:**\n- [x] Story Writer (2026-06-23)\n- [x] Architect: Decomposition 104a-f\n- [ ] Sprite-Polish in Game-Res (human task)\n- [ ] Sub-Tickets nach todo/ wenn Sprites vorliegen	14	\N	2026-06-25 09:24:48.968764	2026-06-25 09:24:48.968764	\N	ticket
30	4	106: Isometrische Weltdarstellung — Engine-Rewrite	**Datei:** project/planning/106-isometric-world-layout.md\n**Aufwand:** XL (Engine-Refactor) | **Refs:** konzept-world-empty.png, konzept-world-populated.png\n\n**Mensch-Entscheidung (2026-06-23): Voll-Isometrie** — wie im Konzept-Bild.\n\nKonzept zeigt: Isometrische Perspektive, Multi-Level-Plattformen, Zentrale Hub-Plattform (Kernel/Spawn), Cyan-glühende Connection-Lines zwischen Plattformen, Matrix-Code-Hintergrund.\n\n**Was sich ändert:**\n- Render-Pipeline: axial-aligned Rects → Diamond-Tiles + Z-Sorting\n- Worldgen: Plattform-Topologie statt freie Tiles\n- Pathfinding: Worker auf Connection-Lines\n- Camera: Coordinate-Math isometrisch\n- Sprites: alle V1/V2 müssen isometrisch ausgerichtet sein\n- Server-Protokoll: Tile-Coords (x,y) bleiben, Render-Translation Client-seitig\n\n**Nächste Schritte (Architect):**\n1. ADR: Isometrie-Entscheidung + Konsequenzen\n2. Engine-Plan (Coordinate-Math, Tile-Geometry 2:1, Z-Sorting)\n3. Worldgen-Plan (Plattform-Topologie, AoI-Komplexität)\n4. Sub-Tickets für Developer\n\n**Agent Log:**\n- [x] Mensch: Voll-Isometrie gewählt\n- [ ] Architect: ADR + Engine-Rewrite-Plan	14	\N	2026-06-25 09:24:57.878667	2026-06-25 09:24:57.878667	\N	ticket
33	4	113a: Network Optimization — Delta-Kompression + RLE	**Datei:** project/planning/113a-network-optimization.md\n\nReduzierung der Netzwerk-Payload-Größe ohne Qualitätsverlust.\n\n**Techniken:**\n- **Delta-Kompression:** MSG_STATE_DELTA (0x11) existiert — nur geänderte Entities/Tiles senden (diff zum letzten Client-Acknowledge)\n- **Bitmasken für Entitäten-Updates:** Vorgeschaltetes Byte signalisiert welche Felder folgen (nur X/Y statt ganzen Worker-State)\n- **RLE für Terrain/Tiles:** `[Anzahl][TileType][Integrity]` für zusammenhängende gleiche Tiles\n- **Striktes Bit-Packing:** is_active, is_online, facing (2 Bit) in Flag-Bytes zusammenfassen\n\n**Success Criteria:**\n- [ ] Konzept für Delta-Updates in protocol.odin spezifiziert (Architect-Ticket)\n- [ ] RLE und Bit-Packing implementiert und getestet\n\n**Agent Log:**\n- [x] Requirement Manager: Ticket aus 113 aufgeteilt, nach planning/ verschoben\n- [ ] Architect: Spec + ADR	14	\N	2026-06-25 09:25:23.613286	2026-06-25 09:25:23.613286	\N	ticket
75	4	017a: Window Resizable and Camera Fix	Datei: project/done/017a-window-resizable-and-camera-fix.md	17	\N	2026-06-25 09:28:40.324944	2026-06-25 09:28:40.324944	\N	ticket
76	4	018: Escape Menu [Roof]	Datei: project/done/018-escape-menu.md	17	\N	2026-06-25 09:28:41.753512	2026-06-25 09:28:41.753512	\N	ticket
77	4	018a: Escape Menu New File	Datei: project/done/018a-escape-menu-new-file.md	17	\N	2026-06-25 09:28:43.322509	2026-06-25 09:28:43.322509	\N	ticket
32	4	110: Visual Polish — Remaining Items	**Datei:** project/planning/110-visual-polish-remaining.md\n**Aufwand:** M | **Refs:** 029 (Visual Overhaul v2), src/client/render.odin\n\nVerbleibende Render-Punkte aus 029. Alles Client-seitig, keine Server-Änderungen.\n\n**Offene Punkte:**\n- 110b: Zone Flicker/Pulse (Hot Zone amber 3Hz, Kernel-Layer magenta edge)\n- 110c: Additional Empty-Tile Variants (scratch pattern, shimmer, fragment-floor `<>`)\n- 110d: GC Dread-Field Halo (wider secondary halo, L1 pulsing ring, L2 full arc)\n- 110f: GC Sweep Visuals (L1 1.5× tile, L2 concentric rings)\n- 110g: HUD Terminal Integration (dark-green bg, 1px border, color-coded dots, dynamic status)\n\n**Bereits done:** 110a (Resource Colors + Glow), 110e (Worker Idle/Active Glyph)\n\n**Agent Log:**\n- [x] 2026-06-23: aus 029 extrahiert\n- [x] Requirement Manager (2026-06-24): 110a + 110e priorisiert, Rest zurückgestellt\n- [x] 110a + 110e: abgeschlossen (110ae-resource-colors-worker-glyph.md)\n- [ ] Architect: Sub-Tickets für 110b/c/d/f/g	17	\N	2026-06-25 09:25:14.672162	2026-06-30 10:30:53.959217	\N	ticket
38	4	001a: Window Init	Datei: project/done/001a-window-init.md	17	\N	2026-06-25 09:27:21.758093	2026-06-25 09:27:21.758093	\N	ticket
39	4	001b: Camera System	Datei: project/done/001b-camera-system.md	17	\N	2026-06-25 09:27:24.462477	2026-06-25 09:27:24.462477	\N	ticket
40	4	002a: Grid Data Structures	Datei: project/done/002a-grid-data-structures.md	17	\N	2026-06-25 09:27:25.978636	2026-06-25 09:27:25.978636	\N	ticket
41	4	003a: Lua Sandbox Setup	Datei: project/done/003a-lua-sandbox-setup.md	17	\N	2026-06-25 09:27:27.794874	2026-06-25 09:27:27.794874	\N	ticket
42	4	003b: Worker API Wrapper	Datei: project/done/003b-worker-api-wrapper.md	17	\N	2026-06-25 09:27:29.397848	2026-06-25 09:27:29.397848	\N	ticket
43	4	004: Garbage Collector System	Datei: project/done/004-garbage-collector-system.md	17	\N	2026-06-25 09:27:31.310199	2026-06-25 09:27:31.310199	\N	ticket
44	4	005: Header Fragment Mechanic [Roof]	Datei: project/done/005-header-fragment-mechanic.md	17	\N	2026-06-25 09:27:32.660393	2026-06-25 09:27:32.660393	\N	ticket
45	4	005a: Header Fragment Data Model	Datei: project/done/005a-header-fragment-data-model.md	17	\N	2026-06-25 09:27:35.618292	2026-06-25 09:27:35.618292	\N	ticket
46	4	005b: Decryption and Dependency Resolution	Datei: project/done/005b-decryption-and-dependency-resolution.md	17	\N	2026-06-25 09:27:37.860434	2026-06-25 09:27:37.860434	\N	ticket
47	4	005b: Header Fragments Revised	Datei: project/done/005b-header-fragments-revised.md	17	\N	2026-06-25 09:27:39.929818	2026-06-25 09:27:39.929818	\N	ticket
48	4	005c: Lua API Injection	Datei: project/done/005c-lua-api-injection.md	17	\N	2026-06-25 09:27:41.532059	2026-06-25 09:27:41.532059	\N	ticket
49	4	007: Entropy Tracker [Roof]	Datei: project/done/007-entropy-tracker.md	17	\N	2026-06-25 09:27:42.915645	2026-06-25 09:27:42.915645	\N	ticket
50	4	007a: Entropy Grid Structure	Datei: project/done/007a-entropy-grid-structure.md	17	\N	2026-06-25 09:27:44.447462	2026-06-25 09:27:44.447462	\N	ticket
51	4	007b: Action Hook and Safety Valve	Datei: project/done/007b-action-hook-and-safety-valve.md	17	\N	2026-06-25 09:27:45.761047	2026-06-25 09:27:45.761047	\N	ticket
52	4	007c: Decay and Diffusion	Datei: project/done/007c-decay-and-diffusion.md	17	\N	2026-06-25 09:27:47.358583	2026-06-25 09:27:47.358583	\N	ticket
53	4	008: Bit Rot Engine [Roof]	Datei: project/done/008-bit-rot-engine.md	17	\N	2026-06-25 09:27:49.265606	2026-06-25 09:27:49.265606	\N	ticket
54	4	008a: Worker Script and Corruption Flags	Datei: project/done/008a-worker-script-and-corruption-flags.md	17	\N	2026-06-25 09:27:55.844482	2026-06-25 09:27:55.844482	\N	ticket
55	4	008b: Parameter Drift and Instruction Swap	Datei: project/done/008b-parameter-drift-and-instruction-swap.md	17	\N	2026-06-25 09:27:57.381861	2026-06-25 09:27:57.381861	\N	ticket
56	4	008c: Logic Hijack and ECC	Datei: project/done/008c-logic-hijack-and-ecc.md	17	\N	2026-06-25 09:27:58.802326	2026-06-25 09:27:58.802326	\N	ticket
57	4	009: Player Entity and Movement [Roof]	Datei: project/done/009-player-entity-and-movement.md	17	\N	2026-06-25 09:28:00.142089	2026-06-25 09:28:00.142089	\N	ticket
58	4	009a: Server Movement Validation	Datei: project/done/009a-server-movement-validation.md	17	\N	2026-06-25 09:28:01.344794	2026-06-25 09:28:01.344794	\N	ticket
59	4	009b: Client Input Handler	Datei: project/done/009b-client-input-handler.md	17	\N	2026-06-25 09:28:03.232928	2026-06-25 09:28:03.232928	\N	ticket
60	4	009c: Client Player Render	Datei: project/done/009c-client-player-render.md	17	\N	2026-06-25 09:28:05.423667	2026-06-25 09:28:05.423667	\N	ticket
61	4	010: Resource Pickup and Inventory [Roof]	Datei: project/done/010-resource-pickup-and-inventory.md	17	\N	2026-06-25 09:28:06.923466	2026-06-25 09:28:06.923466	\N	ticket
62	4	010a: Server Interact Handler	Datei: project/done/010a-server-interact-handler.md	17	\N	2026-06-25 09:28:09.320615	2026-06-25 09:28:09.320615	\N	ticket
63	4	010b: Client Resource HUD	Datei: project/done/010b-client-resource-hud.md	17	\N	2026-06-25 09:28:13.02786	2026-06-25 09:28:13.02786	\N	ticket
64	4	011: Worker Crafting and Placement	Datei: project/done/011-worker-crafting-and-placement.md	17	\N	2026-06-25 09:28:14.709041	2026-06-25 09:28:14.709041	\N	ticket
65	4	012: Worker Script Editor [Roof]	Datei: project/done/012-worker-script-editor.md	17	\N	2026-06-25 09:28:16.248407	2026-06-25 09:28:16.248407	\N	ticket
66	4	012b: Lua Worker Execution	Datei: project/done/012b-lua-worker-execution.md	17	\N	2026-06-25 09:28:17.754436	2026-06-25 09:28:17.754436	\N	ticket
67	4	014: L1 Scrubber	Datei: project/done/014-l1-scrubber.md	17	\N	2026-06-25 09:28:19.024288	2026-06-25 09:28:19.024288	\N	ticket
68	4	015: Multiplayer Foundation [Roof]	Datei: project/done/015-multiplayer-foundation.md	17	\N	2026-06-25 09:28:20.488482	2026-06-25 09:28:20.488482	\N	ticket
69	4	015a: TCP Server and Connections	Datei: project/done/015a-tcp-server-and-connections.md	17	\N	2026-06-25 09:28:21.698232	2026-06-25 09:28:21.698232	\N	ticket
37	4	103: Visual Direction — Sprites an Banner anlehnen [Roof]	**Datei:** project/in-progress/103-visual-direction-banner-alignment.md\n**Aufwand:** L | **Refs:** assets/branding/banner.png, 051a, 069, 102\n\nRoof-Story: Visual-Target = banner.png (Hooded-Avatar, Spider-Boss, Skull-Cluster, CRT-Code-Snippet, HUD-Elemente).\n\n**Ausdekomponiert in:**\n- 103a → 104a: Player-Sprites (EXPLIZIT AUSGESCHLOSSEN — Mensch-Entscheidung)\n- 103b → 105: V2-Expansion-Entities\n- 103c → 104f: Boss-Spider-Sprite\n- 103d → 106: Isometrische Weltdarstellung\n- 103e → 107: HUD-Polish (Entropy + Threat-Level)\n- 103f: Beleuchtung — noch offen\n\n**Completed Sub-Tickets:**\n- [x] 103gc: GC-Tier-Visualisierung (Skull/Spider/Boss-Forms) — 2026-06-24\n\n**Player-Sprite:** Explizit NICHT Teil dieses Tickets (Mensch-Entscheidung 2026-06-24).\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Requirement Manager (2026-06-24): Player-Sprite ausgeschlossen, GC-Tier als nächster Punkt\n- [x] 103gc: abgeschlossen (2026-06-24)\n- [ ] Weitere Sub-Tickets (Beleuchtung 103f, weitere Visual-Elemente)	16	claude-sonnet-4-6	2026-06-25 09:25:57.250792	2026-06-30 10:36:32.782076	\N	ticket
78	4	018b: Escape Menu Integration	Datei: project/done/018b-escape-menu-integration.md	17	\N	2026-06-25 09:28:44.95093	2026-06-25 09:28:44.95093	\N	ticket
79	4	019: Visual Style Overhaul [Roof]	Datei: project/done/019-visual-style-overhaul.md	17	\N	2026-06-25 09:28:47.093118	2026-06-25 09:28:47.093118	\N	ticket
80	4	019a: Draw Reorganization	Datei: project/done/019a-draw-reorganization.md	17	\N	2026-06-25 09:28:49.157966	2026-06-25 09:28:49.157966	\N	ticket
81	4	019b: World Rendering	Datei: project/done/019b-world-rendering.md	17	\N	2026-06-25 09:28:50.855889	2026-06-25 09:28:50.855889	\N	ticket
82	4	019c: Entity Rendering	Datei: project/done/019c-entity-rendering.md	17	\N	2026-06-25 09:28:52.304787	2026-06-25 09:28:52.304787	\N	ticket
83	4	019d: Effect System	Datei: project/done/019d-effect-system.md	17	\N	2026-06-25 09:28:53.761564	2026-06-25 09:28:53.761564	\N	ticket
84	4	020: Smooth Player Movement [Roof]	Datei: project/done/020-smooth-player-movement.md	17	\N	2026-06-25 09:29:00.564696	2026-06-25 09:29:00.564696	\N	ticket
85	4	020a: Smooth Player Movement (impl)	Datei: project/done/020a-smooth-player-movement.md	17	\N	2026-06-25 09:29:02.482946	2026-06-25 09:29:02.482946	\N	ticket
86	4	021a: Zone Overlay Blending	Datei: project/done/021a-zone-overlay-blending.md	17	\N	2026-06-25 09:29:07.485766	2026-06-25 09:29:07.485766	\N	ticket
87	4	021b: Resource Tile Glow	Datei: project/done/021b-resource-tile-glow.md	17	\N	2026-06-25 09:29:09.043926	2026-06-25 09:29:09.043926	\N	ticket
88	4	021c: GC Visual Tier	Datei: project/done/021c-gc-visual-tier.md	17	\N	2026-06-25 09:29:11.756718	2026-06-25 09:29:11.756718	\N	ticket
89	4	021d: Worker Activation Flicker	Datei: project/done/021d-worker-activation-flicker.md	17	\N	2026-06-25 09:29:13.029976	2026-06-25 09:29:13.029976	\N	ticket
90	4	021e: Bitrot Particle Scatter	Datei: project/done/021e-bitrot-particle-scatter.md	17	\N	2026-06-25 09:29:15.239898	2026-06-25 09:29:15.239898	\N	ticket
91	4	022a: Protocol Struct Changes	Datei: project/done/022a-protocol-struct-changes.md	17	\N	2026-06-25 09:29:20.799707	2026-06-25 09:29:20.799707	\N	ticket
92	4	022b: Server Craft Place Pickup	Datei: project/done/022b-server-craft-place-pickup.md	17	\N	2026-06-25 09:29:22.179185	2026-06-25 09:29:22.179185	\N	ticket
93	4	022c: Client Inventory UI	Datei: project/done/022c-client-inventory-ui.md	17	\N	2026-06-25 09:29:23.579473	2026-06-25 09:29:23.579473	\N	ticket
94	4	022d: Client Input Disambiguation	Datei: project/done/022d-client-input-disambiguation.md	17	\N	2026-06-25 09:29:24.824555	2026-06-25 09:29:24.824555	\N	ticket
95	4	023a: Script Request Response Protocol	Datei: project/done/023a-script-request-response-protocol.md	17	\N	2026-06-25 09:29:26.585831	2026-06-25 09:29:26.585831	\N	ticket
96	4	023b: Script Editor UI	Datei: project/done/023b-script-editor-ui.md	17	\N	2026-06-25 09:29:28.180571	2026-06-25 09:29:28.180571	\N	ticket
97	4	023c: Script Editor Input Save	Datei: project/done/023c-script-editor-input-save.md	17	\N	2026-06-25 09:29:29.413991	2026-06-25 09:29:29.413991	\N	ticket
98	4	024a: GC AI Advance	Datei: project/done/024a-gc-ai-advance.md	17	\N	2026-06-25 09:29:30.510896	2026-06-25 09:29:30.510896	\N	ticket
99	4	024b: GC Spawn System	Datei: project/done/024b-gc-spawn-system.md	17	\N	2026-06-25 09:29:31.939644	2026-06-25 09:29:31.939644	\N	ticket
100	4	025: Worker Resource Collection [Roof]	Datei: project/done/025-worker-resource-collection.md	17	\N	2026-06-25 09:29:33.467381	2026-06-25 09:29:33.467381	\N	ticket
101	4	025a: Worker Collect (Server)	Datei: project/done/025a-worker-collect-server.md	17	\N	2026-06-25 09:29:34.829132	2026-06-25 09:29:34.829132	\N	ticket
102	4	025b: Worker Unload Mechanic	Datei: project/done/025b-worker-unload-mechanic.md	17	\N	2026-06-25 09:29:40.262127	2026-06-25 09:29:40.262127	\N	ticket
103	4	026: Keybind Routing Architecture	Datei: project/done/026-keybind-routing-architecture.md	17	\N	2026-06-25 09:29:41.449359	2026-06-25 09:29:41.449359	\N	ticket
104	4	029: Visual Overhaul V2 [Roof]	Datei: project/done/029-visual-overhaul-v2.md	17	\N	2026-06-25 09:29:42.650584	2026-06-25 09:29:42.650584	\N	ticket
105	4	029a: Resource Tile Richness	Datei: project/done/029a-resource-tile-richness.md	17	\N	2026-06-25 09:29:46.461358	2026-06-25 09:29:46.461358	\N	ticket
106	4	029b: HUD Terminal Integration	Datei: project/done/029b-hud-terminal-integration.md	17	\N	2026-06-25 09:29:47.777184	2026-06-25 09:29:47.777184	\N	ticket
107	4	030: Spawn System Overhaul [Roof]	Datei: project/done/030-spawn-system-overhaul.md	17	\N	2026-06-25 09:29:49.312402	2026-06-25 09:29:49.312402	\N	ticket
108	4	030a: Spawn Config and Player Spawn	Datei: project/done/030a-spawn-config-and-player-spawn.md	17	\N	2026-06-25 09:29:50.752184	2026-06-25 09:29:50.752184	\N	ticket
109	4	030b: Resource Respawn	Datei: project/done/030b-resource-respawn.md	17	\N	2026-06-25 09:29:52.068965	2026-06-25 09:29:52.068965	\N	ticket
110	4	030c: Player-Relative GC Spawn	Datei: project/done/030c-player-relative-gc-spawn.md	17	\N	2026-06-25 09:29:53.464949	2026-06-25 09:29:53.464949	\N	ticket
111	4	030d: Entropy GC Spawn Rate	Datei: project/done/030d-entropy-gc-spawn-rate.md	17	\N	2026-06-25 09:29:54.781352	2026-06-25 09:29:54.781352	\N	ticket
112	4	030e: Reconnect Persistence	Datei: project/done/030e-reconnect-persistence.md	17	\N	2026-06-25 09:29:56.079437	2026-06-25 09:29:56.079437	\N	ticket
113	4	031: Sector Tracking [Roof]	Datei: project/done/031-sector-tracking.md	17	\N	2026-06-25 09:29:57.339398	2026-06-25 09:29:57.339398	\N	ticket
114	4	031a: Sector Tracking Implementation	Datei: project/done/031a-sector-tracking-impl.md	17	\N	2026-06-25 09:29:58.460586	2026-06-25 09:29:58.460586	\N	ticket
115	4	032: Server Persistence [Roof]	Datei: project/done/032-server-persistence.md	17	\N	2026-06-25 09:29:59.595847	2026-06-25 09:29:59.595847	\N	ticket
116	4	032a: Persistence Implementation	Datei: project/done/032a-persistence-impl.md	17	\N	2026-06-25 09:30:01.711962	2026-06-25 09:30:01.711962	\N	ticket
117	4	033: World Expansion	Datei: project/done/033-world-expansion.md	17	\N	2026-06-25 09:30:03.036232	2026-06-25 09:30:03.036232	\N	ticket
118	4	034: Artifact System [Roof]	Datei: project/done/034-artifact-system.md	17	\N	2026-06-25 09:30:09.343538	2026-06-25 09:30:09.343538	\N	ticket
119	4	034a: Artifact Types	Datei: project/done/034a-artifact-types.md	17	\N	2026-06-25 09:30:11.139001	2026-06-25 09:30:11.139001	\N	ticket
120	4	034bcd: Artifact Respawn + Pickup + Render	Datei: project/done/034bcd-artifact-respawn-pickup-render.md	17	\N	2026-06-25 09:30:12.378295	2026-06-25 09:30:12.378295	\N	ticket
121	4	035: Login Screen	Datei: project/done/035-login-screen.md	17	\N	2026-06-25 09:30:14.720392	2026-06-25 09:30:14.720392	\N	ticket
122	4	036: Echo Fragment [Roof]	Datei: project/done/036-echo-fragment.md	17	\N	2026-06-25 09:30:15.948754	2026-06-25 09:30:15.948754	\N	ticket
123	4	036a: Echo Server Foundation	Datei: project/done/036a-echo-server-foundation.md	17	\N	2026-06-25 09:30:17.400604	2026-06-25 09:30:17.400604	\N	ticket
124	4	036b: Echo Delivery	Datei: project/done/036b-echo-delivery.md	17	\N	2026-06-25 09:30:23.355785	2026-06-25 09:30:23.355785	\N	ticket
125	4	036c: Artifact Hint Echoes	Datei: project/done/036c-artifact-hint-echoes.md	17	\N	2026-06-25 09:30:24.761794	2026-06-25 09:30:24.761794	\N	ticket
126	4	036def: Client Echo HUD + Ghost PID	Datei: project/done/036def-client-echo-hud-ghost-pid.md	17	\N	2026-06-25 09:30:26.178353	2026-06-25 09:30:26.178353	\N	ticket
127	4	036g: Echo Fragment Lua	Datei: project/done/036g-echo-fragment-lua.md	17	\N	2026-06-25 09:30:27.508965	2026-06-25 09:30:27.508965	\N	ticket
128	4	036h: Client Echo State Followup	Datei: project/done/036h-client-echo-state-followup.md	17	\N	2026-06-25 09:30:29.058787	2026-06-25 09:30:29.058787	\N	ticket
129	4	037: Container and Death [Roof]	Datei: project/done/037-container-and-death.md	17	\N	2026-06-25 09:30:33.030274	2026-06-25 09:30:33.030274	\N	ticket
130	4	037a: Container Struct and Craft	Datei: project/done/037a-container-struct-and-craft.md	17	\N	2026-06-25 09:30:34.43931	2026-06-25 09:30:34.43931	\N	ticket
131	4	037b: Player Death Respawn	Datei: project/done/037b-player-death-respawn.md	17	\N	2026-06-25 09:30:35.854676	2026-06-25 09:30:35.854676	\N	ticket
132	4	037c: GC Container Damage	Datei: project/done/037c-gc-container-damage.md	17	\N	2026-06-25 09:30:37.255373	2026-06-25 09:30:37.255373	\N	ticket
133	4	038: L2 GC Reclaimer	Datei: project/done/038-l2-gc-reclaimer.md	17	\N	2026-06-25 09:30:38.627394	2026-06-25 09:30:38.627394	\N	ticket
134	4	039: Player Feedback Bugs	Datei: project/done/039-player-feedback-bugs.md	17	\N	2026-06-25 09:30:44.004299	2026-06-25 09:30:44.004299	\N	ticket
135	4	040: Mineable Deposits	Datei: project/done/040-mineable-deposits.md	17	\N	2026-06-25 09:30:45.855663	2026-06-25 09:30:45.855663	\N	ticket
136	4	041: Player State Refactor [Roof]	Datei: project/done/041-player-state-refactor.md	17	\N	2026-06-25 09:30:50.315589	2026-06-25 09:30:50.315589	\N	ticket
137	4	041a: Player Inventory Struct	Datei: project/done/041a-player-inventory-struct.md	17	\N	2026-06-25 09:30:51.685122	2026-06-25 09:30:51.685122	\N	ticket
138	4	041b: Player Fragment Set	Datei: project/done/041b-player-fragment-set.md	17	\N	2026-06-25 09:30:52.930342	2026-06-25 09:30:52.930342	\N	ticket
139	4	041c: Migrate Callsites	Datei: project/done/041c-migrate-callsites.md	17	\N	2026-06-25 09:30:55.742458	2026-06-25 09:30:55.742458	\N	ticket
140	4	041d: Helper Procs	Datei: project/done/041d-helper-procs.md	17	\N	2026-06-25 09:30:57.63577	2026-06-25 09:30:57.63577	\N	ticket
141	4	041e: Unit Tests	Datei: project/done/041e-unit-tests.md	17	\N	2026-06-25 09:30:59.067162	2026-06-25 09:30:59.067162	\N	ticket
142	4	042: Test Suite Foundation [Roof]	Datei: project/done/042-test-suite-foundation.md	17	\N	2026-06-25 09:31:00.719449	2026-06-25 09:31:00.719449	\N	ticket
143	4	042ab: Test Infrastructure + Smoke Tests	Datei: project/done/042ab-test-infrastructure-smoke.md	17	\N	2026-06-25 09:31:02.254428	2026-06-25 09:31:02.254428	\N	ticket
144	4	042d: CI Test Integration	Datei: project/done/042d-ci-test-integration.md	17	\N	2026-06-25 09:31:03.6892	2026-06-25 09:31:03.6892	\N	ticket
145	4	043: Kanban Hygiene	Datei: project/done/043-kanban-hygiene.md	17	\N	2026-06-25 09:31:05.227878	2026-06-25 09:31:05.227878	\N	ticket
146	4	044: Reconnect Across Restart [Roof]	Datei: project/done/044-reconnect-across-restart.md	17	\N	2026-06-25 09:31:06.887341	2026-06-25 09:31:06.887341	\N	ticket
147	4	044a: Username Persistence	Datei: project/done/044a-username-persistence.md	17	\N	2026-06-25 09:31:08.894079	2026-06-25 09:31:08.894079	\N	ticket
148	4	045: Playtest Scenarios	Datei: project/done/045-playtest-scenarios.md	17	\N	2026-06-25 09:31:10.526417	2026-06-25 09:31:10.526417	\N	ticket
149	4	046: Configurable Save Path	Datei: project/done/046-configurable-save-path.md	17	\N	2026-06-25 09:31:12.10126	2026-06-25 09:31:12.10126	\N	ticket
150	4	047a: Doc Refresh — Server/Client Architecture	Datei: project/done/047a-doc-refresh-server-client-architecture.md	17	\N	2026-06-25 09:31:18.096578	2026-06-25 09:31:18.096578	\N	ticket
151	4	047b: Doc Refresh — Multiplayer Protocol	Datei: project/done/047b-doc-refresh-multiplayer-protocol.md	17	\N	2026-06-25 09:31:19.774967	2026-06-25 09:31:19.774967	\N	ticket
152	4	047c: Doc Refresh — Header Fragments	Datei: project/done/047c-doc-refresh-header-fragments.md	17	\N	2026-06-25 09:31:21.211991	2026-06-25 09:31:21.211991	\N	ticket
153	4	047d: Doc Refresh — Worker Scripting	Datei: project/done/047d-doc-refresh-worker-scripting.md	17	\N	2026-06-25 09:31:22.813855	2026-06-25 09:31:22.813855	\N	ticket
154	4	047e: Doc Refresh — Spawn System	Datei: project/done/047e-doc-refresh-spawn-system.md	17	\N	2026-06-25 09:31:24.430166	2026-06-25 09:31:24.430166	\N	ticket
155	4	047f: Doc Refresh — Entropy and Bitrot	Datei: project/done/047f-doc-refresh-entropy-and-bitrot.md	17	\N	2026-06-25 09:31:26.277919	2026-06-25 09:31:26.277919	\N	ticket
156	4	047g: Doc Refresh — World and Resources	Datei: project/done/047g-doc-refresh-world-and-resources.md	17	\N	2026-06-25 09:31:27.771989	2026-06-25 09:31:27.771989	\N	ticket
157	4	047h: Doc Refresh — Misc	Datei: project/done/047h-doc-refresh-misc.md	17	\N	2026-06-25 09:31:29.20878	2026-06-25 09:31:29.20878	\N	ticket
158	4	048a: Test — Snapshot Round-Trip	Datei: project/done/048a-test-snapshot-round-trip.md	17	\N	2026-06-25 09:31:30.886318	2026-06-25 09:31:30.886318	\N	ticket
159	4	048b: Test — Echo System	Datei: project/done/048b-test-echo-system.md	17	\N	2026-06-25 09:31:35.642959	2026-06-25 09:31:35.642959	\N	ticket
160	4	048c: Test — Death Respawn	Datei: project/done/048c-test-death-respawn.md	17	\N	2026-06-25 09:31:37.122971	2026-06-25 09:31:37.122971	\N	ticket
161	4	048d: Test — Persistence Round-Trip	Datei: project/done/048d-test-persistence-round-trip.md	17	\N	2026-06-25 09:31:38.523975	2026-06-25 09:31:38.523975	\N	ticket
162	4	049: Kanban Hygiene	Datei: project/done/049-kanban-hygiene.md	17	\N	2026-06-25 09:31:40.202483	2026-06-25 09:31:40.202483	\N	ticket
163	4	050: AGENTS.md Refactor	Datei: project/done/050-agents-md-refactor.md	17	\N	2026-06-25 09:31:41.982188	2026-06-25 09:31:41.982188	\N	ticket
164	4	051: Visual Style Pixel Pivot [Roof]	Datei: project/done/051-visual-style-pixel-pivot.md	17	\N	2026-06-25 09:31:43.249604	2026-06-25 09:31:43.249604	\N	ticket
165	4	051a: Pixel Style Spike	Datei: project/done/051a-pixel-style-spike.md	17	\N	2026-06-25 09:31:45.299419	2026-06-25 09:31:45.299419	\N	ticket
166	4	051c: Worker + GC Sprite Migration	Datei: project/done/051c-worker-gc-sprite-migration.md	17	\N	2026-06-25 09:31:51.266193	2026-06-25 09:31:51.266193	\N	ticket
167	4	051d: Resource Tile Icon Migration	Datei: project/done/051d-resource-tile-icon-migration.md	17	\N	2026-06-25 09:31:53.122777	2026-06-25 09:31:53.122777	\N	ticket
168	4	051e: HUD Substrate Frame	Datei: project/done/051e-hud-substrate-frame.md	17	\N	2026-06-25 09:31:54.722507	2026-06-25 09:31:54.722507	\N	ticket
169	4	051fa: CRT Bloom Shader	Datei: project/done/051f-a-crt-bloom-shader.md	17	\N	2026-06-25 09:31:56.470958	2026-06-25 09:31:56.470958	\N	ticket
170	4	051g: Grid Corner Dots	Datei: project/done/051g-grid-corner-dots.md	17	\N	2026-06-25 09:31:58.900987	2026-06-25 09:31:58.900987	\N	ticket
171	4	054: CI Pipeline Fix	Datei: project/done/054-ci-pipeline-fix.md	17	\N	2026-06-25 09:32:00.553325	2026-06-25 09:32:00.553325	\N	ticket
172	4	055: Zoom Limit	Datei: project/done/055-zoom-limit.md	17	\N	2026-06-25 09:32:02.334726	2026-06-25 09:32:02.334726	\N	ticket
173	4	056: Player Pursuit Smoothing	Datei: project/done/056-player-pursuit-smoothing.md	17	\N	2026-06-25 09:32:04.084791	2026-06-25 09:32:04.084791	\N	ticket
174	4	057: Fragment Interact Fix	Datei: project/done/057-fragment-interact-fix.md	17	\N	2026-06-25 09:32:06.445817	2026-06-25 09:32:06.445817	\N	ticket
175	4	058: Player Movement V2	Datei: project/done/058-player-movement-v2.md	17	\N	2026-06-25 09:32:07.958913	2026-06-25 09:32:07.958913	\N	ticket
176	4	059: World Structure Blocks	Datei: project/done/059-world-structure-blocks.md	17	\N	2026-06-25 09:32:09.947975	2026-06-25 09:32:09.947975	\N	ticket
177	4	060: Windows Builds	Datei: project/done/060-windows-builds.md	17	\N	2026-06-25 09:32:12.109418	2026-06-25 09:32:12.109418	\N	ticket
178	4	061: Interaction Architecture	Datei: project/done/061-interaction-architecture.md	17	\N	2026-06-25 09:32:13.449966	2026-06-25 09:32:13.449966	\N	ticket
179	4	063: Procedural Variable Pointer Bug Fix	Datei: project/done/063-procedural-variable-pointer-bug.md	17	\N	2026-06-25 09:32:15.413898	2026-06-25 09:32:15.413898	\N	ticket
180	4	064: Movement Input-Driven Walk Frame	Datei: project/done/064-movement-input-driven-walk-frame.md	17	\N	2026-06-25 09:32:16.613804	2026-06-25 09:32:16.613804	\N	ticket
181	4	065: Fragment HUD Pointer	Datei: project/done/065-fragment-hud-pointer.md	17	\N	2026-06-25 09:32:18.408099	2026-06-25 09:32:18.408099	\N	ticket
187	4	110f: GC Sweep Visuals	**Datei:** project/todo/110f-gc-sweep-visuals.md\n**Parent:** 110 | **Aufwand:** S\n\nL1: Sweep-Linie 1.5× Tile-Radius. L2: 3 konzentrische Ringe (α 80/50/25).	17	claude-sonnet-4-6	2026-06-29 09:39:05.118829	2026-06-30 10:33:24.339276	\N	ticket
188	4	110g: HUD Terminal Integration	**Datei:** project/todo/110g-hud-terminal-integration.md\n**Parent:** 110 | **Aufwand:** S\n\nDunkelgrüner Panel-BG + 1px HUD_COL-Border. Color-coded Dots vor Ressourcen. Aktive Worker: pulsierender cyan Rand. Optionaler Dynamic-Status-String.	17	claude-sonnet-4-6	2026-06-29 09:39:07.062501	2026-06-30 10:33:25.968505	\N	ticket
27	4	099: Item-Architektur — Extensibles Item-System	**Datei:** project/planning/099-item-architecture.md\n**Aufwand:** M (Architekturentscheidung + Refactoring) | **Blockiert:** 098 (HEAP braucht Items)\n\n**Mensch-Entscheidung (2026-06-23):** Voll-Migration — Bytes, CPU_Cycles, RAM_Chips und Fragmente werden in das neue Item-System überführt. Kein Spezialfall bleibt.\n\n**ADR-Entscheidungen:**\n- Datenmodell: `ItemType` (enum) + `ItemDef` (Registry) + `ItemStack` (runtime) + `Inventory`\n- Registry-Source: Odin-Code-Konstanten (typsicher)\n- Fragmente: Hybrid (Item im Inventar + bit_set-Fastpath für Lua)\n- Volumen: abstrakte "KB"-Einheit (Bytes=1, RAM=2, Fragmente=1)\n- Fähigkeiten: `ItemCapability` enum\n- Persistenz: Schema 0x0002 → 0x0003 (Alpha-Break akzeptabel)\n\n**Sub-Tickets (in todo/ + done/):**\n- [x] 099b: Core data model + items.odin\n- [x] 099c: Resource Migration (scalar → Inventory)\n- [x] 099d: Fragment Migration\n- [x] 099e: Crafting Registry\n- [x] 099f: Persistence + Networking + Lua API\n- [x] 099g: Smoke-Test Transistor\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Mensch (2026-06-23): Voll-Migration\n- [x] Architect (2026-06-23): ADR + Sub-Tickets\n- [x] Developer: alle Sub-Tickets abgeschlossen	17	\N	2026-06-25 09:24:32.764331	2026-06-29 09:30:23.547894	\N	ticket
36	4	098: Craftable Terminals + HEAP-Speicher [Roof]	**Datei:** project/in-progress/098-craftable-terminals-heap.md\n**Aufwand:** L | **Refs:** Mensch-Idee 2026-06-22\n\nTerminal = stationäres craftbares Entity (Transistoren + RAM). Funktionen: Worker-Upgrades (LoC-Limit) + HEAP-Zugang.\nHEAP = pro Spieler, Kapazität in Bytes, Erweiterung durch RAM-Upgrades am Terminal.\n\n**Mensch-Entscheidungen:**\n- Terminal: stationär, gecraftet aus Transistoren + RAM_Chips\n- HEAP: pro Spieler, Bytes-basiert, Erweiterung durch RAM-Upgrades\n- Crafting: [T] im Inventory-Screen\n\n**Sub-Tickets:**\n- [x] 098a: Terminal-Entity + Craft [T] + Snapshot + Persistenz + Render\n- [x] 098c: HEAP-Datenmodell + Terminal-Overlay-UI\n- [ ] **098b: Worker-Upgrade via Terminal (LoC-Limit erhöhen) — OFFEN**\n\n**098b Scope:**\n- Neues MSG_UPGRADE_WORKER_LOC\n- Server: Spieler zeigt auf Terminal, konsumiert RAM_Chips, erhöht w.loC_limit des gehaltenen Workers\n- Terminal-Overlay: Upgrade-Option anzeigen\n- Client-Input: Taste zum Triggern\n\n**Agent Log:**\n- [x] Story Writer (2026-06-22)\n- [x] Requirement Manager (2026-06-24): Mensch-Entscheidungen\n- [x] Architect (2026-06-24): Sub-Tickets 098a + 098c\n- [x] 098a abgeschlossen (2026-06-24)\n- [x] 098c abgeschlossen (2026-06-24)\n- [ ] 098b: noch offen	17	\N	2026-06-25 09:25:48.877461	2026-06-29 09:37:19.883016	\N	ticket
35	4	115: Codeberg Kanban MCP Server	**Datei:** project/todo/115-codeberg-kanban-mcp.md\n\nMaßgeschneiderter MCP-Server in Python als Kanban/Issue-Brücke für Codeberg (Forgejo/Gitea API v1). Liest Token aus ~/.config/codeberg/token, ausführbar via `uv run`.\n\n**Success Criteria:**\n- [x] Server liest Token korrekt (bricht ab falls fehlend)\n- [x] Bietet Tools für Projects, Columns, Cards und Comments an\n- [x] Startbar via `uv run`\n- [x] Robuste asynchrone HTTP-Requests (httpx)\n\n**Verification:** N/A — Python Script (kein odin check nötig)\n\n**Agent Log:**\n- Human hat autorisiert, in-progress/ zu überspringen und direkt todo/ zu nutzen\n- tools/codeberg-mcp/server.py erstellt\n\n**Status:** Alle Kriterien erfüllt — kann zu done/ verschoben werden.	17	\N	2026-06-25 09:25:37.544377	2026-06-29 09:27:14.586486	\N	ticket
34	4	113b: AI Agent Testing Loop — Headless JSON-API	**Datei:** project/planning/113b-ai-agent-testing-loop.md\n\nAutomatisierte Spieler-Simulation ohne echte Menschen via KI-Agenten.\n\n**Techniken:**\n- **Headless JSON/Text Interface via DEBUG_PORT (7374, localhost):** Server sendet Weltzustand als kompaktes JSON, externe LLM-Agenten senden `{"cmd": "move", "dir": "N"}`\n- **Tick-Rate Steuerung:** Lockstep-Modus — Game-Loop pausiert bis Agent Aktion übermittelt (Turn-based statt Echtzeit)\n- **Mock-Client (Headless Odin Client):** CLI-Client ohne Raylib/Rendering, spricht Standard-Protokoll; für Lasttests + Chaos Monkey\n\n**Success Criteria:**\n- [ ] Debug-Port kann Mock-Player spawnen und JSON-Kommandos entgegennehmen\n- [ ] PoC Python-Script verbindet sich und führt Aktionen aus\n- [ ] Lockstep-Modus als Server-Option verfügbar\n\n**Note:** 114a (Debug JSON API), 114b (Lockstep), 114c (Headless Mock Client) sind bereits done.\n\n**Agent Log:**\n- [x] Requirement Manager: aus 113 aufgeteilt\n- [x] Architect: nach todo/ dekomponiert (114a-c)\n- [x] Developer: 114a-c abgeschlossen	17	\N	2026-06-25 09:25:35.48704	2026-06-29 09:30:22.239056	\N	ticket
182	4	098b: Worker-Upgrade via Terminal (LoC-Limit erhöhen)	**Datei:** project/done/098b-worker-upgrade-terminal.md\n**Parent:** 098 (kbai #36)\n\nMSG_UPGRADE_WORKER_LOC (0x1D): Spieler hält Worker, steht vor Terminal, drückt [U] → zahlt 5 RAM_Chips → loC_limit +10.\n\n**Implementiert:**\n- MSG_UPGRADE_WORKER_LOC + LOC_UPGRADE_COST/INCREMENT in protocol.odin\n- Server-Handler _apply_upgrade_worker_loc in simulation.odin\n- U-Taste in _handle_terminal_keys (input.odin)\n- Ack-Handler in client network.odin\n- Upgrade-Option in draw_terminal_overlay (render.odin)\n- Inline-Fix: MSG_TERMINAL_OPEN 0x12→0x1E (Duplikat-Bug, Backlog-Ticket 116)\n\n**Verification:** odin check ./src/server + ./src/client grün.	17	\N	2026-06-29 09:36:56.718206	2026-06-29 09:36:56.718206	\N	ticket
183	4	bug-183: Protocol Opcode Audit — Duplicate-Value-Bereinigung	**Aufwand:** XS | **Refs:** src/shared/protocol.odin\n\nMSG_TERMINAL_OPEN hatte denselben Wert (0x12) wie MSG_PLAYER_JOINED (beide S→C). In 098b inline gefixt (0x12→0x1E). Vollständiger Audit steht noch aus.\n\n**Anforderungen:**\n- Alle MSG_* Konstanten in protocol.odin auf Duplikate prüfen\n- Für jede Konstante: Richtung (C→S / S→C / beide) dokumentieren\n- Kommentar-Tabelle direkt in protocol.odin einfügen\n- Etwaige weitere Duplikate fixen\n\n**Akzeptanzkriterien:**\n- [ ] Kein MSG_*-Wert kommt zweimal vor (pro Richtung)\n- [ ] Kommentar-Tabelle in protocol.odin mit allen Opcodes + Richtung\n- [ ] odin check src/server + src/client grün	17	claude-sonnet-4-6	2026-06-29 09:37:04.858569	2026-06-30 19:25:56.150902	\N	ticket
184	4	110b: Zone Flicker / Pulse Effects (Hot / Kernel)	**Datei:** project/todo/110b-zone-flicker-pulse.md\n**Parent:** 110 | **Aufwand:** S\n\nHot Zone: amber flicker ±15 alpha bei 3 Hz via sin(GetTime()*3π).\nKernel Layer: slow magenta edge pulse 0.5 Hz.\nAlles in render.odin, kein Server-State.	17	claude-sonnet-4-6	2026-06-29 09:38:53.450346	2026-06-30 10:33:19.815356	\N	ticket
185	4	110c: Additional Empty-Tile Variants	**Datei:** project/todo/110c-empty-tile-variants.md\n**Parent:** 110 | **Aufwand:** S\n\n3 Varianten: 15% Scratch-Pattern (diag. Linien α8), 5% Shimmer nahe Zonengrenzen, Fragment-Floor `<>` Symbol (α30 cyan). Deterministisch über chunk-hash.	17	claude-sonnet-4-6	2026-06-29 09:39:00.070537	2026-06-30 10:33:21.182491	\N	ticket
195	4	006b: GC Wall Collision	**Datei:** project/done/006b-gc-wall-collision.md\n**Parent:** 006 | **Status:** done\n\nadvance_gcs prüft tile_is_solid auf Ziel-Tile. Slide-Fallback auf andere Achse. Beide blockiert → GC wartet.\n\nodin check ./src/server OK.	17	\N	2026-06-29 09:43:00.994018	2026-06-29 09:43:00.994018	\N	ticket
200	4	099h: Inventar-Overlay Terminal-Listview	**Parent:** 099-item-architecture | **Aufwand:** S | **Depends:** 099b (Inventory data model)\n\nRedesign draw_inventory_overlay (inventory.odin) vom hardcodierten 3-Zeilen-Stub zum dynamischen Terminal-Listview.\n\nScope:\n- Alle Inventory-Slots dynamisch iterieren (nicht hardcodiert Bytes/CPU/RAM)\n- Volume-Fortschrittsbalken (used_volume / INVENTORY_VOLUME_CAP)\n- Cursor-Navigation ↑↓ (inv_selected Feld in ClientState)\n- [e] use für Items mit consumable Capability\n- KB-Anzeige pro Item-Zeile\n- Crafting-Recipes-Sektion bleibt erhalten\n- Fragments-Sektion bleibt erhalten	17	claude-sonnet-4-6	2026-06-30 11:06:51.54913	2026-06-30 11:09:07.3002	\N	ticket
194	4	100d: Cave Visual Differentiation	**Datei:** project/todo/100d-cave-visual-diff.md\n**Parent:** 100 | **Aufwand:** S | **Depends:** 100a\n\nCave_Entrance: Orange-# mit Schloss-Symbol. Höhlenraum: Ambient-Tint (dunkelrot). Client-seitig.	17	claude-sonnet-4-6	2026-06-29 09:41:45.102623	2026-06-30 10:54:38.549836	\N	ticket
191	4	100a: Cave Worldgen Placement	**Datei:** project/todo/100a-cave-worldgen.md\n**Parent:** 100 | **Aufwand:** M\n\n1 Höhle pro 8×8-Chunk-Region, deterministisch. Neuer Tile-Typ .Cave_Entrance. 5–8 Tile großer Raum, umgeben von .Wall. state.caves hält Eingangs-Coords.	17	claude-sonnet-4-6	2026-06-29 09:41:39.411289	2026-06-30 10:42:54.490096	\N	ticket
190	4	006b: GC Wall Collision	**Datei:** project/todo/006b-gc-wall-collision.md\n**Parent:** 006 | **Aufwand:** S\n\nadvance_gcs Greedy-Step prüft tile_is_solid (bereits in world.odin). Wall-Check hinzufügen + Slide-Fallback auf andere Achse.	17	\N	2026-06-29 09:41:38.16225	2026-06-29 09:46:31.814777	\N	ticket
189	4	006a: Wall Building Input + Resource Cost	**Datei:** project/todo/006a-wall-tile-build-input.md\n**Parent:** 006 | **Aufwand:** S\n\nMSG_BUILD_WALL (0x1F): Facing Empty-Tile → .Wall setzen, kostet 20 Bytes. .Wall Tile-Typ existiert bereits. Taste W im Normal-Modus.	17	claude-sonnet-4-6	2026-06-29 09:41:36.66935	2026-06-30 10:33:18.206376	\N	ticket
186	4	110d: GC Dread-Field Halo	**Datei:** project/todo/110d-gc-dread-halo.md\n**Parent:** 110 | **Aufwand:** S\n\nSekundärer entsättigter Halo (alle Tier). L1: pulsing ring 2Hz. L2: 45°-Arc-Sweep statt Linie.	17	claude-sonnet-4-6	2026-06-29 09:39:03.203348	2026-06-30 10:33:22.366143	\N	ticket
201	4	117: Container-UI — Einlagern / Auslagern	**Aufwand:** M | Alpha-Blocker seit Feature 044\n\nInteraktives Container-Overlay (analog Terminal): Spieler steht vor Container → [space] → Split-View Spieler-Inventar | Container-Inventar. [d] deposit, [w] withdraw, [ESC] close.\n\nNeue Nachrichten: MSG_CONTAINER_OPEN (server→client, sendet container_id), MSG_CONTAINER_DEPOSIT/WITHDRAW (client→server mit container_id+item_type+count).\n\nServer: resolve_interaction prüft Container auf facing-Tile (Prio zwischen Terminal und Worker). apply_interaction sendet MSG_CONTAINER_OPEN. Neue Handler für deposit/withdraw prüfen Adjacency + Ownership.	17	claude-sonnet-4-6	2026-06-30 11:21:33.424044	2026-06-30 11:30:06.283178	\N	ticket
193	4	100c: Cave Loot Tables	**Datei:** project/todo/100c-cave-loot-tables.md\n**Parent:** 100 | **Aufwand:** S | **Depends:** 100a\n\n60% Ressourcen-Spawn-Rate in Höhle. Alle 3 Typen. 10% Crystal Deposits.	17	claude-sonnet-4-6	2026-06-29 09:41:43.712335	2026-06-30 10:50:01.94839	\N	ticket
202	4	118: memory.h Lua-API — mem_read / mem_write	Fragment 2 (memory.h) schaltet 16 geteilte Integer-Register pro Spieler frei. mem_read(addr)/mem_write(addr,val) für alle Worker eines Spielers gemeinsam. Persistent in save/load als trailing optional block.	17	claude-sonnet-4-6	2026-06-30 11:36:13.494302	2026-06-30 11:36:20.080306	\N	ticket
199	5	NavigationV2 #43 - Schickere Navigation mit j/k/g/G/H, Search (v/n/y) und Goto-Prozess (p)	**To Do:**\n- [ ] Erweitere die Navigation so: `j/k` für Vor/Weiter; `g` = auf Zeile 0 (oberste Zeile); `G`=auf letzte Zeile oder scrollen.\n- [ ] Schreibe ein Fenster in der unteren Ecke, das die Sucheingabe "suche" und ein "v" oder ein "n" für die nächste Suche zeigt. Drücke um zu suchen; `y` oder umgekehrt ist es umzukehren.\n- [ ] drücken `p`, um den vorherigen prozess in der Zeile 100 anzuzeigen.\n\nAuf Deutsch? Nein danke.	18	\N	2026-06-29 09:58:46.494612	2026-06-30 12:10:32.4537	\N	ticket
197	5	DisplayV2 #40 - Erweitere die Prozesstabelle auf allen Zeilen (Header, Status-Icons)	Der aktuelle Header `%-8s|%-16.16s|%-64.64s|` ist zu knapp für die volle Process-Struktur:\n`PID USER UID UGID RSS CMDLINE`\n\n**To Do:**\n- [ ] Schreibe einen Header-Footer, der die Spaltentrenner und Labels korrekt positioniert\n- [ ] Wähle `char *data_str` als typischster Platzhirsch für alle Process-Zeilen-Displays\n- [ ] Verwende die neuen Felder zum Populieren der Tabelle\n\nAuf Deutsch? Nein danke.	18	\N	2026-06-29 09:58:46.493697	2026-06-30 12:10:31.889236	\N	ticket
198	5	Sortiert und gefärbt #42 - Sortierte Output-Liste, farbig nach State + User, mit H/W/Tab-Leist	**To Do:**\n- [ ] Speichere die Prozesse in einem Array (im Head), wenn die Anzahl nicht > 100 wird. Sortiere das Array!\n - Die Zeilen 0..99 werden angezeigt; ab der Zeile 100 wird ein scrollbar Header "..." eingegeben, damit der Benutzer weiß: noch mehr Prozess?\n- [ ] Color-Coding nach State: R = Rot (running), S/D = Grün, T/S = Gelb, Z/t = Hellgelb, andere = Grau\n- [ ] User-Spalte: Färbe nach UID (Root 0/65534 blau, andere weiß)\n\nAuf Deutsch? Nein danke.	18	\N	2026-06-29 09:58:46.494201	2026-06-30 12:10:33.030278	\N	ticket
192	4	100b: Cave Entry Mechanic (Pointer Key)	**Datei:** project/todo/100b-cave-entry-mechanic.md\n**Parent:** 100 | **Aufwand:** M | **Depends:** 100a, 099 (done)\n\nNeues Item Pointer_Key. Interact auf Cave_Entrance: Key verbrauchen → Eingang öffnen.	17	\N	2026-06-29 09:41:41.565221	2026-06-30 23:49:15.251776	\N	ticket
203	4	119: net.h Lua-API — net_send / net_recv	Fragment 4 (net.h) schaltet Spieler-übergreifende Nachrichtenübertragung frei. net_send(pid, msg) / net_recv() mit PID-Lookup, max 64 Nachrichten im Puffer, nicht persistiert.	17	claude-sonnet-4-6	2026-06-30 11:36:14.830809	2026-06-30 11:36:21.264653	\N	ticket
204	4	120: Kampf — Spieler greift GC mit [space] an	Attack_GC InteractionKind, Priority 2d in resolve_interaction (nach Container-Check). 25 Schaden/Treffer, 10-Tick-Cooldown. GC wird bei integrity≤0 gelöscht.	17	claude-sonnet-4-6	2026-06-30 11:38:38.718272	2026-06-30 11:38:42.411178	\N	ticket
205	2	bug: hier geht irgendwas nicht	der fehler soll behoben werden	7	\N	2026-06-30 12:08:44.12679	2026-06-30 12:08:57.242792	\N	ticket
206	2	bingo bongo	\N	6	\N	2026-06-30 12:09:15.466681	2026-06-30 12:09:15.466681	\N	ticket
207	2	schlabber blabber	\N	6	\N	2026-06-30 12:09:23.581323	2026-06-30 12:09:23.581323	\N	ticket
208	2	rattatui	\N	6	\N	2026-06-30 12:09:28.886219	2026-06-30 12:09:28.886219	\N	ticket
196	5	ProcessDataV2 #30 - Implementiere Prozess-Daten-Vervollständigung (state/UID/GID/RSS/Swap)	Extend die Datenstruktur und die /proc-Reader, um alle Felder aus der Typdef in `main.c` zu befüllen:\n\n```c\ntypedef struct {\n    uint32_t pid;\n    ProcessName name;\n    ProcessCmdLine cmdline;\n    mode_t umask;             // man 2 umask\n    char state;                // R/S/D/T/t/Z/X (aus /proc/[pid]/status "State:")\n    uint16_t uid[4];           // real, effective, saved set, filesystem\n    uint16_t gid[4];\n    unsigned long VmRSS;       // kB, aus /proc/[pid]/status\n    unsigned long VmSwap;      // kB\n    bool container;            // Docker/Podman-Kontext ermitteln\n} Process;\n```\n\n**To Do:**\n- [ ] Erweitere `read_process_data()` oder ein neues Header so, dass es die vollständige `Process`-Struktur aus /proc liest\n- [ ] Teste unter einem Container auch (state "S/S" bei nicht-running prozessen)\n\nAuf Deutsch? Nein danke.	18	\N	2026-06-29 09:58:46.478275	2026-06-30 12:10:33.57099	\N	ticket
209	4	bug-121: Facing-Indicator nutzt Top-Down-Koordinaten statt Iso	**Datei:** project/backlog/bug-121-facing-indicator-iso-coords.md\n**Aufwand:** XS | **Refs:** src/client/render.odin — draw_players\n\nNach 106b (iso tile renderer) bewegt sich das Facing-Highlight-Quadrat (das Tile vor dem Spieler) noch in alten Top-Down-Koordinaten (`tx * TILE_SIZE`), während der Spieler-Sprite bereits in iso-Raum gerendert wird. Dadurch driftet der Indicator unabhängig vom Sprite.\n\n**Root Cause:**\ndraw_players nutzt noch `DrawRectangleLines(tx * TILE_SIZE, ty * TILE_SIZE, TILE_SIZE, TILE_SIZE, ...)` für den facing-Tile-Highlight.\n\n**Fix:**\ntile_to_screen(tx, ty) für Facing-Tile benutzen + draw_iso_diamond statt DrawRectangleLines.\n\n**Akzeptanzkriterien:**\n- [ ] Facing-Highlight klebt am korrekten iso Diamond vor dem Spieler\n- [ ] odin check ./src/client grün	17	\N	2026-06-30 18:05:11.979449	2026-06-30 18:07:57.272796	\N	ticket
210	4	122: Tester — Coverage für 117 Container, 118 mem_slots, 120 GC-Attack	**Datei:** project/backlog/122-tester-coverage-117-118-120.md\n**Aufwand:** S | **Refs:** src/server/interaction_test.odin, src/server/persistence_test.odin\n\nFehlende Tests nach Tickets 117, 118, 120:\n\n**interaction_test.odin (117 + 120):**\n- resolve: Open_Container (facing tile hat Container → .Open_Container)\n- resolve: Attack_GC (facing tile hat GC, cooldown=0 → .Attack_GC)\n- resolve: Attack_GC blockiert wenn cooldown > 0\n- apply: Attack_GC reduziert gc.integrity\n- apply: Attack_GC löscht GC bei integrity ≤ 0\n- apply: attack_cooldown wird auf PLAYER_ATTACK_COOLDOWN_TICKS gesetzt\n\n**persistence_test.odin (118):**\n- mem_slots Roundtrip: speichern + laden erhält alle 16 Slots\n\n**Akzeptanzkriterien:**\n- [ ] Alle genannten Tests grün\n- [ ] odin test ./src/server: count gestiegen, alle grün	17	\N	2026-06-30 18:05:18.765394	2026-06-30 18:08:00.731959	\N	ticket
214	4	123c — Server: apply_movement auf InputVector umstellen	apply_movement nimmt InputVector{dx, dy} statt Direction. Validierung: |dx|≤1, |dy|≤1. Facing-Regel: dy!=0 → N/S, sonst E/W. Setzt x+=dx, y+=dy.	17	claude-sonnet-4-6	2026-06-30 18:14:06.41942	2026-06-30 18:20:24.723624	\N	ticket
215	4	123d — Client: handle_input Multi-Key Detection	handle_input baut InputVector aus IsKeyDown(W/A/S/D). Sendet MSG_MOVE nur wenn InputVector sich geändert hat (verhindert Flood). Erfordert 123b (neues Protokoll).	17	claude-sonnet-4-6	2026-06-30 18:14:14.079721	2026-06-30 18:22:50.893597	\N	ticket
217	4	124a — Shared: Biome-Enum + Tile.biome Feld	**Rolle:** Developer | **Aufwand:** S | **Refs:** `src/shared/world.odin`, `src/server/persistence.odin`\n\nNeues `Biome :: enum u8 { None, Heap, Stack, Cache, Registers }` in world.odin. Tile-Struct erhält `biome: Biome`. Persistence: SAVE_VERSION 3→4, save_chunk schreibt biome-Byte, load_chunk liest es (ver<4 → biome=.None für Backward-Compat).\n\n**Akzeptanzkriterien:**\n- Biome-Enum in shared/world.odin\n- Tile.biome Feld vorhanden\n- odin test src/server grün (Persistence-Tests müssen Biome erhalten)	17	claude-sonnet-4-6	2026-06-30 18:14:32.792497	2026-06-30 18:35:26.438562	\N	ticket
216	4	123e — Tests: Diagonal-Movement + Facing-Regel	Server-Tests: (1) W+A → ty-1 tx-1, (2) W+D → ty-1 tx+1, (3) S+A → ty+1 tx-1, (4) S+D → ty+1 tx+1. Facing: W+A → North, W+D → North, A allein → West. Erfordert 123b+123c.	17	claude-sonnet-4-6	2026-06-30 18:14:25.717045	2026-06-30 18:25:17.082816	\N	ticket
213	4	123b — Shared: InputVector + MSG_MOVE Protocol-Update	Neues `InputVector :: struct{dx, dy: i8}` in shared/types.odin. MSG_MOVE Body von 1-Byte-Direction auf 2-Byte `[dx, dy]` erweitern. Direction bleibt für Facing erhalten.	17	claude-sonnet-4-6	2026-06-30 18:14:03.455199	2026-06-30 18:19:11.80199	\N	ticket
218	4	124b — Server: Biom-Noise in world_gen + Chunk-Seeding	world_gen.odin: Simplex/Value-Noise bestimmt Biom pro Tile beim Chunk-Init. Seeding: Ressourcen nach Biom-Profil (HEAP→RAM_Chips, STACK→CPU_Cycles, CACHE→Bytes, REGISTERS→Pointers). Erfordert 124a.	17	claude-sonnet-4-6	2026-06-30 18:14:35.077955	2026-06-30 18:37:54.567411	\N	ticket
219	4	124c — Server: Respawn-Mechanik in tick.odin	Tile erhält `ticks_since_harvested: u32`. Server-Tick: nach konfigurierbarem Schwellwert (z.B. 6000 Ticks = 5min @20Hz) respawnt Tile-Ressource gemäß Biom-Profil. Erfordert 124a.	17	claude-sonnet-4-6	2026-06-30 18:14:41.235603	2026-06-30 18:41:36.757517	\N	ticket
220	4	124e — Tests: Biom-Seeding + Respawn	Tests: (1) HEAP-Chunk enthält RAM_Chips, (2) STACK-Chunk enthält CPU_Cycles, (3) Harvested tile respawnt nach RESPAWN_TICKS, (4) Respawn-Typ entspricht Biom. Erfordert 124b+124c.	17	claude-sonnet-4-6	2026-06-30 18:14:43.606881	2026-06-30 18:42:40.056376	\N	ticket
211	4	123: Iso-Steuerung — Bewegungsrichtungen für isometrische Perspektive [Story]	**Datei:** project/backlog/123-iso-controls-redesign.md\n**Aufwand:** M | **Refs:** src/client/input.odin, src/server/simulation.odin\n\nIn der Top-Down-Ansicht repräsentierten W/S/A/D die Bildschirm-Achsen (oben/unten/links/rechts). In der isometrischen Perspektive bewegen sich Entities jetzt diagonal — W fühlt sich eher horizontal an als vertikal.\n\n**Kernfrage:** Wie sollen Bewegungstasten in iso aussehen/fühlen?\n\n**Optionen (Architect soll entscheiden):**\n1. **Status Quo** — W=NE, A=NW, S=SW, D=SE (iso-korrekt, aber für neue Spieler fremd). HUD-Hints anpassen.\n2. **Achsen tauschen** — W/S steuern screen-vertikale Iso-Achse, A/D die andere. Fühlt sich anders an, löst das Problem nicht grundsätzlich.\n3. **8-Richtungs-Bewegung** — W+A=screen-oben, W+D=screen-rechts etc. Erfordert diagonale Tile-Bewegung (Server-Änderung), löst die Intuition vollständig.\n\n**Success Criteria:**\n- [ ] Bewegung fühlt sich für iso-Novizen intuitiv an\n- [ ] HUD zeigt korrekte Richtungshinweise\n- [ ] Architect-ADR vorhanden	17	claude-sonnet-4-6	2026-06-30 18:05:26.39284	2026-06-30 18:25:52.182701	\N	ticket
212	4	124: Resource- und Spawn-Overhaul — Iso-Welt-Anpassung [Story]	**Datei:** project/backlog/124-resource-spawn-overhaul.md\n**Aufwand:** XL | **Refs:** src/server/world_gen.odin, src/server/spawn.odin, adr/106-isometric-rendering.md\n\nDas bestehende Resource-/Spawn-System wurde für Top-Down entwickelt. Mit der isometrischen Perspektive und dem Konzeptbild (Plattformen, Hub-Struktur, Connection-Lines) ist ein Überdenken sinnvoll.\n\n**Bestehende Systeme:**\n- Worldgen: freie Tile-Platzierung mit Distanz-basierter Zone (kalt/heiß/Kernel)\n- Resource-Respawn (030b): zeit-basiert, zufällig in bekannten Chunks\n- GC-Spawn (030c/d): Spieler-relativ, Entropie-skaliert\n- Cave-Loot (100c): erhöhte Dichte in Höhlen\n- Artifact-Spawn (034bcd)\n\n**Was soll überdacht werden:**\n- Plattform-Topologie aus Konzeptbild: diskrete Inseln statt freie Fläche?\n- Resource-Clustering: thematische Zonen (HEAP-Bereich = RAM-Cluster, CPU-Zone = Cycle-Cluster)\n- GC-Spawn: Wellen statt kontinuierlich? Tier-Progression über Zeit?\n- Spawn-Balance für Iso-Sichtfeld (iso-Frustum ist breiter als Top-Down)\n- Cave-Integration in Plattform-Topologie\n\n**Success Criteria:**\n- [ ] Architect-ADR mit Entscheidungen zu Plattform-Topologie und Spawn-Modell\n- [ ] Sub-Tickets für alle größeren Änderungen\n- [ ] Mensch hat Plattform-Entscheidung getroffen (Insel-Welt vs. freie Welt)	17	claude-sonnet-4-6	2026-06-30 18:05:35.867804	2026-06-30 18:43:02.202028	\N	ticket
221	4	bug-221: WASD-Bewegung — falsche Richtungs-Mappings + Geschwindigkeits-Inkonsistenz in Iso-Ansicht	**Aufwand:** S | **Refs:** src/client/input.odin, src/server/simulation.odin\n\n**Symptom (Tester-Report):**\n1. W allein bewegt den Spieler nicht nach oben auf dem Bildschirm, sondern diagonal (NW in iso = screen upper-right).\n2. Kombinationen wie S+A oder W+D sind doppelt so schnell wie W+A oder S+D.\n\n**Root Cause — Analyse:**\nIn der 2:1-Iso-Projektion gilt `tile_to_screen(dx,dy) = (dx*32-dy*32, dx*16+dy*16)`.\n\nAktuelles Mapping (nach Ticket 123):\n- W allein → (0,-1) → screen (+32,-16) = 35.8px — *geht nicht gerade nach oben*\n- W+A → (-1,-1) → screen (0,-32) = 32px — korrekt screen-up, aber nur als Kombination\n\nGeschwindigkeitsproblem bei Kombinationen:\n- W+A / S+D → 32px pro Schritt (screen-vertikal)\n- W+D / S+A → 64px pro Schritt (screen-horizontal) → **2× schneller**\n\n**Anforderungen:**\n- W allein soll den Spieler direkt nach oben auf dem Bildschirm bewegen (screen-up = tile(-1,-1))\n- S allein → screen-down (tile +1,+1)\n- A allein → screen-left (tile -1,+1)\n- D allein → screen-right (tile +1,-1)\n- **Keine Kombinationen mehr**: Mehrere Tasten gleichzeitig → erste/priorisierte Taste gewinnt\n- Alle 4 Richtungen erzeugen exactly 1-Tile-Schritt pro Move-Event (keine Geschwindigkeits-Diskrepanz durch Kombis)\n\n**Edge Cases:**\n- HJKL-Mappings ebenfalls anpassen (falls vorhanden)\n- Turn-before-move (Ticket 058) muss weiterhin funktionieren\n- Build-Wall [B] Key unberührt lassen\n\n**Akzeptanzkriterien:**\n- W bewegt Spieler screen-up (Iso-Diamond bewegt sich nach oben)\n- A/D/S bewegen entsprechend screen-left/right/down\n- Keine Key-Kombination erzeugt 64px-Horizontalbewegung\n- odin check src/client + src/server grün\n- Vorhandene Movement-Tests weiterhin grün (ggf. Update der Test-Erwartungen)	17	\N	2026-06-30 18:42:31.035288	2026-06-30 18:55:22.052831	\N	ticket
224	4	222b — architecture/interaction-system.md: Interaction-System-Spec	**Aufwand:** S | **Parent:** 222 | **Refs:** src/server/simulation.odin (resolve_interaction, apply_interaction), src/shared/entities.odin (InteractionKind)\n\nDokumentiert das Interaction-System: wie der Server entscheidet, was passiert wenn [space] gedrückt wird.\n\n**Scope:**\n- InteractionKind-Enum: alle Werte + Bedeutung\n- resolve_interaction: Prioritäts-Stack (Worker > Container > Terminal > GC > Resource > Fragment > Wall)\n- apply_interaction: Dispatch-Tabelle + Seiteneffekte (Cooldowns, State-Änderungen, Nachrichten)\n- Facing-Mechanic: wie target_x/target_y aus Spieler-Position berechnet wird\n- Cooldowns: attack_cooldown, interaction-spezifische Guards\n- Protokoll: MSG_INPUT_INTERACT → Ack-Nachrichten\n\n**Akzeptanzkriterien:**\n- [ ] architecture/interaction-system.md existiert\n- [ ] Prioritäts-Stack dokumentiert\n- [ ] Alle InteractionKind-Werte beschrieben	17	claude-sonnet-4-6	2026-06-30 18:57:10.796828	2026-06-30 19:03:45.642344	\N	ticket
226	4	222d — architecture/item-system.md: Item-System-Spec (ergänzt ADR 099)	**Aufwand:** S | **Parent:** 222 | **Refs:** src/shared/items.odin, adr/099-item-architecture.md\n\nADR 099 dokumentiert die Entscheidung. Diese Spec dokumentiert die laufende Implementierung für Entwickler.\n\n**Scope:**\n- Datenmodell: ItemType (enum), ItemDef (Registry-Eintrag), ItemStack, Inventory, INVENTORY_VOLUME_CAP\n- Registry: wie ItemDef-Konstanten definiert werden (Odin-Code), ItemCapability-Flags\n- Inventory-Operationen: add_item, remove_item, find_slot, volume-Berechnung\n- Fragment-Hybrid: bit_set-Fastpath für Lua, ItemStack im Inventar\n- Protokoll: wie Inventory in MSG_STATE_UPDATE serialisiert wird\n- Crafting-Integration: Rezept-Lookup, resource-cost-Prüfung\n\n**Akzeptanzkriterien:**\n- [ ] architecture/item-system.md existiert\n- [ ] Datenmodell-Diagram oder Tabelle\n- [ ] Inventory-Operationen beschrieben	17	claude-sonnet-4-6	2026-06-30 18:57:31.751087	2026-06-30 19:04:04.38352	\N	ticket
223	4	222a — architecture/README.md: Subsystem-Index (Eingangspunkt)	**Aufwand:** XS | **Parent:** 222 | **Refs:** architecture/\n\nErstellt architecture/README.md als Wegweiser für neue Entwickler und Agenten. Listet alle Subsysteme mit Kurzbeschreibung, Datei-Link zur architecture/-Spec und Link zu relevanten ADRs.\n\n**Scope:**\n- Tabellarische Übersicht aller Subsysteme (Name | Kurzbeschreibung | Spec-Datei | ADR-Links | Source-Dateien)\n- Abschnitt "Wie lese ich diese Docs" (Architektur-Doc vs. ADR vs. concept/)\n- Keine neuen Specs schreiben — nur Index\n\n**Akzeptanzkriterien:**\n- [ ] architecture/README.md existiert\n- [ ] Alle bestehenden architecture/-Dateien referenziert\n- [ ] Alle fehlenden Specs als "ausstehend" markiert	17	claude-sonnet-4-6	2026-06-30 18:57:01.411911	2026-06-30 19:03:36.752702	\N	ticket
273	3	Review-Fixes: SSE-Connection-Leak, Cross-Projekt-Relations, Special-Status-Bypass	Code-Review über den gesamten Session-Diff (V4-Integration, Inbox, Relations, Pagination, Kommentare, Markdown, SSE-Erweiterung) hat 5 Findings ergeben:\n\n1. KRITISCH: sql.unlisten() existiert nicht auf dem postgres.js Sql-Objekt — unlisten() gehört zum Rückgabewert von sql.listen(), nicht zu sql selbst. Jeder Tab-Close warf beim Cleanup eine TypeError, sql.end() wurde nie erreicht → Connection-Pool-Leak.\n2. Cross-Projekt-Relation: POST /api/tickets/[id]/relations prüfte nicht, ob to_ticket_id im selben Projekt liegt wie das Quell-Ticket.\n3. human_intervention/human_answered waren über das normale Status-Dropdown (Neues-Ticket-Formular + TicketModal-Edit + Standalone-Ticketseite) frei wählbar, obwohl das nur über die Inbox laufen soll.\n4. GET/POST /relations prüfte nicht, ob das Quell-Ticket existiert → FK-Violation als 500 statt sauberem 404.\n5. SSE-Handler konnte bei mehreren schnellen Events auf demselben Ticket (z.B. Agent postet mehrere Kommentare hintereinander) eine ältere Fetch-Response nach einer neueren anwenden (Race Condition).\n\nFixes: unlisten() korrekt über den Rückgabewert von listen() aufgerufen; project_id-Check in relations/+server.ts ergänzt; editableStatuses-Filter (special_type ausgeblendet, aktueller Status bleibt sichtbar) in TicketModal, Standalone-Ticketseite und Neues-Ticket-Formular; Existenzprüfung fürs Quell-Ticket in GET/POST relations; Sequenz-Zähler pro Ticket-ID im SSE-Handler verwirft veraltete Responses.	12	\N	2026-07-01 21:01:09.81167	2026-07-01 21:11:01.728537	\N	ticket
227	4	222e — architecture/biome-system.md + gc-ai.md: Biome + GC-AI-Specs	**Aufwand:** S | **Parent:** 222 | **Refs:** src/server/world_gen.odin, src/server/simulation.odin (advance_gcs, gc-Tier-Logic), src/server/spawn.odin\n\nZwei kleinere Specs in einem Ticket.\n\n**Biome-System (architecture/biome-system.md):**\n- Biome-Enum: None/Heap/Stack/Cache/Registers + thematische Bedeutung\n- _biome_for_chunk: Downscale (÷3), _world_hash, 4-Biome-Aufteilung\n- _tile_type_for_biome: skew-Bits 20-21, 50% biome-forced / 50% dist-fallback\n- RESOURCE_RESPAWN_TICKS: Threshold-Pass vs. Background-Pass\n- harvested_tiles: sparse map [2]i32→u64, Lifecycle\n\n**GC-AI (architecture/gc-ai.md):**\n- Tier-System: L1 Scrubber / L2 Reclaimer / Boss (integrity-Werte, Speed, Spawn-Regeln)\n- advance_gcs: Greedy-Step + Slide-Fallback, Wall-Kollision\n- Sweep-Mechanik: L1/L2 Sweep-Radius, Schaden pro Tick\n- Spawn: player-relative, Entropie-skaliert, ARTIFACT_RESPAWN_INTERVAL\n\n**Akzeptanzkriterien:**\n- [ ] architecture/biome-system.md existiert\n- [ ] architecture/gc-ai.md existiert\n- [ ] Tier-Tabelle (L1/L2/Boss) in gc-ai.md	17	claude-sonnet-4-6	2026-06-30 18:57:45.27291	2026-06-30 19:04:14.797044	\N	ticket
225	4	222c — architecture/render-pipeline.md: Client Render-Pipeline-Spec	**Aufwand:** M | **Parent:** 222 | **Refs:** src/client/render.odin, adr/106-isometric-rendering.md\n\nDokumentiert die Client-Render-Pipeline: Iso-Tile-Rendering, Entity-Rendering, HUD, Shader.\n\n**Scope:**\n- Koordinatensystem: tile_to_screen(tx,ty) Formel, Iso-Grid-Geometrie (64×32 Diamond)\n- Render-Reihenfolge: Background → Tiles → Zone-Overlay → Entities (Z-Sort) → Effects → HUD\n- Iso-Tile-Rendering: draw_iso_diamond, draw_iso_tile, Biom-Tinting (ausstehend 124d)\n- Entity-Rendering: draw_players, draw_workers, draw_gcs, draw_artifacts (Z-Sort-Regel)\n- Effect-System: Bitrot-Partikel, GC-Halo, Zone-Flicker (110b-g)\n- HUD: draw_hud, Terminal-Overlay, Inventory-Overlay, Fragment-Overlay\n- Shader: CRT-Bloom (051fa), RenderTexture-Pipeline\n- Area-of-Interest: wie AoI-Chunks bestimmen was gerendert wird\n\n**Akzeptanzkriterien:**\n- [ ] architecture/render-pipeline.md existiert\n- [ ] Koordinatensystem-Formel dokumentiert\n- [ ] Render-Reihenfolge als Tabelle oder Diagramm	17	claude-sonnet-4-6	2026-06-30 18:57:22.754834	2026-06-30 19:03:54.9413	\N	ticket
242	1	Kein Batch-Tool für mehrere Tickets — viele sequentielle Einzelaufrufe bei Triage-Arbeit	**Erfahrung:** Beim Triagieren von 5 Bug-Tickets in einer Session (Kernel-Panic #231-235) brauchte es für jedes Ticket einzeln: update_ticket (description), add_comment (×2 typischerweise), move_ticket (×2), assign_ticket, mehrere add_task-Calls. Macht in Summe ~35-40 einzelne Tool-Calls für 5 Tickets. Viele davon sind zwar parallelisierbar (mehrere Tool-Calls in einer Antwort), aber es gibt kein Tool, das z.B. "wende diese Tasks/Kommentare auf mehrere Ticket-IDs gleichzeitig an" oder "verschiebe diese Liste von Tickets in einem Aufruf" anbietet.\n\n**Konkreter Vorschlag:**\n1. `move_tickets(ticket_ids: []number, new_status_id)` — Batch-Variante von move_ticket für den Fall "mehrere Tickets erreichen gleichzeitig denselben Workflow-Schritt" (z.B. eine ganze Sprint-Charge wird gemeinsam nach todo verschoben).\n2. Kein Muss für add_task/add_comment, da die pro Ticket meist unterschiedlichen Text brauchen — aber move/assign sind oft identisch über mehrere Tickets hinweg.\n\n**Auswirkung auf Agenten:** Mehr Tool-Calls bedeuten mehr Tokens (jeder Call + Response kostet Kontext) und mehr Latenz pro Roundtrip. Bei Batch-Triage-Arbeit (häufiger Anwendungsfall für AI-Agenten, die ein Projekt "aufräumen") summiert sich das spürbar.	5	claude-sonnet-5	2026-06-30 22:46:20.677893	2026-07-01 15:11:42.489583	\N	ticket
243	1	Keine Timestamps in Ticket-/Kommentar-Daten sichtbar — erschwert Staleness-Einschätzung	**Erfahrung:** Weder get_ticket_detailed noch list_tickets noch list_comments liefern (soweit in dieser Session beobachtet) created_at/updated_at-Zeitstempel in den Tool-Ergebnissen. Bei der Frage "ist Ticket #37 (in_progress seit wann?) noch aktiv oder verwaist?" musste ich mich auf Datumsangaben im Freitext der Kommentare verlassen ("Status-Update — 2026-06-30"), die der Agent händisch eingetragen hat — kein verlässliches Datenfeld.\n\n**Konkreter Vorschlag:**\n1. `created_at`/`updated_at` (und idealerweise `status_changed_at`) als strukturierte Felder in den Ticket- und Kommentar-Antworten zurückgeben.\n2. Damit ließe sich ein einfaches Hygiene-Tool bauen: "zeig mir alle Tickets die seit > N Tagen in in_progress hängen" — aktuell nur durch manuelles Lesen aller Kommentare möglich.\n\n**Auswirkung auf Agenten:** Ohne verlässliche Zeitstempel ist jede Aussage über Ticket-Alter/Staleness eine Vermutung basierend auf Freitext, nicht eine Abfrage.	5	claude-sonnet-5	2026-06-30 22:46:21.872514	2026-07-01 15:11:44.549128	\N	ticket
222	4	222: Subsystem-Dokumentation — Lücken schließen, Architektur-Übersicht erstellen	**Aufwand:** L | **Refs:** architecture/, adr/, docs/\n\n**Ziel:** Alle produktiv genutzten Subsysteme haben eine klare, aktuelle Architekturdokumentation. Neue Entwickler (Mensch oder Agent) können ein Subsystem ohne Codesuche verstehen.\n\n**Subsystem-Inventar (Ist-Zustand):**\n\nDokumentiert (architecture/):\n- ✅ server_client_architecture.md — Gesamtarchitektur\n- ✅ multiplayer-protocol.md — TCP-Protokoll + Nachrichtenformat\n- ✅ worker-scripting.md — Lua-Worker-API\n- ✅ player-state.md — Spieler-Datenmodell\n- ✅ spawn-system.md — Spawn + Respawn\n- ✅ entropy-and-bitrot.md — Entropie-Grid + Bitrot\n- ✅ world_and_resources.md — Welt, Tiles, Ressourcen\n- ✅ header-fragments.md — Fragment-Mechanik\n- ✅ input-routing.md — Client Input-Kontext-Routing\n\nNicht oder nur als ADR dokumentiert (Lücken):\n- ❌ **Render-Pipeline** — render.odin (~1200 Zeilen): Iso-Tile-Rendering, Entity-Rendering, HUD, CRT-Shader, Z-Sorting; größte Lücke\n- ❌ **Interaction-System** — resolve_interaction + apply_interaction (simulation.odin): Prioritäts-Stack, InteractionKind-Enum, Cooldowns\n- ❌ **Crafting-System** — Rezepte, resource-cost, Terminal-Crafting, Worker/Container-Craft\n- ❌ **Biome-System** — world_gen.odin: Biom-Zonen, biome_for_chunk, tile_type_for_biome (Ticket 124)\n- ❌ **GC-AI** — Tier-System (L1/L2/Boss), advance_gcs, pathfinding, Sweep/Halo-Visuals\n- ❌ **Item-System** — ADR 099 vorhanden, aber kein architecture/-Spec für ItemType/ItemDef/Inventory/ItemCapability\n- ❌ **Container-System** — MSG_CONTAINER_* Protokoll, Server-Handler, Client-Overlay\n- ❌ **Cave-System** — Cave_Entrance, Worldgen-Placement, Pointer_Key-Mechanik (Ticket 100)\n\nZentrale Übersicht fehlend:\n- ❌ **architecture/README.md** — Subsystem-Index mit Querverweisen (Eingangspunkt für neue Entwickler)\n\n**Priorisierung (Mensch-Entscheidung oder Standardprio):**\n1. architecture/README.md (Wegweiser)\n2. Render-Pipeline (größte Lücke, größtes System)\n3. Interaction-System (zentrale Spielmechanik)\n4. Item-System-Spec (ADR vorhanden, Spec fehlt)\n5. GC-AI, Biome, Crafting, Container (kleinere Lücken)\n6. Cave-System (noch in Entwicklung, Doku nach Fertigstellung)\n\n**Akzeptanzkriterien:**\n- architecture/README.md existiert und listet alle Subsysteme mit Datei-Link\n- Render-Pipeline-Spec in architecture/render-pipeline.md\n- Interaction-System-Spec in architecture/interaction-system.md\n- Item-System-Spec in architecture/item-system.md\n- Biome-System-Spec in architecture/biome-system.md\n- GC-AI-Spec in architecture/gc-ai.md\n- Crafting-Spec in architecture/crafting-system.md	17	claude-sonnet-4-6	2026-06-30 18:44:53.74219	2026-06-30 19:04:23.430536	\N	ticket
231	4	BUG: container/crate does not have a visual representation	**Symptom (Reporter):** "it's invisible in game" — crafted containers/crates don't render.\n\n**Concept summary:** Containers are a deployable storage object (see `architecture/inventory-system.md`, kbai #229, #117). Players craft a container, it should appear as a visible world entity they can walk up to and interact with.\n\n**Likely already resolved:** kbai #229 ("BUG: boxes/crates are craftable but do not spawn or are placeable", done, commit 5d38bc7) added the missing `draw_containers` proc to `src/client/render.odin` and wired it into `draw_entities`. Root cause there was identical: server created/encoded/decoded containers correctly, client simply never drew them.\n\n**Cross-references:** `architecture/inventory-system.md`, `architecture/render-pipeline.md`, kbai #229 (done).\n\n**Success criteria:** Confirm in a live client session that a freshly crafted container renders visibly on the tile it was placed on. If still invisible after #229's fix, re-open with concrete repro steps (which container, placement method, client version/commit).	15	claude-sonnet-5	2026-06-30 19:42:36.360405	2026-06-30 22:30:35.678149	\N	ticket
272	3	Review-Fund: Epic-Erstellung + Relation-Richtungslabel-Bug	Beim Review von Ticket 270/271 (V4-Integration) zwei Lücken gefunden:\n\n1. BUG: In TicketModal.svelte ist die Richtungsbezeichnung für eingehende Relationen fehlerhaft. Der Code macht `relationLabels[rel.relation_type].replace(/^ist /, '')` + "-Ziel von" als Hack, was bei parent_of zu "ist Parent von-Ziel von" führt (Unsinn). Braucht getrennte Label-Maps für outgoing/incoming.\n\n2. UX-Lücke: "Neues Ticket"-Formular (src/routes/projects/[id]/tickets/new/+page.svelte) hat kein Typ-Feld (ticket/epic). Epics lassen sich nur nachträglich per Edit im TicketModal erstellen — nicht auffindbar. Außerdem gibt es keine Möglichkeit, beim Anlegen eines Tickets direkt einen Parent-Epic auszuwählen (müsste die Relation danach manuell im TicketModal nachtragen).\n\nFix: Typ-Select + optionales "Parent-Epic"-Dropdown im Neues-Ticket-Formular ergänzen (bei Auswahl automatisch parent_of-Relation anlegen), und die Relation-Labels korrigieren.	12	\N	2026-07-01 20:49:08.986195	2026-07-01 21:10:49.978148	\N	ticket
278	1	MCP-Tool zum Bearbeiten bestehender Board-Statuses fehlt	Beim Arbeiten an kbai-ui (Projekt 3) wollte ich die agent_role_instruction bestehender Spalten (backlog/todo/in_progress/review) ergänzen, um dort auf den human_intervention-Eskalationsweg hinzuweisen. Der MCP-Server bietet dafür kein Tool.\n\nVorhanden: kb_ai_create_board_status (nur Neuanlage), kb_ai_list_board_statuses (nur Lesen).\nFehlt: ein kb_ai_update_board_status (project_id, status_id, display_name?, position?, agent_role_instruction?) analog zu kb_ai_update_ticket.\n\nOhne dieses Tool kann ein Agent bestehende Spalten-Konfigurationen (insbesondere agent_role_instruction) nur über die kbai-ui-Weboberfläche selbst ändern, nicht programmatisch über MCP — das bricht den sonst durchgängigen "Agent verwaltet Board selbst"-Ansatz.\n\nKonkret aufgefallen: sowohl in Projekt 3 (kbai-ui) als auch in diesem Projekt (Kanban AI, Projekt 1) haben die normalen Spalten (backlog/ready/in_progress/in_review) keinerlei Hinweis in ihrer agent_role_instruction, dass ein Agent bei Bedarf nach "human_intervention" eskalieren kann. Das lässt sich aktuell nicht per MCP nachtragen.	1	\N	2026-07-01 21:29:18.422794	2026-07-01 21:29:18.422794	\N	ticket
229	4	BUG: boxes/crates are craftable but do not spawn or are placeable	crafting boxes/crates just consumes resources, but nothing happens	17	claude-sonnet-4-6	2026-06-30 19:07:15.420993	2026-06-30 19:30:19.56747	\N	ticket
228	4	subsystem: inventar	Das inventar soll ein eigenes subsystem sein	17	claude-sonnet-4-6	2026-06-30 19:05:50.149428	2026-06-30 19:34:18.034887	\N	ticket
230	4	subsystem: for ui	we need a ui subsystem, with modals, hud, etc. so we have standardized components to use later on\n	17	claude-sonnet-4-6	2026-06-30 19:08:13.257277	2026-06-30 19:38:21.422239	\N	ticket
233	4	bug: movent in isometric does not have propper colission	**Symptom (Reporter):** "if the player stands between two occupied tiles which he cant move through, (one tile to the bottom of him, one tile to the left of him) - he can do a horizontal movement to get past the 'wall'. that should not be possible in any direction" — classic corner-cutting through diagonally-adjacent solid tiles.\n\n**Root cause (code-confirmed):** `_apply_move` in `src/server/simulation.odin:1099-1138`. Collision check (lines 1132-1136) only validates the destination tile `(nx, ny)`:\n```\ntarget := shared.get_tile(&state.world, nx, ny)\nif !shared.tile_is_solid(target.type) {\n    p.x = nx\n    p.y = ny\n}\n```\nFor diagonal input (dx≠0 AND dy≠0), it never checks the two axis-aligned intermediate tiles `(nx, p.y)` and `(p.x, ny)`. If both are solid but the diagonal destination happens to be open, the player slides through the corner.\n\n**Existing test coverage:** `src/server/movement_test.odin:96-106` (`test_move_diagonal_blocked_by_solid`) covers a solid *destination* tile, but not the corner-cut case (both intermediates solid, destination open).\n\n**Success criteria:**\n1. Diagonal movement is rejected when *either* axis-aligned intermediate tile is solid, matching standard tile-grid corner-cutting prevention.\n2. New unit test: player at (x,y), solid tile at (x-1,y) and (x,y+1) [or equivalent down+left example from the report], diagonal move to (x-1,y+1) must be rejected even if (x-1,y+1) itself is open.\n3. Existing `test_move_diagonal_blocked_by_solid` still passes.\n\n**Cross-references:** `architecture/` has no dedicated movement-collision spec yet — Architect should decide whether this warrants a short addendum to an existing doc or is small enough for inline comment + ADR-skip (Hard Negative #9 in AGENTS.md re: not obvious from code argues for at least a one-line ADR given this is a behavior change other agents could silently regress).	17	claude-sonnet-5	2026-06-30 19:47:34.806256	2026-06-30 22:52:11.916728	\N	ticket
253	4	108b: Live Worker Code auf Terminal-Panels	**Ausgegliedert aus #31/108 (Architect-Pass, siehe dortiger Kommentar).** Variante (b) der Mensch-Entscheidung vom 2026-06-23: Terminal-Panels zeigen die aktuell ausgeführte Code-Zeile des nächsten aktiven Workers.\n\n**Offene Architektur-Frage (nächster Architect-Schritt):** Server trackt aktuell keine "aktuelle Zeile" pro Worker-Script-Ausführung (geprüft: kein current_line/active_line-Feld in scripting.odin/WorkerScriptContext). Braucht:\n1. Server-seitiges Tracking der Lua-Coroutine-Position (vermutlich via lua.getinfo o.ä. beim yield-Punkt in lua_move_forward/turn/mine).\n2. Übertragungsmechanismus zum Client — Snapshot-Erweiterung (jeder Worker bekommt ein current_line:u16-Feld) vs. separate Nachricht bei Zeilenwechsel (weniger Traffic, aber komplexer).\n3. Terminal-zu-Worker-Zuordnung: "nächster aktiver Worker" — welcher Worker, wenn mehrere in Terminal-Nähe sind? Tie-Breaking-Regel nötig.\n\n**Cross-References:** kbai #31/108a (Ambient Decoration, Schwester-Ticket, done), `architecture/worker-scripting.md`, `architecture/multiplayer-protocol.md`.\n\n**Aufwand:** M-L (Server-Tracking + Protokoll-Erweiterung) — muss vor Developer-Pickup durch Architect verfeinert werden.	13	claude-sonnet-5	2026-06-30 23:57:11.89459	2026-06-30 23:57:23.346299	\N	ticket
232	4	bug: container interface buggy	**Symptom (Reporter):** Container overlay (`[e]` on a container → split-view, see #117/#228) lets you deposit/withdraw successfully, but cursor navigation breaks down once *both* panes (player inventory and container) hold items. Reporter: "if you have items in both ... you can't select anything, because up/down stops working. there should be a way to move the cursor between inventory and container."\n\n**Concept summary:** `draw_container_overlay` (`src/client/inventory.odin:156`) is a two-pane view: left = player inventory (`cs.inventory_selected`), right = container inventory (`cs.container_inv_selected`). The reporter expects `[up/down]` to navigate whichever pane has focus, plus a way to switch focus between panes.\n\n**Root cause (code-confirmed):** `_handle_container_keys` (`src/client/input.odin:179-227`) only ever updates `cs.container_inv_selected` on Up/Down. `cs.inventory_selected` — used by `[d]` deposit to pick the source item — is never touched while the container overlay is open; it retains whatever value it last had from the standalone inventory overlay (`[i]`), which may be stale, out of bounds for the current player inventory size, or pointing at the wrong item entirely. There is no key binding to switch which pane Up/Down controls. This matches the reporter's "can't select anything" symptom for the player-inventory side.\n\n**Success criteria:**\n1. A dedicated key (candidate: `[Tab]` or `[Left/Right]`) toggles focus between player-inventory pane and container pane.\n2. `[Up/Down]` navigates whichever pane currently has focus.\n3. The focused pane is visually indicated (e.g. brighter border/highlight) — ui_panel/UI_CYAN from the UI subsystem (#230) can be reused here.\n4. `[d]`/`[w]` continue to use the currently-selected slot in their respective (correct) pane regardless of which pane has focus.\n\n**Cross-references:** `architecture/inventory-system.md` (container overlay section), `architecture/ui-system.md` (this overlay is NOT yet migrated — listed as TODO; migrating it could happen in the same ticket as this fix since both touch `draw_container_overlay`/`_handle_container_keys`).	17	claude-sonnet-5	2026-06-30 19:45:00.038338	2026-06-30 22:50:48.769901	\N	ticket
254	4	108c: Echo Surfaces auf Terminal-Panels	**Ausgegliedert aus #31/108 (Architect-Pass, siehe dortiger Kommentar).** Variante (c) der Mensch-Entscheidung vom 2026-06-23: verlassene Terminals zeigen Code-Echos toter/vergangener Spieler.\n\n**Offene Architektur-Frage (nächster Architect-Schritt):** Das bestehende Echo-System (#036, EchoRecord) liefert aktuell proximity-getriggerte, transiente Toast-Nachrichten (MSG_ECHO_DELIVERY → cs.pending_echoes im Client, geprüft in network.odin) — keine persistente Orts-Bindung, die ein Terminal-Panel dauerhaft anzeigen könnte. Braucht:\n1. Sektor↔Terminal-Assoziation: welche EchoRecords gehören "zu" einem bestimmten Terminal (räumliche Nähe? Sektor-Match?).\n2. Anzeige-Mechanismus: Terminal-Panel zeigt den letzten/relevantesten Echo dauerhaft statt als Toast — braucht Client-seitigen State pro Terminal, nicht nur eine globale pending-Queue.\n3. Persistenz: bleiben Echo-Surfaces nach Server-Neustart erhalten? (Echo-Persistenz-Status prüfen.)\n\n**Cross-References:** kbai #31/108a (Ambient Decoration, Schwester-Ticket, done), `architecture/header-fragments.md` (Echo-System-Spec, falls vorhanden — sonst Researcher-Aufgabe).\n\n**Aufwand:** M (Sektor-Assoziation + neuer Client-State) — muss vor Developer-Pickup durch Architect verfeinert werden.	13	claude-sonnet-5	2026-06-30 23:57:19.1343	2026-06-30 23:57:25.261297	\N	ticket
277	3	BUG: Ui meldet ab und an Fehler 500	wodurch der fhler getriggert wird ist unbekannt - exploriere. hier die fehlermeldung:\n[vite] Error when evaluating SSR module /src/lib/components/tickets/TicketModal.svelte: failed to import "isomorphic-dompurify"\n|- Error [ERR_REQUIRE_ESM]: require() of ES Module /home/david/Projekte/Sprachen/Javascript/kbai-ui/node_modules/@exodus/bytes/encoding-lite.js from /home/david/Projekte/Sprachen/Javascript/kbai-ui/node_modules/isomorphic-dompurify/node_modules/html-encoding-sniffer/lib/html-encoding-sniffer.js not supported.\nInstead change the require of encoding-lite.js in /home/david/Projekte/Sprachen/Javascript/kbai-ui/node_modules/isomorphic-dompurify/node_modules/html-encoding-sniffer/lib/html-encoding-sniffer.js to a dynamic import() which is available in all CommonJS modules.\n    at TracingChannel.traceSync (node:diagnostics_channel:315:14)\n    at Object.<anonymous> (/home/david/Projekte/Sprachen/Javascript/kbai-ui/node_modules/isomorphic-dompurify/node_modules/html-encoding-sniffer/lib/html-encoding-sniffer.js:2:41)	12	\N	2026-07-01 21:28:11.876592	2026-07-01 21:40:56.958603	\N	ticket
236	4	234b: Diagonal player sprite art + frame wiring	**Concept summary:** Follow-up to #234 (8-directional facing/targeting). #234 ships diagonal facing with sprite fallback to nearest cardinal frame (no new art). This ticket adds actual diagonal-facing sprite frames (NE/SE/SW/NW) and wires them into draw_player_sprite (src/client/render.odin:298-331), replacing the cardinal-fallback mapping documented in #234's ADR.\n\n**Not urgent:** Current fallback (#234) is functionally correct — diagonal targeting/movement works, only the visual facing is approximated to the nearest cardinal sprite. This is a polish ticket.\n\n**Success criteria:**\n1. Sprite sheet extended with 4 diagonal frames (idle/walk variants per existing convention — check current sheet layout before adding).\n2. draw_player_sprite frame-index switch covers all 8 Direction values directly.\n3. ADR from #234 updated or superseded noting the fallback is now replaced.\n\n**Cross-references:** kbai #234 (prerequisite, must be done first), architecture/render-pipeline.md.	13	claude-sonnet-5	2026-06-30 22:36:47.47254	2026-06-30 22:36:51.085488	\N	ticket
239	1	Kein delete_ticket / merge_tickets-Tool	**Erfahrung:** Nachdem ich die Duplikate #190/#195 (siehe Schwester-Ticket "Keine Such-/Duplicate-Detection") gefunden hatte, konnte ich sie nicht bereinigen — es gibt kein delete_ticket und kein merge_tickets im MCP-Tool-Set. Ich musste stattdessen einen Kommentar auf beiden Tickets hinterlassen und den Menschen bitten, manuell aufzuräumen. Das ist für eine vom Agenten selbst gefundene und diagnostizierte Daten-Inkonsistenz ein unbefriedigender Endzustand.\n\n**Konkreter Vorschlag:**\n1. `delete_ticket(ticket_id, reason)` — mit Pflichtfeld `reason` fürs Audit-Log, ggf. nur erlaubt wenn Ticket in `done` oder explizit als `duplicate` markiert ist (Sicherheitsnetz gegen versehentliches Löschen von aktiver Arbeit).\n2. Besser: `merge_tickets(source_id, target_id)` — verschiebt Kommentare/Tasks von source nach target und schließt source mit automatischem Verweis-Kommentar "merged into #target". Erhält die Historie statt sie zu löschen.\n\n**Auswirkung auf Agenten:** Agenten können Probleme finden, aber nicht vollständig selbst beheben — jede Hygiene-Aktion endet als Eskalation an den Menschen, auch wenn die richtige Aktion eindeutig ist (zwei identische done-Tickets zusammenführen ist kein Graubereich).	5	claude-sonnet-5	2026-06-30 22:45:41.535164	2026-07-01 15:11:35.540727	\N	ticket
257	4	doc: ADR-101/ADR-032 fehlt lockstep-Config-Feld + Persistence-Versionshistorie	**Code-Review-Fund (2026-07-01).**\n\n1. `ServerConfig` (src/server/config.odin:9-17) hat ein Feld `lockstep` (story 114b), das in `adr/101-server-config-file.md` (V1-Spec, 6 Felder dokumentiert, Code hat 7) nicht erwähnt wird — weder im Config-File-Format noch bei den Env-Vars.\n2. `src/server/persistence.odin` ist bei Binärformat-Version 4 (v2: home_x/home_y, v3: Inventory-Format-Wechsel, v4: Tile.biome — alles nur als Inline-Kommentare bei Zeilen 14/18/20 dokumentiert). `adr/032-server-persistence-binary-format.md` beschreibt nur Version 1, keine Migrations-Historie.\n\n**Success Criteria:** ADR-101 um `lockstep` ergänzt. ADR-032 (oder neue ADR) dokumentiert v1→v4 Versions-Historie mit Breaking-Changes pro Version.\n\n**Aufwand:** S.	17	claude-sonnet-5	2026-07-01 00:05:48.197243	2026-07-01 14:36:46.699899	\N	ticket
234	4	BUG: indicator for vieweing direction is skewed	**Symptom (Reporter):** "indicator for viewing direction is skewed. it should be possible to look at the tile in either diagonal direction. that makes movement and tile selection much more easier."\n\n**Scope (post-Architect-review, see comments):** Direction enum + facing/targeting logic only. Diagonal sprite art is split out to a separate backlog ticket (234b, not yet created) — this ticket ships with sprite fallback to nearest cardinal frame for diagonal facings.\n\n**Root cause:** `Direction` enum (`src/shared/entities.odin:3`) has only 4 cardinal values. `direction_delta` (`entities.odin:12-20`) is the single chokepoint for ALL interaction-targeting facing-tile math (containers, terminals, walls, worker pickup — simulation.odin:233-238, 267-272, 817, 958-963 — plus the client facing-indicator, render.odin:238-243). Client (input.odin:323-329) and server (simulation.odin:1122-1128) facing-calc both discard dx whenever dy≠0, collapsing diagonal movement to a cardinal facing.\n\n**Effort:** M (2-5h) — single enum extension + one chokepoint function + 2 facing-calc call sites + sprite-fallback mapping decision, but touches protocol-adjacent shared state so needs care and an ADR.\n\n**Success criteria:**\n1. `Direction` enum: North, NorthEast, East, SouthEast, South, SouthWest, West, NorthWest.\n2. Client + server facing-calc preserve both dx and dy for diagonal inputs.\n3. `direction_delta` returns correct diagonal offsets; all targeting call sites (containers/terminals/walls/worker-pickup) work correctly for diagonal-adjacent tiles.\n4. Facing-indicator diamond renders correctly at all 8 positions (verify tile_to_screen offsets for diagonals, not just cardinals).\n5. Sprite selection (draw_player_sprite, render.odin:298-331) maps each diagonal to its nearest cardinal frame — documented choice (ADR), not a silent guess.\n6. New `adr/` entry recording the enum extension + sprite-fallback decision.\n7. `234b` (diagonal sprite art) filed as new backlog ticket by whichever role closes this one, referencing this ticket.\n\n**Cross-references:** `architecture/player-state.md` (owns Direction/facing — update on close), kbai #209 (done, distinct — coordinate drift not direction coverage), kbai #123 (done — movement input, the targeting counterpart to this ticket).	17	claude-sonnet-5	2026-06-30 19:48:25.364238	2026-06-30 22:57:47.155672	\N	ticket
259	4	doc: world_and_resources.md listet nur 6 von 11 TileTypes	**Code-Review-Fund (2026-07-01).** `architecture/world_and_resources.md` § 2 (Zeilen 15-25) listet nur Empty, Byte, CPU_Cycle, RAM_Chip, Variable, Pointer.\n\n`src/shared/world.odin:3-20` hat zusätzlich: `Wall` (story 059), `Crystal_Bytes`, `Crystal_CPU`, `Crystal_RAM` (story 087), `Cave_Entrance` (story 100a) — alles spielmechanisch relevante Tile-Typen (Wandbau, Crystal-Mining, Höhlen), die in der Resource-Spec komplett fehlen.\n\n**Success Criteria:** Alle 11 TileTypes in world_and_resources.md dokumentiert, inkl. solid/mineable/Sonderverhalten je Typ.\n\n**Aufwand:** S.	17	claude-sonnet-5	2026-07-01 00:05:58.866085	2026-07-01 14:36:59.860562	\N	ticket
241	1	Keine strukturierten Ticket-Relationen (parent/child, blocks, duplicate-of)	**Erfahrung:** Im Projekt "Kernel Panic" werden Parent/Child- und Dependency-Beziehungen ausschließlich als Freitext in der description geführt ("**Parent:** 006", "**Depends:** 100a, 099 (done)", "Cross-references: kbai #234"). Das funktioniert für einen lesenden Menschen, aber nicht für einen Agenten, der z.B. "alle offenen Kinder von Ticket 103" oder "alle Tickets die auf #234 warten" abfragen will — er müsste wieder den kompletten list_tickets-Dump laden und Freitext parsen.\n\nKonkret aufgefallen bei: Ticket #234 wurde in #234a (umbenannt zu #234 selbst) und #234b (= neues Ticket #236) zerlegt — die Beziehung "236 ist Kind von 234, blockiert auf 234" existiert nur als Text in beiden descriptions, nicht als Datenbankfeld.\n\n**Konkreter Vorschlag:**\n1. `link_tickets(from_id, to_id, relation)` mit relation ∈ {parent_of, blocks, duplicate_of, relates_to}.\n2. `get_ticket_detailed` liefert dann zusätzlich ein `relations` Array statt dass jeder Agent die description nach Mustern wie "Parent:" durchsucht.\n3. Praktischer Nebeneffekt: Roof/Epic-Tickets (siehe Kernel-Panic-Projekt, Tickets mit "[Roof]" im Titel) könnten ihren Fertigstellungs-Status automatisch aus dem Status ihrer Kinder ableiten, statt dass ein Agent das manuell in einem Kommentar zusammenfasst.\n\n**Auswirkung auf Agenten:** Beziehungen sind aktuell nur durch volles Lesen + Text-Pattern-Matching auffindbar — teuer und fehleranfällig (ein Tippfehler im Freitext-Verweis bricht die Kette unsichtbar).	5	claude-sonnet-5	2026-06-30 22:46:03.145745	2026-07-01 15:29:06.300226	\N	ticket
235	4	BUG: diagonal movement (left/right) still feels much more fast than other directions	**Symptom (Reporter):** "diagonal movement (left/right) still feels much more fast than other directions. this might be due to perspective, but is still disruptive for gameplay."\n\n**Root cause (code-confirmed):** Movement speed is purely client-paced, not server-throttled. `src/client/input.odin:18` `MOVE_REPEAT_INTERVAL :: f32(0.10)` gates how often the client sends `MSG_INPUT_MOVE` (10 tiles/sec). `_apply_move` (server) has no independent rate limit — it executes exactly 1 tile per received message regardless of cardinal/diagonal. Since diagonal sends arrive at the same 100ms cadence as cardinal sends, but cover √2× the Euclidean distance per tile, diagonal movement is √2× ≈ 1.41× faster in practice.\n\n**Fix (effort XS):** At the send site (`input.odin`, where `is_diagonal := mv_dx != 0 && mv_dy != 0` is already computed, ~line 329), scale the post-send `cs.move_timer = MOVE_REPEAT_INTERVAL` assignment by `√2` when `is_diagonal`. Pure client-side fix — no server, protocol, collision (#233), or GC-pursuit change needed.\n\n**Success criteria:**\n1. New named constant (e.g. `MOVE_DIAGONAL_FACTOR :: f32(1.41421356)`) next to `MOVE_REPEAT_INTERVAL`.\n2. Diagonal move sends use `MOVE_REPEAT_INTERVAL * MOVE_DIAGONAL_FACTOR` as the next `move_timer`; cardinal sends unchanged.\n3. Manual playtest: moving diagonally for N seconds covers the same Euclidean distance as moving cardinally for N seconds (within float tolerance).\n\n**Cross-references:** `architecture/player-state.md` or a new `architecture/input-routing.md` note (check which owns movement pacing — `input-routing.md` exists and owns `src/client/input.odin` per `architecture/README.md`), kbai #233 (sibling ticket, diagonal collision — different file, no conflict).	17	claude-sonnet-5	2026-06-30 19:49:48.710125	2026-06-30 23:00:14.612299	\N	ticket
245	4	BUG: container still not visible	**Symptom (Reporter):** "can't see the crafted container and often cant even interact with it. plz fix" — filed after kbai #229 (believed to have fixed this) was already merged.\n\n**Root cause (code-confirmed, supersedes #229's diagnosis):** `draw_containers` (`src/client/render.odin:1090-1106`) computes its screen position with the legacy top-down formula `tile * TILE_SIZE`, while the camera, terrain, players, and workers all use the isometric `tile_to_screen(tx,ty) = ((tx-ty)*32, (tx+ty)*16)` transform (since kbai #106). `cs.camera.target` is set from a player's `tile_to_screen`-derived `display_pos` (`render.odin:461`). A container at, say, tile (20,15) renders at raw pixel `(800,600)` but the camera expects iso-space `((20-15)*32, (20+15)*16) = (160,560)` — completely different screen location relative to where the camera is actually centered. #229 added the `draw_containers` call but copied the position-calc pattern from `draw_terminals`, which has the *same* pre-existing bug — so #229 fixed "no draw call" but not "wrong draw position," and the container remained effectively invisible (or visible only by coincidence near the world origin).\n\n**Confirmed NOT broken:** Workers use a lerp system (`_update_worker_lerps`, `render.odin:206-222`) that correctly computes its target via `tile_to_screen(w.x, w.y)` — only a transient one-frame fallback at `render.odin:794` uses raw `TILE_SIZE`, which is not user-visible. Players are unaffected (`tile_to_screen` used directly, `render.odin:179`). Terrain is unaffected.\n\n**Also confirmed broken by the same root cause (NOT in this ticket's scope — filed separately, see kbai #247):** `draw_terminals`, `draw_artifacts`, `draw_gcs` all use the same incorrect `tile * TILE_SIZE` pattern with no lerp/iso correction.\n\n**Success criteria:**\n1. `draw_containers` uses `tile_to_screen(c.x, c.y)` instead of `i32(c.x) * TILE_SIZE`.\n2. Crafted container renders at the iso screen position matching the facing tile the player crafted it on (visually adjacent to the player, not at an unrelated location).\n3. Container remains interactable at that same (now-correct) visual position — confirms the "often can't interact" symptom was a visual/mental-model mismatch, not a separate server bug (server-side facing-tile math was already confirmed correct in the original #234 investigation).\n\n**Cross-references:** `architecture/render-pipeline.md` (owns coordinate transforms), kbai #229 (done, incomplete fix — this ticket completes it), kbai #106 (isometric rewrite — root cause of the inconsistency), kbai #247 (sibling ticket, same root cause in draw_terminals/draw_artifacts/draw_gcs).	17	claude-sonnet-5	2026-06-30 23:04:01.466997	2026-06-30 23:12:55.843438	\N	ticket
248	4	container's useless because cant store stuff	**Symptom (Reporter):** "the container is now visible as 'C' glyphe [confirms #245's fix worked], but i cant store stuff. if i press 'd', nothing happens"\n\n**Root cause (code-confirmed):** `_apply_container_transfer` (`src/server/simulation.odin:680-715`) gates deposit/withdraw on an adjacency check at line 692-695:\n```\ndx := c.x - p.x; if dx < 0 do dx = -dx\ndy := c.y - p.y; if dy < 0 do dy = -dy\nif dx + dy > 1 do return\n```\nThis is **Manhattan distance ≤ 1**, which only accepts the 4 cardinal neighbors (dx+dy=1) or the same tile (0). A diagonal neighbor has Manhattan distance 2 (e.g. dx=1,dy=1) and is silently rejected — the proc just `return`s with no error message to the client, matching the reporter's "nothing happens."\n\n**Why this is newly/commonly broken:** Container crafting's facing-tile math (`_apply_craft_container`) now supports all 8 directions since kbai #234 (this session). Worse: the iso WASD control scheme (kbai #221) makes a **single key press already diagonal in tile-space** (`input.odin` comment: "All single-key moves have dx≠0&&dy≠0"), so diagonal facing is the *common* case, not an edge case. Most freshly-crafted containers will sit at a diagonally-adjacent tile, where this check always silently rejects deposit/withdraw. This is a regression introduced by #234 interacting badly with pre-existing Manhattan-distance logic that was never updated for diagonal adjacency.\n\n**Fix:** Replace Manhattan distance with **Chebyshev distance** (`max(|dx|, |dy|) ≤ 1`), the standard 8-neighbor adjacency check, consistent with diagonal movement (#233's collision fix) and diagonal facing (#234) already being first-class everywhere else in the codebase.\n\n**Success criteria:**\n1. `_apply_container_transfer` adjacency check uses Chebyshev distance, accepts all 8 neighbors + same tile.\n2. New test: player diagonally adjacent to a container (e.g. dx=1,dy=1) can deposit and withdraw successfully.\n3. Existing cardinal-adjacency behavior unchanged (still works, still rejects distance ≥ 2 in any direction).\n4. Check whether any other adjacency-gated interaction has the same Manhattan-distance bug (terminal open, worker pickup use exact-tile-match via `direction_delta`, not a distance check, so likely unaffected — verify before closing).\n\n**Cross-references:** kbai #245 (container now visible — this is the bug that became reachable once visibility was fixed), kbai #234 (introduced diagonal-capable crafting that exposed this), `architecture/inventory-system.md` (container overlay section).	17	claude-sonnet-5	2026-06-30 23:22:36.98311	2026-06-30 23:31:20.768985	\N	ticket
246	4	BUG: isometric perspective looks skewed	**Symptom (Reporter):** "somehow the isometric view looks skewed. like if the camera is too close to the horizon of the plain."\n\n**Investigated, ruled out:**\n- `tile_to_screen` (`src/client/render.odin:22-26`) uses a standard 2:1 dimetric projection (`sx=(tx-ty)*32, sy=(tx+ty)*16`) — mathematically correct, matches `draw_iso_diamond`'s 64×32 footprint.\n- `cs.camera` (`src/client/main.odin:254-259`): `rotation=0`, `zoom=1.0` at init, clamped `[0.6, 2.5]` on scroll (`main.odin:195-197`). No skew/shear in the Camera2D setup itself.\n- `screen_to_tile` is the exact inverse of `tile_to_screen` — no asymmetry there.\n\n**Not yet investigated:**\n- Whether ground/structural tile textures (vs. the procedural `draw_iso_diamond` fill) maintain the correct 64×32 footprint, or are drawn with a mismatched aspect ratio (square texture stretched onto a diamond slot could look "skewed").\n- Sprite assets for entities (player, workers) — do their footprints visually align with the 2:1 tile grid, or do they look like they're standing at the wrong depth?\n\n**Open question — likely NOT a code bug:** 2:1 dimetric projection is a deliberate, standard choice for pixel-art tile games (shallower angle than "true" isometric ~35°) — this may be the reporter's subjective expectation mismatch rather than a defect. Need either (a) a screenshot from the reporter showing what looks wrong, or (b) a side-by-side comparison against the intended look (e.g. `assets/branding/banner.png`, referenced as the visual target in kbai #103/#106) to determine if this is a real regression or an aesthetic preference.\n\n**Cross-references:** `architecture/render-pipeline.md` (coordinate system section), `adr/106-isometric-rendering.md` (original iso design decision — check if it documents an intended viewing angle to compare against), kbai #234 (sibling session ticket, also perspective-adjacent but about facing direction not projection).	13	claude-sonnet-5	2026-06-30 23:05:33.935652	2026-06-30 23:23:54.784643	\N	ticket
247	4	bug-247: Terminals/Artifacts/GCs render in top-down coords against iso camera (same bug as #245)	**Discovered during:** kbai #245 investigation (container invisibility). Same root cause, different entities — filed separately per Ticket-First Mandate rather than fixed inline in #245.\n\n**Root cause:** `draw_terminals` (`src/client/render.odin:1108-1127`), `draw_artifacts` (`render.odin:1074-1087`), and `draw_gcs` (`render.odin:950-953` for the position calc) all compute screen position via raw `tile * TILE_SIZE` (legacy top-down), while the camera, terrain, players, and workers use the isometric `tile_to_screen` transform (since kbai #106). Same mismatch as #245: these entities render at the wrong screen location relative to where the camera is actually centered.\n\n**Note on draw_workers:** NOT affected — `_update_worker_lerps` (`render.odin:206-222`) already correctly uses `tile_to_screen`. Only its one-frame fallback default uses `TILE_SIZE`, which isn't user-visible. Use this as the reference pattern for the fix.\n\n**Success criteria:**\n1. `draw_terminals`, `draw_artifacts` position calc replaced with `tile_to_screen(x, y)`.\n2. `draw_gcs` position calc replaced with `tile_to_screen(gc.x, gc.y)` — check whether GCs need a lerp system like workers (smooth movement) or whether direct per-frame `tile_to_screen` is sufficient (GCs may move every tick, workers move on player command — check GC movement cadence in `advance_gcs` before deciding lerp is needed).\n3. Crafted/spawned terminals, artifacts, and GCs render at their correct iso screen position, verified visually adjacent to where the server places them (cross-check against player position for a freshly-crafted terminal).\n\n**Cross-references:** kbai #245 (sibling, same root cause, containers), `architecture/render-pipeline.md`, kbai #106 (isometric rewrite — origin of the inconsistency).	17	claude-sonnet-5	2026-06-30 23:11:12.393994	2026-06-30 23:27:02.580719	\N	ticket
244	1	agent_role_instruction wird bei jedem get_ticket_detailed komplett mitgeschickt — Redundanz über viele Tickets hinweg	**Erfahrung:** Jeder get_ticket_detailed-Call liefert das volle agent_role_instruction-Feld der Spalte (mehrere hundert Zeichen Markdown) mit zurück — identisch für jedes Ticket in derselben Status-Spalte. Beim parallelen Abfragen mehrerer Tickets in derselben Spalte (z.B. 5× backlog-Tickets in dieser Session) wird derselbe Instruction-Text 5× in den Kontext geladen.\n\n**Konkreter Vorschlag:**\n1. `get_ticket_detailed` um einen optionalen Parameter `include_role_instruction: bool = true` erweitern — Agent kann nach dem ersten Laden pro Session abschalten, da die Instruction pro Spalte stabil ist.\n2. Alternativ: separates `get_role_instructions(project_id)` Tool, das alle Spalten-Instructions einmalig auf einmal liefert (passt gut neben list_board_statuses, das die Instructions teilweise schon mitliefert — siehe list_board_statuses-Ergebnis für Projekt 1, das ready/in_progress/in_review-Instructions direkt enthält. get_ticket_detailed dupliziert das pro Ticket unnötig).\n\n**Auswirkung auf Agenten:** Bei Triage-Arbeit über mehrere Tickets derselben Spalte hinweg ist das spürbarer Tokenoverhead ohne neuen Informationsgehalt ab dem zweiten Ticket.	5	claude-sonnet-5	2026-06-30 22:46:42.581099	2026-07-01 15:11:46.299965	\N	ticket
238	1	Keine Such-/Duplicate-Detection — list_tickets ist der einzige Weg, Titel zu vergleichen	**Erfahrung:** Beim Triagieren von Bug-Reports im Projekt "Kernel Panic" fand ich zwei exakte Duplikate (#190 und #195, beide "006b: GC Wall Collision", beide done). Es gibt kein Tool, um vor dem Anlegen eines Tickets gezielt nach ähnlichen Titeln zu suchen — die einzige Möglichkeit ist, den kompletten list_tickets-Dump zu laden und manuell zu grep'en (siehe Schwester-Ticket zu list_tickets-Pagination).\n\n**Konkreter Vorschlag:**\n1. `search_tickets(project_id, query)` — Volltextsuche über title (+ optional description), gibt nur Treffer zurück (id, title, status_id, similarity-score falls möglich).\n2. Optional: `create_ticket` könnte serverseitig vor dem Anlegen einen Fuzzy-Match gegen bestehende Titel laufen lassen und eine Warnung mit den Top-3-Kandidaten zurückgeben (nicht blockieren, nur warnen) — würde das Problem an der Quelle verhindern statt es nur auffindbar zu machen.\n\n**Auswirkung auf Agenten:** Ohne Suchfunktion ist "prüfe ob es das schon gibt" für einen Agenten teurer als "leg einfach ein neues Ticket an" — das System belohnt strukturell Duplikate.	5	claude-sonnet-5	2026-06-30 22:45:29.396591	2026-07-01 15:11:32.862025	\N	ticket
249	3	Liste kollabieren bei zu vielen eintragen	ich habe bemerkt das in projekten mit vielen abgeschlossenen tickets die seite sehr lang wird, weil hier alle tickets angezeigt werden. hier soll nach den ersten 25 ticket schluss sein. alle anderen, nachfolgenden tickets pro spalte, sollen durch ein "stapel" symbolisiert werden. wenn der user da drauf klickt, soll der stapel die 25 naechten tickets laden	12	\N	2026-06-30 23:28:50.226912	2026-07-01 21:09:17.458781	\N	ticket
250	4	237: UI-System — restliche Overlays migrieren (terminal/fragment/help/escape-menu/hud)	**Konzept:** kbai #230 hat das UI-Subsystem (`src/client/ui.odin`: `UI_COL/DIM/HEADER/CYAN/WARN/ERR` Palette, `ui_dim_screen`, `ui_panel`, `ui_divider`, `ui_list_row`) etabliert und `inventory.odin` (`draw_inventory_overlay`, `draw_container_overlay`) als Referenzimplementierung migriert. Laut `architecture/ui-system.md` "Current overlay inventory" sind folgende Overlays noch nicht migriert und deklarieren weiterhin lokale Farbkonstanten statt der Shared-Palette:\n\n| Overlay | Datei | UILayer |\n|---|---|---|\n| `draw_terminal_overlay` | `src/client/render.odin` | Overlay |\n| `draw_fragment_overlay` | `src/client/render.odin` | Modal (sollte `ui_panel` nutzen) |\n| `draw_help_overlay` | `src/client/render.odin` | Overlay |\n| `draw_escape_menu` | `src/client/escape_menu.odin` | Modal |\n| `draw_hud` | `src/client/render.odin` | HUD (Palette-Migration) |\n\n**Success Criteria:**\n1. Jedes der 5 Overlays nutzt `UI_COL/UI_DIM/UI_HEADER/UI_CYAN/UI_WARN/UI_ERR` statt lokaler Farbkonstanten.\n2. `draw_fragment_overlay` und `draw_escape_menu` (Modal-Layer) nutzen `ui_panel(rect, title)` für Hintergrund+Rahmen statt manueller `DrawRectangleRec`/`DrawRectangleLinesEx`-Kombination.\n3. Alle Overlays/Modals beginnen mit `ui_dim_screen()`.\n4. Horizontale Trenner nutzen `ui_divider` statt manueller `"───..."`/`"---..."`-Strings (ASCII-Font-Constraint aus #230 beachten).\n5. `architecture/ui-system.md` "Current overlay inventory"-Tabelle aktualisiert — alle Zeilen auf "uses UI system".\n\n**Cross-References:** `architecture/ui-system.md` (Extension Guide), kbai #230 (done, Referenzimplementierung in `inventory.odin`).\n\n**Aufwand:** M (5 Overlays, mechanisch aber mehrere Dateien) — Architect sollte prüfen ob Dekomposition in Sub-Tickets pro Overlay sinnvoll ist oder ob ein Ticket reicht (gleiche Art Change, geringes Risiko pro Overlay).	17	claude-sonnet-5	2026-06-30 23:32:03.249106	2026-06-30 23:37:35.736106	\N	ticket
31	4	108a: Ambient Code-Panel-Deko auf Terminals	**Datei:** project/planning/108-ingame-code-terminal-panels.md (Original)\n**Aufwand:** S | **Refs:** konzept-world-populated.png, 098 (Terminals)\n\n**Reduzierter Scope (siehe Architect-Kommentar):** Nur Variante (a) — Ambient Decoration. Varianten (b) Live Worker Code und (c) Echo Surfaces sind als eigene Tickets 108b/108c ausgegliedert (größerer Scope, brauchen neue Server-Mechanismen).\n\nGrüne Terminal-Panels mit zufälligen, rein dekorativen Lua-Code-Snippets — kein Gameplay-Effekt, kein Protokoll-Change. Deterministische Auswahl per Terminal-ID (stabil über Frames/Sessions).\n\n**Akzeptanzkriterien:**\n- Snippet-Pool (statische String-Liste, mehrere kurze Lua-Zeilen im "Worker-Script"-Stil).\n- `draw_terminals` (render.odin) zeigt 2-3 Zeilen eines deterministisch gewählten Snippets neben/über dem Terminal-Glyph.\n- CRT-Grün, UI-System-Palette (UI_DIM für die Code-Zeilen, dezent — Deko, nicht Fokus).\n- Iso-Koordinaten via tile_to_screen (Lektion aus #245/#247 — NICHT TILE_SIZE-Raw-Multiply verwenden).\n\n**Cross-References:** architecture/render-pipeline.md, kbai #098 (Terminals, done).	17	\N	2026-06-25 09:25:05.309611	2026-06-30 23:59:06.662759	\N	ticket
251	3	menschen duerfen auch kommentieren	als user von kbai-ui moechte ich auch in der lage sein den tickets kommentare hinzuzufuegen.	12	\N	2026-06-30 23:41:20.861537	2026-07-01 21:09:35.61882	\N	ticket
252	1	feature request: EPIC	we need an epic type ticket. epics have a title/goal, description and child tickets. epics are huge and describe a greater over-all goal in the project. goals should be used to filter tickets, and statuses to get to work on a single topic	5	\N	2026-06-30 23:55:24.403097	2026-07-01 15:28:31.817462	\N	ticket
255	1	special inbox function	there shoud be an option for the ai agent to pass an ticket from within every status indo an "human-intervention" status. that should be used if the ai agents needs the human to test, specify or else intervene within this tickets lifecycle. tickets from within the status "human-intervention" could only be moved to "human-answered". from within "human-answered" tickets shoud be able to be moved to whatever state.	5	\N	2026-06-30 23:59:10.899101	2026-07-01 15:28:35.929054	\N	ticket
261	4	doc: player-state.md + worker-scripting.md beschreiben entfernte Scalar-Inventory-Felder	**Code-Review-Fund (2026-07-01).** Beide Docs beschreiben die Pre-099c-Architektur, die längst durch das slot-basierte `Inventory`-System ersetzt wurde:\n\n1. `architecture/player-state.md:27-48`: behauptet `Player` embedde `PlayerInventory` (Felder `bytes`, `cpu_cycles`, `ram_chips` via `using inventory: PlayerInventory`, Direct-Access wie `p.bytes`). **`PlayerInventory` existiert nicht mehr im Code.** `Player.inventory` ist direkt `shared.Inventory` (slot-basiert, `slots[32]`, `slot_count`, `used_volume`) — `src/shared/entities.odin:44`.\n2. `architecture/worker-scripting.md:19`: behauptet `Worker.inventory: [16]u64` + separate `stored_bytes/stored_cpu_cycles/stored_ram_chips`-Felder. Code (`src/shared/entities.odin:67-68`): `Worker.inventory: Inventory` (gleiches Slot-System wie Player); die alten Felder existieren nur noch als `_unused_old_inventory: [16]u64` für Wire-Kompatibilität.\n\n**Vermutung:** Story 099c (Inventory-Migration) hat beide Docs nie aktualisiert.\n\n**Success Criteria:** Beide Docs auf das aktuelle `Inventory`-Slot-System umgeschrieben, Cross-Reference zu `architecture/inventory-system.md` ergänzt.\n\n**Aufwand:** S-M.	17	claude-sonnet-5	2026-07-01 00:06:14.273973	2026-07-01 14:37:46.941417	\N	ticket
262	4	doc: interaction-system.md — direction_delta-Beispiel mathematisch falsch (verwechselt mit Client-Movement-Vektoren) + Open_Cave_Entrance fehlt	**Code-Review-Fund (2026-07-01), verifiziert.**\n\n`architecture/interaction-system.md:52,56`: zeigt `direction_delta` als `N=(-1,-1) S=(+1,+1) W=(-1,+1) E=(+1,-1) — iso` und nennt das "asymmetrisch".\n\n**Tatsächlicher Code** (`src/shared/entities.odin`, `direction_delta`, seit #234 mit 8 Werten): `North=(0,-1) South=(0,1) East=(1,0) West=(-1,0)` (plus 4 Diagonalen NE/SE/SW/NW) — Standard-Kartesisch, NICHT die in der Doku gezeigten Werte.\n\n**Root Cause vermutet:** Die Doku verwechselt `direction_delta` (Server, Facing-Targeting) mit den Client-seitigen iso-WASD-Bewegungsvektoren aus `src/client/input.odin` (W→(-1,-1), S→(+1,+1), A→(-1,+1), D→(+1,-1) — das sind die in der Doku gezeigten Werte, gehören aber zu einem anderen Mechanismus: Tastatur-Input→Tile-Movement, nicht Facing→Interaktions-Ziel).\n\n**Zusätzlich:** `InteractionKind`-Liste (Zeilen 19-30) hat 15 Werte, Code hat 16 — `Open_Cave_Entrance` (#192, diese Woche) fehlt.\n\n**Success Criteria:** direction_delta-Beispiel korrigiert (echte Werte aus entities.odin), klare Trennung Client-Movement-Vektoren vs. Server-Facing-Deltas dokumentiert, Open_Cave_Entrance in der Liste ergänzt.\n\n**Aufwand:** S — aber wichtig, da das falsche Beispiel jeden zukünftigen Agenten in die Irre führt, der Targeting-Logik ändert.	17	claude-sonnet-5	2026-07-01 00:06:21.901372	2026-07-01 14:37:54.987249	\N	ticket
263	4	doc: header-fragments.md fragment_decrypt-Signatur veraltet + item-system.md fehlt Pointer_Key	**Code-Review-Fund (2026-07-01).**\n\n1. `architecture/header-fragments.md:151-156`: zeigt alte API `fragment_decrypt(&state.kernel_registry, &unlocked, fid, &p.cpu_cycles)` (nimmt CPU-Cycles direkt). Aktueller Code (`src/server/player_state.odin:86-94`): `fragment_decrypt(global_reg, fset, fid, inv: ^shared.Inventory)` — nimmt jetzt ein Inventory-Pointer und pusht das Fragment-Item ins Inventar (Zeile 93). Doku erwähnt die Inventory-Integration nicht.\n2. `architecture/item-system.md:42-56`: Tabelle listet 12 ItemTypes, endet bei `ContainerShell`. Code (`src/shared/items.odin:19`) hat 13 — `Pointer_Key` (#192, diese Woche) fehlt, obwohl in `architecture/cave-system.md` bereits referenziert.\n\n**Success Criteria:** Beide Docs aktualisiert.\n\n**Aufwand:** XS.	17	claude-sonnet-5	2026-07-01 00:06:26.227363	2026-07-01 14:37:14.675651	\N	ticket
265	4	doc+bug: render-pipeline.md nennt nicht-existente draw_worker_commands + falsche Proc-Namen; draw_man_pages_overlay nutzt UI-System nicht	**Code-Review-Fund (2026-07-01), verifiziert.**\n\n**Doku-Fehler in `architecture/render-pipeline.md`:**\n1. Zeile ~146-147: nennt `draw_editor` (tatsächlich `draw_script_editor`, render.odin:1629) und `draw_help` (tatsächlich `draw_help_overlay`, render.odin:1762).\n2. `draw_worker_commands` wird referenziert (Zeile ~141), existiert aber nirgends im Code (grep bestätigt: keine Treffer).\n3. HUD-Tabelle (Zeilen 133-149) fehlen `draw_spike_indicator`, `draw_perf_hud`, `draw_man_pages_overlay` — alle drei aktiv in `_draw_hud_layer` aufgerufen.\n\n**Echter Code-Bug (nicht nur Doku):** `draw_man_pages_overlay` (render.odin ~Zeile 1732-1740) deklariert lokale Farbkonstanten `COL/DIM/HEAD/SIG` statt der `UI_*`-Palette aus #237 (UI-Subsystem-Migration). Wurde bei #237 übersehen — `architecture/ui-system.md` behauptet "All overlays now use the shared UI subsystem", was für diesen Overlay nicht stimmt.\n\n**Success Criteria:**\n1. render-pipeline.md: Proc-Namen korrigiert, `draw_worker_commands`-Referenz entfernt, fehlende HUD-Procs ergänzt.\n2. `draw_man_pages_overlay` auf `UI_COL/UI_DIM/UI_HEADER` + `ui_dim_screen` migriert (analog zu #237s anderen Overlays).\n3. `architecture/ui-system.md` "Current overlay inventory"-Tabelle um `draw_man_pages_overlay` ergänzt.\n\n**Aufwand:** S (Code-Fix ist mechanisch wie #237, Doku-Fix trivial).	17	claude-sonnet-5	2026-07-01 00:06:37.219938	2026-07-01 14:41:13.894496	\N	ticket
264	4	doc: input-routing.md fehlen 3 InputContext-Werte (ManPages/Terminal/Container)	**Code-Review-Fund (2026-07-01), verifiziert.** `architecture/input-routing.md` (Zeilen 11-18) listet 6 `InputContext`-Werte (Normal, Inventory, Help, Fragment, EscapeMenu, Editor).\n\nTatsächlicher Enum (`src/client/main.odin:19-29`) hat 9 Werte — zusätzlich `.ManPages`, `.Terminal`, `.Container`. Alle drei haben vollständige Key-Handler (`_handle_terminal_keys`, `_handle_container_keys`, Man-Pages-ESC-Handling) und sind seit #117/#192-Vorläufern in Betrieb — keine Stubs.\n\n**Success Criteria:** Kontext-Tabelle + "Kontext-Details"-Abschnitt um alle 3 fehlenden Kontexte ergänzt (Keys, Priorität, ESC-Verhalten).\n\n**Aufwand:** S.	17	claude-sonnet-5	2026-07-01 00:06:29.889045	2026-07-01 14:37:22.223111	\N	ticket
256	4	doc: multiplayer-protocol.md fehlen 12 Message-Types + Snapshot-Erweiterungen + ServerState-Felder	**Code-Review-Fund (2026-07-01).** `architecture/multiplayer-protocol.md` listet 24 Message-Types, `src/shared/protocol.odin` implementiert 36.\n\n**Fehlende Message-Types in der Doku:**\n- C→S: `MSG_CRAFT_TRANSISTOR` (0x0E), `MSG_CRAFT_TERMINAL` (0x0F), `MSG_BUILD_WALL` (0x1F), `MSG_CONTAINER_DEPOSIT` (0x21), `MSG_CONTAINER_WITHDRAW` (0x22), `MSG_HEAP_DEPOSIT` (0x23), `MSG_HEAP_WITHDRAW` (0x24)\n- S→C: `MSG_FRAGMENT_TABLE` (0x1B), `MSG_FRAGMENT_HINT` (0x1C), `MSG_TERMINAL_OPEN` (0x1E), `MSG_CONTAINER_OPEN` (0x20)\n- Bidirektional: `MSG_UPGRADE_WORKER_LOC` (0x1D)\n\n**World-Snapshot-Encoding (protocol.odin:290-478) nicht dokumentiert:** Terminal-Sektion, `avg_entropy`-Feld, Memory-Leak-Positions-Array.\n\n**ServerState-Felder (main.odin:17-65) fehlen in `architecture/server_client_architecture.md` (Zeilen 157-187):** `terminals`, `net_inboxes`, `harvested_tiles`, `caves`, `memory_leaks`.\n\n**Success Criteria:** Beide Docs vollständig aktualisiert, jede Message-Type/jedes Feld referenziert. Kein Code-Fix nötig, reine Doku-Arbeit.\n\n**Aufwand:** S-M (reine Doku-Aktualisierung, kein Code-Risk).	17	claude-sonnet-5	2026-07-01 00:05:44.054248	2026-07-01 14:35:17.482553	\N	ticket
260	4	doc: gc-ai.md mehrfach falsch — L1-Priorität invertiert, Worker-Damage fehlt, Cold-Zone-Radius 4× falsch	**Code-Review-Fund (2026-07-01), verifiziert.** `architecture/gc-ai.md` hat mehrere konkrete Faktenfehler gegen `src/server/simulation.odin`:\n\n1. **L1-Ziel-Priorität invertiert** (gc-ai.md:36-37 vs. simulation.odin `_gc_find_target_l1`, ~Zeile 1174-1210): Doku sagt "Containers first". Code-Kommentar bei der Player-Prüfung sagt explizit "Scan Players first (preferred)" — Reihenfolge ist Player → Worker → Container, Container ist Fallback/"last resort".\n2. **L1 Worker-Damage fehlt komplett** (gc-ai.md Tier-Tabelle Zeile 22 zeigt "—" für L1): Verifiziert in `_gc_apply_damage` (simulation.odin ~Zeile 1320-1325) — `damage := GC_DAMAGE_WORKER` (25.0) ist der DEFAULT-Pfad, gilt für L1 genauso wie L2 (nur L2 überschreibt mit `GC_L2_DAMAGE_WORKER`). L1 macht also Worker-Schaden, Doku sagt das Gegenteil.\n3. **Cold-Zone-Radius 4× zu klein dokumentiert** (gc-ai.md:68 "radius ~5 tiles"): Code (`GC_COLD_ZONE_RADIUS_SQ :: 400`, simulation.odin Zeile 29) ergibt Radius 20 (√400), nicht 5.\n4. **L1 Player-Damage veraltet** (gc-ai.md:23 "~5.0/contact"): Code `GC_DAMAGE_PLAYER :: f32(10.0)`, Zeile 23 — gilt für L1, L2 überschreibt separat.\n5. **L1 Detection-Radius nicht dokumentiert** (gc-ai.md:20 nur "pursues nearest target", kein Wert): Code definiert `GC_DETECTION_RADIUS :: 16` (simulation.odin Zeile 21) — explizit anders als L2s 24 Tiles.\n\n**Vermutung:** Doku wurde vor einem L1/L2-Differenzierungs-Refactor geschrieben und nie nachgezogen — die Fehler sind zu konsistent für Tippfehler.\n\n**Success Criteria:** Tabelle + Priority-Abschnitt komplett neu gegen aktuellen Code abgeglichen, alle 5 Punkte korrigiert.\n\n**Aufwand:** S (reine Doku-Korrektur, Werte sind im Code bereits korrekt und getestet).	17	claude-sonnet-5	2026-07-01 00:06:09.792268	2026-07-01 14:37:38.263351	\N	ticket
237	1	list_tickets has no pagination/filter — breaks on mid-size projects	**Erfahrung:** kb_ai_list_tickets(project_id=4) auf einem ~235-Ticket-Projekt lieferte 65.776 Zeichen in einer Zeile zurück — Fehler "exceeds maximum allowed tokens", Ergebnis wurde stattdessen in eine Datei geschrieben. Die Umgebung hatte weder jq noch python3 verfügbar, ich musste den JSON-Dump mit sed/grep manuell in Zeilen splitten, um überhaupt id/status_id/title herauszulesen. Für eine reine Status-Übersicht (z.B. "zeig mir alle backlog-Tickets") ist das unverhältnismäßig teuer — sowohl an Tokens als auch an Agentenzeit.\n\n**Konkreter Vorschlag:**\n1. `list_tickets` um optionale Parameter erweitern: `status_id` (Filter), `limit`/`offset` (Pagination), `fields` (z.B. nur id+title+status_id statt voller description).\n2. Alternativ/zusätzlich: ein `list_tickets_summary` Tool, das nur id/title/status_id/assignee zurückgibt — für genau den Use-Case "Überblick verschaffen" ohne volle Description/Tasks/Comments mitzuschleppen.\n\n**Auswirkung auf Agenten:** Jeder Agent, der sich zu Sessionbeginn einen Überblick verschaffen will (z.B. nach Context-Compaction), zahlt aktuell den vollen Token-Preis für das gesamte Projekt, auch wenn nur eine Spalte interessiert.	5	claude-sonnet-5	2026-06-30 22:45:07.909413	2026-07-01 15:11:31.140662	\N	ticket
240	1	assign_ticket erfordert manuellen 'assignee'-Parameter trotz KB_AI_AGENT_NAME-Fallback	**Erfahrung:** kb_ai_assign_ticket(ticket_id=231) ohne assignee-Parameter schlug fehl mit "Missing assignee: provide 'assignee' parameter or set KB_AI_AGENT_NAME". Die Tool-Beschreibung verspricht "Uses KB_AI_AGENT_NAME as default assignee", aber die Env-Variable war in meiner Session offenbar nicht gesetzt — ich musste in jedem der ~6 assign_ticket-Aufrufe dieser Session manuell `assignee: "claude-sonnet-5"` mitgeben.\n\n**Konkreter Vorschlag:**\n1. Klareres Fehlerbild: wenn KB_AI_AGENT_NAME fehlt, sollte der Fehler das explizit sagen ("KB_AI_AGENT_NAME env var is not set in this session — pass assignee explicitly or configure the var"), nicht nur "Missing assignee" — spart dem Agenten einen Debug-Schritt.\n2. Alternativ: MCP-Server-Setup-Doku (für Menschen, die kb.ai in neue Projekte/Sessions einbinden) sollte explizit auflisten, dass KB_AI_AGENT_NAME und KB_AI_AGENT_MODEL gesetzt werden müssen, idealerweise mit einer Validierungsroutine beim Server-Start, die fehlende Env-Vars frühzeitig meldet statt erst beim ersten assign_ticket-Call.\n\n**Beobachtung zu model-Feld:** assign_ticket "always writes KB_AI_AGENT_MODEL to the model field" — unklar was passiert wenn auch diese Variable fehlt (leerer String? Fehler? wurde in meiner Session nicht getestet, da ich nie explizit geprüft habe ob model korrekt gesetzt wurde).	5	claude-sonnet-5	2026-06-30 22:46:01.643625	2026-07-01 15:11:39.6271	\N	ticket
267	3	markdown in ticket details	the ticket details model should support markdown in the description field of the ticket	12	\N	2026-07-01 00:07:00.227053	2026-07-01 21:09:54.570588	\N	ticket
258	4	bug/doc: sector_activity_score-Semantik widerspricht ADR-031	**Mensch-Entscheidung (2026-07-01):** Echoes sollen häufiger in kalten Sektoren auftauchen. Code wird an ADR-031 angepasst (nicht die ADR).\n\n**Analyse (wichtig für Umsetzung):** Das aktuelle Spielverhalten liefert BEREITS "mehr Echoes in kalten Sektoren" — `sector_activity_score` gibt 1.0=aktiv/0.0=kalt zurück, `echo.odin:78` nutzt `chance = 0.25*(1.0-activity)`, also 0 bei aktiv, 0.25 bei kalt. Die doppelte Invertierung ergibt zufällig das richtige Spielverhalten, aber mit einer Variable, deren eigene Semantik der ADR widerspricht.\n\n**Fix (2 Dateien, zusammenhängend — NICHT nur eine):**\n1. `src/server/sector.odin` `sector_activity_score`: Formel auf ADR-Semantik drehen — `clamp(seconds_since / COLD_SECS, 0, 1)`, also 0.0=gerade besucht, 1.0=kalt. Auch den Doc-Kommentar direkt über der Funktion korrigieren (aktuell "0.0 = cold/unvisited, 1.0 = visited" — muss auf "0.0 = gerade besucht, 1.0 = kalt" gedreht werden).\n2. `src/server/echo.odin:78`: Formel MUSS mitgezogen werden — `chance = 0.25 * (1.0 - activity)` → `chance = 0.25 * activity`, damit sich das tatsächliche Spielverhalten (mehr Echoes in kalten Sektoren) NICHT ändert, nur die interne Variablen-Semantik wird korrekt.\n\n**Success Criteria:**\n1. `sector_activity_score(rec)` gibt 0.0 für gerade besuchte, 1.0 für kalte Sektoren (Grenzfälle: seconds_since<=0 → 0.0, seconds_since>=COLD_SECS → 1.0).\n2. `echo.odin`s Chance-Formel weiterhin: niedrige Chance bei aktiven, hohe Chance bei kalten Sektoren (unverändertes Spielverhalten).\n3. Test für `sector_activity_score` (neu, falls noch keiner existiert — prüfen) deckt beide Grenzfälle + einen Mittelwert ab.\n4. ADR-031 bleibt unverändert (sie war schon korrekt).\n\n**Aufwand:** XS.	17	claude-sonnet-5	2026-07-01 00:05:54.660437	2026-07-01 14:46:48.290417	\N	ticket
266	4	architecture: render.odin ist ein God-File (1865 Zeilen, 6+ unabhängige Verantwortlichkeiten) — Split empfohlen	**Code-Review-Fund (2026-07-01), verifiziert.** `wc -l src/client/*.odin`:\n\n```\n   575 input.odin\n   681 main.odin\n   683 network.odin\n  1865 render.odin   ← 2.7× größer als der nächstgrößte Peer\n```\n\n`render.odin` mischt mindestens 6 unabhängige Verantwortlichkeiten in einer Datei:\n1. Koordinaten-Transformationen (`tile_to_screen`, `screen_to_tile`)\n2. Terrain-Layer (`draw_world`, `draw_substrate_layer`, `draw_structural_tile`)\n3. Entity-Rendering inkl. Lerp/Smoothing (`draw_containers`, `draw_terminals`, `draw_workers`, `draw_artifacts`, `draw_gcs`, `draw_players`, `_update_*_lerps`)\n4. Overlays/Modals (`draw_script_editor`, `draw_fragment_overlay`, `draw_help_overlay`, `draw_man_pages_overlay`, `draw_terminal_overlay`)\n5. HUD (`draw_hud`, `draw_status_bar`, `draw_perf_hud`, `draw_spike_indicator`)\n6. Partikel/Effekte (`draw_effects` + Spawn-Logik)\n\n**Zusätzliche Beobachtung (Modularisierung):** `draw_containers`, `draw_terminals`, `draw_artifacts` sind nach #245/#247 strukturell fast identisch (gleiches Muster: `tile_to_screen` → Rechteck/Kreis-Rahmen → zentrierter Glyph). Könnten zu einem generischen `draw_static_world_icon(pos, glyph, color, ...)`-Helper zusammengefasst werden, statt 3 Procs mit dupliziertem Layout-Code zu pflegen.\n\n**Vorschlag (Architect-Entscheidung nötig vor Umsetzung):**\n- `render_world.odin` — Terrain + Koordinaten-Transforms\n- `render_entities.odin` — alle Entity-Draws + Lerp-Systeme\n- `render_overlays.odin` — alle Modal/Overlay-Draws\n- `render_hud.odin` — immer-sichtbare HUD-Strips\n- `render.odin` bleibt als dünner Orchestrator (`draw_entities`, `_draw_world_layer`, `_draw_hud_layer`, Haupt-Dispatch)\n\n**Risiko:** Reiner Datei-Split ohne Verhaltensänderung — mechanisch, aber 1865 Zeilen verschieben ist fehleranfällig (Copy-Paste-Fehler, vergessene Imports). Sollte NICHT mit einer Verhaltensänderung in einem Commit gemischt werden (Hard Negative #3).\n\n**Aufwand:** L (reiner Umbau, hohe Sorgfaltspflicht, kein neues Feature) — Architect sollte vor Umsetzung den genauen Split-Schnitt festlegen und ggf. in Sub-Tickets zerlegen.	17	claude-sonnet-5	2026-07-01 00:06:48.859605	2026-07-01 14:56:15.845051	\N	ticket
268	3	feature request: visualize ticket updates	ai agents can update the description, comments, etc. without moving the tickets from one status to another. is there an easy way to also visualize updates on tickets? that would be awesome	12	\N	2026-07-01 14:42:09.207703	2026-07-01 21:10:08.465787	\N	ticket
269	3	spike: explore oidc	explore the possibilities fo implementing OIDC authentication via postgresql	12	\N	2026-07-01 14:43:02.146936	2026-07-01 21:10:16.732612	\N	ticket
279	3	Forgejo build pipeline und releases	Das projekt liegt auf deinem forgejo server und soll per forgejo workflow gebaut und releaed werden. released wird immer dann wenn ein tag gepushed wird. ai agents sollen selbststaendig entscheiden wann ein neues release gerechtfertig ist.\nwenn released wird, soll ein container image vom kbai-ui gebaut und auf codeberg gepublished werden.	11	\N	2026-07-01 21:34:38.530182	2026-07-01 21:50:00.345487	\N	ticket
270	3	V4 Migrationen beruecksichtigen	im verzeichnis init-db findest du die neuste migration der DB V4.\nDiese fuehrt ticket relationen und epics ein, neben anderen dingen. analysiere sie und finde wege die neuen daten darzustellen 	12	\N	2026-07-01 16:37:36.457072	2026-07-01 21:10:25.07087	\N	ticket
276	3	 Dashboard ist useless	Der Link auf "Dashbaord" ist aktuell ohne funktion. entweder designe ein funktionales dashboard das statistiken der projekte auflistet, oder entfernde diesen Menueeintrag	12	\N	2026-07-01 21:18:13.746178	2026-07-01 21:26:42.091291	\N	ticket
271	3	human intervention | new ui elements needed	mit der V4 migration kamen auch 2 neue status human_intervention und human_answered hinzu. diese sollen nicht als klassische spalten im kanban board zu sehen sein, sondern in einem eigenen modal auftauchen. der user soll dazu im haeder einen neuen button bekommen "Inbox" in der er alle tickets sehen kann, die die ki an ihn uebergeben hat. von dortaus kann er die einzelnen tickets oeffenen und bearbeiten. der knopt "an die KI zurueckgeben" verschiebt dann die jeweiligen tickets in den status "human_answered"	12	\N	2026-07-01 16:39:53.414344	2026-07-01 21:10:40.6654	\N	ticket
274	2	Das ist ein Epic	# This is epic!	6	\N	2026-07-01 21:13:12.258042	2026-07-01 21:13:12.258042	\N	epic
275	2	Epic-child 1	\N	6	\N	2026-07-01 21:13:28.525195	2026-07-01 21:13:28.525195	\N	ticket
281	3	fehlende Animation/visualisierung	das updaten von tickets ohne sie zu verschieben wird aktuell nicht visualiseirt. dies muss geaendert werden!	11	\N	2026-07-01 21:45:50.101956	2026-07-01 21:57:59.100282	\N	ticket
280	3	ein kleiner Test	bitte schiebe dieses Ticket mal in den status human_intervention, damit ich live testen kann.	12	\N	2026-07-01 21:42:26.898478	2026-07-01 22:01:43.709672	\N	ticket
\.


--
-- Name: board_statuses_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.board_statuses_id_seq', 30, true);


--
-- Name: projects_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.projects_id_seq', 5, true);


--
-- Name: ticket_comments_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.ticket_comments_id_seq', 133, true);


--
-- Name: ticket_documents_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.ticket_documents_id_seq', 1, false);


--
-- Name: ticket_relations_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.ticket_relations_id_seq', 1, true);


--
-- Name: ticket_tasks_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.ticket_tasks_id_seq', 118, true);


--
-- Name: tickets_id_seq; Type: SEQUENCE SET; Schema: public; Owner: david
--

SELECT pg_catalog.setval('public.tickets_id_seq', 281, true);


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
-- Name: ticket_relations ticket_relations_from_ticket_id_to_ticket_id_relation_type_key; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_relations
    ADD CONSTRAINT ticket_relations_from_ticket_id_to_ticket_id_relation_type_key UNIQUE (from_ticket_id, to_ticket_id, relation_type);


--
-- Name: ticket_relations ticket_relations_pkey; Type: CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_relations
    ADD CONSTRAINT ticket_relations_pkey PRIMARY KEY (id);


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
-- Name: projects projects_create_human_statuses; Type: TRIGGER; Schema: public; Owner: david
--

CREATE TRIGGER projects_create_human_statuses AFTER INSERT ON public.projects FOR EACH ROW EXECUTE FUNCTION public.create_human_intervention_statuses();


--
-- Name: ticket_comments ticket_comments_notify; Type: TRIGGER; Schema: public; Owner: david
--

CREATE TRIGGER ticket_comments_notify AFTER INSERT OR DELETE OR UPDATE ON public.ticket_comments FOR EACH ROW EXECUTE FUNCTION public.notify_ticket_child_change();


--
-- Name: ticket_tasks ticket_tasks_notify; Type: TRIGGER; Schema: public; Owner: david
--

CREATE TRIGGER ticket_tasks_notify AFTER INSERT OR DELETE OR UPDATE ON public.ticket_tasks FOR EACH ROW EXECUTE FUNCTION public.notify_ticket_child_change();


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
-- Name: ticket_relations ticket_relations_from_ticket_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_relations
    ADD CONSTRAINT ticket_relations_from_ticket_id_fkey FOREIGN KEY (from_ticket_id) REFERENCES public.tickets(id) ON DELETE CASCADE;


--
-- Name: ticket_relations ticket_relations_to_ticket_id_fkey; Type: FK CONSTRAINT; Schema: public; Owner: david
--

ALTER TABLE ONLY public.ticket_relations
    ADD CONSTRAINT ticket_relations_to_ticket_id_fkey FOREIGN KEY (to_ticket_id) REFERENCES public.tickets(id) ON DELETE CASCADE;


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

\unrestrict NmpJstSgE1WyqTriZiynuXQ4sYCXCkYcgnBfcC2l4rTCSacIocx1lj7N3gFVcBK

