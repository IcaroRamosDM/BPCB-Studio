#define _POSIX_C_SOURCE 200809L
#include "../workbench.h"
#include <glib.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int string(json_object *o, const char *k, int required, size_t maximum, bc_error *e) {
    json_object *v = bc_get(o, k);
    if (!v)
        return required ? bc_fail(e, "Campo obrigatório: %s.", k) : 0;
    if (!json_object_is_type(v, json_type_string) || (required && !*bc_str(o, k)) ||
        strlen(bc_str(o, k)) > maximum ||
        !bc_utf8((const unsigned char *)bc_str(o, k), strlen(bc_str(o, k))))
        return bc_fail(e, "Texto inválido: %s.", k);
    return 0;
}

static unsigned width(const char *type) {
    return !strcmp(type, "u8") || !strcmp(type, "s8")                             ? 1
           : !strcmp(type, "u16") || !strcmp(type, "s16")                         ? 2
           : !strcmp(type, "u32") || !strcmp(type, "s32") || !strcmp(type, "f32") ? 4
                                                                                  : 0;
}

static int checksum_width(const char *type) {
    return !strcmp(type, "none")                            ? 0
           : !strcmp(type, "sum8") || !strcmp(type, "xor8") ? 1
           : !strcmp(type, "crc16_modbus")                  ? 2
                                                            : -1;
}

int bc_frame_validate(json_object *spec, bc_error *e) {
    if (bc_lab_object(spec, "|schema||name||length||prefix||fields||checksum|", e) < 0 ||
        !json_object_is_type(bc_get(spec, "schema"), json_type_int) ||
        bc_num(spec, "schema") != 1 || string(spec, "name", 1, 120, e) < 0 ||
        string(spec, "prefix", 0, 64, e) < 0)
        return bc_fail(e, "Formato de quadro inválido (schema 1).");
    int length;
    if (bc_lab_int(spec, "length", 0, 1, 4096, &length, e) < 0)
        return -1;
    uint8_t prefix[16];
    size_t np = 0;
    if (bc_comm_hex(bc_str(spec, "prefix"), prefix, sizeof prefix, &np, e) < 0 ||
        np > (size_t)length)
        return bc_fail(e, "Prefixo maior que o quadro.");
    json_object *fields = bc_get(spec, "fields");
    if (!json_object_is_type(fields, json_type_array) || !json_object_array_length(fields) ||
        json_object_array_length(fields) > 32)
        return bc_fail(e, "Use 1 a 32 campos por quadro.");
    uint8_t used[4096] = {0};
    memset(used, 1, np);
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        json_object *f = json_object_array_get_idx(fields, i);
        int offset;
        double value;
        if (bc_lab_object(f, "|id||label||offset||type||endian||scale||bias||unit||min||max|", e) <
                0 ||
            !bc_lab_id(bc_str(f, "id")) || string(f, "id", 1, 40, e) < 0 ||
            string(f, "label", 0, 120, e) < 0 || string(f, "type", 1, 8, e) < 0 ||
            string(f, "endian", 0, 8, e) < 0 || string(f, "unit", 0, 24, e) < 0 ||
            bc_lab_int(f, "offset", 0, 0, length - 1, &offset, e) < 0 ||
            bc_lab_number(f, "scale", 1, -1e12, 1e12, &value, e) < 0 ||
            bc_lab_number(f, "bias", 0, -1e12, 1e12, &value, e) < 0)
            return bc_fail(e, "Campo %u inválido.", (unsigned)(i + 1));
        unsigned n = width(bc_str(f, "type"));
        const char *endian = bc_str(f, "endian");
        if (!n || offset + (int)n > length ||
            (*endian && strcmp(endian, "little") && strcmp(endian, "big")))
            return bc_fail(e, "Tipo/posição/ordem de bytes inválidos.");
        for (unsigned j = 0; j < n; j++)
            if (used[(unsigned)offset + j]++)
                return bc_fail(e, "Campos/prefixo sobrepostos.");
        for (size_t j = 0; j < i; j++)
            if (!strcmp(bc_str(json_object_array_get_idx(fields, j), "id"), bc_str(f, "id")))
                return bc_fail(e, "Identificador de campo repetido.");
        double minimum, maximum;
        if (bc_lab_number(f, "min", -1e100, -1e100, 1e100, &minimum, e) < 0 ||
            bc_lab_number(f, "max", 1e100, -1e100, 1e100, &maximum, e) < 0 || minimum > maximum)
            return bc_fail(e, "Limites do campo inválidos.");
    }
    json_object *c = bc_get(spec, "checksum");
    if (c) {
        int offset, start, count;
        if (bc_lab_object(c, "|type||offset||start||length||endian|", e) < 0 ||
            string(c, "type", 1, 24, e) < 0 || string(c, "endian", 0, 8, e) < 0)
            return -1;
        int n = checksum_width(bc_str(c, "type"));
        if (n < 0)
            return bc_fail(e, "Checksum não suportado.");
        if (n) {
            if (bc_lab_int(c, "offset", length - n, 0, length - n, &offset, e) < 0 ||
                bc_lab_int(c, "start", 0, 0, length - 1, &start, e) < 0 ||
                bc_lab_int(c, "length", offset - start, 1, length, &count, e) < 0 ||
                start + count > length)
                return bc_fail(e, "Faixa do checksum inválida.");
            if (start < offset + n && offset < start + count)
                return bc_fail(e, "Checksum inclui seus próprios bytes.");
            for (int j = 0; j < n; j++)
                if (used[offset + j]++)
                    return bc_fail(e, "Checksum sobrepõe campos/prefixo.");
            const char *endian = bc_str(c, "endian");
            if (*endian && strcmp(endian, "little") && strcmp(endian, "big"))
                return bc_fail(e, "Ordem do checksum inválida.");
        } else if (bc_get(c, "offset") || bc_get(c, "start") || bc_get(c, "length") ||
                   bc_get(c, "endian"))
            return bc_fail(e, "Checksum 'none' não recebe parâmetros.");
    }
    return 0;
}

