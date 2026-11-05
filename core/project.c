#include "studio.h"
#include <glib.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <stdio.h>
#ifndef _WIN32
#include <sys/utsname.h>
#include <gnu/libc-version.h>
#endif

static void str(json_object *o, const char *k, const char *v) {
    json_object_object_add(o, k, json_object_new_string(v));
}

json_object *bs_new(void) {
    json_object *p = json_object_new_object();
    str(p, "format", "bpcb.studio");
    json_object_object_add(p, "schema", json_object_new_int(BS_SCHEMA));
    json_object_object_add(p, "board", bs_defaults("board"));
    const char *keys[] = {"components", "devices", "signals", "tests", "documents"};
    for (unsigned i = 0; i < 5; i++)
        json_object_object_add(p, keys[i], json_object_new_array());
    return p;
}

json_object *bs_find(json_object *a, const char *id) {
    if (!a || !json_object_is_type(a, json_type_array))
        return NULL;
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *v = json_object_array_get_idx(a, i);
        if (!strcmp(bc_str(v, "id"), id) || (!*bc_str(v, "id") && !strcmp(bc_str(v, "ref"), id)))
            return v;
    }
    return NULL;
}

json_object *bs_rows(json_object *p, const char *kind, const char *test) {
    if (!strcmp(kind, "component"))
        return bc_get(p, "components");
    if (!strcmp(kind, "device"))
        return bc_get(p, "devices");
    if (!strcmp(kind, "signal"))
        return bc_get(p, "signals");
    if (!strcmp(kind, "document"))
        return bc_get(p, "documents");
    if (!strcmp(kind, "test"))
        return bc_get(p, "tests");
    json_object *t = bs_find(bc_get(p, "tests"), test ? test : "");
    if (!strcmp(kind, "parameter"))
        return bc_get(t, "parameters");
    if (!strcmp(kind, "formula"))
        return bc_get(t, "formulas");
    if (!strcmp(kind, "step"))
        return bc_get(t, "steps");
    return NULL;
}

int bs_put(json_object *p, const char *kind, const char *test, int index, json_object *item,
           bc_error *e) {
    json_object *n = NULL;
    if (bs_form(kind, item, &n, e) < 0)
        return -1;
    if (!strcmp(kind, "board")) {
        json_object_object_add(p, "board", n);
        return 0;
    }
    json_object *a = bs_rows(p, kind, test);
    if (!a || index < -1 || (index >= 0 && (size_t)index >= json_object_array_length(a))) {
        json_object_put(n);
        return bc_fail(e, "Seleção inexistente.");
    }
    const char *key = !strcmp(kind, "component") ? "ref" : "id";
    if (strcmp(kind, "document")) {
        for (size_t i = 0; i < json_object_array_length(a); i++)
            if ((int)i != index &&
                !strcmp(bc_str(json_object_array_get_idx(a, i), key), bc_str(n, key))) {
                json_object_put(n);
                return bc_fail(e, "O identificador já existe nesta lista.");
            }
    }
    if (!strcmp(kind, "test")) {
        const char *keys[] = {"parameters", "formulas", "steps"};
        for (unsigned k = 0; k < 3; k++) {
            json_object *old =
                index >= 0 ? bc_get(json_object_array_get_idx(a, (size_t)index), keys[k]) : NULL;
            json_object_object_add(n, keys[k],
                                   old ? json_object_get(old) : json_object_new_array());
        }
    }
    char *before =
        index >= 0 ? strdup(bc_str(json_object_array_get_idx(a, (size_t)index), key)) : NULL;
    char *variable = index >= 0 && !strcmp(kind, "step")
                         ? strdup(bc_str(json_object_array_get_idx(a, (size_t)index), "variable"))
                         : NULL;
    if (index < 0)
        json_object_array_add(a, n);
    else
        json_object_array_put_idx(a, (size_t)index, n);
    if (before) {
        bs_refactor(p, kind, test, before, bc_str(n, key));
        free(before);
    }
    if (variable) {
        bs_refactor(p, "variable", test, variable, bc_str(n, "variable"));
        free(variable);
    }
    return 0;
}

