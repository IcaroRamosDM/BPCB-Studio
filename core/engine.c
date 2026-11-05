#include "studio.h"
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <glib.h>

typedef struct {
    json_object *project, *test, *vars, *touched, *sources, *compiled;
    bs_host *host;
    double deadline;
    int prepared;
    double started;
    json_object *reads, *step;
} run;

static int emit(run *r, const char *kind, const char *text) {
    json_object *v = json_object_new_object();
    json_object_object_add(v, "type", json_object_new_string(kind));
    json_object_object_add(v, "message", json_object_new_string(text));
    int rc = r->host->emit(r->host->context, v);
    json_object_put(v);
    return rc;
}

static int alive(run *r, bc_error *e) {
    if (r->host->stopped(r->host->context))
        return bc_fail(e, "Teste cancelado.");
    if (r->host->clock(r->host->context) > r->deadline)
        return bc_fail(e, "O tempo máximo do teste foi atingido.");
    return 0;
}

static int pause_ms(run *r, unsigned ms, bc_error *e) {
    while (ms) {
        unsigned n = ms > 50 ? 50 : ms;
        if (alive(r, e) < 0 || r->host->sleep_ms(r->host->context, n) < 0)
            return bc_fail(e, "Teste interrompido durante a pausa.");
        ms -= n;
    }
    return alive(r, e);
}

static json_object *device(run *r, json_object *signal) {
    return bs_find(bc_get(r->project, "devices"), bc_str(signal, "device"));
}

static int enabled(json_object *o, const char *key) {
    return json_object_get_boolean(bc_get(o, key));
}

static double option(json_object *o, const char *key, double fallback) {
    return bc_get(o, key) ? bc_num(o, key) : fallback;
}

static int reading_event(run *r, const char *label, const char *unit, double value, double low,
                         double high, const char *source) {
    json_object *ev = json_object_new_object();
    json_object_object_add(ev, "type", json_object_new_string("measurement"));
    json_object_object_add(ev, "label", json_object_new_string(label));
    json_object_object_add(ev, "unit", json_object_new_string(unit));
    json_object_object_add(ev, "value", json_object_new_double(value));
    json_object_object_add(ev, "source",
                           json_object_new_string(r->host->simulation ? "Simulação" : source));
    json_object_object_add(ev, "expected_min", json_object_new_double(low));
    json_object_object_add(ev, "expected_max", json_object_new_double(high));
    json_object_object_add(ev, "elapsed",
                           json_object_new_double(r->host->clock(r->host->context) - r->started));
    json_object_object_add(ev, "step_id", json_object_new_string(bc_str(r->step, "id")));
    json_object *acquired = bc_get(r->sources, bc_str(r->step, "variable"));
    if (acquired)
        json_object_object_add(ev, "acquisition", bc_clone(acquired));
    if (acquired && bc_get(bc_get(acquired, "operation"), "kind"))
        json_object_object_add(
            ev, "communication_valid",
            json_object_new_boolean(json_object_get_boolean(bc_get(acquired, "matched"))));
    if (acquired && bc_get(acquired, "operation") &&
        !bc_get(bc_get(acquired, "operation"), "kind")) {
        json_object *op = bc_get(acquired, "operation");
        json_object_object_add(ev, "transmitted_bytes", bc_clone(bc_get(op, "write")));
        json_object_object_add(ev, "received_bytes", bc_clone(bc_get(acquired, "received")));
        json_object_object_del(ev, "expected_min");
        json_object_object_del(ev, "expected_max");
        if (*bc_str(op, "expect")) {
            json_object_object_add(ev, "byte_comparison", json_object_new_boolean(1));
            json_object_object_add(ev, "expected_bytes", bc_clone(bc_get(op, "expect")));
            if (bc_get(op, "mask"))
                json_object_object_add(ev, "mask_bytes", bc_clone(bc_get(op, "mask")));
            if (bc_get(op, "bits"))
                json_object_object_add(ev, "comparison_bits", bc_clone(bc_get(op, "bits")));
        }
    }

    int rc = r->host->emit(r->host->context, ev);
    json_object_put(ev);
    return rc;
}

