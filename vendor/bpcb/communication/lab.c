#define _POSIX_C_SOURCE 200809L
#include "../lab.h"
#include "internal.h"
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int clean(json_object *v, unsigned depth, bc_error *e) {
    if (depth > 20)
        return bc_fail(e, "JSON excede 20 níveis.");
    if (json_object_is_type(v, json_type_string))
        return (size_t)json_object_get_string_len(v) == strlen(json_object_get_string(v)) &&
                       json_object_get_string_len(v) <= 32768
                   ? 0
                   : bc_fail(e, "Texto contém NUL ou excede o limite.");
    if (json_object_is_type(v, json_type_array)) {
        if (json_object_array_length(v) > 1024)
            return bc_fail(e, "Lista excede 1024 itens.");
        for (size_t i = 0; i < json_object_array_length(v); i++)
            if (clean(json_object_array_get_idx(v, i), depth + 1, e) < 0)
                return -1;
    }
    if (json_object_is_type(v, json_type_object)) {
        if (json_object_object_length(v) > 128)
            return bc_fail(e, "Objeto excede 128 campos.");
        json_object_object_foreach(v, k, item) {
            if (strlen(k) > 128 || clean(item, depth + 1, e) < 0)
                return bc_fail(e, "Campo JSON inválido.");
        }
    }
    if (json_object_is_type(v, json_type_double) && !isfinite(json_object_get_double(v)))
        return bc_fail(e, "Número não finito.");
    return 0;
}

int bc_lab_object(json_object *v, const char *keys, bc_error *e) {
    if (!json_object_is_type(v, json_type_object) || clean(v, 0, e) < 0)
        return bc_fail(e, "Objeto JSON inválido.");
    json_object_object_foreach(v, key, item) {
        (void)item;
        char token[160];
        for (const char *p = key; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_'))
                return bc_fail(e, "Nome de campo inválido.");
        if (strlen(key) > 128)
            return bc_fail(e, "Nome de campo longo.");
        snprintf(token, sizeof token, "|%s|", key);
        if (!strstr(keys, token))
            return bc_fail(e, "Campo desconhecido: %s.", key);
    }
    return 0;
}

int bc_lab_int(json_object *o, const char *key, int def, int min, int max, int *out, bc_error *e) {
    json_object *v = bc_get(o, key);
    int64_t n = v ? json_object_get_int64(v) : def;
    if ((v && !json_object_is_type(v, json_type_int)) || n < min || n > max)
        return bc_fail(e, "%s deve ser inteiro entre %d e %d.", key, min, max);
    *out = (int)n;
    return 0;
}

int bc_lab_number(json_object *o, const char *key, double def, double min, double max, double *out,
                  bc_error *e) {
    json_object *v = bc_get(o, key);
    double n = v ? json_object_get_double(v) : def;
    if ((v && !json_object_is_type(v, json_type_int) &&
         !json_object_is_type(v, json_type_double)) ||
        !isfinite(n) || n < min || n > max)
        return bc_fail(e, "Valor de %s fora da faixa.", key);
    *out = n;
    return 0;
}

int bc_lab_id(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 40)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return 0;
    return 1;
}

json_object *bc_lab_channel_config(json_object *c, const char *id) {
    return bc_num(c, "api") == 2 ? bc_get(bc_get(c, "channels"), id)
                                 : (!strcmp(id, "main") ? c : NULL);
}

json_object *bc_lab_channel_definition(json_object *d, const char *id) {
    return bc_num(d, "api") == 2 ? bc_get(bc_get(d, "channels"), id)
                                 : (!strcmp(id, "main") ? d : NULL);
}