int bs_delete(json_object *p, const char *kind, const char *test, int index, bc_error *e) {
    json_object *a = bs_rows(p, kind, test);
    if (!a || index < 0 || (size_t)index >= json_object_array_length(a))
        return bc_fail(e, "Seleção inexistente.");
    return json_object_array_del_idx(a, (size_t)index, 1);
}

int bs_move(json_object *a, int index, int direction) {
    int dest = index + direction;
    if (!a || index < 0 || dest < 0 || (size_t)dest >= json_object_array_length(a) ||
        (size_t)index >= json_object_array_length(a))
        return -1;
    json_object *x = json_object_get(json_object_array_get_idx(a, (size_t)index));
    json_object *y = json_object_get(json_object_array_get_idx(a, (size_t)dest));
    json_object_array_put_idx(a, (size_t)index, y);
    json_object_array_put_idx(a, (size_t)dest, x);
    return 0;
}

static int arr(json_object *p, const char *k, size_t limit, bc_error *e) {
    json_object *a = bc_get(p, k);
    if (!a || !json_object_is_type(a, json_type_array) || json_object_array_length(a) > limit)
        return bc_fail(e, "Lista inválida ou muito grande: %s", k);
    for (size_t i = 0; i < json_object_array_length(a); i++)
        if (!json_object_is_type(json_object_array_get_idx(a, i), json_type_object))
            return bc_fail(e, "Item inválido em %s.", k);
    return 0;
}

/* Legacy plain projects retain schema 1. New lab fields must not be silently
   stripped by an old editor, including when added to an imported project. */
int bs_project_schema(json_object *p) {
    int version = bc_num(p, "schema") >= 3 ? 3 : bc_num(p, "schema") >= 2 ? 2 : 1;
    json_object *tests = bc_get(p, "tests");
    for (size_t i = 0; i < json_object_array_length(tests); i++) {
        json_object *t = json_object_array_get_idx(tests, i);
        if (json_object_is_type(bc_get(t, "wiring"), json_type_array) &&
            json_object_array_length(bc_get(t, "wiring")))
            return 3;
        if (!strcmp(bc_str(t, "comm_mode"), "multi"))
            version = version < 2 ? 2 : version;
        json_object *steps = bc_get(t, "steps");
        for (size_t j = 0; j < json_object_array_length(steps); j++) {
            json_object *s = json_object_array_get_idx(steps, j);
            if (!strcmp(bc_str(s, "lab_protocol"), "frame"))
                return 3;
            const char *mode = bc_str(s, "lab_mode");
            if ((*mode && strcmp(mode, "raw")) || bc_get(s, "lab_capture") ||
                *bc_str(s, "comm_channel"))
                version = version < 2 ? 2 : version;
        }
    }
    return version;
}

int bs_shape(json_object *p, bc_error *e) {
    if (!p || strcmp(bc_str(p, "format"), "bpcb.studio") ||
        (!json_object_is_type(bc_get(p, "schema"), json_type_int) ||
         (bc_num(p, "schema") < 1 || bc_num(p, "schema") > BS_SCHEMA)) ||
        !json_object_is_type(bc_get(p, "board"), json_type_object))
        return bc_fail(e, "Este arquivo não é um projeto BPCB Studio compatível.");
    const char *keys[] = {"components", "devices", "signals", "tests", "documents"};
    size_t limits[] = {2000, 64, 256, 100, 100};
    for (unsigned i = 0; i < 5; i++)
        if (arr(p, keys[i], limits[i], e) < 0)
            return -1;
    json_object *a = bc_get(p, "tests");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *t = json_object_array_get_idx(a, i);
        if (arr(t, "parameters", 32, e) < 0 || arr(t, "formulas", 64, e) < 0 ||
            arr(t, "steps", 512, e) < 0)
            return -1;
    }
    return 0;
}