static json_object *field(const char *id, int offset, const char *type, double scale,
                          const char *unit, double minimum, double maximum) {
    json_object *f = json_object_new_object();
#define STR(k, v) json_object_object_add(f, k, json_object_new_string(v))
    STR("id", id);
    STR("label", id);
    STR("type", type);
    STR("unit", unit);
    STR("endian", "little");
#undef STR
    json_object_object_add(f, "offset", json_object_new_int(offset));
    json_object_object_add(f, "scale", json_object_new_double(scale));
    json_object_object_add(f, "bias", json_object_new_double(0));
    json_object_object_add(f, "min", json_object_new_double(minimum));
    json_object_object_add(f, "max", json_object_new_double(maximum));
    return f;
}

json_object *bc_frame_example(void) {
    json_object *r = json_tokener_parse(
        "{\"schema\":1,\"name\":\"Quadro ilustrativo de 8 bytes\",\"length\":8,\"prefix\":\"AA "
        "55\",\"checksum\":{\"type\":\"sum8\",\"offset\":7,\"start\":0,\"length\":7}}");
    json_object *a = json_object_new_array();
    json_object_object_add(r, "fields", a);
    json_object_array_add(a, field("temperature", 2, "s16", .01, "°C", -40, 85));
    json_object_array_add(a, field("voltage", 4, "u16", .001, "V", 0, 5.5));
    json_object_array_add(a, field("status", 6, "u8", 1, "código", 0, 255));
    return r;
}

json_object *bc_frame_decode(json_object *spec, const uint8_t *data, size_t size,
                             const char *metric, bc_error *e) {
    if (bc_frame_validate(spec, e) < 0 || (!data && size))
        return NULL;
    json_object *fields = bc_get(spec, "fields"), *chosen = NULL;
    if (!metric || !*metric)
        metric = bc_str(json_object_array_get_idx(fields, 0), "id");
    for (size_t i = 0; i < json_object_array_length(fields); i++)
        if (!strcmp(metric, bc_str(json_object_array_get_idx(fields, i), "id")))
            chosen = json_object_array_get_idx(fields, i);
    if (!chosen)
        return bc_fail(e, "Campo selecionado não existe no formato."), NULL;
    json_object *r = json_object_new_object(), *readings = json_object_new_object();
    json_object_object_add(r, "readings", readings);
    int matched = 1;
    const char *message = "Quadro e campos válidos.";
    if (size != (size_t)bc_num(spec, "length")) {
        matched = 0;
        message = "Comprimento do quadro divergente.";
        goto finish;
    }
    uint8_t prefix[16];
    size_t np = 0;
    bc_comm_hex(bc_str(spec, "prefix"), prefix, sizeof prefix, &np, e);
    if (np && memcmp(data, prefix, np)) {
        matched = 0;
        message = "Prefixo do quadro divergente.";
        goto finish;
    }
    json_object *c = bc_get(spec, "checksum");
    int n = c ? checksum_width(bc_str(c, "type")) : 0;
    if (n > 0) {
        int offset, start, count;
        bc_lab_int(c, "offset", (int)size - n, 0, (int)size - n, &offset, e);
        bc_lab_int(c, "start", 0, 0, (int)size - 1, &start, e);
        bc_lab_int(c, "length", offset - start, 1, (int)size, &count, e);
        unsigned expected = 0;
        if (n == 2)
            expected = bc_modbus_crc(data + start, (size_t)count);
        else
            for (int i = start; i < start + count; i++)
                expected = !strcmp(bc_str(c, "type"), "xor8") ? expected ^ data[i]
                                                              : (expected + data[i]) & 255u;
        unsigned observed = data[offset];
        if (n == 2)
            observed = !strcmp(bc_str(c, "endian"), "big")
                           ? (observed << 8) | data[offset + 1]
                           : observed | ((unsigned)data[offset + 1] << 8);
        if (observed != expected) {
            matched = 0;
            message = "Checksum do quadro inválido.";
            goto finish;
        }
    }
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        json_object *f = json_object_array_get_idx(fields, i), *reading = json_object_new_object();
        unsigned nbytes = width(bc_str(f, "type")), offset = (unsigned)bc_num(f, "offset");
        uint32_t bits = 0;
        for (unsigned j = 0; j < nbytes; j++)
            bits = (bits << 8) |
                   data[offset + (!strcmp(bc_str(f, "endian"), "big") ? j : nbytes - 1 - j)];
        double raw = (double)bits;
        if (bc_str(f, "type")[0] == 's' && (bits & (1u << (nbytes * 8 - 1))))
            raw -= ldexp(1, (int)nbytes * 8);
        if (!strcmp(bc_str(f, "type"), "f32")) {
            float v;
            memcpy(&v, &bits, sizeof v);
            raw = (double)v;
        }
        double scale = bc_get(f, "scale") ? bc_num(f, "scale") : 1, bias = bc_num(f, "bias"),
               value = raw * scale + bias;
        int valid = isfinite(raw) && isfinite(value) &&
                    (!bc_get(f, "min") || value >= bc_num(f, "min")) &&
                    (!bc_get(f, "max") || value <= bc_num(f, "max"));
        if (!valid) {
            matched = 0;
            message = "Campo do quadro fora dos limites ou não finito.";
        }
        if (isfinite(raw))
            json_object_object_add(reading, "raw", json_object_new_double(raw));
        if (isfinite(value))
            json_object_object_add(reading, "value", json_object_new_double(value));
        json_object_object_add(
            reading, "unit",
            json_object_new_string(*bc_str(f, "unit") ? bc_str(f, "unit") : "código"));
        json_object_object_add(reading, "matched", json_object_new_boolean(valid));
        json_object_object_add(
            reading, "label",
            json_object_new_string(*bc_str(f, "label") ? bc_str(f, "label") : bc_str(f, "id")));
        json_object_object_add(readings, bc_str(f, "id"), reading);
    }
