#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "attachments/attachment_tools.h"
#include "mcp/schema.h"
#include "db/transaction.h"

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
 * Shared helpers for add/delete (ticket #829)
 * ============================================================================ */

#define ATTACH_MAX_BYTES (10 * 1024 * 1024)  /* mirrors the V13 CHECK */
#define ATTACH_LIMIT_HINT "only PNG/JPEG/WebP/GIF up to 10 MiB are accepted; convert or shrink the file"

/* The MIME type comes ONLY from the content, never from extension or caller
 * input: a non-image file (key, config, ...) can therefore never be stored. */
static const char *sniff_mime(const unsigned char *b, size_t n) {
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0)) return "image/gif";
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) return "image/webp";
    return NULL;
}

static const char *mime_ext(const char *mime) {
    if (strcmp(mime, "image/png") == 0)  return "png";
    if (strcmp(mime, "image/jpeg") == 0) return "jpg";
    if (strcmp(mime, "image/gif") == 0)  return "gif";
    return "webp";
}

/* Standard-alphabet base64; ASCII whitespace is skipped. Returns the decoded
 * buffer (caller frees) or NULL if the input is not valid base64. */
static unsigned char *b64_decode(const char *s, size_t *out_len) {
    size_t cap = strlen(s) / 4 * 3 + 3, n = 0;
    unsigned char *out = malloc(cap);
    if (!out) return NULL;
    unsigned acc = 0;
    int bits = 0, pad = 0;
    for (const char *p = s; *p; p++) {
        int v;
        char c = *p;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') { pad++; continue; }
        if (pad) goto bad;  /* data after padding */
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else goto bad;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (unsigned char)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    if (pad > 2 || bits >= 6) goto bad;  /* a lone trailing sextet cannot encode a byte */
    *out_len = n;
    return out;
bad:
    free(out);
    return NULL;
}

/* Reads a regular file of 1..10 MiB. On failure returns NULL and sets *err. */
static unsigned char *read_image_file(const char *path, size_t *out_len, const char **err) {
    /* O_NONBLOCK: opening a FIFO must not hang before fstat can reject it. */
    int flags = O_RDONLY;
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    int fd = open(path, flags);
    if (fd < 0) { *err = "Cannot open file_path: check that the path exists and is readable"; return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *err = "file_path is not a regular file (directory, pipe or device): pass the path of an image file";
        return NULL;
    }
    if (st.st_size <= 0 || st.st_size > ATTACH_MAX_BYTES) {
        close(fd);
        *err = st.st_size <= 0
            ? "File is empty: " ATTACH_LIMIT_HINT
            : "File exceeds 10 MiB: " ATTACH_LIMIT_HINT;
        return NULL;
    }
    size_t want = (size_t)st.st_size, got = 0;
    unsigned char *buf = malloc(want);
    while (buf && got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!buf || got != want) {
        free(buf);
        *err = "Could not read the whole file (it changed while reading?): retry";
        return NULL;
    }
    *out_len = want;
    return buf;
}

/* Same message as the V11 archive guard triggers. NULL when the ticket exists
 * and its project is writable; otherwise a message copied into buf.
 * ticket_attachments has no V11 trigger, and no schema change is allowed here. */
static const char *ticket_write_check(PGconn *conn, const char *ticket_id_str, char *buf, size_t cap) {
    const char *p[1] = {ticket_id_str};
    PGresult *res = PQexecParams(conn,
        "SELECT p.archived FROM tickets t JOIN projects p ON p.id = t.project_id WHERE t.id = $1",
        1, NULL, p, NULL, NULL, 0);
    const char *msg = NULL;
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) msg = "Failed to look up ticket";
    else if (PQntuples(res) == 0) msg = "Ticket not found";
    else if (PQgetvalue(res, 0, 0)[0] == 't') {
        snprintf(buf, cap, "Ticket #%s belongs to an archived project and is read-only — "
                 "a human can reactivate the project in the UI before this can be changed.", ticket_id_str);
        msg = buf;
    }
    if (res) PQclear(res);
    return msg;
}