/* Reusa o formatador de instruções da mesma API consumida pelo BPCB. */
char *bs_render(const char *text, json_object *vars, bc_error *e) {
    json_object *m = json_object_new_object(), *t = json_object_new_object(),
                *pa = json_object_new_array();
    json_object_object_foreach(vars, k, v) {
        json_object *q = json_object_new_object();
        str(q, "id", k);
        json_object_object_add(q, "default", json_object_get(v));
        json_object_object_add(q, "min", json_object_new_double(-1.7976931348623157e308));
        json_object_object_add(q, "max", json_object_new_double(1.7976931348623157e308));
        json_object_array_add(pa, q);
    }
    json_object_object_add(t, "parameters", pa);
    str(t, "instructions", text);
    str(t, "expected", "");
    json_object_object_add(m, "components", json_object_new_object());
    json_object_object_add(t, "derived", json_object_new_object());
    json_object *r = bc_preview(m, t, e);
    char *out = r ? strdup(bc_str(r, "instructions")) : NULL;
    json_object_put(r);
    json_object_put(t);
    json_object_put(m);
    return out;
}

static json_object *step(const char *id, const char *type, const char *title) {
    json_object *s = bs_defaults("step");
    str(s, "id", id);
    str(s, "type", type);
    str(s, "title", title);
    return s;
}

static void diagnostics(json_object *p) {
    json_object *a = bc_get(p, "signals"), *ts = bc_get(p, "tests");
    int simulation_only = 1;
    for (size_t i = 0; i < json_object_array_length(ts); i++)
        if (strcmp(bc_str(json_object_array_get_idx(ts, i), "mode"), "simulation"))
            simulation_only = 0;
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *s = json_object_array_get_idx(a, i);
        if (!bc_num(s, "diagnostic"))
            continue;
        char id[120];
        snprintf(id, sizeof id, "specific_%s", bc_str(s, "id"));
        json_object *t = bs_defaults("test");
        str(t, "id", id);
        str(t, "title", bc_str(s, "label"));
        str(t, "purpose", "Diagnóstico individual do sinal cadastrado pelo engenheiro.");
        if (simulation_only)
            str(t, "mode", "simulation");
        str(t, "group", "diagnostics");
        str(t, "instructions", bc_str(bc_get(p, "board"), "connections"));
        str(t, "expected",
            "A conferência física segue os pontos e os limites cadastrados para este sinal.");
        json_object *params = json_object_new_array(), *steps = json_object_new_array();
        json_object_object_add(t, "parameters", params);
        json_object_object_add(t, "formulas", json_object_new_array());
        json_object_object_add(t, "steps", steps);
        json_object *diagnostic_device = bs_find(bc_get(p, "devices"), bc_str(s, "device"));
        if (!strcmp(bc_str(diagnostic_device, "transport"), "selected"))
            json_object_object_add(t, "comm_enabled", json_object_new_boolean(1));
        int output = !strcmp(bc_str(s, "direction"), "output");
        json_object *dev = bs_find(bc_get(p, "devices"), bc_str(s, "device"));
        if (output && !strcmp(bc_str(dev, "driver"), "tca9534a")) {
            json_object *st = step("prepare", "initialize", "Preparar as direções do expansor");
            str(st, "device", bc_str(s, "device"));
            json_object_array_add(steps, st);
        }
        if (output) {
            json_object *par = bs_defaults("parameter");
            str(par, "id", "diagnostic_value");
            str(par, "label", "Valor a aplicar");
            str(par, "unit", bc_str(s, "unit"));
            str(par, "help",
                "O comando permanece até a confirmação e depois retorna ao valor de encerramento.");
            const char *ks[] = {"min", "max", "safe"};
            const char *kd[] = {"min", "max", "default"};
            for (unsigned k = 0; k < 3; k++)
                json_object_object_add(par, kd[k], json_object_get(bc_get(s, ks[k])));
            json_object_array_add(params, par);
            json_object *st = step("apply", "write", "Aplicar o valor");
            str(st, "signal", bc_str(s, "id"));
            str(st, "expression", "diagnostic_value");
            json_object_array_add(steps, st);
            st = step("observe", "confirm", "Conferir o sinal");
            char b[1800];
            snprintf(b, sizeof b,
                     "Sinal: %s. Ponto: %s. Referência: %s.\nConfira a resposta física. Ao "
                     "continuar, o comando de encerramento será aplicado.",
                     bc_str(s, "label"), bc_str(s, "location"), bc_str(s, "return"));
            str(st, "message", b);
            json_object_array_add(steps, st);
        } else {
            json_object *st = step("read_signal", "read", "Ler o sinal");
            str(st, "signal", bc_str(s, "id"));
            str(st, "variable", "diagnostic_reading");
            json_object_array_add(steps, st);
            st = step("show_reading", "result", bc_str(s, "label"));
            str(st, "expression", "diagnostic_reading");
            str(st, "unit", bc_str(s, "unit"));
            json_object_array_add(steps, st);
        }
        json_object_array_add(ts, t);
    }
}

