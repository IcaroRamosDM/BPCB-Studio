#include "studio.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    const char *kind;
    const char *label;
    const char *const *fields;
    size_t field_count;
} catalog;

/* JSON files are embedded by CMake; no runtime data files are required. */
#include "catalog/board.h"
#include "catalog/component.h"
#include "catalog/device.h"
#include "catalog/signal.h"
#include "catalog/test.h"
#include "catalog/parameter.h"
#include "catalog/formula.h"
#include "catalog/step.h"
#include "catalog/document.h"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static const catalog catalogs[] = {
    {"board", "Identificação da placa", catalog_board, ARRAY_COUNT(catalog_board)},
    {"component", "Componente substituível", catalog_component, ARRAY_COUNT(catalog_component)},
    {"device", "Dispositivo I2C", catalog_device, ARRAY_COUNT(catalog_device)},
    {"signal", "Pino ou canal configurável", catalog_signal, ARRAY_COUNT(catalog_signal)},
    {"test", "Teste da PCB", catalog_test, ARRAY_COUNT(catalog_test)},
    {"parameter", "Configuração do operador", catalog_parameter, ARRAY_COUNT(catalog_parameter)},
    {"formula", "Previsão antes do teste", catalog_formula, ARRAY_COUNT(catalog_formula)},
    {"step", "Etapa do teste", catalog_step, ARRAY_COUNT(catalog_step)},
    {"document", "Documento técnico", catalog_document, ARRAY_COUNT(catalog_document)},
};

#undef ARRAY_COUNT

const char *bs_kind_label(const char *kind) {
    for (size_t i = 0; i < sizeof catalogs / sizeof *catalogs; i++)
        if (!strcmp(kind, catalogs[i].kind))
            return catalogs[i].label;
    return "Cadastro";
}

json_object *bs_catalog(const char *kind) {
    if (!strcmp(kind, "connection")) {
        json_object *a = json_object_new_array();
        bs_workbench_catalog(kind, a);
        return a;
    }
    if (!strcmp(kind, "channel")) {
        json_object *a = json_object_new_array();
        bs_lab_catalog(kind, a);
        return a;
    }
    for (size_t i = 0; i < sizeof catalogs / sizeof *catalogs; i++)
        if (!strcmp(kind, catalogs[i].kind)) {
            json_object *a = json_object_new_array();
            if (!a)
                return NULL;
            for (size_t field_index = 0; field_index < catalogs[i].field_count; field_index++) {
                json_object *field = json_tokener_parse(catalogs[i].fields[field_index]);
                if (!field) {
                    json_object_put(a);
                    return NULL;
                }
                json_object_array_add(a, field);
            }
            bs_catalog_extend(kind, a);
            bs_workbench_catalog(kind, a);
            return a;
        }
    return json_object_new_array();
}

json_object *bs_defaults(const char *kind) {
    json_object *fields = bs_catalog(kind), *o = json_object_new_object();
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        json_object *f = json_object_array_get_idx(fields, i);
        json_object_object_add(o, bc_str(f, "key"), json_object_get(bc_get(f, "default")));
    }
    json_object_put(fields);
    return o;
}

int bs_field_visible(json_object *field, json_object *values) {
    const char *protocol = bc_str(field, "when_protocol");
    if (*protocol && strcmp(protocol, bc_str(values, "lab_protocol")))
        return 0;
    const char *lab = bc_str(field, "when_lab"), *mode = bc_str(values, "lab_mode");
    if (!*mode)
        mode = "raw";
    if (*lab && strcmp(lab, mode))
        return 0;
    const char *comm = bc_str(field, "when_comm_mode"), *cm = bc_str(values, "comm_mode");
    if (!*cm)
        cm = "single";
    if (*comm && strcmp(comm, cm))
        return 0;
    const char *bus = bc_str(field, "when_bus");
    if (*bus && strcmp(bus, bc_str(values, "comm_bus")))
        return 0;
    const char *when = bc_str(field, "when");
    if (!*when)
        return 1;
    char *copy = strdup(when), *save = NULL;
    int yes = 0;
    for (char *p = strtok_r(copy, ",", &save); p; p = strtok_r(NULL, ",", &save))
        if (!strcmp(p, bc_str(values, "type")))
            yes = 1;
    free(copy);
    return yes;
}

int bs_identifier(const char *s) {
    if (!s || !*s || strlen(s) > 80 || !(isalpha((unsigned char)*s) || *s == '_'))
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '_'))
            return 0;
    return 1;
}