finish:
    json_object_object_add(r, "matched", json_object_new_boolean(matched));
    json_object_object_add(r, "message", json_object_new_string(message));
    json_object_object_add(r, "metric", json_object_new_string(metric));
    json_object_object_add(
        r, "unit",
        json_object_new_string(*bc_str(chosen, "unit") ? bc_str(chosen, "unit") : "código"));
    json_object_object_add(r, "value",
                           json_object_new_double(bc_num(bc_get(readings, metric), "value")));
    return r;
}

static json_object *underlying(json_object *op) {
    json_object *r = bc_clone(op);
    json_object_object_del(r, "frame");
    json_object_object_del(r, "metric");
    json_object_object_add(r, "protocol", json_object_new_string("length"));
    json_object_object_add(r, "length", bc_clone(bc_get(bc_get(op, "frame"), "length")));
    return r;
}

int bc_frame_operation_validate(json_object *op, bc_error *e) {
    if (bc_lab_object(
            op, "|kind||protocol||write||delimiter||max_bytes||timeout_ms||frame||metric||label|",
            e) < 0 ||
        strcmp(bc_str(op, "protocol"), "frame") || bc_frame_validate(bc_get(op, "frame"), e) < 0 ||
        string(op, "metric", 0, 40, e) < 0)
        return -1;
    const char *metric = bc_str(op, "metric");
    json_object *fields = bc_get(bc_get(op, "frame"), "fields");
    int found = !*metric;
    for (size_t i = 0; i < json_object_array_length(fields); i++)
        if (!strcmp(metric, bc_str(json_object_array_get_idx(fields, i), "id")))
            found = 1;
    if (!found)
        return bc_fail(e, "Grandeza ausente do quadro.");
    json_object *r = underlying(op);
    int rc = bc_uart_protocol_validate(r, e);
    json_object_put(r);
    return rc;
}

json_object *bc_frame_execute(json_object *op, bc_lab_io *io, bc_error *e) {
    if (bc_frame_operation_validate(op, e) < 0)
        return NULL;
    json_object *raw = underlying(op), *r = bc_uart_protocol_execute(raw, io, e);
    json_object_put(raw);
    if (!r)
        return NULL;
    uint8_t bytes[4096];
    size_t n = 0;
    if (bc_comm_hex(bc_str(r, "received"), bytes, sizeof bytes, &n, e) < 0) {
        json_object_put(r);
        return NULL;
    }
    json_object *decoded = bc_frame_decode(bc_get(op, "frame"), bytes, n, bc_str(op, "metric"), e);
    if (!decoded) {
        json_object_put(r);
        return NULL;
    }
    int valid = json_object_get_boolean(bc_get(r, "matched")) &&
                json_object_get_boolean(bc_get(decoded, "matched"));
    json_object_object_foreach(decoded, k, v) {
        if (!strcmp(k, "message") && !json_object_get_boolean(bc_get(r, "matched")))
            continue;
        json_object_object_add(r, k, bc_clone(v));
    }
    json_object_object_add(r, "matched", json_object_new_boolean(valid));
    json_object_object_add(r, "protocol", json_object_new_string("frame"));
    json_object_object_add(r, "operation", bc_clone(op));
    json_object_put(decoded);
    return r;
}