int bc_lab_declaration(json_object *d, bc_error *e) {
    if (bc_lab_object(d, "|api||channels||operations|", e) < 0 ||
        !json_object_is_type(bc_get(d, "api"), json_type_int) || bc_num(d, "api") != 2)
        return bc_fail(e, "Contrato multicanal exige API 2.");
    json_object *channels = bc_get(d, "channels"), *ops = bc_get(d, "operations");
    if (!json_object_is_type(channels, json_type_object) ||
        json_object_object_length(channels) < 1 ||
        json_object_object_length(channels) > BC_LAB_CHANNELS)
        return bc_fail(e, "Use de 1 a 8 canais nomeados.");
    json_object_object_foreach(channels, id, decl) {
        if (!bc_lab_id(id) || bc_num(decl, "api") != 1 || bc_comm_declaration(decl, e) < 0)
            return bc_fail(e, "Canal %s inválido.", id);
    }
    if (ops && (!json_object_is_type(ops, json_type_array) || json_object_array_length(ops) > 64))
        return bc_fail(e, "Use até 64 operações.");
    for (size_t i = 0; ops && i < json_object_array_length(ops); i++) {
        json_object *op = json_object_array_get_idx(ops, i);
        if (bc_lab_object(op, "|channel||operation|", e) < 0)
            return -1;
        json_object *decl = bc_get(channels, bc_str(op, "channel"));
        if (!decl ||
            bc_comm_operation_validate(bc_str(decl, "bus"), bc_get(op, "operation"), e) < 0)
            return bc_fail(e, "Operação aponta para canal ausente ou incompatível.");
    }
    return 0;
}

json_object *bc_lab_resolve(json_object *d, json_object *selected, bc_error *e) {
    if (bc_lab_declaration(d, e) < 0 || bc_lab_object(selected, "|api||channels|", e) < 0 ||
        !json_object_is_type(bc_get(selected, "api"), json_type_int) ||
        bc_num(selected, "api") != 2)
        return NULL;
    json_object *input = bc_get(selected, "channels"), *defs = bc_get(d, "channels");
    if (!json_object_is_type(input, json_type_object) ||
        json_object_object_length(input) != json_object_object_length(defs)) {
        bc_fail(e, "Selecione exatamente os canais declarados pelo teste.");
        return NULL;
    }
    json_object *result = json_object_new_object(), *channels = json_object_new_object();
    json_object_object_add(result, "api", json_object_new_int(2));
    json_object_object_add(result, "channels", channels);
    json_object_object_foreach(defs, id, decl) {
        json_object *test = json_object_new_object();
        json_object_object_add(test, "communication", bc_clone(decl));
        json_object *c =
            bc_get(input, id) ? bc_comm_resolve(NULL, test, bc_get(input, id), e) : NULL;
        json_object_put(test);
        if (!c) {
            if (!e->text[0])
                bc_fail(e, "Seleção ausente para %s.", id);
            goto bad;
        }
        if (!*bc_str(c, "device") && !*bc_str(c, "serial")) {
            json_object_put(c);
            bc_fail(e, "Multicanal exige caminho ou série exata para cada adaptador (%s).", id);
            goto bad;
        }
        json_object_object_add(channels, id, c);
    }
    json_object *ops = bc_get(d, "operations");
    for (size_t i = 0; ops && i < json_object_array_length(ops); i++)
        if (bc_lab_operation_check(result, d, json_object_array_get_idx(ops, i), e) < 0)
            goto bad;
    return result;
bad:
    json_object_put(result);
    return NULL;
}

int bc_lab_operation_validate(const char *bus, json_object *op, bc_error *e) {
    if (!strcmp(bc_str(op, "kind"), "peripheral"))
        return bc_peripheral_validate(bus, op, e);
    if (!strcmp(bc_str(op, "kind"), "uart_protocol") && !strcmp(bus, "uart"))
        return bc_uart_protocol_validate(op, e);
    return bc_fail(e, "Tipo de operação incompatível com %s.", bus);
}