json_object *bs_manifest(json_object *project, int simulate, bc_error *e) {
    if (bs_check(project, e) < 0)
        return NULL;
    json_object *p = bc_clone(project);
    diagnostics(p);
    json_object *m = json_object_new_object(), *b = bc_get(p, "board");
    str(m, "format", "axtor.bancada");
    json_object_object_add(m, "schema", json_object_new_int(1));
    json_object_object_add(m, "api", json_object_new_int(2));
    str(m, "runtime", "native");
    str(m, "min_app", "0.11.0");
    const char *keys[] = {"id", "pcb", "revision", "version", "author"};
    for (unsigned i = 0; i < 5; i++)
        str(m, keys[i], bc_str(b, keys[i]));
#ifdef _WIN32
    const char *platform = "windows-x86_64";
    str(m, "min_app", "0.11.0");
#else
    struct utsname u;
    if (uname(&u) < 0) {
        json_object_put(m);
        json_object_put(p);
        bc_fail(e, "Não foi possível identificar a plataforma.");
        return NULL;
    }
    char platform[160];
    snprintf(platform, sizeof platform, "linux-%s", u.machine);
    str(m, "min_glibc", gnu_get_libc_version());
#endif
    json_object *pl = json_object_new_array();
    json_object_array_add(pl, json_object_new_string(platform));
    json_object_object_add(m, "platforms", pl);
    json_object *cs = json_object_new_object(), *ds = json_object_new_object(),
                *ed = json_object_new_array();
    json_object *a = bc_get(p, "components");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *c = json_object_array_get_idx(a, i);
        const char *r = bc_str(c, "ref");
        str(cs, r, bc_str(c, "value"));
        str(ds, r, bc_str(c, "description"));
        json_object_array_add(ed, json_object_new_string(r));
    }
    json_object_object_add(m, "components", cs);
    json_object_object_add(m, "component_descriptions", ds);
    json_object_object_add(m, "editable_components", ed);
    json_object_object_add(m, "signals", bc_clone(bc_get(p, "signals")));
    json_object_object_add(m, "documents", json_object_new_array());
    json_object *order = bs_session_order(p, e), *plan = json_object_new_object();
    str(plan, "title",
        *bc_str(b, "session_title") ? bc_str(b, "session_title") : "Roteiro da bancada");
    json_object_object_add(plan, "tests", order);
    json_object_object_add(m, "session_plan", plan);
    double vref = 0;
    json_object *sig = bc_get(p, "signals"), *channels = json_object_new_array();
    for (unsigned k = 0; k < 4; k++) {
        json_object *ch = json_object_new_object();
        char label[80];
        snprintf(label, sizeof label, "ADC%u · volts no pino", k);
        str(ch, "label", label);
        str(ch, "factor", "1");
        json_object_object_add(ch, "components", json_object_new_array());
        json_object_array_add(channels, ch);
    }
    for (size_t k = 0; k < json_object_array_length(sig); k++) {
        json_object *signal = json_object_array_get_idx(sig, k);
        const char *ch = bc_str(signal, "calibration_channel");
        if (!*ch || !strcmp(ch, "none"))
            continue;
        vref = bc_num(bs_find(bc_get(p, "devices"), bc_str(signal, "device")), "vref") * 1000;
        str(json_object_array_get_idx(channels, (size_t)atoi(ch)), "label",
            bc_str(signal, "label"));
    }
    if (vref) {
        json_object *cal = json_object_new_object();
        str(cal, "kind", "adc-linear-v1");
        json_object_object_add(cal, "channels", channels);
        json_object_object_add(m, "calibration", cal);
    } else
        json_object_put(channels);
    json_object *tests = json_object_new_array();
    a = bc_get(p, "tests");
    int advanced_communication = 0, workbench = 0;
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *t = json_object_array_get_idx(a, i), *n = json_object_new_object();
        const char *ks[] = {"id", "title", "purpose", "expected", "group"};
        for (unsigned k = 0; k < 5; k++)
            if (*bc_str(t, ks[k]))
                str(n, ks[k], bc_str(t, ks[k]));
        if (json_object_get_boolean(bc_get(t, "comm_enabled"))) {
            json_object *d = bs_communication(t, e);
            if (!d) {
                json_object_put(n);
                json_object_put(tests);
                json_object_put(m);
                json_object_put(p);
                return NULL;
            }
            json_object *operations = json_object_new_array(), *steps = bc_get(t, "steps");
            for (size_t j = 0; j < json_object_array_length(steps); j++) {
                json_object *s = json_object_array_get_idx(steps, j);
                if (!strcmp(bc_str(s, "type"), "communication")) {
                    json_object *op = bs_communication_operation(t, s, e);
                    if (!op) {
                        json_object_put(operations);
                        json_object_put(d);
                        json_object_put(n);
                        json_object_put(tests);
                        json_object_put(m);
                        json_object_put(p);
                        return NULL;
                    }
                    if (bc_get(op, "kind") || bc_get(op, "operation"))
                        advanced_communication = 1;
                    if (!strcmp(bc_str(bc_get(op, "operation") ? bc_get(op, "operation") : op,
                                       "protocol"),
                                "frame"))
                        workbench = 1;
                    json_object_array_add(operations, op);
                }
            }
            if (json_object_array_length(operations))
                json_object_object_add(d, "operations", operations);
            else
                json_object_put(operations);
            json_object_object_add(n, "communication", d);
            if (bc_num(d, "api") == 2)
                advanced_communication = 1;
            str(m, "min_app", advanced_communication ? "0.15.0" : "0.14.0");
        }
        if (json_object_is_type(bc_get(t, "wiring"), json_type_array) &&
            json_object_array_length(bc_get(t, "wiring"))) {
            workbench = 1;
            json_object_object_add(n, "wiring", bc_clone(bc_get(t, "wiring")));
        }
        str(n, "entry", "code/" BS_WORKER_NAME);
        str(n, "mode", simulate ? "simulation" : bc_str(t, "mode"));
        json_object_object_add(n, "enabled", json_object_new_boolean(1));
        json_object_object_add(n, "timeout", json_object_get(bc_get(t, "timeout")));
        json_object *parameters = bc_clone(bc_get(t, "parameters"));
        if (vref && !bs_find(parameters, "vref_mv")) {
            json_object *v = bs_defaults("parameter");
            str(v, "id", "vref_mv");
            str(v, "label", "Referência do ADC (mV)");
            str(v, "unit", "mV");
            str(v, "help",
                "Deve coincidir com a referência do dispositivo cadastrado e da calibração "
                "selecionada.");
            json_object_object_add(v, "default", json_object_new_double(vref));
            json_object_object_add(v, "min", json_object_new_double(vref));
            json_object_object_add(v, "max", json_object_new_double(vref));
            json_object_array_add(parameters, v);
        }
        json_object_object_add(n, "parameters", parameters);
        json_object *dr = json_object_new_object(), *fs = bc_get(t, "formulas");
        for (size_t j = 0; j < json_object_array_length(fs); j++) {
            json_object *f = json_object_array_get_idx(fs, j);
            str(dr, bc_str(f, "id"), bc_str(f, "expression"));
        }
        json_object_object_add(n, "derived", dr);
        char *instructions = malloc(100000);
        if (!instructions) {
            json_object_put(n);
            json_object_put(tests);
            json_object_put(m);
            json_object_put(p);
            return NULL;
        }
        snprintf(instructions, 100000, "CONEXÕES DA PLACA\n%s\n\n%s%s%s", bc_str(b, "connections"),
                 bc_str(t, "instructions"), *bc_str(b, "notes") ? "\n\n● NOTAS DA BANCADA\n" : "",
                 bc_str(b, "notes"));
        char *map = bc_get(n, "wiring") ? bc_wiring_text(bc_get(n, "wiring"), e) : NULL;
        if (map) {
            char *combined = g_strdup_printf("%s\n\n%s", map, instructions);
            str(n, "instructions", combined);
            g_free(combined);
            free(map);
        } else
            str(n, "instructions", instructions);
        free(instructions);
        json_object_array_add(tests, n);
    }
    if (workbench)
        str(m, "min_app", "0.16.0");
    json_object_object_add(m, "tests", tests);
    json_object_object_add(m, "studio", p);
    json_object *source = bc_clone(project);
    json_object_object_add(source, "schema", json_object_new_int(bs_project_schema(project)));
    json_object_object_add(m, "studio_source", source);
    if (bc_validate(m, e) < 0) {
        json_object_put(m);
        return NULL;
    }
    return m;
}