/* ============================================================================
 * kabai_add_attachment (ticket #829)
 * ============================================================================ */

static cJSON *tool_add_attachment(McpContext *ctx, cJSON *id, cJSON *params) {
    int ticket_id;
    if (!param_num(params, "ticket_id", &ticket_id))
        return mcp_tool_err(id, "Missing required parameter: ticket_id");
    const char *description = param_str(params, "description");
    if (!description || !*description)
        return mcp_tool_err(id, "Missing required parameter: description — the alt-text that tells "
                                "non-multimodal agents what the image shows");
    const char *path = param_str(params, "file_path");
    const char *b64  = param_str(params, "data_base64");
    if ((path && *path) == (b64 && *b64))
        return mcp_tool_err(id, "Pass exactly one of file_path or data_base64");

    unsigned char *data = NULL;
    size_t len = 0;
    if (path && *path) {
        const char *err = NULL;
        data = read_image_file(path, &len, &err);
        if (!data) return mcp_tool_err(id, err);
    } else {
        data = b64_decode(b64, &len);
        if (!data) return mcp_tool_err(id, "data_base64 is not valid standard base64: re-encode the file or use file_path");
        if (len == 0 || len > ATTACH_MAX_BYTES) {
            free(data);
            return mcp_tool_err(id, "Decoded data is empty or exceeds 10 MiB: " ATTACH_LIMIT_HINT);
        }
    }

    const char *mime = sniff_mime(data, len);
    if (!mime) {
        free(data);
        return mcp_tool_err(id, "Content is not a PNG, JPEG, WebP or GIF image (detected from the file's "
                                "bytes, the extension is ignored): " ATTACH_LIMIT_HINT);
    }

    /* Filename: caller value or path basename, directories stripped. */
    const char *name = param_str(params, "filename");
    if (!name || !*name) name = (path && *path) ? path : NULL;
    char fname[256];
    if (name) {
        const char *slash = strrchr(name, '/');
        const char *bslash = strrchr(name, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        snprintf(fname, sizeof(fname), "%s", slash ? slash + 1 : name);
    } else {
        fname[0] = '\0';
    }
    if (!fname[0]) snprintf(fname, sizeof(fname), "attachment.%s", mime_ext(mime));

    char tid[32], size_str[32];
    snprintf(tid, sizeof(tid), "%d", ticket_id);
    snprintf(size_str, sizeof(size_str), "%zu", len);

    if (!db_begin_transaction(ctx->db)) {
        free(data);
        return mcp_tool_err(id, "Failed to start transaction");
    }
    char guard[256];
    const char *gmsg = ticket_write_check(ctx->db->conn, tid, guard, sizeof(guard));
    if (gmsg) {
        free(data);
        db_rollback_transaction(ctx->db);
        return mcp_tool_err(id, gmsg);
    }

    /* data goes over the wire as binary bytea (format 1), everything else as text. */
    const char *vals[6] = {fname, mime, size_str, (const char *)data, description, ctx->agent_name};
    int lens[6]    = {0, 0, 0, (int)len, 0, 0};
    int formats[6] = {0, 0, 0, 1, 0, 0};
    PGresult *res = PQexecParams(ctx->db->conn,
        "INSERT INTO attachments (filename, mime_type, size_bytes, data, description, uploaded_by) "
        "VALUES ($1, $2, $3::int, $4::bytea, $5, $6) RETURNING id",
        6, NULL, vals, lens, formats, 0);
    free(data);
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        const char *msg = attachment_db_error(res);
        if (res) PQclear(res);
        db_rollback_transaction(ctx->db);
        return mcp_tool_err(id, msg ? msg : "Failed to store attachment");
    }
    int att_id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    char aid[32];
    snprintf(aid, sizeof(aid), "%d", att_id);
    const char *lp[2] = {tid, aid};
    res = PQexecParams(ctx->db->conn,
        "INSERT INTO ticket_attachments (ticket_id, attachment_id) VALUES ($1, $2)",
        2, NULL, lp, NULL, NULL, 0);
    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        const char *msg = attachment_db_error(res);
        if (res) PQclear(res);
        db_rollback_transaction(ctx->db);
        return mcp_tool_err(id, msg ? msg : "Failed to link attachment to ticket");
    }
    PQclear(res);

    if (!db_commit_transaction(ctx->db))
        return mcp_tool_err(id, "Failed to commit transaction");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "attachment_id", att_id);
    cJSON_AddNumberToObject(r, "ticket_id", ticket_id);
    cJSON_AddStringToObject(r, "filename", fname);
    cJSON_AddStringToObject(r, "mime_type", mime);
    cJSON_AddNumberToObject(r, "size_bytes", (double)len);
    return mcp_tool_ok(id, r);
}

