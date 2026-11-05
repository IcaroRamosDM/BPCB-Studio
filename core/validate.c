#include "studio.h"
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

static void issue(json_object *a, const char *place, const char *text) {
    json_object *v = json_object_new_object();
    json_object_object_add(v, "where", json_object_new_string(place));
    json_object_object_add(v, "message", json_object_new_string(text));
    json_object_array_add(a, v);
}

static void locate(json_object *issues, size_t first, const char *page, const char *kind,
                   const char *test, int index) {
    for (size_t i = first; i < json_object_array_length(issues); i++) {
        json_object *v = json_object_array_get_idx(issues, i);
        json_object_object_add(v, "page", json_object_new_string(page));
        json_object_object_add(v, "kind", json_object_new_string(kind));
        json_object_object_add(v, "test", json_object_new_string(test));
        json_object_object_add(v, "index", json_object_new_int(index));
    }
}

static int check_form(json_object *a, const char *kind, json_object *v, const char *where) {
    bc_error e = {{0}};
    json_object *n = NULL;
    int rc = bs_form(kind, v, &n, &e);
    if (rc < 0)
        issue(a, where, e.text);
    json_object_put(n);
    return rc;
}

static void unique(json_object *issues, json_object *a, const char *key, const char *where) {
    for (size_t i = 0; i < json_object_array_length(a); i++)
        for (size_t j = 0; j < i; j++)
            if (!strcmp(bc_str(json_object_array_get_idx(a, i), key),
                        bc_str(json_object_array_get_idx(a, j), key)))
                issue(issues, where, "Há identificadores repetidos.");
}

static int numeric(json_object *o, const char *k) {
    json_object *v = bc_get(o, k);
    return v &&
           (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double)) &&
           isfinite(json_object_get_double(v));
}

static void number_fields(json_object *issues, const char *kind, json_object *o,
                          const char *where) {
    json_object *cat = bs_catalog(kind);
    for (size_t i = 0; i < json_object_array_length(cat); i++) {
        json_object *f = json_object_array_get_idx(cat, i);
        const char *t = bc_str(f, "type"), *k = bc_str(f, "key");
        if (bc_get(f, "optional") && !bc_get(o, k))
            continue;
        if ((!strcmp(t, "number") || !strcmp(t, "integer") || !strcmp(t, "pinmask")) &&
            !numeric(o, k))
            issue(issues, where, "Campo numérico ausente ou inválido; edite e salve este item.");
    }
    json_object_put(cat);
}

static void field(json_object *issues, const char *where, json_object *o, const char *k) {
    if (!*bc_str(o, k)) {
        char s[200];
        snprintf(s, sizeof s, "Preencha o campo %s.", k);
        issue(issues, where, s);
    }
}

static void expr(json_object *issues, const char *where, const char *ex, json_object *vars) {
    double v;
    bc_error e = {{0}};
    if (bc_eval(ex, vars, &v, &e) < 0)
        issue(issues, where, e.text);
}