json_object *bs_example(void) {
    json_object *p = bs_new(), *b = bc_get(p, "board");
    str(b, "id", "exemplo_divisor");
    str(b, "pcb", "Exemplo · divisor resistivo");
    str(b, "revision", "V1");
    str(b, "version", "1.0.0");
    str(b, "connections",
        "Exemplo didático sem hardware. Na simulação, informe 5 V na entrada e 2.5 V no ponto "
        "central. Não conecte a PCB para executar este exemplo.");
    str(b, "notes",
        "Este exemplo mostra parâmetros, fórmulas, medições externas, confirmação e comparação com "
        "referência. Edite os valores antes de criar uma bancada real.");
    json_object *c = bs_defaults("component");
    str(c, "ref", "R1");
    str(c, "value", "1 kΩ");
    str(c, "description",
        "Resistor de leitura do divisor; seu valor entra no cálculo da resistência.");
    json_object_array_add(bc_get(p, "components"), c);
    json_object *t = bs_defaults("test");
    str(t, "id", "divisor");
    str(t, "title", "Resistência por divisor");
    str(t, "purpose", "Aprender o fluxo de criação e validar o cálculo com valores simulados.");
    str(t, "mode", "simulation");
    str(t, "instructions",
        "PREPARAÇÃO\nEste é um exemplo sem hardware.\n\nMEDIÇÕES\nInforme as tensões solicitadas "
        "nas pausas. Para uma entrada de {voltage:.2f} V e resistências iguais, o centro deverá "
        "ter {expected_mid:.2f} V.");
    str(t, "expected",
        "Resistência de referência: {R1:.2f} Ω. Tensão central esperada: {expected_mid:.2f} V.");
    json_object *params = json_object_new_array(), *formulas = json_object_new_array(),
                *steps = json_object_new_array();
    json_object_object_add(t, "parameters", params);
    json_object_object_add(t, "formulas", formulas);
    json_object_object_add(t, "steps", steps);
    json_object *v = bs_defaults("parameter");
    str(v, "id", "voltage");
    str(v, "label", "Tensão de entrada");
    str(v, "unit", "V");
    str(v, "help", "Valor usado na previsão; a medição será informada durante a pausa.");
    json_object_object_add(v, "default", json_object_new_double(5));
    json_object_object_add(v, "min", json_object_new_double(1));
    json_object_object_add(v, "max", json_object_new_double(12));
    json_object_array_add(params, v);
    v = bs_defaults("formula");
    str(v, "id", "expected_mid");
    str(v, "expression", "voltage / 2");
    json_object_array_add(formulas, v);
    v = step("preparacao", "confirm", "Conferir a preparação");
    str(v, "message",
        "Simulação do divisor: nenhum dispositivo será acessado. Separe os valores de exemplo "
        "(entrada 5 V; centro 2.5 V) e clique em Continuar.");
    json_object_array_add(steps, v);
    v = step("entrada", "manual", "Informar a tensão de entrada");
    str(v, "message", "Qual é a tensão de entrada? No exemplo, digite 5.");
    str(v, "variable", "u_input");
    str(v, "unit", "V");
    json_object_object_add(v, "min", json_object_new_double(0.001));
    json_object_object_add(v, "max", json_object_new_double(20));
    json_object_object_add(v, "sample", json_object_new_double(5));
    json_object_array_add(steps, v);
    v = step("centro", "manual", "Informar a tensão no centro");
    str(v, "message", "Qual é a tensão no ponto central? No exemplo, digite 2.5.");
    str(v, "variable", "u_mid");
    str(v, "unit", "V");
    json_object_object_add(v, "min", json_object_new_double(0.001));
    json_object_object_add(v, "max", json_object_new_double(20));
    json_object_object_add(v, "sample", json_object_new_double(2.5));
    json_object_array_add(steps, v);
    v = step("resultado", "result", "Resistência calculada");
    str(v, "expression", "R1 * (u_input / u_mid - 1)");
    str(v, "reference", "R1");
    str(v, "unit", "Ω");
    json_object_array_add(steps, v);
    json_object_array_add(bc_get(p, "tests"), t);
    return p;
}