int bc_lab_operation_check(json_object *c, json_object *d, json_object *op, bc_error *e) {
    if (bc_num(d, "api") == 2) {
        if (bc_lab_object(op, "|channel||operation|", e) < 0)
            return -1;
        const char *id = bc_str(op, "channel");
        json_object *config = bc_lab_channel_config(c, id),
                    *decl = bc_lab_channel_definition(d, id);
        if (!config || !decl)
            return bc_fail(e, "Canal não selecionado.");
        return bc_comm_operation_check(config, decl, bc_get(op, "operation"), e);
    }
    if (bc_lab_operation_validate(bc_str(c, "bus"), op, e) < 0)
        return -1;
    if (bc_get(op, "timeout_ms") &&
        ((bc_get(d, "max_timeout_ms") && bc_num(op, "timeout_ms") > bc_num(d, "max_timeout_ms")) ||
         (bc_get(c, "timeout_ms") && bc_num(op, "timeout_ms") > bc_num(c, "timeout_ms"))))
        return bc_fail(e, "Tempo do protocolo excede o limite do canal selecionado/declarado.");
    unsigned required = !strcmp(bc_str(op, "kind"), "peripheral")
                            ? 32u
                            : (unsigned)(bc_get(op, "max_bytes") ? bc_num(op, "max_bytes") : 1024);
    if (bc_get(d, "max_transfer") && required > bc_num(d, "max_transfer"))
        return bc_fail(e, "Ação exige limite de transferência de pelo menos %u bytes.", required);
    return 0;
}

struct bc_lab {
    json_object *config, *definition;
    bc_comm *sessions[BC_LAB_CHANNELS];
    char ids[BC_LAB_CHANNELS][41];
    unsigned count;
    int (*cancel)(void *);
    void *context;
    int needs_ir[BC_LAB_CHANNELS];
    int failed;
};

bc_lab *bc_lab_new(json_object *config, json_object *definition, int (*cancel)(void *),
                   void *context, bc_error *e) {
    json_object *test = json_object_new_object();
    if (definition)
        json_object_object_add(test, "communication", bc_clone(definition));
    json_object *c = bc_comm_resolve(NULL, test, config, e);
    json_object_put(test);
    if (!c)
        return NULL;
    bc_lab *lab = calloc(1, sizeof *lab);
    if (!lab) {
        json_object_put(c);
        bc_fail(e, "Memória insuficiente.");
        return NULL;
    }
    lab->config = c;
    lab->definition = bc_clone(definition);
    lab->cancel = cancel;
    lab->context = context;
    if (bc_num(c, "api") == 2) {
        json_object_object_foreach(bc_get(c, "channels"), id, item) {
            (void)item;
            snprintf(lab->ids[lab->count++], 41, "%s", id);
        }
    } else {
        strcpy(lab->ids[0], "main");
        lab->count = 1;
    }
    return lab;
}

void bc_lab_free(bc_lab *lab) {
    if (lab) {
        for (unsigned i = 0; i < lab->count; i++)
            bc_comm_close(lab->sessions[i]);
        json_object_put(lab->config);
        json_object_put(lab->definition);
        free(lab);
    }
}

static json_object *tx(void *ctx, json_object *op, bc_error *e) {
    return bc_comm_transaction(ctx, op, e);
}

static int poll_uart(void *ctx, uint8_t *data, size_t n, unsigned ms, bc_error *e) {
    return bc_comm_uart_poll(ctx, data, n, ms, e);
}

static int unread_uart(void *ctx, const uint8_t *data, size_t size, bc_error *e) {
    return bc_comm_uart_unread(ctx, data, size, e);
}

static double clock_io(void *ctx) {
    (void)ctx;
    return bc_comm_now();
}

static int cancelled(void *ctx) {
    bc_comm *s = ctx;
    return s->cancel && s->cancel(s->cancel_context);
}

static int delay(void *ctx, unsigned ms, bc_error *e) {
    double until = bc_comm_now() + ms / 1000.0;
    while (bc_comm_now() < until) {
        if (cancelled(ctx))
            return bc_fail(e, "Operação cancelada.");
        struct timespec t = {0, 5000000};
        nanosleep(&t, NULL);
    }
    return 0;
}

