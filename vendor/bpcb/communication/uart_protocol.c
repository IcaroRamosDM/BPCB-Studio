#define _POSIX_C_SOURCE 200809L
#include "../workbench.h"
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t bc_modbus_crc(const uint8_t *p, size_t n) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned j = 0; j < 8; j++)
            crc = (uint16_t)((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
    }
    return crc;
}

static int ending(const char *s, const uint8_t **bytes, size_t *n) {
    static const uint8_t lf[] = {10}, cr[] = {13}, crlf[] = {13, 10};
    *n = 0;
    *bytes = NULL;
    if (!strcmp(s, "none"))
        return 0;
    if (!strcmp(s, "lf")) {
        *bytes = lf;
        *n = 1;
    } else if (!strcmp(s, "cr")) {
        *bytes = cr;
        *n = 1;
    } else if (!strcmp(s, "crlf")) {
        *bytes = crlf;
        *n = 2;
    } else
        return -1;
    return 0;
}

int bc_uart_protocol_validate(json_object *op, bc_error *e) {
    if (!strcmp(bc_str(op, "protocol"), "frame"))
        return bc_frame_operation_validate(op, e);
    if (bc_lab_object(
            op,
            "|kind||protocol||write||delimiter||success||success_mode||max_bytes||timeout_ms||"
            "length||slave||function||start||quantity||values||silence_ms||label|",
            e) < 0 ||
        strcmp(bc_str(op, "kind"), "uart_protocol"))
        return -1;
    const char *strings[] = {"kind",    "protocol",     "write",  "delimiter",
                             "success", "success_mode", "values", "label"};
    for (unsigned i = 0; i < sizeof strings / sizeof *strings; i++)
        if (bc_get(op, strings[i]) &&
            !json_object_is_type(bc_get(op, strings[i]), json_type_string))
            return bc_fail(e, "Campo textual obrigatório: %s.", strings[i]);
    const char *proto = bc_str(op, "protocol");
    if (strcmp(proto, "at") && strcmp(proto, "delimited") && strcmp(proto, "length") &&
        strcmp(proto, "modbus_rtu"))
        return bc_fail(e, "Protocolo UART desconhecido.");
    int max, timeout, n;
    if (bc_lab_int(op, "max_bytes", 1024, 1, 4096, &max, e) < 0 ||
        bc_lab_int(op, "timeout_ms", 1000, 50, 10000, &timeout, e) < 0)
        return -1;
    if (!strcmp(proto, "modbus_rtu")) {
        int function, quantity, start;
        if (bc_lab_int(op, "slave", 1, 1, 247, &n, e) < 0 ||
            bc_lab_int(op, "function", 3, 1, 16, &function, e) < 0 ||
            bc_lab_int(op, "start", 0, 0, 65535, &start, e) < 0 ||
            bc_lab_int(op, "quantity", 1, 1, 2000, &quantity, e) < 0 ||
            bc_lab_int(op, "silence_ms", 150, 20, 1000, &n, e) < 0)
            return -1;
        if (function != 1 && function != 2 && function != 3 && function != 4 && function != 5 &&
            function != 6 && function != 15 && function != 16)
            return bc_fail(e, "Função Modbus não suportada.");
        if (start + quantity > 65536 || ((function == 3 || function == 4) && quantity > 125) ||
            (function == 15 && quantity > 1968) || (function == 16 && quantity > 123) ||
            ((function == 5 || function == 6) && quantity != 1))
            return bc_fail(e, "Faixa Modbus inválida.");
        uint8_t data[246];
        size_t size;
        if (bc_comm_hex(bc_str(op, "values"), data, sizeof data, &size, e) < 0)
            return -1;
        size_t required = function == 5 || function == 6 ? 2
                          : function == 15               ? (size_t)(quantity + 7) / 8
                          : function == 16               ? (size_t)quantity * 2
                                                         : 0;
        if (size != required ||
            (function == 5 && ((data[0] != 0 && data[0] != 255) || data[1] != 0)) ||
            (function == 15 && (quantity % 8) && (data[size - 1] >> (quantity % 8))))
            return bc_fail(e, "Dados de escrita Modbus inválidos.");
        unsigned response = function == 1 || function == 2   ? 5u + (unsigned)(quantity + 7) / 8
                            : function == 3 || function == 4 ? 5u + (unsigned)quantity * 2
                                                             : 8u;
        if (response > (unsigned)max)
            return bc_fail(e, "Resposta Modbus excede max_bytes.");
        if (bc_get(op, "write") || bc_get(op, "delimiter"))
            return bc_fail(e, "Modbus monta seu próprio quadro/CRC.");
    } else {
        uint8_t data[BC_COMM_MAX];
        size_t size = 0;
        if (bc_comm_hex(bc_str(op, "write"), data, sizeof data, &size, e) < 0 ||
            (!size && strcmp(proto, "length")) || size > (size_t)max)
            return bc_fail(e, "Comando UART vazio ou longo.");
        const uint8_t *end;
        size_t ne;
        if (ending(bc_get(op, "delimiter") ? bc_str(op, "delimiter") : "crlf", &end, &ne) < 0 ||
            size + ne > (size_t)max)
            return bc_fail(e, "Delimitador UART inválido.");
        if (!size && ne)
            return bc_fail(e, "Leitura passiva exige terminador Nenhum.");
        if (!strcmp(proto, "at")) {
            for (size_t i = 0; i < size; i++)
                if (data[i] < 32 || data[i] > 126)
                    return bc_fail(e, "AT exige um único comando ASCII sem CR/LF embutido.");
        }
        if (!strcmp(proto, "length") && bc_lab_int(op, "length", 1, 1, max, &n, e) < 0)
            return -1;
        if (!strcmp(proto, "delimited") && !ne)
            return bc_fail(e, "Resposta delimitada exige terminador.");
        if (bc_get(op, "success") &&
            (strlen(bc_str(op, "success")) > 80 || !*bc_str(op, "success")))
            return bc_fail(e, "Terminador de sucesso inválido.");
        if (bc_get(op, "success_mode") && strcmp(bc_str(op, "success_mode"), "exact") &&
            strcmp(bc_str(op, "success_mode"), "prefix"))
            return bc_fail(e, "Comparação deve ser exact ou prefix.");
    }
    return 0;
}