static int sample_event(run *r, json_object *s, double value, bc_error *e) {
    if (!enabled(s, "plot") && !enabled(r->step, "plot"))
        return 0;
    const char *label = *bc_str(s, "label") ? bc_str(s, "label") : bc_str(s, "title");
    const char *unit = bc_str(s, "unit");
    if (!*unit)
        unit = "valor";
    json_object *ev = json_object_new_object(), *values = json_object_new_object();
    json_object_object_add(ev, "type", json_object_new_string("sample"));
    json_object_object_add(ev, "unit", json_object_new_string(unit));
    json_object_object_add(ev, "elapsed",
                           json_object_new_double(r->host->clock(r->host->context) - r->started));
    json_object_object_add(values, label, json_object_new_double(value));
    json_object_object_add(ev, "values", values);
    json_object_object_add(
        ev, "source",
        json_object_new_string(r->host->simulation ? "Simulação" : "Leitura da sequência Studio"));
    const char *fields[] = {"plot_target", "plot_limit"}, *keys[] = {"target", "limit"};
    for (unsigned i = 0; i < 2; i++)
        if (*bc_str(r->step, fields[i])) {
            double v;
            if (bc_eval(bc_str(r->step, fields[i]), r->vars, &v, e) < 0) {
                json_object_put(ev);
                return -1;
            }
            json_object_object_add(ev, keys[i], json_object_new_double(v));
        }
    int rc = r->host->emit(r->host->context, ev);
    json_object_put(ev);
    return rc;
}

static int read_signal(run *r, json_object *s, double *v, bc_error *e) {
    if (!s)
        return bc_fail(e, "Sinal inexistente.");
    if (r->host->simulation) {
        json_object *stored = bc_get(r->touched, bc_str(s, "id"));
        *v = stored ? json_object_get_double(stored) : bc_num(s, "simulate");
        if (!stored && *bc_str(s, "simulation_values")) {
            json_object *seq = bs_simulation_values(bc_str(s, "simulation_values"), e);
            if (!seq)
                return -1;
            size_t count = json_object_array_length(seq),
                   at = (size_t)bc_num(r->reads, bc_str(s, "id"));
            *v =
                json_object_get_double(json_object_array_get_idx(seq, at < count ? at : count - 1));
            json_object_put(seq);
            json_object_object_add(r->reads, bc_str(s, "id"),
                                   json_object_new_int64((int64_t)at + 1));
        }
    } else {
        if (!r->host->read || r->host->read(r->host->context, device(r, s), s, v, e) < 0)
            return -1;
        const char *channel = bc_str(s, "calibration_channel");
        if (*channel && strcmp(channel, "none") && r->host->calibration)
            *v = bc_cal_voltage(r->host->calibration, (unsigned)atoi(channel), *v);
    }
    if (!isfinite(*v))
        return bc_fail(e, "Leitura não finita em %s.", bc_str(s, "label"));
    if (sample_event(r, s, *v, e) < 0)
        return -1;
    if (*v < bc_num(s, "min") || *v > bc_num(s, "max")) {
        reading_event(r, bc_str(s, "label"), bc_str(s, "unit"), *v, bc_num(s, "min"),
                      bc_num(s, "max"), "ADC / canal da placa");
        return bc_fail(
            e,
            "Leitura de %s fora dos limites cadastrados; valor registrado, sequência interrompida.",
            bc_str(s, "label"));
    }
    return 0;
}

