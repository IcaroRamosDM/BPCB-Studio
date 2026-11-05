#define _POSIX_C_SOURCE 200809L
#include "../lab.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void text(json_object *o, const char *k, const char *s) {
    json_object_object_add(o, k, json_object_new_string(s));
}

int bc_replay_validate(json_object *spec, bc_error *e) {
    if (bc_lab_object(spec, "|schema||name||bus||exchanges||preamble|", e) < 0 ||
        !json_object_is_type(bc_get(spec, "schema"), json_type_int) || bc_num(spec, "schema") != 1)
        return bc_fail(e, "Reprodução exige schema 1.");
    const char *keys[] = {"name", "bus", "preamble"};
    for (unsigned k = 0; k < 3; k++)
        if (bc_get(spec, keys[k]) && !json_object_is_type(bc_get(spec, keys[k]), json_type_string))
            return bc_fail(e, "Campo textual inválido na captura: %s.", keys[k]);
    const char *bus = bc_str(spec, "bus");
    if (strcmp(bus, "i2c") && strcmp(bus, "spi") && strcmp(bus, "uart") && strcmp(bus, "jtag"))
        return bc_fail(e, "Barramento da reprodução inválido.");
    json_object *rows = bc_get(spec, "exchanges");
    if (!json_object_is_type(rows, json_type_array) || !json_object_array_length(rows) ||
        json_object_array_length(rows) > 512)
        return bc_fail(e, "Reprodução exige 1…512 transações.");
    uint8_t data[BC_COMM_MAX];
    size_t n, total = 0;
    if (bc_comm_hex(bc_str(spec, "preamble"), data, sizeof data, &n, e) < 0 ||
        (n && strcmp(bus, "uart")))
        return bc_fail(e, "Preâmbulo permitido somente em UART.");
    total = n;
    for (size_t i = 0; i < json_object_array_length(rows); i++) {
        json_object *row = json_object_array_get_idx(rows, i), *op = bc_get(row, "operation");
        int delay;
        if (bc_lab_object(row, "|operation||received||delay_ms|", e) < 0 ||
            !json_object_is_type(bc_get(row, "received"), json_type_string) || bc_get(op, "kind") ||
            bc_comm_operation_validate(bus, op, e) < 0 ||
            bc_lab_int(row, "delay_ms", 0, 0, 10000, &delay, e) < 0 ||
            bc_comm_hex(bc_str(row, "received"), data, sizeof data, &n, e) < 0)
            return -1;
        size_t required = (size_t)bc_num(op, "read");
        if (!strcmp(bus, "spi")) {
            size_t written;
            if (bc_comm_hex(bc_str(op, "write"), data, sizeof data, &written, e) < 0)
                return -1;
            required = written;
        } else if (!strcmp(bus, "jtag") && !bc_get(op, "read"))
            required = ((size_t)bc_num(op, "bits") + 7) / 8;
        if (n != required)
            return bc_fail(e, "Transação %u: comprimento gravado incorreto.", (unsigned)i);
        total += n + strlen(bc_str(op, "write")) / 2;
        if (total > 4 * 1024 * 1024)
            return bc_fail(e, "Reprodução excede 4 MiB.");
    }
    return 0;
}