static int unsolicited(const char *line) {
    const char *prefix[] = {"WIFI ", "+IPD,", "+ETH_", "+STA_",      "+DIST_STA_",
                            "ready", "+BLE",  "+MQTT", "+LINK_CONN:"};
    for (size_t i = 0; i < sizeof prefix / sizeof *prefix; i++)
        if (!strncmp(line, prefix[i], strlen(prefix[i])))
            return 1;
    return 0;
}

static void text(json_object *o, const char *k, const char *s) {
    json_object_object_add(o, k, json_object_new_string(s));
}

static int packet(bc_lab_io *io, const uint8_t *data, size_t size, bc_error *e) {
    json_object *o = json_object_new_object();
    char *hex = bc_comm_format_bytes(data, size, 1);
    text(o, "write", hex);
    free(hex);
    json_object_object_add(o, "read", json_object_new_int(0));
    json_object *r = io->transaction(io->context, o, e);
    json_object_put(o);
    if (!r)
        return -1;
    json_object_put(r);
    return 0;
}

static int line_end(const uint8_t *b, size_t n, const uint8_t *end, size_t ne) {
    return ne && n >= ne && !memcmp(b + n - ne, end, ne);
}

json_object *bc_uart_protocol_execute(json_object *op, bc_lab_io *io, bc_error *e) {
    if (!strcmp(bc_str(op, "protocol"), "frame"))
        return bc_frame_execute(op, io, e);
    if (!io || !io->clock || !isfinite(io->clock(io->context))) {
        bc_fail(e, "Relógio de I/O inválido.");
        return NULL;
    }
    if (bc_uart_protocol_validate(op, e) < 0 || !io || !io->transaction || !io->poll ||
        !io->clock || !io->wait_ms)
        return NULL;
    if (io->cancelled && io->cancelled(io->context)) {
        bc_fail(e, "Protocolo UART cancelado antes de transmitir.");
        return NULL;
    }
    uint8_t tx[BC_COMM_MAX], rx[BC_COMM_MAX];
    size_t nt = 0, nr = 0, needed = 0;
    int maximum = 1024, timeout = 1000;
    bc_lab_int(op, "max_bytes", 1024, 1, 4096, &maximum, e);
    bc_lab_int(op, "timeout_ms", 1000, 50, 10000, &timeout, e);
    const char *proto = bc_str(op, "protocol");
    int modbus = !strcmp(proto, "modbus_rtu"), at = !strcmp(proto, "at");
    const uint8_t *end = NULL;
    size_t ne = 0;
    unsigned function = 0, quantity = 0;
    if (modbus) {
        int slave, start, q, f, silence;
        bc_lab_int(op, "slave", 1, 1, 247, &slave, e);
        bc_lab_int(op, "start", 0, 0, 65535, &start, e);
        bc_lab_int(op, "quantity", 1, 1, 2000, &q, e);
        bc_lab_int(op, "function", 3, 1, 16, &f, e);
        bc_lab_int(op, "silence_ms", 150, 20, 1000, &silence, e);
        quantity = (unsigned)q;
        function = (unsigned)f;
        /* This conservative guard is not a measurement of sub-millisecond bus timing.
           Use an auto-direction RS-485 adapter and qualify timings on physical hardware. */
        if (io->wait_ms(io->context, (unsigned)silence, e) < 0)
            return NULL;
        tx[0] = (uint8_t)slave;
        tx[1] = (uint8_t)f;
        tx[2] = (uint8_t)((unsigned)start >> 8);
        tx[3] = (uint8_t)start;
        nt = 6;
        if (f == 5 || f == 6) {
            size_t n;
            bc_comm_hex(bc_str(op, "values"), tx + 4, 2, &n, e);
        } else {
            tx[4] = (uint8_t)((unsigned)q >> 8);
            tx[5] = (uint8_t)q;
            if (f == 15 || f == 16) {
                size_t n;
                bc_comm_hex(bc_str(op, "values"), tx + 7, sizeof tx - 7, &n, e);
                tx[6] = (uint8_t)n;
                nt = 7 + n;
            }
        }
        uint16_t crc = bc_modbus_crc(tx, nt);
        tx[nt++] = (uint8_t)crc;
        tx[nt++] = (uint8_t)(crc >> 8);
        needed = 3;
    } else {
        bc_comm_hex(bc_str(op, "write"), tx, sizeof tx, &nt, e);
        ending(bc_get(op, "delimiter") ? bc_str(op, "delimiter") : "crlf", &end, &ne);
        if (ne) {
            memcpy(tx + nt, end, ne);
            nt += ne;
        }
        if (!strcmp(proto, "length")) {
            int length;
            bc_lab_int(op, "length", 1, 1, maximum, &length, e);
            needed = (size_t)length;
        }
    }
    json_object *r = json_object_new_object(), *events = json_object_new_array(),
                *lines = json_object_new_array();
    json_object_object_add(r, "unsolicited", events);
    json_object_object_add(r, "lines", lines);
    if (at) {
        uint8_t old_data[BC_COMM_MAX];
        size_t old_size = 0;
        int quiet = 0;
        for (unsigned attempt = 0; attempt < 5; attempt++) {
            if (old_size == (size_t)maximum)
                break;
            int got = io->poll(io->context, old_data + old_size, (size_t)maximum - old_size, 10, e);
            if (got < 0)
                goto bad;
            if (got == 0) {
                quiet = 1;
                break;
            }
            if (got > (int)((size_t)maximum - old_size)) {
                bc_fail(e, "Fila UART inválida.");
                goto bad;
            }
            old_size += (size_t)got;
        }
        if (old_size) {
            char *hex = bc_comm_format_bytes(old_data, old_size, 1);
            text(r, "preexisting_bytes", hex);
            free(hex);
        }
        if (!quiet) {
            bc_fail(e, "UART não ficou em silêncio antes do comando; nenhum comando foi enviado.");
            goto bad;
        }
    }
    if (io->cancelled && io->cancelled(io->context)) {
        bc_fail(e, "Protocolo UART cancelado antes de transmitir.");
        goto bad;
    }
    if (nt && packet(io, tx, nt, e) < 0)
        goto bad;
    size_t line_start = 0, ipd_remaining = 0;
    int matched = 0, done = 0, timed_out = 0;
    double until = io->clock(io->context) + (double)timeout / 1000.0;
    while (!done && nr < (size_t)maximum) {
        if (io->cancelled && io->cancelled(io->context)) {
            bc_fail(e, "Protocolo UART cancelado.");
            goto bad;
        }
        if (!isfinite(io->clock(io->context))) {
            bc_fail(e, "Relógio de I/O inválido.");
            goto bad;
        }
        if (io->clock(io->context) >= until) {
            timed_out = 1;
            break;
        }
        size_t cap = (size_t)maximum - nr;
        if (needed && needed > nr && cap > needed - nr)
            cap = needed - nr;
        int got = io->poll(io->context, rx + nr, cap, 10, e);
        if (got < 0)
            goto bad;
        if (got > (int)cap) {
            bc_fail(e, "Transporte devolveu dados além da capacidade.");
            goto bad;
        }
        if (!got) {
            double before = io->clock(io->context);
            if (io->wait_ms(io->context, 1, e) < 0)
                goto bad;
            if (!isfinite(io->clock(io->context)) || io->clock(io->context) <= before) {
                bc_fail(e, "Relógio de I/O não avançou durante a espera.");
                goto bad;
            }
            continue;
        }
        if (io->clock(io->context) > until) {
            nr += (size_t)got;
            timed_out = 1;
            break;
        }
        size_t old = nr;
        nr += (size_t)got;
        if (modbus) {
            if (nr >= 3 && needed == 3) {
                if (rx[0] != tx[0] ||
                    (rx[1] != (uint8_t)function && rx[1] != (uint8_t)(function | 0x80u))) {
                    text(r, "message", "Endereço/função Modbus divergente.");
                    done = 1;
                    continue;
                }
                needed = (rx[1] & 0x80) ? 5 : (function <= 4 ? 5u + rx[2] : 8u);
                if (needed > (size_t)maximum || needed > 256) {
                    text(r, "message", "Comprimento Modbus inválido.");
                    done = 1;
                    continue;
                }
            }
            if (nr == needed) {
                done = 1;
                uint16_t expected = bc_modbus_crc(rx, nr - 2),
                         observed = (uint16_t)((unsigned)rx[nr - 2] | ((unsigned)rx[nr - 1] << 8));
                matched = expected == observed && !(rx[1] & 0x80);
                if (matched && function <= 4) {
                    size_t bytes = function <= 2 ? (quantity + 7u) / 8u : quantity * 2u;
                    matched = rx[2] == bytes;
                }
                if (matched && function > 4)
                    matched = !memcmp(rx, tx, 6);
                if (rx[1] & 0x80) {
                    json_object_object_add(r, "exception", json_object_new_int(rx[2]));
                    text(r, "message", "Exceção Modbus recebida.");
                } else if (expected != observed)
                    text(r, "message", "CRC Modbus inválido.");
            }
        } else if (!strcmp(proto, "length")) {
            if (nr == needed) {
                matched = done = 1;
            }
        } else if (!at) {
            size_t from = old >= ne ? old - ne + 1 : 0;
            for (size_t at_end = from + ne; at_end <= nr; at_end++)
                if (line_end(rx, at_end, end, ne)) {
                    size_t trailing = nr - at_end;
                    if (trailing) {
                        char *hex = bc_comm_format_bytes(rx + at_end, trailing, 1);
                        text(r, "trailing_bytes", hex);
                        free(hex);
                        if (!io->unread || io->unread(io->context, rx + at_end, trailing, e) < 0)
                            goto bad;
                    }
                    nr = at_end;
                    matched = done = 1;
                    break;
                }
        } else {
            for (size_t i = old; i < nr; i++) {
                if (ipd_remaining) {
                    ipd_remaining--;
                    if (!ipd_remaining)
                        line_start = i + 1;
                    continue;
                }
                if (rx[i] == ':' && i - line_start >= 5 && !memcmp(rx + line_start, "+IPD,", 5)) {
                    char header[96];
                    size_t n = i - line_start;
                    if (n >= sizeof header) {
                        text(r, "message", "Cabeçalho +IPD longo.");
                        done = 1;
                        break;
                    }
                    memcpy(header, rx + line_start, n);
                    header[n] = 0;
                    char *last = strrchr(header, ',');
                    char *after = NULL;
                    unsigned long payload = last ? strtoul(last + 1, &after, 10) : 0;
                    unsigned commas = 0;
                    int numeric_header = 1;
                    for (char *p = header + 5; *p; p++) {
                        if (*p == ',')
                            commas++;
                        else if (*p < '0' || *p > '9')
                            numeric_header = 0;
                    }
                    if (!numeric_header || commas > 1 || !last || last[1] < '0' || last[1] > '9' ||
                        !after || *after || payload > (unsigned long)maximum ||
                        strchr(header, '"')) {
                        text(r, "message", "Formato +IPD não suportado/ambíguo.");
                        done = 1;
                        break;
                    }
                    json_object_array_add(events, json_object_new_string(header));
                    ipd_remaining = (size_t)payload;
                    line_start = i + 1;
                    continue;
                }
                if (rx[i] == '\n' || rx[i] == '\r') {
                    size_t n = i - line_start;
                    if (!n) {
                        line_start = i + 1;
                        continue;
                    }
                    char line[4097];
                    memcpy(line, rx + line_start, n);
                    line[n] = 0;
                    line_start = i + 1;
                    if (memchr(line, 0, n)) {
                        text(r, "message", "NUL em resposta AT textual.");
                        done = 1;
                        break;
                    }
                    if (json_object_array_length(lines) + json_object_array_length(events) >= 128) {
                        text(r, "message", "Excesso de linhas AT.");
                        done = 1;
                        break;
                    }
                    if (n == nt - ne && !memcmp(line, tx, n))
                        continue;
                    if (done) {
                        if (unsolicited(line))
                            json_object_array_add(events, json_object_new_string(line));
                        continue;
                    }
                    const char *success = bc_get(op, "success") ? bc_str(op, "success") : "OK";
                    int success_match = !strcmp(bc_str(op, "success_mode"), "prefix")
                                            ? !strncmp(line, success, strlen(success))
                                            : !strcmp(line, success);
                    if (success_match) {
                        matched = done = 1;
                        size_t frame_end = i + 1;
                        if (rx[i] == '\r' && frame_end < nr && rx[frame_end] == '\n')
                            frame_end++;
                        if (frame_end < nr) {
                            size_t remaining = nr - frame_end;
                            char *hex = bc_comm_format_bytes(rx + frame_end, remaining, 1);
                            text(r, "trailing_bytes", hex);
                            free(hex);
                            if (!io->unread ||
                                io->unread(io->context, rx + frame_end, remaining, e) < 0)
                                goto bad;
                        }
                        nr = frame_end;
                        break;
                    }
                    if (!strcmp(line, "ERROR") || !strcmp(line, "FAIL") ||
                        !strncmp(line, "+CME ERROR", 10) || !strncmp(line, "busy", 4)) {
                        text(r, "message", line);
                        done = 1;
                        matched = 0;
                        break;
                    }
                    json_object_array_add(unsolicited(line) ? events : lines,
                                          json_object_new_string(line));
                }
            }
        }
    }
    if (!done && !timed_out)
        text(r, "message", "Resposta excedeu o limite de bytes ou ficou incompleta.");
    if (timed_out)
        text(r, "message", "Tempo de resposta esgotado.");
    char *sent = bc_comm_format_bytes(tx, nt, 1), *received = bc_comm_format_bytes(rx, nr, 1);
    text(r, "transmitted", sent);
    text(r, "received", received);
    free(sent);
    free(received);
    text(r, "protocol", proto);
    text(r, "unit", modbus ? "código" : at ? "resposta" : "bytes");
    double value = at ? (matched ? 1 : 0) : (double)nr;
    if (modbus) {
        value = matched ? 1 : 0;
        if (matched && function <= 2 && nr > 5)
            value = rx[3] & 1u;
        if (matched && (function == 3 || function == 4) && nr >= 7)
            value = ((unsigned)rx[3] << 8) | rx[4];
    }
    json_object_object_add(r, "value", json_object_new_double(value));
    json_object_object_add(r, "matched", json_object_new_boolean(matched));
    json_object_object_add(r, "timed_out", json_object_new_boolean(timed_out));
    json_object_object_add(r, "operation", bc_clone(op));
    return r;
bad:
    json_object_put(r);
    return NULL;
}