static int write_signal(run *r, json_object *s, double v, bc_error *e) {
    if (!s || strcmp(bc_str(s, "direction"), "output"))
        return bc_fail(e, "O sinal não é uma saída.");
    if (!isfinite(v) || v < bc_num(s, "min") || v > bc_num(s, "max"))
        return bc_fail(e, "Comando fora da faixa de %s.", bc_str(s, "label"));
    if (v != bc_num(s, "safe") && *bc_str(s, "interlock")) {
        json_object *a = bc_get(r->project, "signals");
        for (size_t i = 0; i < json_object_array_length(a); i++) {
            json_object *x = json_object_array_get_idx(a, i);
            if (x == s || strcmp(bc_str(x, "interlock"), bc_str(s, "interlock")) ||
                strcmp(bc_str(x, "direction"), "output"))
                continue;
            double other;
            if (read_signal(r, x, &other, e) < 0)
                return -1;
            if (other != bc_num(x, "safe"))
                return bc_fail(e,
                               "Intertravamento: %s deve retornar ao comando de encerramento antes "
                               "de ativar %s.",
                               bc_str(x, "label"), bc_str(s, "label"));
        }
    }
    /* Registra antes da transferência: uma falha pode ter alterado parcialmente a saída. */
    json_object_object_add(r->touched, bc_str(s, "id"), json_object_new_double(v));
    if (!r->host->simulation &&
        (!r->host->write || r->host->write(r->host->context, device(r, s), s, v, e) < 0))
        return -1;
    return 0;
}

static int cleanup(run *r, bc_error *e) {
    json_object *a = bc_get(r->project, "signals");
    int failed = 0;
    double limit = r->host->clock(r->host->context) + 5.5;
    /* Ordem explícita do engenheiro, independente da ordem das etapas. */
    for (int order = 0; order <= 1000; order++)
        for (size_t i = 0; i < json_object_array_length(a); i++) {
            json_object *s = json_object_array_get_idx(a, i);
            if ((int)bc_num(s, "stop_order") != order || !bc_get(r->touched, bc_str(s, "id")))
                continue;
            bc_error local = {{0}};
            if (!r->host->simulation &&
                (r->host->clock(r->host->context) > limit || !r->host->write ||
                 r->host->write(r->host->context, device(r, s), s, bc_num(s, "safe"), &local) <
                     0)) {
                failed = 1;
                char b[1000];
                snprintf(b, sizeof b, "Encerramento de %s não confirmado: %s", bc_str(s, "label"),
                         local.text);
                emit(r, "status", b);
            }
        }
    if (failed)
        return bc_fail(
            e, "Falha no encerramento. Estado elétrico desconhecido; confira externamente.");
    if (json_object_object_length(r->touched))
        emit(r, "status",
             r->host->simulation ? "Encerramento simulado concluído."
                                 : "Comandos de encerramento enviados. A descarga e as tensões "
                                   "físicas exigem medição.");
    return 0;
}

static int eval(run *r, json_object *s, const char *field, double *value, bc_error *e) {
    return bc_eval(bc_str(s, field), r->vars, value, e);
}

static int confirm(run *r, const char *id, const char *message, int number, double min, double max,
                   double seconds, double *v, bc_error *e) {
    double remain = r->deadline - r->host->clock(r->host->context);
    if (remain <= 0)
        return bc_fail(e, "Tempo máximo atingido.");
    if (seconds > remain)
        seconds = remain;
    if (!r->host->confirm ||
        r->host->confirm(r->host->context, id, message, number, min, max, seconds, v) < 0)
        return bc_fail(e, "Etapa cancelada ou tempo de confirmação esgotado.");
    if (number && (!isfinite(*v) || *v < min || *v > max))
        return bc_fail(e, "Medição informada fora da faixa permitida.");
    return alive(r, e);
}