json_object *bc_replay_from_capture(json_object *capture, bc_error *e) {
    json_object *spec = json_object_new_object(), *rows = json_object_new_array();
    json_object_object_add(spec, "schema", json_object_new_int(1));
    text(spec, "name", "Captura importada explicitamente");
    json_object_object_add(spec, "exchanges", rows);
    json_object *trace = bc_get(capture, "trace");
    if (json_object_is_type(trace, json_type_array)) {
        text(spec, "bus", "i2c");
        for (size_t i = 0; i < json_object_array_length(trace); i++) {
            json_object *entry = json_object_array_get_idx(trace, i),
                        *row = json_object_new_object();
            json_object_object_add(row, "operation", bc_clone(bc_get(entry, "operation")));
            text(row, "received", bc_str(entry, "received"));
            json_object_array_add(rows, row);
        }
    } else {
        json_object *events = bc_get(capture, "events");
        if (strcmp(bc_str(capture, "format"), "bpcb.uart-capture") ||
            bc_num(capture, "schema") != 1 || !json_object_is_type(events, json_type_array) ||
            json_object_array_length(events) > 4096 || bc_num(capture, "dropped_events")) {
            bc_fail(
                e, "Use captura UART completa, sem eventos removidos, ou resultado IoT com trace.");
            goto bad;
        }
        text(spec, "bus", "uart");
        json_object *current = NULL;
        uint8_t pending[BC_COMM_MAX];
        size_t used = 0;
        char *session = NULL;
        double previous = 0, transmitted_at = 0;
        int have_time = 0;
        for (size_t i = 0; i < json_object_array_length(events); i++) {
            json_object *event = json_object_array_get_idx(events, i);
            uint8_t bytes[BC_COMM_MAX];
            size_t n = 0;
            double now;
            if (!json_object_is_type(event, json_type_object) ||
                bc_comm_hex(bc_str(event, "hex"), bytes, sizeof bytes, &n, e) < 0 ||
                bc_lab_number(event, "elapsed", 0, 0, 3600, &now, e) < 0 ||
                (have_time && now < previous)) {
                free(session);
                bc_fail(e, "Captura contém evento inválido ou tempo regressivo.");
                goto bad;
            }
            const char *identity =
                json_object_to_json_string_ext(bc_get(event, "session"), JSON_C_TO_STRING_PLAIN);
            if (!identity)
                identity = "null";
            if (!session)
                session = strdup(identity);
            else if (strcmp(session, identity)) {
                free(session);
                bc_fail(e, "Separe as sessões antes de importar a captura.");
                goto bad;
            }
            if (!strcmp(bc_str(event, "direction"), "tx")) {
                char *hex = bc_comm_format_bytes(pending, used, 1);
                if (current) {
                    text(current, "received", hex);
                    json_object_object_add(bc_get(current, "operation"), "read",
                                           json_object_new_int64((int64_t)used));
                } else
                    text(spec, "preamble", hex);
                free(hex);
                used = 0;
                transmitted_at = now;
                current = json_object_new_object();
                json_object *op = json_object_new_object();
                text(op, "write", bc_str(event, "hex"));
                json_object_object_add(op, "read", json_object_new_int(0));
                json_object_object_add(current, "operation", op);
                text(current, "received", "");
                json_object_array_add(rows, current);
            } else if (!strcmp(bc_str(event, "direction"), "rx")) {
                if (used + n > sizeof pending) {
                    free(session);
                    bc_fail(e, "Resposta entre comandos excede 4096 bytes.");
                    goto bad;
                }
                if (current && !used && n) {
                    double delay = (now - transmitted_at) * 1000;
                    if (delay > 10000)
                        delay = 10000;
                    json_object_object_add(current, "delay_ms", json_object_new_int((int)delay));
                }
                memcpy(pending + used, bytes, n);
                used += n;
            } else {
                free(session);
                bc_fail(e, "Direção desconhecida na captura.");
                goto bad;
            }
            previous = now;
            have_time = 1;
        }
        free(session);
        char *hex = bc_comm_format_bytes(pending, used, 1);
        if (current) {
            text(current, "received", hex);
            json_object_object_add(bc_get(current, "operation"), "read",
                                   json_object_new_int64((int64_t)used));
        }
        free(hex);
    }
    if (bc_replay_validate(spec, e) < 0)
        goto bad;
    return spec;
bad:
    json_object_put(spec);
    return NULL;
}

typedef struct {
    json_object *spec, *fault;
    size_t cursor;
    uint8_t queued[BC_COMM_MAX];
    size_t used, at;
    double now, available;
    int broken;
    unsigned fragment;
} replay;

static double replay_clock(void *ctx) {
    return ((replay *)ctx)->now;
}

static int replay_wait(void *ctx, unsigned ms, bc_error *e) {
    (void)e;
    ((replay *)ctx)->now += ms / 1000.0;
    return 0;
}

