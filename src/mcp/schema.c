#include "mcp/schema.h"
#include <stdio.h>

cJSON *schema_new(void) {
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
    cJSON_AddItemToObject(s, "required", cJSON_CreateArray());
    return s;
}

static void schema_add(cJSON *s, const char *name, const char *type,
                       const char *desc, bool required) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(s, "properties"), name, p);
    if (required)
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(s, "required"),
                             cJSON_CreateString(name));
}

void schema_num(cJSON *s, const char *name, const char *desc, bool required) {
    schema_add(s, name, "number", desc, required);
}

void schema_str(cJSON *s, const char *name, const char *desc, bool required) {
    schema_add(s, name, "string", desc, required);
}

void schema_bool(cJSON *s, const char *name, const char *desc, bool required) {
    schema_add(s, name, "boolean", desc, required);
}

void schema_num_array(cJSON *s, const char *name, const char *desc, bool required) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "array");
    cJSON *items = cJSON_CreateObject();
    cJSON_AddStringToObject(items, "type", "number");
    cJSON_AddItemToObject(p, "items", items);
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(s, "properties"), name, p);
    if (required)
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(s, "required"),
                             cJSON_CreateString(name));
}

bool param_num(cJSON *params, const char *name, int *out) {
    cJSON *j = cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsNumber(j)) return false;
    *out = (int)j->valueint;
    return true;
}

bool param_double(cJSON *params, const char *name, double *out) {
    cJSON *j = cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsNumber(j)) return false;
    *out = j->valuedouble;
    return true;
}

const char *param_str(cJSON *params, const char *name) {
    cJSON *j = cJSON_GetObjectItemCaseSensitive(params, name);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

bool param_bool(cJSON *params, const char *name, bool dflt) {
    cJSON *j = cJSON_GetObjectItemCaseSensitive(params, name);
    if (cJSON_IsBool(j)) return cJSON_IsTrue(j);
    return dflt;
}

bool param_is_null(cJSON *params, const char *name) {
    cJSON *j = cJSON_GetObjectItemCaseSensitive(params, name);
    return j && cJSON_IsNull(j);
}

bool param_present(cJSON *params, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(params, name) != NULL;
}

bool param_num_alias(cJSON *params, const char *name, const char *alias, int *out) {
    return param_num(params, name, out) || param_num(params, alias, out);
}

const char *param_str_alias(cJSON *params, const char *name, const char *alias) {
    const char *v = param_str(params, name);
    return v ? v : param_str(params, alias);
}

const char *missing_params_msg(char *buf, size_t size, const char *const *names,
                               const bool *ok, int count) {
    size_t n = (size_t)snprintf(buf, size, "Missing required parameters:");
    const char *sep = " ";
    for (int i = 0; i < count && n < size; i++) {
        if (ok[i]) continue;
        n += (size_t)snprintf(buf + n, size - n, "%s%s", sep, names[i]);
        sep = ", ";
    }
    return buf;
}

static void schema_array_of(cJSON *s, const char *name, const char *item_type,
                            const char *desc, bool required) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "array");
    cJSON *items = cJSON_CreateObject();
    cJSON_AddStringToObject(items, "type", item_type);
    cJSON_AddItemToObject(p, "items", items);
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(s, "properties"), name, p);
    if (required)
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(s, "required"),
                             cJSON_CreateString(name));
}

void schema_str_array(cJSON *s, const char *name, const char *desc, bool required) {
    schema_array_of(s, name, "string", desc, required);
}

void schema_obj_array(cJSON *s, const char *name, const char *desc, bool required) {
    schema_array_of(s, name, "object", desc, required);
}