static int prepare(run *r, bc_error *e) {
    if (r->prepared)
        return 0;
    char *text = bs_render(bc_str(bc_get(r->project, "board"), "connections"), r->vars, e);
    if (!text)
        return -1;
    char *msg = malloc(strlen(text) + 400);
    if (!msg) {
        free(text);
        return bc_fail(e, "Memória insuficiente.");
    }
    sprintf(msg,
            "%s\n\n%sConfirme as conexões e as condições iniciais descritas acima. O cancelamento "
            "interrompe a sequência.",
            text, r->host->simulation ? "SIMULAÇÃO: nenhum acesso a hardware será feito.\n" : "");
    double ignored = 0;
    int rc = confirm(r, "studio_prepare", msg, 0, 0, 0, 120, &ignored, e);
    free(text);
    free(msg);
    if (!rc)
        r->prepared = 1;
    return rc;
}

static int execution_forms(json_object *project, json_object *test, json_object *compiled,
                           bc_error *e) {
    json_object *normalized = NULL;
    if (bs_form("test", test, &normalized, e) < 0)
        return -1;
    json_object_put(normalized);
    const char *keys[] = {"devices", "signals", "steps", "parameters"},
               *kinds[] = {"device", "signal", "step", "parameter"};
    for (unsigned k = 0; k < 4; k++) {
        json_object *rows = bc_get(k < 2 ? project : test, keys[k]);
        for (size_t i = 0; i < json_object_array_length(rows); i++) {
            json_object *row = json_object_array_get_idx(rows, i);
            normalized = NULL;
            if (bs_form(kinds[k], row, &normalized, e) < 0)
                return -1;
            if (k == 2 && !strcmp(bc_str(normalized, "type"), "communication")) {
                json_object *op = bs_communication_operation(test, normalized, e);
                if (!op) {
                    json_object_put(normalized);
                    return -1;
                }
                json_object *entry = json_object_new_object();
                json_object_object_add(entry, "step", normalized);
                normalized = NULL;
                json_object_object_add(entry, "operation", op);
                json_object_array_put_idx(compiled, i, entry);
            }
            json_object_put(normalized);
        }
    }
    return 0;
}