static json_object *replay_tx(void *ctx, json_object *op, bc_error *e) {
    replay *r = ctx;
    json_object *rows = bc_get(r->spec, "exchanges");
    if (r->cursor >= json_object_array_length(rows)) {
        bc_fail(e, "Reprodução: operação adicional não capturada.");
        return NULL;
    }
    json_object *row = json_object_array_get_idx(rows, r->cursor),
                *original = bc_get(row, "operation");
    const char *bus = bc_str(r->spec, "bus");
    uint8_t a[BC_COMM_MAX], b[BC_COMM_MAX];
    size_t na = 0, nb = 0;
    if (bc_comm_hex(bc_str(op, "write"), a, sizeof a, &na, e) < 0 ||
        bc_comm_hex(bc_str(original, "write"), b, sizeof b, &nb, e) < 0 || na != nb ||
        memcmp(a, b, na) ||
        (!strcmp(bus, "i2c") && bc_num(op, "address") != bc_num(original, "address"))) {
        bc_fail(e, "Reprodução: comando/endereço diverge da captura (%u).", (unsigned)r->cursor);
        return NULL;
    }
    const char *fields[] = {"register", "bits", "idle_cycles", "ir_bits", "ir_write"};
    if (!strcmp(bus, "jtag"))
        for (unsigned i = 0; i < 5; i++) {
            const char *x = json_object_to_json_string_ext(bc_get(op, fields[i]),
                                                           JSON_C_TO_STRING_PLAIN),
                       *y = json_object_to_json_string_ext(bc_get(original, fields[i]),
                                                           JSON_C_TO_STRING_PLAIN);
            if (strcmp(x ? x : "null", y ? y : "null")) {
                bc_fail(e, "Reprodução: comando JTAG diverge.");
                return NULL;
            }
        }
    if ((strcmp(bus, "uart") || bc_num(op, "read") > 0) &&
        bc_num(op, "read") != bc_num(original, "read")) {
        bc_fail(e, "Reprodução: tamanho da leitura diverge.");
        return NULL;
    }
    int injected = (int)r->cursor == (int)bc_num(r->fault, "exchange");
    const char *fault = injected ? bc_str(r->fault, "type") : "none";
    r->fragment = 0;
    if (!strcmp(fault, "fragment")) {
        int fragment = 1;
        bc_lab_int(r->fault, "fragment_size", 1, 1, 4096, &fragment, e);
        r->fragment = (unsigned)fragment;
    }
    r->cursor++;
    if (!strcmp(fault, "nack") || !strcmp(fault, "disconnect")) {
        r->broken = 1;
        errno = !strcmp(fault, "nack") ? ENXIO : EIO;
        bc_fail(e, !strcmp(fault, "nack") ? "NACK simulado." : "Desconexão simulada.");
        return NULL;
    }
    size_t received = 0;
    bc_comm_hex(bc_str(row, "received"), b, sizeof b, &received, e);
    if (!strcmp(fault, "truncate") && received)
        received--;
    if (!strcmp(fault, "corrupt") && received) {
        int offset = 0, xor= 1;
        bc_lab_int(r->fault, "offset", 0, 0, 4095, &offset, e);
        bc_lab_int(r->fault, "xor", 1, 1, 255, &xor, e);
        if ((size_t)offset >= received) {
            bc_fail(e, "Offset da corrupção fora da resposta.");
            return NULL;
        }
        b[offset] ^= (uint8_t)xor;
    }
    int delay = 0;
    bc_lab_int(row, "delay_ms", 0, 0, 10000, &delay, e);
    if (!strcmp(fault, "delay"))
        bc_lab_int(r->fault, "delay_ms", 1000, 0, 60000, &delay, e);
    if (!strcmp(fault, "timeout"))
        delay = 60000;
    json_object *result = json_object_new_object();
    char *sent = bc_comm_format_bytes(a, na, 1);
    text(result, "transmitted", sent);
    free(sent);
    json_object_object_add(result, "matched", json_object_new_boolean(1));
    if (!strcmp(bus, "uart") && bc_num(op, "read") == 0) {
        size_t remain = r->used - r->at;
        if (remain + received > sizeof r->queued) {
            json_object_put(result);
            bc_fail(e, "Fila de reprodução excedida.");
            return NULL;
        }
        memmove(r->queued, r->queued + r->at, remain);
        memcpy(r->queued + remain, b, received);
        r->used = remain + received;
        r->at = 0;
        r->available = r->now + delay / 1000.0;
        text(result, "received", "");
    } else {
        r->now += delay / 1000.0;
        if (!strcmp(fault, "timeout")) {
            json_object_put(result);
            bc_fail(e, "Timeout simulado.");
            return NULL;
        }
        char *hex = bc_comm_format_bytes(b, received, 1);
        text(result, "received", hex);
        free(hex);
        size_t required = !strcmp(bus, "spi") ? na
                          : !strcmp(bus, "jtag") && !bc_get(op, "read")
                              ? ((size_t)bc_num(op, "bits") + 7) / 8
                              : (size_t)bc_num(op, "read");
        if (received != required) {
            json_object_object_add(result, "matched", json_object_new_boolean(0));
            text(result, "message", "Resposta reproduzida com comprimento incorreto.");
        }
        if (bc_get(op, "expect")) {
            uint8_t expected[BC_COMM_MAX], mask[BC_COMM_MAX];
            size_t ne = 0, nm = 0;
            bc_comm_hex(bc_str(op, "expect"), expected, sizeof expected, &ne, e);
            memset(mask, 255, sizeof mask);
            if (bc_get(op, "mask"))
                bc_comm_hex(bc_str(op, "mask"), mask, sizeof mask, &nm, e);
            int same = ne == received;
            for (size_t i = 0; same && i < ne; i++)
                same = (expected[i] & mask[i]) == (b[i] & mask[i]);
            json_object_object_add(result, "matched", json_object_new_boolean(same));
        }
    }
    return result;
}