int bs_form(const char *kind, json_object *raw, json_object **out, bc_error *e) {
    json_object *fields = bs_catalog(kind), *o = json_object_new_object();
    int rc = -1;
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        json_object *f = json_object_array_get_idx(fields, i);
        const char *key = bc_str(f, "key"), *title = bc_str(f, "label"), *type = bc_str(f, "type");
        json_object *v = bc_get(raw, key);
        if (!v)
            v = bc_get(f, "default");
        const char *s = json_object_get_string(v);
        if (!s)
            s = "";
        if (json_object_is_type(v, json_type_string) &&
            (size_t)json_object_get_string_len(v) != strlen(s)) {
            bc_fail(e, "%s: NUL embutido não é aceito; use hexadecimal para bytes.", title);
            goto done;
        }
        if (!strcmp(type, "wiring")) {
            if (!json_object_is_type(v, json_type_array) || json_object_array_length(v) > 128) {
                bc_fail(e, "Use até 128 conexões.");
                goto done;
            }
            json_object_object_add(o, key, bc_clone(v));
        } else if (!strcmp(type, "frame")) {
            if (!v) {
                if (!strcmp(bc_str(raw, "lab_protocol"), "frame") &&
                    !strcmp(bc_str(raw, "lab_mode"), "protocol"))
                    json_object_object_add(o, key, bc_frame_example());
                continue;
            }
            if (bc_frame_validate(v, e) < 0)
                goto done;
            json_object_object_add(o, key, bc_clone(v));
        } else if (!strcmp(type, "channels")) {
            if (!json_object_is_type(v, json_type_array) || json_object_array_length(v) > 8) {
                bc_fail(e, "Use até oito canais.");
                goto done;
            }
            json_object *rows = json_object_new_array(), *ids = json_object_new_object();
            for (size_t j = 0; j < json_object_array_length(v); j++) {
                json_object *row = NULL;
                if (bs_form("channel", json_object_array_get_idx(v, j), &row, e) < 0 ||
                    !bc_lab_id(bc_str(row, "id")) || bc_get(ids, bc_str(row, "id"))) {
                    json_object_put(row);
                    json_object_put(rows);
                    json_object_put(ids);
                    if (!e->text[0])
                        bc_fail(e, "Canal inválido/repetido.");
                    goto done;
                }
                json_object_object_add(ids, bc_str(row, "id"), json_object_new_boolean(1));
                json_object_array_add(rows, row);
            }
            json_object_put(ids);
            json_object_object_add(o, key, rows);
        } else if (!strcmp(type, "replay")) {
            json_object *capture = json_object_is_type(v, json_type_object) ? bc_clone(v)
                                   : v && *s ? bc_parse_json(s, strlen(s), e)
                                             : NULL;
            if (v && *s && !capture)
                goto done;
            if (capture && bc_replay_validate(capture, e) < 0) {
                json_object_put(capture);
                goto done;
            }
            json_object_object_add(o, key, capture);
        } else if (!strcmp(type, "number") || !strcmp(type, "integer") ||
                   !strcmp(type, "pinmask") || !strcmp(type, "boolean")) {
            char *copy = strdup(s);
            if (!copy)
                goto done;
            for (char *p = copy; *p; p++)
                if (*p == ',')
                    *p = '.';
            char *end = NULL;
            errno = 0;
            int hex = strlen(copy) > 2 && copy[0] == '0' && (copy[1] == 'x' || copy[1] == 'X');
            double x = (!strcmp(type, "integer") || !strcmp(type, "pinmask")) && hex
                           ? (double)strtoll(copy, &end, 16)
                           : strtod(copy, &end);
            if (!strcmp(type, "boolean") && (!strcmp(s, "true") || !strcmp(s, "false"))) {
                x = !strcmp(s, "true");
                end = copy + strlen(copy);
            }
            int good = end != copy && !*end && !errno && isfinite(x);
            free(copy);
            if (bc_get(f, "min") && (x < bc_num(f, "min") || x > bc_num(f, "max")))
                good = 0;
            if ((!strcmp(type, "integer") || !strcmp(type, "pinmask")) && floor(x) != x)
                good = 0;
            if (!strcmp(type, "boolean") && x != 0 && x != 1)
                good = 0;
            if (!good) {
                bc_fail(e, "%s: informe um numero dentro da faixa.", title);
                goto done;
            }
            json_object_object_add(o, key,
                                   !strcmp(type, "boolean") ? json_object_new_boolean(x != 0)
                                   : (!strcmp(type, "integer") || !strcmp(type, "pinmask"))
                                       ? json_object_new_int64((int64_t)x)
                                       : json_object_new_double(x));
        } else {
            if (strlen(s) > 32000 || (!strcmp(type, "id") && !bs_identifier(s))) {
                bc_fail(e, "%s: use letras sem acentos, numeros e sublinhado.", title);
                goto done;
            }
            if (!strcmp(type, "choice")) {
                json_object *choices = bc_get(f, "options");
                int found = 0;
                for (size_t j = 0; j < json_object_array_length(choices); j++)
                    if (!strcmp(s, bc_str(json_object_array_get_idx(choices, j), "id")))
                        found = 1;
                if (!found) {
                    bc_fail(e, "%s: escolha uma opcao.", title);
                    goto done;
                }
            }
            json_object_object_add(o, key, json_object_new_string(s));
        }
    }
    if (!strcmp(kind, "channel")) {
        json_object *ch = bc_clone(o);
        json_object_object_add(ch, "comm_enabled", json_object_new_boolean(1));
        json_object_object_add(ch, "comm_mode", json_object_new_string("single"));
        json_object *decl = bs_communication(ch, e);
        json_object_put(ch);
        if (!decl)
            goto done;
        json_object_put(decl);
    }
    if (!strcmp(kind, "test")) {
        json_object *d =
            json_object_get_boolean(bc_get(o, "comm_enabled")) ? bs_communication(o, e) : NULL;
        if (json_object_get_boolean(bc_get(o, "comm_enabled")) && !d)
            goto done;
        int valid = bc_wiring_validate(bc_get(o, "wiring"), d, e);
        json_object_put(d);
        if (valid < 0)
            goto done;
    }
    *out = o;
    o = NULL;
    rc = 0;
done:
    if (o)
        json_object_put(o);
    json_object_put(fields);
    return rc;
}
