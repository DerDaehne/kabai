#ifndef KANBAN_BOARD_STATUSES_H
#define KANBAN_BOARD_STATUSES_H

#include "db/connection.h"

typedef struct {
    int   id;
    int   project_id;
    char *name;                  /* machine name, e.g. "in_progress" */
    char *display_name;          /* human label, e.g. "In Arbeit" */
    int   position;              /* column order */
    char *agent_role_instruction; /* dynamic persona prompt; may be NULL */
} BoardStatus;

typedef struct {
    int from_status_id;
    int to_status_id;
} StatusTransition;

BoardStatus  *board_status_create(
    DatabaseConnection *db,
    int project_id,
    const char *name,
    const char *display_name,
    int position,
    const char *agent_role_instruction
);

BoardStatus **board_status_list_by_project(DatabaseConnection *db, int project_id);

int status_transition_create(
    DatabaseConnection *db,
    int project_id,
    int from_status_id,
    int to_status_id
);

StatusTransition **status_transition_list_by_project(DatabaseConnection *db, int project_id);

void board_status_free(BoardStatus *bs);
void board_status_free_array(BoardStatus **arr);
void status_transition_free_array(StatusTransition **arr);

#endif /* KANBAN_BOARD_STATUSES_H */