static int replay_poll(void *ctx, uint8_t *out, size_t cap, unsigned wait, bc_error *e) {
    replay *r = ctx;
    if (r->broken)
        return bc_fail(e, "Transporte simulado desconectado.");
    if (r->at == r->used) {
        json_object *rows = bc_get(r->spec, "exchanges");
        if (r->cursor < json_object_array_length(rows)) {
            json_object *original = bc_get(json_object_array_get_idx(rows, r->cursor), "operation");
            uint8_t bytes[BC_COMM_MAX];
            size_t n = 0;
            if (bc_comm_hex(bc_str(original, "write"), bytes, sizeof bytes, &n, e) < 0)
                return -1;
            if (!n) {
                json_object *q = json_tokener_parse("{\"write\":\"\",\"read\":0}");
                json_object *response = replay_tx(r, q, e);
                json_object_put(q);
                if (!response)
                    return -1;
                json_object_put(response);
            }
        }
    }
    r->now += wait / 1000.0;
    if (r->now < r->available || r->at == r->used)
        return 0;
    size_t n = r->used - r->at;
    if (n > cap)
        n = cap;
    if (r->fragment && n > r->fragment)
        n = r->fragment;
    memcpy(out, r->queued + r->at, n);
    r->at += n;
    return (int)n;
}

static int replay_unread(void *ctx, const uint8_t *bytes, size_t n, bc_error *e) {
    replay *r = ctx;
    size_t remaining = r->used - r->at;
    if (remaining + n > sizeof r->queued)
        return bc_fail(e, "Fila simulada excedida.");
    memmove(r->queued + n, r->queued + r->at, remaining);
    memcpy(r->queued, bytes, n);
    r->used = remaining + n;
    r->at = 0;
    return 0;
}

