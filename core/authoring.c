#include "studio.h"
#include <glib.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

json_object *bs_simulation_values(const char *text, bc_error *e) {
    json_object *out = json_object_new_array();
    char *copy = strdup(text), *save = NULL;
    for (char *v = strtok_r(copy, ";\n\r", &save); v; v = strtok_r(NULL, ";\n\r", &save)) {
        v = g_strstrip(v);
        if (!*v)
            continue;
        for (char *c = v; *c; c++)
            if (*c == ',')
                *c = '.';
        char *end;
        errno = 0;
        double number = strtod(v, &end);
        if (end == v || *end || errno || !isfinite(number) ||
            json_object_array_length(out) >= 3000) {
            bc_fail(e, "Cenário: use até 3000 números finitos separados por ponto e vírgula.");
            goto fail;
        }
        json_object_array_add(out, json_object_new_double(number));
    }
    free(copy);
    if (json_object_array_length(out))
        return out;
    bc_fail(e, "A sequência simulada não contém valores.");
    json_object_put(out);
    return NULL;
fail:
    free(copy);
    json_object_put(out);
    return NULL;
}

int bs_check_calibration(json_object *p, json_object *test, json_object *components,
                         json_object *cal, int simulation, bc_error *e) {
    if (simulation)
        return 0;
    if (cal && (bc_cal_validate(cal, e) < 0 ||
                strcmp(bc_str(cal, "board_id"), bc_str(bc_get(p, "board"), "id")) ||
                strcmp(bc_str(cal, "revision"), bc_str(bc_get(p, "board"), "revision"))))
        return bc_fail(e, "Calibração inválida ou de outra placa/revisão.");
    json_object *values = json_object_new_object(), *a = bc_get(p, "components");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *c = json_object_array_get_idx(a, i);
        const char *id = bc_str(c, "ref");
        json_object_object_add(
            values, id,
            json_object_get(bc_get(components, id) ? bc_get(components, id) : bc_get(c, "value")));
    }
    a = bc_get(test, "steps");
    int rc = 0;
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *step = json_object_array_get_idx(a, i);
        if (strcmp(bc_str(step, "type"), "read") && strcmp(bc_str(step, "type"), "wait"))
            continue;
        json_object *signal = bs_find(bc_get(p, "signals"), bc_str(step, "signal"));
        const char *ch = bc_str(signal, "calibration_channel");
        if (!*ch || !strcmp(ch, "none"))
            continue;
        json_object *device = bs_find(bc_get(p, "devices"), bc_str(signal, "device"));
        if (cal && bc_cal_check(cal, values, bc_num(device, "vref") * 1000, e) < 0) {
            rc = -1;
            break;
        }
        if (json_object_get_boolean(bc_get(test, "require_calibration")) &&
            (!cal || !json_object_get_boolean(bc_get(
                         json_object_array_get_idx(bc_get(cal, "channels"), (size_t)atoi(ch)),
                         "calibrated")))) {
            rc = bc_fail(e, "O teste exige calibração válida do canal associado a %s.",
                         bc_str(signal, "label"));
            break;
        }
    }
    json_object_put(values);
    return rc;
}

json_object *bs_session_order(json_object *p, bc_error *e) {
    json_object *out = json_object_new_array();
    char *copy = strdup(bc_str(bc_get(p, "board"), "session_order")), *save = NULL;
    for (char *v = strtok_r(copy, " ,;\t\r\n", &save); v; v = strtok_r(NULL, " ,;\t\r\n", &save)) {
        if (!bs_find(bc_get(p, "tests"), v) || json_object_array_length(out) >= 16) {
            bc_fail(e, "Roteiro: use até 16 identificadores de testes existentes.");
            goto fail;
        }
        for (size_t i = 0; i < json_object_array_length(out); i++)
            if (!strcmp(v, json_object_get_string(json_object_array_get_idx(out, i)))) {
                bc_fail(e, "Roteiro: teste repetido %s.", v);
                goto fail;
            }
        json_object_array_add(out, json_object_new_string(v));
    }
    free(copy);
    return out;
fail:
    free(copy);
    json_object_put(out);
    return NULL;
}

static char *tokens(const char *text, const char *before, const char *after, int braces_only) {
    GString *out = g_string_new("");
    int braces = 0;
    while (*text) {
        if (*text == '{')
            braces = 1;
        else if (*text == '}')
            braces = 0;
        if ((!braces_only || braces) && (isalpha((unsigned char)*text) || *text == '_')) {
            const char *start = text++;
            while (isalnum((unsigned char)*text) || *text == '_')
                text++;
            size_t n = (size_t)(text - start);
            if (n == strlen(before) && !strncmp(start, before, n))
                g_string_append(out, after);
            else
                g_string_append_len(out, start, (gssize)n);
        } else
            g_string_append_c(out, *text++);
    }
    return g_string_free(out, FALSE);
}