/* ============================================================================
 * kabai_delete_attachment (ticket #829)
 * ============================================================================ */

/* Canvas image elements reference attachments via content->>'attachment_id'
 * with no FK (V13), so every delete path has to look there itself. */
#define CANVAS_REF_SQL \
    "SELECT id, canvas_id FROM canvas_elements WHERE content->>'attachment_id' = $1 ORDER BY id LIMIT 1"

static cJSON *tool_delete_attachment(McpContext *ctx, cJSON *id, cJSON *params) {
    int att_id, ticket_id = 0;
    if (!param_num(params, "attachment_id", &att_id))
        return mcp_tool_err(id, "Missing required parameter: attachment_id");
    bool with_ticket = param_num(params, "ticket_id", &ticket_id);

    char aid[32], tid[32];
    snprintf(aid, sizeof(aid), "%d", att_id);
    snprintf(tid, sizeof(tid), "%d", ticket_id);
    const char *ap[1] = {aid};
    char guard[256], errbuf[400];
    const char *fail = NULL;
    bool deleted = false;

    if (!db_begin_transaction(ctx->db))
        return mcp_tool_err(id, "Failed to start transaction");

    PGresult *res = PQexecParams(ctx->db->conn, "SELECT 1 FROM attachments WHERE id = $1", 1, NULL, ap, NULL, NULL, 0);
    bool exists = res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0;
    if (res) PQclear(res);
    if (!exists) { db_rollback_transaction(ctx->db); return mcp_tool_err(id, "Attachment not found"); }

    if (with_ticket) {
        if ((fail = ticket_write_check(ctx->db->conn, tid, guard, sizeof(guard)))) goto rollback;
        const char *lp[2] = {tid, aid};
        res = PQexecParams(ctx->db->conn,
            "DELETE FROM ticket_attachments WHERE ticket_id = $1 AND attachment_id = $2",
            2, NULL, lp, NULL, NULL, 0);
        bool ok = res && PQresultStatus(res) == PGRES_COMMAND_OK;
        bool linked = ok && atoi(PQcmdTuples(res)) > 0;
        if (res) PQclear(res);
        if (!ok) { fail = "Failed to remove the attachment link"; goto rollback; }
        if (!linked) {
            snprintf(errbuf, sizeof(errbuf), "Attachment #%d is not linked to ticket #%d: check "
                     "kabai_get_ticket_detailed's attachments array", att_id, ticket_id);
            fail = errbuf;
            goto rollback;
        }
        /* Row goes only when nothing else (other ticket, canvas) still points at it. */
        res = PQexecParams(ctx->db->conn,
            "DELETE FROM attachments a WHERE a.id = $1::int "
            "  AND NOT EXISTS (SELECT 1 FROM ticket_attachments WHERE attachment_id = a.id) "
            "  AND NOT EXISTS (SELECT 1 FROM canvas_elements WHERE content->>'attachment_id' = $1::text)",
            1, NULL, ap, NULL, NULL, 0);
        ok = res && PQresultStatus(res) == PGRES_COMMAND_OK;
        deleted = ok && atoi(PQcmdTuples(res)) > 0;
        if (res) PQclear(res);
        if (!ok) { fail = "Failed to delete the attachment"; goto rollback; }
    } else {
        res = PQexecParams(ctx->db->conn, CANVAS_REF_SQL, 1, NULL, ap, NULL, NULL, 0);
        if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            snprintf(errbuf, sizeof(errbuf), "Attachment #%d is used by canvas element #%s (canvas #%s): "
                     "delete that element with kabai_delete_canvas_element first, or pass ticket_id to "
                     "only unlink it from one ticket", att_id, PQgetvalue(res, 0, 0), PQgetvalue(res, 0, 1));
            fail = errbuf;
        } else if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
            fail = "Failed to check canvas references";
        }
        if (res) PQclear(res);
        if (fail) goto rollback;

        /* Deleting removes the links too; refuse if any linked ticket is read-only. */
        res = PQexecParams(ctx->db->conn,
            "SELECT ta.ticket_id FROM ticket_attachments ta JOIN tickets t ON t.id = ta.ticket_id "
            "JOIN projects p ON p.id = t.project_id WHERE ta.attachment_id = $1 AND p.archived LIMIT 1",
            1, NULL, ap, NULL, NULL, 0);
        if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
            snprintf(errbuf, sizeof(errbuf), "Ticket #%s belongs to an archived project and is read-only — "
                     "a human can reactivate the project in the UI before this can be changed.",
                     PQgetvalue(res, 0, 0)), fail = errbuf;
        else if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
            fail = "Failed to check linked tickets";
        if (res) PQclear(res);
        if (fail) goto rollback;

        res = PQexecParams(ctx->db->conn, "DELETE FROM attachments WHERE id = $1", 1, NULL, ap, NULL, NULL, 0);
        deleted = res && PQresultStatus(res) == PGRES_COMMAND_OK;
        if (res) PQclear(res);
        if (!deleted) { fail = "Failed to delete the attachment"; goto rollback; }
    }

    if (!db_commit_transaction(ctx->db))
        return mcp_tool_err(id, "Failed to commit transaction");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "attachment_id", att_id);
    if (with_ticket) cJSON_AddNumberToObject(r, "ticket_id", ticket_id);
    cJSON_AddBoolToObject(r, "unlinked", with_ticket);
    cJSON_AddBoolToObject(r, "attachment_deleted", deleted);
    return mcp_tool_ok(id, r);