json_object *bc_replay_execute(json_object *spec, json_object *operation, json_object *fault,
                               bc_error *e) {
    if (bc_replay_validate(spec, e) < 0 ||
        bc_comm_operation_validate(bc_str(spec, "bus"), operation, e) < 0)
        return NULL;
    json_object *empty = NULL;
    if (!fault) {
        empty = json_object_new_object();
        fault = empty;
    }
    int index, fragment;
    if (bc_lab_object(fault, "|type||exchange||fragment_size||delay_ms||offset||xor|", e) < 0 ||
        bc_lab_int(fault, "exchange", 0, 0, 511, &index, e) < 0 ||
        bc_lab_int(fault, "fragment_size", 1, 1, 4096, &fragment, e) < 0) {
        json_object_put(empty);
        return NULL;
    }
    const char *kind = bc_str(fault, "type"),
               *valid[] = {"",         "none",    "fragment",   "delay", "timeout",
                           "truncate", "corrupt", "disconnect", "nack"};
    int found = 0;
    for (unsigned i = 0; i < sizeof valid / sizeof *valid; i++)
        if (!strcmp(kind, valid[i]))
            found = 1;
    if (!found) {
        json_object_put(empty);
        bc_fail(e, "Falha simulada desconhecida.");
        return NULL;
    }
    if (*kind && strcmp(kind, "none") &&
        (size_t)index >= json_object_array_length(bc_get(spec, "exchanges"))) {
        json_object_put(empty);
        bc_fail(e, "Transação da falha não existe na captura.");
        return NULL;
    }
    const char *bus = bc_str(spec, "bus");
    json_object *row = json_object_array_get_idx(bc_get(spec, "exchanges"), (size_t)index);
    uint8_t bytes[BC_COMM_MAX];
    size_t received = 0;
    if (row)
        bc_comm_hex(bc_str(row, "received"), bytes, sizeof bytes, &received, e);
    if ((bc_get(fault, "type") && !json_object_is_type(bc_get(fault, "type"), json_type_string)) ||
        (!strcmp(kind, "fragment") && strcmp(bus, "uart")) ||
        (!strcmp(kind, "nack") && strcmp(bus, "i2c")) ||
        ((!strcmp(kind, "truncate") || !strcmp(kind, "corrupt")) && !received)) {
        json_object_put(empty);
        bc_fail(e, "Falha incompatível com o barramento/resposta da captura.");
        return NULL;
    }
    int n;
    if (bc_lab_int(fault, "delay_ms", 1000, 0, 60000, &n, e) < 0 ||
        bc_lab_int(fault, "offset", 0, 0, 4095, &n, e) < 0 ||
        bc_lab_int(fault, "xor", 1, 1, 255, &n, e) < 0) {
        json_object_put(empty);
        return NULL;
    }
    replay state = {.spec = spec, .fault = fault, .fragment = 0};
    bc_comm_hex(bc_str(spec, "preamble"), state.queued, sizeof state.queued, &state.used, e);
    bc_lab_io io = {&state, replay_tx, replay_poll, replay_clock, replay_wait, NULL, replay_unread};
    bc_error execution = {{0}};
    json_object *r = bc_lab_execute_io(bc_str(spec, "bus"), operation, &io, &execution);
    if (!r)
        r = json_object_new_object();
    int ok = !execution.text[0] && json_object_get_boolean(bc_get(r, "matched"));
    if (!execution.text[0] && state.cursor != json_object_array_length(bc_get(spec, "exchanges"))) {
        ok = 0;
        snprintf(execution.text, sizeof execution.text,
                 "Restam transações não consumidas na captura.");
    }
    text(r, "status", ok ? "pass" : "fail");
    if (execution.text[0])
        text(r, "message", execution.text);
    json_object_object_add(r, "simulation", json_object_new_boolean(1));
    json_object_object_add(r, "hardware_accessed", json_object_new_boolean(0));
    json_object_object_add(r, "exchanges_consumed", json_object_new_int64((int64_t)state.cursor));
    json_object_object_add(
        r, "fault_applied",
        json_object_new_boolean(*kind && strcmp(kind, "none") && (size_t)index < state.cursor));
    json_object_object_add(r, "virtual_seconds", json_object_new_double(state.now));
    json_object_put(empty);
    return r;
}