static void refactor(json_object *v, const char *kind, const char *before, const char *after) {
    if (json_object_is_type(v, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(v); i++)
            refactor(json_object_array_get_idx(v, i), kind, before, after);
        return;
    }
    if (!json_object_is_type(v, json_type_object))
        return;
    json_object_object_foreach(v, key, value) {
        if (!json_object_is_type(value, json_type_string)) {
            refactor(value, kind, before, after);
            continue;
        }
        const char *text = json_object_get_string(value);
        int exact =
            (!strcmp(kind, "signal") && !strcmp(key, "signal")) ||
            (!strcmp(kind, "device") && !strcmp(key, "device")) ||
            (!strcmp(kind, "step") && (!strcmp(key, "target") || !strcmp(key, "simulation_at")));
        if (exact && !strcmp(text, before)) {
            json_object_object_add(v, key, json_object_new_string(after));
            continue;
        }
        int expression = !strcmp(key, "expression") || !strcmp(key, "reference") ||
                         !strcmp(key, "right") || !strcmp(key, "plot_target") ||
                         !strcmp(key, "plot_limit");
        int prose = !strcmp(key, "message") || !strcmp(key, "instructions") ||
                    !strcmp(key, "expected") || !strcmp(key, "connections");
        int variable = !strcmp(kind, "component") || !strcmp(kind, "parameter") ||
                       !strcmp(kind, "formula") || !strcmp(kind, "variable");
        if ((variable && (expression || prose)) ||
            (!strcmp(kind, "test") && !strcmp(key, "session_order"))) {
            char *changed = tokens(text, before, after, prose);
            json_object_object_add(v, key, json_object_new_string(changed));
            g_free(changed);
        }
    }
}

void bs_refactor(json_object *project, const char *kind, const char *test, const char *before,
                 const char *after) {
    if (!*before || !strcmp(before, after))
        return;
    json_object *scope = (!strcmp(kind, "step") || !strcmp(kind, "parameter") ||
                          !strcmp(kind, "formula") || !strcmp(kind, "variable"))
                             ? bs_find(bc_get(project, "tests"), test)
                             : project;
    refactor(scope, kind, before, after);
}

static void unique_id(json_object *items, json_object *item, const char *key) {
    char id[81];
    const char *old = bc_str(item, key);
    unsigned n = 1;
    do {
        snprintf(id, sizeof id, "%.65s_copy%u", old, n++);
    } while (bs_find(items, id));
    json_object_object_add(item, key, json_object_new_string(id));
}

int bs_duplicate(json_object *p, const char *kind, const char *test, int index, bc_error *e) {
    json_object *a = bs_rows(p, kind, test);
    if (!a || index < 0 || (size_t)index >= json_object_array_length(a))
        return bc_fail(e, "Selecione o item para duplicar.");
    json_object *item = bc_clone(json_object_array_get_idx(a, (size_t)index));
    unique_id(a, item, !strcmp(kind, "component") ? "ref" : "id");
    if (!strcmp(kind, "step")) {
        const char *type = bc_str(item, "type");
        if (!strcmp(type, "read") || !strcmp(type, "wait") || !strcmp(type, "manual") ||
            !strcmp(type, "calculate")) {
            char name[81];
            unsigned n = 1;
            int used;
            json_object *t = bs_find(bc_get(p, "tests"), test);
            do {
                snprintf(name, sizeof name, "%.65s_copy%u", bc_str(item, "variable"), n++);
                used = bs_find(bc_get(p, "components"), name) ||
                       bs_find(bc_get(t, "parameters"), name) ||
                       bs_find(bc_get(t, "formulas"), name);
                for (size_t i = 0; i < json_object_array_length(a); i++)
                    if (!strcmp(name, bc_str(json_object_array_get_idx(a, i), "variable")))
                        used = 1;
            } while (used);
            json_object_object_add(item, "variable", json_object_new_string(name));
        }
    }
    json_object_array_add(a, item);
    return (int)json_object_array_length(a) - 1;
}

json_object *bs_reuse_test(json_object *p, json_object *source, const char *id, bc_error *e) {
    if (bs_shape(source, e) < 0)
        return NULL;
    json_object *test = bs_find(bc_get(source, "tests"), id);
    if (!test) {
        bc_fail(e, "Teste de origem inexistente.");
        return NULL;
    }
    json_object *copy = bc_clone(p);
    const char *keys[] = {"components", "devices", "signals"};
    for (unsigned k = 0; k < 3; k++) {
        json_object *from = bc_get(source, keys[k]), *to = bc_get(copy, keys[k]);
        for (size_t i = 0; i < json_object_array_length(from); i++) {
            json_object *v = json_object_array_get_idx(from, i);
            const char *name = bc_str(v, k ? "id" : "ref");
            json_object *old = bs_find(to, name);
            if (old && !json_object_equal(old, v)) {
                bc_fail(e, "Conflito em %s: %s. O cadastro atual foi preservado.", keys[k], name);
                goto fail;
            }
            if (!old)
                json_object_array_add(to, bc_clone(v));
        }
    }
    json_object *item = bc_clone(test), *to = bc_get(copy, "tests");
    if (bs_find(to, bc_str(item, "id")))
        unique_id(to, item, "id");
    GString *instructions = g_string_new(bc_str(bc_get(source, "board"), "connections"));
    g_string_append(instructions, "\n\n");
    g_string_append(instructions, bc_str(item, "instructions"));
    json_object_object_add(item, "instructions", json_object_new_string(instructions->str));
    g_string_free(instructions, TRUE);
    json_object_array_add(to, item);
    if (bs_check(copy, e) < 0)
        goto fail;
    return copy;
fail:
    json_object_put(copy);
    return NULL;
}
