#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "attachments/attachment_tools.h"
#include "mcp/schema.h"

/* ============================================================================
 * Error mapping (mirrors docs_db_error in src/docs/docs_tools.c)
 * ============================================================================ */

static const char *attachment_db_error(PGresult *res) {
    const char *state      = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
    const char *constraint = res ? PQresultErrorField(res, PG_DIAG_CONSTRAINT_NAME) : NULL;

    if (state && strcmp(state, "23514") == 0) {  /* check_violation */
        if (constraint && strstr(constraint, "mime_type"))
            return "Unsupported image type: must be image/png, image/jpeg, image/webp, or image/gif";
        if (constraint && strstr(constraint, "size_bytes"))
            return "Image too large: attachments are capped at 10 MiB";
        return "Constraint violation";
    }
    if (state && strcmp(state, "23503") == 0)  /* foreign_key_violation */
        return "Referenced ticket or attachment does not exist";
    return NULL;
}

/* ============================================================================
 * kabai_get_attachment (ticket #468)
 * ============================================================================ */

/* Binary retrieval: the base64 encoding happens IN THE QUERY via Postgres'
 * encode(data, 'base64'), not in C. This avoids a hand-rolled base64 encoder
 * and a second binary-format round trip through libpq (PQexecParams with
 * resultFormat=1) purely to re-encode the bytes right back into text for the
 * JSON response — encode() gives us the final wire representation directly
 * as a text column. */
static cJSON *tool_get_attachment(McpContext *ctx, cJSON *id, cJSON *params) {
    int attachment_id;
    if (!param_num(params, "attachment_id", &attachment_id))
        return mcp_tool_err(id, "Missing required parameter: attachment_id");

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", attachment_id);
    const char *q_params[1] = {id_str};

    PGresult *res = PQexecParams(ctx->db->conn,
        "SELECT filename, mime_type, size_bytes, description, uploaded_by, "
        "       created_at::text, encode(data, 'base64') "
        "FROM attachments WHERE id = $1",
        1, NULL, q_params, NULL, NULL, 0);

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = attachment_db_error(res);
        if (res) PQclear(res);
        return mcp_tool_err(id, msg ? msg : "Failed to fetch attachment");
    }
    if (PQntuples(res) == 0) {
        PQclear(res);
        return mcp_tool_err(id, "Attachment not found");
    }

    const char *mime_type = PQgetvalue(res, 0, 1);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "id", attachment_id);
    cJSON_AddStringToObject(meta, "filename", PQgetvalue(res, 0, 0));
    cJSON_AddStringToObject(meta, "mime_type", mime_type);
    cJSON_AddNumberToObject(meta, "size_bytes", atoi(PQgetvalue(res, 0, 2)));
    if (!PQgetisnull(res, 0, 3))
        cJSON_AddStringToObject(meta, "description", PQgetvalue(res, 0, 3));
    if (!PQgetisnull(res, 0, 4))
        cJSON_AddStringToObject(meta, "uploaded_by", PQgetvalue(res, 0, 4));
    cJSON_AddStringToObject(meta, "created_at", PQgetvalue(res, 0, 5));

    /* PQgetvalue's buffer is only valid until PQclear — copy the base64
     * text out before freeing the result. */
    char *b64 = strdup(PQgetvalue(res, 0, 6));
    char mime_copy[64];
    snprintf(mime_copy, sizeof(mime_copy), "%s", mime_type);
    PQclear(res);

    cJSON *result = mcp_tool_ok_with_image(id, meta, b64, mime_copy);
    free(b64);
    return result;
}

/* ============================================================================
 * Registration
 * ============================================================================ */

void attachments_register_tools(McpRegistry *r) {
    cJSON *s = schema_new();
    schema_num(s, "attachment_id", "Numeric attachment ID (from kabai_get_ticket_detailed's "
        "attachments array, or a canvas image element's content.attachment_id)", true);
    mcp_registry_add(r, "kabai_get_attachment",
        "Fetch one image attachment's full content as an MCP image block, plus its "
        "metadata (filename, mime_type, size_bytes, description, uploaded_by, created_at). "
        "Call this ONLY on explicit need — the image is never embedded automatically "
        "elsewhere (token cost). 'description' is the alt-text authored for non-multimodal "
        "agents: read it first: it may already tell you everything the image shows, without "
        "spending tokens on the image block at all.",
        s, tool_get_attachment);
}