json_object *bc_lab_execute_io(const char *bus, json_object *op, bc_lab_io *io, bc_error *e) {
    if (!io || !io->transaction)
        return NULL;
    if (bc_get(op, "kind")) {
        if (bc_lab_operation_validate(bus, op, e) < 0)
            return NULL;
        return !strcmp(bc_str(op, "kind"), "peripheral") ? bc_peripheral_execute(op, io, e)
                                                         : bc_uart_protocol_execute(op, io, e);
    }
    if (bc_comm_operation_validate(bus, op, e) < 0)
        return NULL;
    return io->transaction(io->context, op, e);
}

json_object *bc_lab_execute(bc_lab *lab, json_object *input, bc_error *e) {
    if (!lab || bc_comm_operation_check(lab->config, lab->definition, input, e) < 0)
        return NULL;
    const char *id = bc_num(lab->config, "api") == 2 ? bc_str(input, "channel") : "main";
    json_object *op = bc_num(lab->config, "api") == 2 ? bc_get(input, "operation") : input;
    unsigned index = 0;
    while (index < lab->count && strcmp(id, lab->ids[index]))
        index++;
    if (index == lab->count) {
        bc_fail(e, "Canal ausente.");
        return NULL;
    }
    json_object *config = bc_lab_channel_config(lab->config, id);
    if (!lab->sessions[index]) {
        if (lab->failed) {
            bc_fail(e, "Falha anterior: nenhuma interface é reaberta automaticamente.");
            return NULL;
        }
        /* One USB interface cannot be configured for two buses simultaneously. Close only
           an explicitly identical interface; a later JTAG DR must supply its IR prefix. */
        for (unsigned i = 0; i < lab->count; i++)
            if (lab->sessions[i]) {
                json_object *old = lab->sessions[i]->config;
                int same_path = *bc_str(config, "device") &&
                                !strcmp(bc_str(config, "device"), bc_str(old, "device"));
                int same_serial = !*bc_str(config, "device") && !*bc_str(old, "device") &&
                                  *bc_str(config, "serial") &&
                                  !strcmp(bc_str(config, "serial"), bc_str(old, "serial")) &&
                                  !strcmp(bc_str(config, "adapter"), bc_str(old, "adapter")) &&
                                  strcmp(bc_str(config, "bus"), "uart") &&
                                  strcmp(bc_str(old, "bus"), "uart");
                if (same_path || same_serial) {
                    bc_comm_close(lab->sessions[i]);
                    lab->sessions[i] = NULL;
                    lab->needs_ir[i] = 1;
                }
            }
        if (lab->needs_ir[index] && !strcmp(bc_str(config, "bus"), "jtag") &&
            !strcmp(bc_str(op, "register"), "dr") && !bc_get(op, "ir_bits")) {
            bc_fail(e, "O canal JTAG foi reaberto: defina IR explicitamente antes de DR ou use o "
                       "prefixo IR.");
            return NULL;
        }
        if (bc_comm_open(&lab->sessions[index], config, lab->cancel, lab->context, e) < 0)
            return NULL;
    }
    lab->needs_ir[index] = 0;
    bc_comm *s = lab->sessions[index];
    bc_comm_deadline(s);
    if (bc_comm_check(s, e) < 0) {
        lab->failed = 1;
        return NULL;
    }
    bc_lab_io io = {s, tx, poll_uart, clock_io, delay, cancelled, unread_uart};
    json_object *r = bc_lab_execute_io(bc_str(config, "bus"), op, &io, e);
    if (!r || (bc_get(r, "matched") && !json_object_get_boolean(bc_get(r, "matched"))))
        lab->failed = 1;
    if (r) {
        json_object_object_add(r, "transport", bc_comm_information(s));
        json_object_object_add(bc_get(r, "transport"), "channel", json_object_new_string(id));
        json_object_object_add(r, "channel", json_object_new_string(id));
    }
    return r;
}