json_object *bs_validate(json_object *p) {
    json_object *issues = json_object_new_array();
    bc_error e = {{0}};
    if (bs_shape(p, &e) < 0) {
        issue(issues, "Projeto", e.text);
        return issues;
    }
    json_object *board = bc_get(p, "board");
    check_form(issues, "board", board, "Placa");
    const char *required[] = {"pcb", "revision", "version", "connections"};
    for (unsigned i = 0; i < 4; i++)
        field(issues, "Placa", board, required[i]);
    json_object *components = bc_get(p, "components"), *devices = bc_get(p, "devices"),
                *signals = bc_get(p, "signals"), *tests = bc_get(p, "tests"),
                *docs = bc_get(p, "documents");
    unique(issues, components, "ref", "Componentes");
    unique(issues, devices, "id", "Dispositivos");
    unique(issues, signals, "id", "Sinais");
    unique(issues, tests, "id", "Testes");
    int invalid_form = 0;
    json_object *base = json_object_new_object();
    for (size_t i = 0; i < json_object_array_length(components); i++) {
        json_object *c = json_object_array_get_idx(components, i);
        const char *ref = bc_str(c, "ref");
        size_t first = json_object_array_length(issues);
        check_form(issues, "component", c, ref);
        field(issues, ref, c, "value");
        double v;
        if (strlen(bc_str(c, "value")) > 180 || strlen(bc_str(c, "description")) > 1000)
            issue(issues, ref, "Valor ou descrição muito longo (180 / 1000 caracteres).");
        if (!strcmp(bc_str(c, "kind"), "resistor") || !strcmp(bc_str(c, "kind"), "capacitor") ||
            !strcmp(bc_str(c, "kind"), "inductor")) {
            if (bs_quantity(bc_str(c, "value"), bc_str(c, "kind"), &v) < 0)
                issue(issues, ref,
                      "Use um valor numérico com unidade, por exemplo 4.7 kΩ ou 3.3 uF.");
            else
                json_object_object_add(base, ref, json_object_new_double(v));
        }
        locate(issues, first, "components", "component", "", (int)i);
    }
    for (size_t i = 0; i < json_object_array_length(devices); i++) {
        json_object *d = json_object_array_get_idx(devices, i);
        const char *id = bc_str(d, "id");
        size_t first = json_object_array_length(issues);
        if (check_form(issues, "device", d, id) < 0) {
            invalid_form = 1;
            locate(issues, first, "devices", "device", "", (int)i);
            continue;
        }
        number_fields(issues, "device", d, id);
        field(issues, id, d, "name");
        if (!strcmp(bc_str(d, "transport"), "linux_i2c")) {
            const char *path = bc_str(d, "path");
            int good = !strncmp(path, "/dev/i2c-", 9) && path[9];
            if (good)
                for (const char *c = path + 9; *c; c++)
                    if (*c < '0' || *c > '9')
                        good = 0;
            if (!good)
                issue(issues, id,
                      "O transporte Linux requer um caminho exato /dev/i2c-N, sem links ou "
                      "subpastas.");
        }
        for (size_t j = 0; j < i; j++) {
            json_object *x = json_object_array_get_idx(devices, j);
            if (!strcmp(bc_str(d, "transport"), bc_str(x, "transport")) &&
                bc_num(d, "address") == bc_num(x, "address") &&
                (!strcmp(bc_str(d, "transport"), "ch341") ||
                 !strcmp(bc_str(d, "transport"), "selected") ||
                 !strcmp(bc_str(d, "path"), bc_str(x, "path"))))
                issue(issues, id, "Endereço I²C duplicado no mesmo transporte.");
        }
        locate(issues, first, "devices", "device", "", (int)i);
    }
    for (size_t i = 0; i < json_object_array_length(signals); i++) {
        json_object *s = json_object_array_get_idx(signals, i);
        const char *id = bc_str(s, "id");
        size_t first = json_object_array_length(issues);
        if (check_form(issues, "signal", s, id) < 0) {
            invalid_form = 1;
            locate(issues, first, "signals", "signal", "", (int)i);
            continue;
        }
        number_fields(issues, "signal", s, id);
        const char *req[] = {"label", "location", "return", "unit"};
        for (unsigned j = 0; j < 4; j++)
            field(issues, id, s, req[j]);
        json_object *d = bs_find(devices, bc_str(s, "device"));
        if (!d)
            issue(issues, id, "Selecione um dispositivo cadastrado.");
        else {
            const char *driver = bc_str(d, "driver");
            int out = !strcmp(bc_str(s, "direction"), "output");
            int channel = (int)bc_num(s, "channel");
            if (!strcmp(driver, "mcp4728") && (!out || channel > 3 || bc_num(s, "min") < 0 ||
                                               bc_num(s, "max") > bc_num(d, "vref")))
                issue(issues, id, "MCP4728: saída 0..3; limites em volts dentro de 0..Vref.");
            if (!strcmp(driver, "ads7924") && (out || channel > 3 || bc_num(s, "min") < 0 ||
                                               bc_num(s, "max") > bc_num(d, "vref")))
                issue(issues, id, "ADS7924: entrada 0..3; limites em volts dentro de 0..Vref.");
            if (!strcmp(driver, "tca9534a")) {
                int bit = ((unsigned)bc_num(d, "output_mask") >> channel) & 1u;
                if (bit != out || bc_num(s, "min") != 0 || bc_num(s, "max") != 1 ||
                    bc_num(s, "safe") != (int)bc_num(s, "safe"))
                    issue(issues, id,
                          "TCA9534A: limites 0 e 1, valor de encerramento inteiro e direção igual "
                          "à máscara do dispositivo.");
                if (out && ((((unsigned)bc_num(d, "initial_output") >> channel) & 1u) !=
                            (unsigned)bc_num(s, "safe")))
                    issue(issues, id,
                          "A saída inicial do dispositivo deve coincidir com o valor de "
                          "encerramento.");
            }
            if (!strcmp(driver, "register") &&
                (bc_num(s, "shift") + bc_num(s, "bits") > 8 * bc_num(s, "width")))
                issue(issues, id, "Bits + deslocamento excedem a largura do registro.");
            for (size_t j = 0; j < i; j++) {
                json_object *x = json_object_array_get_idx(signals, j);
                if (!strcmp(bc_str(s, "device"), bc_str(x, "device")) &&
                    ((!strcmp(driver, "register") &&
                      bc_num(s, "register") == bc_num(x, "register")) ||
                     (strcmp(driver, "register") && bc_num(s, "channel") == bc_num(x, "channel"))))
                    issue(issues, id, "Um canal/registro não pode ser cadastrado duas vezes.");
            }
        }
        if (*bc_str(s, "simulation_values")) {
            json_object *sequence = bs_simulation_values(bc_str(s, "simulation_values"), &e);
            if (!sequence)
                issue(issues, id, e.text);
            json_object_put(sequence);
        }
        const char *channel = bc_str(s, "calibration_channel");
        if (*channel && strcmp(channel, "none")) {
            if (!d || strcmp(bc_str(d, "driver"), "ads7924") || strcmp(bc_str(s, "unit"), "V") ||
                strcmp(bc_str(s, "direction"), "input") || bc_num(d, "vref") < 2.2 ||
                bc_num(d, "vref") > 5.5 ||
                floor(bc_num(d, "vref") * 1000) != bc_num(d, "vref") * 1000)
                issue(issues, id,
                      "Calibração requer ADS7924, entrada em volts e Vref de 2200 a 5500 mV "
                      "inteiros.");
            for (size_t k = 0; k < i; k++) {
                json_object *other = json_object_array_get_idx(signals, k);
                const char *mapped = bc_str(other, "calibration_channel");
                if (*mapped && strcmp(mapped, "none") &&
                    (!strcmp(mapped, channel) ||
                     strcmp(bc_str(other, "device"), bc_str(s, "device"))))
                    issue(issues, id,
                          "Associe canais distintos de um único ADC à calibração da placa.");
            }
        }
        if (bc_num(s, "min") >= bc_num(s, "max") || bc_num(s, "safe") < bc_num(s, "min") ||
            bc_num(s, "safe") > bc_num(s, "max") || bc_num(s, "simulate") < bc_num(s, "min") ||
            bc_num(s, "simulate") > bc_num(s, "max"))
            issue(issues, id, "Limites, valor simulado ou comando de encerramento incompatíveis.");
        locate(issues, first, "signals", "signal", "", (int)i);
    }

    if (invalid_form) {
        json_object_put(base);
        return issues;
    }
    for (size_t i = 0; i < json_object_array_length(devices); i++) {
        json_object *d = json_object_array_get_idx(devices, i);
        if (strcmp(bc_str(d, "driver"), "tca9534a"))
            continue;
        unsigned covered = 0;
        for (size_t j = 0; j < json_object_array_length(signals); j++) {
            json_object *s = json_object_array_get_idx(signals, j);
            if (!strcmp(bc_str(s, "device"), bc_str(d, "id")) &&
                !strcmp(bc_str(s, "direction"), "output"))
                covered |= 1u << (unsigned)bc_num(s, "channel");
        }
        if (covered != (unsigned)bc_num(d, "output_mask"))
            issue(issues, bc_str(d, "id"),
                  "Cadastre todos os canais marcados como saída para definir seus comandos de "
                  "encerramento.");
    }
    if (!json_object_array_length(tests))
        issue(issues, "Testes", "Crie pelo menos um teste.");
    for (size_t i = 0; i < json_object_array_length(tests); i++) {
        json_object *t = json_object_array_get_idx(tests, i);
        const char *id = bc_str(t, "id");
        if (json_object_get_boolean(bc_get(t, "comm_enabled"))) {
            json_object *d = bs_communication(t, &e);
            if (!d)
                issue(issues, id, e.text);
            json_object_put(d);
        }
        if (check_form(issues, "test", t, id) < 0)
            continue;
        number_fields(issues, "test", t, id);
        field(issues, id, t, "title");
        field(issues, id, t, "instructions");
        if (!strncmp(id, "specific_", 9))
            issue(issues, id, "O prefixo specific_ é reservado aos diagnósticos dos sinais.");
        json_object *vars = bc_clone(base), *pa = bc_get(t, "parameters"),
                    *fs = bc_get(t, "formulas"), *steps = bc_get(t, "steps");
        unique(issues, pa, "id", id);
        unique(issues, fs, "id", id);
        unique(issues, steps, "id", id);
        for (size_t j = 0; j < json_object_array_length(pa); j++) {
            json_object *x = json_object_array_get_idx(pa, j);
            const char *name = bc_str(x, "id");
            size_t first = json_object_array_length(issues);
            check_form(issues, "parameter", x, id);
            number_fields(issues, "parameter", x, id);
            if (bc_get(vars, name) || bs_find(components, name))
                issue(issues, id, "Parâmetro repete o nome de outra variável/componente.");
            if (bc_num(x, "min") >= bc_num(x, "max") || bc_num(x, "default") < bc_num(x, "min") ||
                bc_num(x, "default") > bc_num(x, "max"))
                issue(issues, id, "Valor inicial do parâmetro está fora dos limites.");
            json_object_object_add(vars, name, json_object_new_double(bc_num(x, "default")));
            locate(issues, first, "tests", "parameter", id, (int)j);
        }
        for (size_t j = 0; j < json_object_array_length(fs); j++) {
            json_object *x = json_object_array_get_idx(fs, j);
            const char *name = bc_str(x, "id");
            double value;
            size_t first = json_object_array_length(issues);
            check_form(issues, "formula", x, id);
            if (bc_get(vars, name) || bs_find(components, name))
                issue(issues, id, "Fórmula repete nome já usado.");
            if (bc_eval(bc_str(x, "expression"), vars, &value, &e) < 0)
                issue(issues, id, e.text);
            else
                json_object_object_add(vars, name, json_object_new_double(value));
            locate(issues, first, "tests", "formula", id, (int)j);
        }
        if (*bc_str(t, "simulation_fault") && strcmp(bc_str(t, "simulation_fault"), "none") &&
            !bs_find(steps, bc_str(t, "simulation_at")))
            issue(issues, id, "Escolha a etapa onde injetar a falha simulada.");
        if (bc_get(t, "require_calibration") &&
            json_object_get_boolean(bc_get(t, "require_calibration"))) {
            int mapped = 0;
            for (size_t k = 0; k < json_object_array_length(steps); k++) {
                json_object *st = json_object_array_get_idx(steps, k),
                            *signal = bs_find(signals, bc_str(st, "signal"));
                const char *ch = bc_str(signal, "calibration_channel");
                if ((!strcmp(bc_str(st, "type"), "read") || !strcmp(bc_str(st, "type"), "wait")) &&
                    *ch && strcmp(ch, "none"))
                    mapped = 1;
            }
            if (!mapped)
                issue(issues, id, "O teste exige calibração, mas não lê um canal associado.");
        }
        char *rendered = bs_render(bc_str(t, "instructions"), vars, &e);
        if (!rendered)
            issue(issues, id, e.text);
        free(rendered);
        rendered = bs_render(bc_str(t, "expected"), vars, &e);
        if (!rendered)
            issue(issues, id, e.text);
        free(rendered);
        if (!json_object_array_length(steps))
            issue(issues, id, "Inclua as etapas do teste.");
        unsigned comm_count = 0;
        for (size_t j = 0; j < json_object_array_length(steps); j++)
            if (!strcmp(bc_str(json_object_array_get_idx(steps, j), "type"), "communication"))
                comm_count++;
        if (comm_count > 64)
            issue(issues, id,
                  "Use até 64 blocos de comunicação por teste; separe sequências maiores em testes "
                  "distintos.");
        for (size_t j = 0; j < json_object_array_length(steps); j++) {
            json_object *s = json_object_array_get_idx(steps, j);
            const char *type = bc_str(s, "type");
            char where[240];
            snprintf(where, sizeof where, "%s · etapa %u (%s)", id, (unsigned)(j + 1),
                     bc_str(s, "title"));
            if (check_form(issues, "step", s, where) < 0)
                continue;
            number_fields(issues, "step", s, where);
            field(issues, where, s, "title");
            json_object *sig = bs_find(signals, bc_str(s, "signal"));
            if (!strcmp(type, "read") || !strcmp(type, "write") || !strcmp(type, "wait")) {
                if (!sig)
                    issue(issues, where, "Escolha um sinal existente.");
                else if (!strcmp(type, "write") && strcmp(bc_str(sig, "direction"), "output"))
                    issue(issues, where, "Só é permitido escrever em saída.");
            }
            if (!strcmp(type, "communication")) {
                json_object *op = bs_communication_operation(t, s, &e);
                if (!op)
                    issue(issues, where, e.text);
                else {
                    json_object *sim = bs_communication_result(t, s, NULL, 1, &e);
                    if (!sim)
                        issue(issues, where, e.text);
                    json_object_put(sim);
                }
                json_object_put(op);
            }
            if ((!strcmp(type, "read") || !strcmp(type, "write") || !strcmp(type, "wait") ||
                 !strcmp(type, "initialize"))) {
                json_object *used =
                    bs_find(devices, !strcmp(type, "initialize") ? bc_str(s, "device")
                                                                 : bc_str(sig, "device"));
                if (used) {
                    int selected = !strcmp(bc_str(used, "transport"), "selected"),
                        enabled = json_object_get_boolean(bc_get(t, "comm_enabled"));
                    if (selected) {
                        json_object *ch =
                            enabled ? bs_lab_test_channel(t,
                                                          *bc_str(used, "comm_channel")
                                                              ? bc_str(used, "comm_channel")
                                                              : "main",
                                                          &e)
                                    : NULL;
                        if (!ch || strcmp(bc_str(ch, "comm_bus"), "i2c"))
                            issue(issues, where,
                                  "Dispositivo selecionável exige canal I²C declarado no teste.");
                        json_object_put(ch);
                    }
                    if (enabled && !selected)
                        issue(issues, where,
                              "Não misture o transporte legado com adaptador selecionável no mesmo "
                              "teste.");
                }
            }
            if (!strcmp(type, "initialize") && !bs_find(devices, bc_str(s, "device")))
                issue(issues, where, "Escolha um dispositivo existente.");
            if (!strcmp(type, "manual") || !strcmp(type, "wait") || !strcmp(type, "assert")) {
                if (bc_num(s, "min") > bc_num(s, "max"))
                    issue(issues, where, "Mínimo maior que máximo.");
            }
            if (!strcmp(type, "manual")) {
                field(issues, where, s, "unit");
                const char *display = bc_str(s, "display_unit");
                double value;
                if (*display && bc_parse_reading("1", bc_str(s, "unit"), display, &value, &e) < 0)
                    issue(issues, where, e.text);
                if (!strcmp(bc_str(s, "source"), "rigol")) {
                    const char *unit = !strcmp(bc_str(s, "rigol_metric"), "TIME") ? "ms" : "V";
                    if (strcmp(bc_str(s, "unit"), unit))
                        issue(issues, where, "Rigol: tensões usam V; cursores de tempo usam ms.");
                }
            }
            if (strlen(bc_str(s, "unit")) > 32 || strlen(bc_str(s, "title")) > 160)
                issue(issues, where, "Use título até 160 caracteres e unidade até 32 caracteres.");
            const char *plot_fields[] = {"plot_target", "plot_limit"};
            for (unsigned k = 0; k < 2; k++)
                if (*bc_str(s, plot_fields[k]))
                    expr(issues, where, bc_str(s, plot_fields[k]), vars);
            if (!strcmp(type, "manual") || !strcmp(type, "confirm") || !strcmp(type, "message")) {
                field(issues, where, s, "message");
                char *r = bs_render(bc_str(s, "message"), vars, &e);
                if (!r)
                    issue(issues, where, e.text);
                free(r);
            }
            if (!strcmp(type, "write") || !strcmp(type, "calculate") || !strcmp(type, "assert") ||
                !strcmp(type, "branch") || !strcmp(type, "result"))
                expr(issues, where, bc_str(s, "expression"), vars);
            if (!strcmp(type, "branch")) {
                expr(issues, where, bc_str(s, "right"), vars);
                int found = 0;
                for (size_t k = 0; k < json_object_array_length(steps); k++)
                    if (k != j && !strcmp(bc_str(json_object_array_get_idx(steps, k), "id"),
                                          bc_str(s, "target")))
                        found = 1;
                if (!found)
                    issue(issues, where,
                          "Escolha outra etapa como destino; repetições respeitam o limite "
                          "cadastrado.");
            }
            if (!strcmp(type, "result")) {
                field(issues, where, s, "unit");
                if (*bc_str(s, "reference"))
                    expr(issues, where, bc_str(s, "reference"), vars);
            }
            if (!strcmp(type, "read") || !strcmp(type, "wait") || !strcmp(type, "calculate") ||
                !strcmp(type, "manual") || !strcmp(type, "communication")) {
                const char *name = bc_str(s, "variable");
                if (!bs_identifier(name))
                    issue(issues, where, "Nome da medição inválido.");
                if (bs_find(components, name) || bs_find(pa, name) || bs_find(fs, name))
                    issue(issues, where, "Não sobrescreva componentes, parâmetros ou previsões.");
                double v = 1;
                if (!strcmp(type, "read") || !strcmp(type, "wait"))
                    v = sig ? bc_num(sig, "simulate") : 1;
                if (!strcmp(type, "manual")) {
                    v = bc_num(
                        s, "sample"); /* Um cenário pode reproduzir uma leitura fora da faixa. */
                }
                if (!strcmp(type, "calculate"))
                    (void)bc_eval(bc_str(s, "expression"), vars, &v, &e);
                json_object_object_add(vars, name, json_object_new_double(v));
            }
        }
        json_object_put(vars);
    }
    size_t diagnostics = 0;
    for (size_t i = 0; i < json_object_array_length(signals); i++)
        if (bc_num(json_object_array_get_idx(signals, i), "diagnostic"))
            diagnostics++;
    if (diagnostics + json_object_array_length(tests) > 128)
        issue(issues, "Testes", "Testes e diagnósticos somam mais de 128.");
    for (size_t i = 0; i < json_object_array_length(docs); i++) {
        json_object *d = json_object_array_get_idx(docs, i);
        field(issues, "Documentos", d, "title");
        if (!bc_safe_path(bc_str(d, "file")) || strncmp(bc_str(d, "file"), "assets/", 7))
            issue(issues, "Documentos", "Arquivo deve estar copiado dentro de assets/ do projeto.");
    }
    json_object *order = bs_session_order(p, &e);
    if (!order)
        issue(issues, "Placa", e.text);
    json_object_put(order);
    for (size_t q = 0; q < json_object_array_length(issues); q++) {
        json_object *v = json_object_array_get_idx(issues, q);
        if (bc_get(v, "page"))
            continue;
        const char *where = bc_str(v, "where");
        const char *page = "overview", *kind = "board", *test = "";
        int index = -1;
        const char *lists[] = {"components", "devices", "signals"},
                   *kinds[] = {"component", "device", "signal"};
        for (unsigned k = 0; k < 3; k++) {
            json_object *a = bc_get(p, lists[k]);
            for (size_t j = 0; j < json_object_array_length(a); j++) {
                json_object *row = json_object_array_get_idx(a, j);
                if (!strcmp(where, bc_str(row, k ? "id" : "ref"))) {
                    page = lists[k];
                    kind = kinds[k];
                    index = (int)j;
                }
            }
        }
        for (size_t k = 0; k < json_object_array_length(tests); k++) {
            json_object *t = json_object_array_get_idx(tests, k);
            const char *id = bc_str(t, "id");
            if (!strcmp(where, id) ||
                (g_str_has_prefix(where, id) && !strncmp(where + strlen(id), " · etapa ", 10))) {
                page = "tests";
                kind = "test";
                test = id;
                if (strcmp(where, id)) {
                    unsigned long step =
                        strtoul(where + strlen(id) + strlen(" · etapa "), NULL, 10);
                    if (step > 0 && step <= 512) {
                        kind = "step";
                        index = (int)step - 1;
                    }
                }
            }
        }
        if (!strcmp(where, "Documentos")) {
            page = "documents";
            kind = "document";
        }
        json_object_object_add(v, "page", json_object_new_string(page));
        json_object_object_add(v, "kind", json_object_new_string(kind));
        json_object_object_add(v, "test", json_object_new_string(test));
        json_object_object_add(v, "index", json_object_new_int(index));
    }
    json_object_put(base);
    return issues;
}

int bs_check(json_object *p, bc_error *e) {
    json_object *a = bs_validate(p);
    int rc = 0;
    if (json_object_array_length(a)) {
        json_object *x = json_object_array_get_idx(a, 0);
        rc = bc_fail(e, "%s: %s (%u pendências)", bc_str(x, "where"), bc_str(x, "message"),
                     (unsigned)json_object_array_length(a));
    }
    json_object_put(a);
    return rc;
}