rollback:
    db_rollback_transaction(ctx->db);
    return mcp_tool_err(id, fail);
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

    s = schema_new();
    schema_num(s, "ticket_id", "Numeric ticket ID to attach the image to", true);
    schema_str(s, "file_path", "Absolute path of a local image file (preferred: no base64 through the context). "
        "Exactly one of file_path / data_base64", false);
    schema_str(s, "data_base64", "Standard base64 of the image bytes. Exactly one of file_path / data_base64", false);
    schema_str(s, "filename", "Display filename (default: basename of file_path)", false);
    schema_str(s, "description", "REQUIRED alt-text: what the image shows, so non-multimodal agents can read it "
        "without fetching the image", true);
    mcp_registry_add(r, "kabai_add_attachment",
        "Attach an image to a ticket (atomic: stores the image and links it). Only PNG, JPEG, WebP and GIF up "
        "to 10 MiB are accepted; the type is detected from the file's bytes, never from the extension, and "
        "anything else is rejected. 'description' is required (alt-text, same rule as canvas images). Returns "
        "attachment_id, ticket_id, filename, mime_type, size_bytes (no image data). Fails on tickets of "
        "archived projects. Fetch an image later with kabai_get_attachment.",
        s, tool_add_attachment);

    s = schema_new();
    schema_num(s, "attachment_id", "Numeric attachment ID", true);
    schema_num(s, "ticket_id", "Only remove the link to this ticket; the image itself is deleted once no "
        "ticket or canvas element references it any more", false);
    mcp_registry_add(r, "kabai_delete_attachment",
        "Delete an attachment. With ticket_id: unlink it from that ticket only (the image is deleted too when "
        "nothing else references it). Without ticket_id: delete the image and all its ticket links, but only "
        "if no canvas element uses it (otherwise the error names the element). Returns unlinked and "
        "attachment_deleted flags.",
        s, tool_delete_attachment);
}