int bs_quantity(const char *text, const char *kind, double *out) {
    char b[192];
    if (!text || strlen(text) >= sizeof b)
        return -1;
    strcpy(b, text);
    for (char *p = b; *p; p++)
        if (*p == ',')
            *p = '.';
    char *end;
    errno = 0;
    double v = strtod(b, &end);
    if (end == b || errno || !isfinite(v) || v < 0)
        return -1;
    while (isspace((unsigned char)*end))
        end++;
    double scale = 1;
    if (!strncmp(end, "µ", 2)) {
        scale = 1e-6;
        end += 2;
    } else if (*end && strchr("kKMGmunp", *end)) {
        switch (*end++) {
            case 'k':
            case 'K':
                scale = 1e3;
                break;
            case 'M':
                scale = 1e6;
                break;
            case 'G':
                scale = 1e9;
                break;
            case 'm':
                scale = 1e-3;
                break;
            case 'u':
                scale = 1e-6;
                break;
            case 'n':
                scale = 1e-9;
                break;
            case 'p':
                scale = 1e-12;
                break;
        }
    }
    while (isspace((unsigned char)*end))
        end++;
    size_t n = strlen(end);
    while (n && isspace((unsigned char)end[n - 1]))
        end[--n] = 0;
    int ok = !*end ||
             (!strcmp(kind, "resistor") &&
              (!strcmp(end, "Ω") || !strcasecmp(end, "ohm") || !strcasecmp(end, "ohms"))) ||
             (!strcmp(kind, "capacitor") && !strcasecmp(end, "F")) ||
             (!strcmp(kind, "inductor") && !strcasecmp(end, "H"));
    *out = v * scale;
    return ok && isfinite(*out) ? 0 : -1;
}