int bs_execute(json_object *project, const char *test_id, json_object *parameters,
               json_object *components, bs_host *host, bc_error *e) {
    if (bs_shape(project, e) < 0 || !host || !host->clock || !host->stopped || !host->sleep_ms ||
        !host->emit)
        return bc_fail(e, "Contexto de execução inválido.");
    if (host->checkpoint && !host->simulation)
        return bc_fail(e, "Depuração é permitida somente na simulação.");
    run r = {.project = project,
             .test = bs_find(bc_get(project, "tests"), test_id),
             .vars = json_object_new_object(),
             .touched = json_object_new_object(),
             .sources = json_object_new_object(),
             .host = host,
             .reads = json_object_new_object(),
             .compiled = json_object_new_array()};
    int rc = -1;
    if (!r.test) {
        bc_fail(e, "Teste inexistente: %s", test_id);
        goto end;
    }
    if (execution_forms(project, r.test, r.compiled, e) < 0)
        goto end;
    r.started = host->clock(host->context);
    r.deadline = r.started + bc_num(r.test, "timeout");
    if (bs_check_calibration(project, r.test, components, host->calibration, host->simulation, e) <
        0)
        goto end;
    json_object *a = bc_get(project, "components");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *c = json_object_array_get_idx(a, i);
        double v;
        const char *ref = bc_str(c, "ref");
        const char *value = bc_get(components, ref) ? bc_str(components, ref) : bc_str(c, "value");
        if (!strcmp(bc_str(c, "kind"), "resistor") || !strcmp(bc_str(c, "kind"), "capacitor") ||
            !strcmp(bc_str(c, "kind"), "inductor")) {
            if (bs_quantity(value, bc_str(c, "kind"), &v) < 0) {
                bc_fail(e, "Valor inválido no componente %s.", ref);
                goto end;
            }
            json_object_object_add(r.vars, ref, json_object_new_double(v));
        }
    }
    a = bc_get(r.test, "parameters");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *x = json_object_array_get_idx(a, i);
        const char *id = bc_str(x, "id");
        json_object *input = bc_get(parameters, id);
        if (!input)
            input = bc_get(x, "default");
        double v = json_object_get_double(input);
        if (!(json_object_is_type(input, json_type_double) ||
              json_object_is_type(input, json_type_int)) ||
            !isfinite(v) || v < bc_num(x, "min") || v > bc_num(x, "max")) {
            bc_fail(e, "Parâmetro %s fora dos limites.", id);
            goto end;
        }
        json_object_object_add(r.vars, id, json_object_new_double(v));
    }
    a = bc_get(r.test, "formulas");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *x = json_object_array_get_idx(a, i);
        double v;
        if (bc_eval(bc_str(x, "expression"), r.vars, &v, e) < 0)
            goto end;
        json_object_object_add(r.vars, bc_str(x, "id"), json_object_new_double(v));
    }
    a = bc_get(r.test, "steps");
    unsigned visits[512] = {0};
    unsigned total_steps = 0;
    for (size_t index = 0; index < json_object_array_length(a); index++) {
        if (alive(&r, e) < 0)
            goto stop;
        if (++total_steps > 10000) {
            bc_fail(e, "Limite global de etapas atingido.");
            goto stop;
        }
        json_object *s = json_object_array_get_idx(a, index),
                    *sig = bs_find(bc_get(project, "signals"), bc_str(s, "signal"));
        r.step = s;
        if (host->checkpoint) {
            double before = host->clock(host->context);
            if (host->checkpoint(host->context, s, index, total_steps, r.vars, e) < 0)
                goto stop;
            double pause = host->clock(host->context) - before;
            if (!isfinite(pause) || pause < 0 || pause > 7200) {
                bc_fail(e, "Relógio/limite de pausa de depuração inválido.");
                goto stop;
            }
            r.deadline += pause;
            r.started += pause;
            if (alive(&r, e) < 0)
                goto stop;
        }
        if (host->simulation && strcmp(bc_str(r.test, "simulation_fault"), "none") &&
            *bc_str(r.test, "simulation_fault") &&
            !strcmp(bc_str(r.test, "simulation_at"), bc_str(s, "id"))) {
            bc_fail(e, "Cenário simulado: %s antes da etapa %s.",
                    bc_str(r.test, "simulation_fault"), bc_str(s, "title"));
            goto stop;
        }
        const char *type = bc_str(s, "type");
        double v = 0;
        char status[600];
        snprintf(status, sizeof status, "%u/%u · %s", (unsigned)(index + 1),
                 (unsigned)json_object_array_length(a), bc_str(s, "title"));
        emit(&r, "status", status);
        if (!strcmp(type, "confirm") || !strcmp(type, "manual") || !strcmp(type, "message")) {
            char *text = bs_render(bc_str(s, "message"), r.vars, e);
            if (!text)
                goto stop;
            int number = !strcmp(type, "manual"), ok;
            if (number && host->simulation)
                v = bc_num(s, "sample");
            if (number) {
                size_t cap = strlen(text) + 200;
                char *with_unit = malloc(cap);
                if (!with_unit) {
                    free(text);
                    bc_fail(e, "Memória insuficiente.");
                    goto stop;
                }
                snprintf(with_unit, cap, "%s\n\nUnidade: %s. Faixa aceita: %.8g a %.8g.", text,
                         bc_str(s, "unit"), bc_num(s, "min"), bc_num(s, "max"));
                free(text);
                text = with_unit;
            }
            if (!strcmp(type, "message"))
                ok = emit(&r, "status", text);
            else if (number && host->confirm_detail) {
                json_object *request = bc_clone(s), *acquired = NULL;
                json_object_object_add(request, "message", json_object_new_string(text));
                json_object_object_add(
                    request, "timeout",
                    json_object_new_double(
                        fmin(bc_num(s, "timeout"), r.deadline - host->clock(host->context))));
                ok = host->confirm_detail(host->context, request, &v, &acquired);
                json_object_put(request);
                if (acquired)
                    json_object_object_add(r.sources, bc_str(s, "variable"), acquired);
                if (ok < 0)
                    bc_fail(e, "Etapa cancelada ou tempo de confirmação esgotado.");
            } else
                ok = confirm(&r, bc_str(s, "id"), text, number, number ? -DBL_MAX : 0,
                             number ? DBL_MAX : 0, bc_num(s, "timeout"), &v, e);
            free(text);
            if (ok < 0 || alive(&r, e) < 0)
                goto stop;
            if (number) {
                if (!isfinite(v)) {
                    bc_fail(e, "Medição não finita.");
                    goto stop;
                }
                json_object_object_add(r.vars, bc_str(s, "variable"), json_object_new_double(v));
                int outside = v < bc_num(s, "min") || v > bc_num(s, "max");
                if (enabled(s, "record_reading") || outside)
                    reading_event(&r, bc_str(s, "title"), bc_str(s, "unit"), v, bc_num(s, "min"),
                                  bc_num(s, "max"), "Medição externa confirmada pelo operador");
                if (sample_event(&r, s, v, e) < 0)
                    goto stop;
                if (outside) {
                    bc_fail(e,
                            "Medição %.8g %s registrada fora da faixa %.8g a %.8g; sequência "
                            "interrompida em %s.",
                            v, bc_str(s, "unit"), bc_num(s, "min"), bc_num(s, "max"),
                            bc_str(s, "title"));
                    goto stop;
                }
            }
        } else if (!strcmp(type, "communication")) {
            if (prepare(&r, e) < 0)
                goto stop;
            json_object *cached = json_object_array_get_idx(r.compiled, index);
            json_object *prepared = bc_get(cached, "operation");
            s = bc_get(cached, "step");
            r.step = s;
            if (!s || !prepared) {
                bc_fail(e, "Operação não foi validada antes da execução.");
                goto stop;
            }
            /* A callback may retain or modify its argument; keep the validated cache private. */
            json_object *op = host->simulation ? NULL : bc_clone(prepared);
            json_object *response = host->simulation    ? NULL
                                    : host->communicate ? host->communicate(host->context, op, e)
                                                        : NULL;
            json_object_put(op);
            if (!host->simulation && !response) {
                if (!e->text[0])
                    bc_fail(e, "Transporte de comunicação indisponível.");
                goto stop;
            }
            json_object *data = bs_communication_result_prepared(r.test, s, response,
                                                                 host->simulation, prepared, e);
            json_object_put(response);
            if (!data)
                goto stop;
            v = bc_num(data, "value");
            json_object_object_add(r.vars, bc_str(s, "variable"), json_object_new_double(v));
            json_object_object_add(r.sources, bc_str(s, "variable"), data);
            reading_event(&r, bc_str(s, "title"),
                          *bc_str(data, "unit")                        ? bc_str(data, "unit")
                          : !strcmp(bc_str(s, "comm_decode"), "count") ? "bytes"
                                                                       : "código",
                          v, -DBL_MAX, DBL_MAX, "Comunicação da PCB");
            if (sample_event(&r, s, v, e) < 0)
                goto stop;
            if (!json_object_get_boolean(bc_get(data, "matched"))) {
                bc_fail(e, "Resposta/medição inválida (%s); sequência interrompida.",
                        *bc_str(data, "message") ? bc_str(data, "message")
                                                 : "divergência do critério de comunicação");
                goto stop;
            }
        } else if (!strcmp(type, "delay")) {
            if (pause_ms(&r, (unsigned)llround(bc_num(s, "seconds") * 1000), e) < 0)
                goto stop;
        } else if (!strcmp(type, "initialize")) {
            if (prepare(&r, e) < 0)
                goto stop;
            json_object *d = bs_find(bc_get(project, "devices"), bc_str(s, "device"));
            if (!d) {
                bc_fail(e, "Dispositivo ausente.");
                goto stop;
            }
            json_object *ss = bc_get(project, "signals");
            for (size_t j = 0; j < json_object_array_length(ss); j++) {
                json_object *x = json_object_array_get_idx(ss, j);
                if (!strcmp(bc_str(x, "device"), bc_str(d, "id")) &&
                    !strcmp(bc_str(x, "direction"), "output"))
                    json_object_object_add(r.touched, bc_str(x, "id"),
                                           json_object_new_double(bc_num(x, "safe")));
            }
            if (!host->simulation &&
                (!host->initialize || host->initialize(host->context, d, project, e) < 0))
                goto stop;
        } else if (!strcmp(type, "write")) {
            if (prepare(&r, e) < 0 || eval(&r, s, "expression", &v, e) < 0 ||
                write_signal(&r, sig, v, e) < 0)
                goto stop;
        } else if (!strcmp(type, "read") || !strcmp(type, "wait")) {
            if (prepare(&r, e) < 0)
                goto stop;
            double until = fmin(r.deadline, host->clock(host->context) + bc_num(s, "timeout"));
            unsigned seen = 0, stable = 0;
            for (;;) {
                if (seen && host->clock(host->context) > until) {
                    reading_event(&r, bc_str(s, "title"), bc_str(sig, "unit"), v, bc_num(s, "min"),
                                  bc_num(s, "max"), "Última leitura antes do timeout");
                    bc_fail(e, "Tempo esgotado em %s; última leitura %.8g %s.", bc_str(s, "title"),
                            v, bc_str(sig, "unit"));
                    goto stop;
                }
                if (alive(&r, e) < 0 || read_signal(&r, sig, &v, e) < 0)
                    goto stop;
                json_object_object_add(r.vars, bc_str(s, "variable"), json_object_new_double(v));
                seen++;
                if (v >= bc_num(s, "min") && v <= bc_num(s, "max"))
                    stable++;
                else
                    stable = 0;
                if ((!strcmp(type, "read") && seen >= (unsigned)option(s, "samples", 1)) ||
                    (!strcmp(type, "wait") && stable >= (unsigned)option(s, "stable_count", 1)))
                    break;
                if (host->clock(host->context) >= until) {
                    reading_event(&r, bc_str(s, "title"), bc_str(sig, "unit"), v, bc_num(s, "min"),
                                  bc_num(s, "max"), "Última leitura antes do timeout");
                    bc_fail(e, "Tempo esgotado aguardando %s; última leitura %.8g %s.",
                            bc_str(sig, "label"), v, bc_str(sig, "unit"));
                    goto stop;
                }
                if (pause_ms(&r, (unsigned)option(s, "interval_ms", 50), e) < 0)
                    goto stop;
            }
            if (enabled(s, "record_reading"))
                reading_event(&r, bc_str(s, "title"), bc_str(sig, "unit"), v, bc_num(sig, "min"),
                              bc_num(sig, "max"), "ADC / canal da placa");
        } else if (!strcmp(type, "calculate")) {
            if (eval(&r, s, "expression", &v, e) < 0 || sample_event(&r, s, v, e) < 0)
                goto stop;
            json_object_object_add(r.vars, bc_str(s, "variable"), json_object_new_double(v));
        } else if (!strcmp(type, "assert")) {
            if (eval(&r, s, "expression", &v, e) < 0)
                goto stop;
            if (v < bc_num(s, "min") || v > bc_num(s, "max")) {
                bc_fail(e, "Condição não atendida: %s (%.8g; faixa %.8g..%.8g).",
                        bc_str(s, "title"), v, bc_num(s, "min"), bc_num(s, "max"));
                goto stop;
            }
        } else if (!strcmp(type, "branch")) {
            double right;
            if (eval(&r, s, "expression", &v, e) < 0 || eval(&r, s, "right", &right, e) < 0)
                goto stop;
            const char *op = bc_str(s, "operator");
            int yes = (!strcmp(op, "lt") && v < right) || (!strcmp(op, "le") && v <= right) ||
                      (!strcmp(op, "eq") && v == right) || (!strcmp(op, "ne") && v != right) ||
                      (!strcmp(op, "ge") && v >= right) || (!strcmp(op, "gt") && v > right);
            if (yes) {
                if (++visits[index] > (unsigned)bc_num(s, "repeat_limit")) {
                    bc_fail(e, "Limite de repetições atingido em %s.", bc_str(s, "title"));
                    goto stop;
                }
                int found = 0;
                for (size_t j = 0; j < json_object_array_length(a); j++)
                    if (j != index && !strcmp(bc_str(json_object_array_get_idx(a, j), "id"),
                                              bc_str(s, "target"))) {
                        index = j - 1;
                        found = 1;
                        break;
                    }
                if (!found) {
                    bc_fail(e, "Destino inválido na condição.");
                    goto stop;
                }
            }
        } else if (!strcmp(type, "result")) {
            if (eval(&r, s, "expression", &v, e) < 0)
                goto stop;
            json_object *ev = json_object_new_object();
            json_object_object_add(ev, "type", json_object_new_string("measurement"));
            json_object_object_add(ev, "label", json_object_new_string(bc_str(s, "title")));
            json_object_object_add(ev, "value", json_object_new_double(v));
            json_object_object_add(ev, "unit", json_object_new_string(bc_str(s, "unit")));
            json_object_object_add(ev, "elapsed",
                                   json_object_new_double(host->clock(host->context) - r.started));
            json_object_object_add(ev, "inputs", bc_clone(r.sources));
            json_object_object_add(
                ev, "source",
                json_object_new_string(
                    host->simulation ? "Simulação"
                                     : "Sequência BPCB Studio: medições e cálculos das etapas"));
            if (*bc_str(s, "reference")) {
                double expected;
                if (eval(&r, s, "reference", &expected, e) < 0) {
                    json_object_put(ev);
                    goto stop;
                }
                json_object_object_add(ev, "expected", json_object_new_double(expected));
                json_object_object_add(ev, "tolerance_pct",
                                       json_object_new_double(bc_num(s, "tolerance_pct")));
                json_object_object_add(ev, "tolerance_abs",
                                       json_object_new_double(bc_num(s, "tolerance_abs")));
            }
            int ok = host->emit(host->context, ev);
            json_object_put(ev);
            if (sample_event(&r, s, v, e) < 0)
                goto stop;
            if (ok < 0) {
                bc_fail(e, "Canal de resultados fechado.");
                goto stop;
            }
        } else {
            bc_fail(e, "Tipo de etapa desconhecido: %s", type);
            goto stop;
        }
        json_object *state = json_object_new_object();
        json_object_object_add(state, "type", json_object_new_string("status"));
        json_object_object_add(state, "message", json_object_new_string(status));
        json_object_object_add(state, "variables", bc_clone(r.vars));
        json_object_object_add(state, "step_id", json_object_new_string(bc_str(s, "id")));
        host->emit(host->context, state);
        json_object_put(state);
    }
    rc = 0;
stop:
    {
        if (rc < 0) {
            json_object *state = json_object_new_object();
            json_object_object_add(state, "type", json_object_new_string("status"));
            json_object_object_add(state, "message", json_object_new_string(e->text));
            json_object_object_add(state, "variables", bc_clone(r.vars));
            json_object_object_add(state, "step_id", json_object_new_string(bc_str(r.step, "id")));
            host->emit(host->context, state);
            json_object_put(state);
        }
        bc_error cleanup_error = {{0}};
        if (cleanup(&r, &cleanup_error) < 0) {
            bc_fail(e, "%s", cleanup_error.text);
            rc = -1;
        }
    }
end:
    json_object_put(r.compiled);
    json_object_put(r.reads);
    json_object_put(r.vars);
    json_object_put(r.touched);
    json_object_put(r.sources);
    return rc;
}
