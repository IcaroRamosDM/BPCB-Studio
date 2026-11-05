#include "studio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bs_communication_catalog(const char *kind, json_object *fields) {
    if (!strcmp(kind, "test")) {
        json_object_array_add(
            fields, json_tokener_parse(
                        "{\"key\":\"comm_enabled\",\"label\":\"Usar comunicação "
                        "selecionável?\",\"type\":\"boolean\",\"default\":false,\"help\":\"Exige a "
                        "escolha explícita de adaptador no BPCB. Um barramento por teste; não "
                        "salva porta nem autoriza ligações.\",\"optional\":true}"));
        json_object_array_add(
            fields, json_tokener_parse(
                        "{\"key\":\"comm_bus\",\"label\":\"Barramento desta "
                        "sequência\",\"type\":\"choice\",\"default\":\"i2c\",\"help\":\"O mesmo "
                        "barramento é mantido durante toda a "
                        "execução.\",\"optional\":true,\"options\":[{\"id\":\"i2c\",\"label\":"
                        "\"I2C\"},{\"id\":\"spi\",\"label\":\"SPI\"},{\"id\":\"uart\",\"label\":"
                        "\"UART\"},{\"id\":\"jtag\",\"label\":\"JTAG\"}]}"));
        json_object_array_add(
            fields,
            json_tokener_parse(
                "{\"key\":\"comm_adapters\",\"label\":\"Adaptadores permitidos (separados por "
                "vírgula)\",\"type\":\"text\",\"default\":\"ch341t,ft232h,mcp2221a,cp2112,ch347\","
                "\"help\":\"I²C: ch341t/ft232h/mcp2221a/cp2112/ch347. SPI: ft232h/ch347. UART: "
                "serial/ch340/cp2102/ft232rl/ft232h/mcp2221a/ch347. JTAG: "
                "ch347.\",\"optional\":true}"));
        json_object_array_add(
            fields,
            json_tokener_parse("{\"key\":\"comm_min_hz\",\"label\":\"Clock mínimo (Hz; UART "
                               "ignora)\",\"type\":\"integer\",\"default\":20000,\"help\":\"Limite "
                               "publicado no .bancada, conferido antes de abrir o "
                               "adaptador.\",\"optional\":true,\"min\":1,\"max\":30000000}"));
        json_object_array_add(
            fields,
            json_tokener_parse("{\"key\":\"comm_max_hz\",\"label\":\"Clock máximo (Hz; UART "
                               "ignora)\",\"type\":\"integer\",\"default\":400000,\"help\":"
                               "\"Limite publicado no .bancada, conferido antes de abrir o "
                               "adaptador.\",\"optional\":true,\"min\":1,\"max\":30000000}"));
        json_object_array_add(
            fields,
            json_tokener_parse("{\"key\":\"comm_max_transfer\",\"label\":\"Máximo por operação "
                               "(bytes)\",\"type\":\"integer\",\"default\":32,\"help\":\"Limite "
                               "publicado no .bancada, conferido antes de abrir o "
                               "adaptador.\",\"optional\":true,\"min\":1,\"max\":4096}"));
        json_object_array_add(
            fields, json_tokener_parse(
                        "{\"key\":\"comm_timeout_ms\",\"label\":\"Timeout máximo por operação "
                        "(ms)\",\"type\":\"integer\",\"default\":1000,\"help\":\"Limite publicado "
                        "no .bancada, conferido antes de abrir o "
                        "adaptador.\",\"optional\":true,\"min\":50,\"max\":10000}"));
    }
    if (!strcmp(kind, "step")) {
        json_object_array_add(
            fields, json_tokener_parse(
                        "{\"key\":\"comm_bus\",\"label\":\"Barramento do "
                        "bloco\",\"type\":\"choice\",\"default\":\"i2c\",\"help\":\"Deve "
                        "corresponder ao barramento do "
                        "teste.\",\"optional\":true,\"options\":[{\"id\":\"i2c\",\"label\":\"I2C\"}"
                        ",{\"id\":\"spi\",\"label\":\"SPI\"},{\"id\":\"uart\",\"label\":\"UART\"},{"
                        "\"id\":\"jtag\",\"label\":\"JTAG\"}],\"when\":\"communication\"}"));
        json_object_array_add(
            fields,
            json_tokener_parse(
                "{\"key\":\"comm_encoding\",\"label\":\"Formato da "
                "escrita\",\"type\":\"choice\",\"default\":\"hex\",\"help\":\"ASCII restrito a "
                "0..127; hexadecimal permite todos os "
                "bytes.\",\"optional\":true,\"options\":[{\"id\":\"hex\",\"label\":\"Hexadecimal\"}"
                ",{\"id\":\"ascii\",\"label\":\"ASCII\"}],\"when\":\"communication\"}"));
        json_object_array_add(
            fields, json_tokener_parse(
                        "{\"key\":\"comm_write\",\"label\":\"Bytes ou texto a "
                        "transmitir\",\"type\":\"multiline\",\"default\":\"\",\"help\":\"No SPI o "
                        "tamanho da leitura é o mesmo da escrita. Nenhum comando é repetido "
                        "automaticamente.\",\"optional\":true,\"when\":\"communication\"}"));
        json_object_array_add(
            fields,
            json_tokener_parse(
                "{\"key\":\"comm_ending\",\"label\":\"Delimitador UART no "
                "envio\",\"type\":\"choice\",\"default\":\"none\",\"help\":\"Aplicado somente a "
                "UART; não altera a leitura exata do "
                "bloco.\",\"optional\":true,\"options\":[{\"id\":\"none\",\"label\":\"Nenhum\"},{"
                "\"id\":\"lf\",\"label\":\"LF\"},{\"id\":\"cr\",\"label\":\"CR\"},{\"id\":\"crlf\","
                "\"label\":\"CRLF\"}],\"when\":\"communication\",\"when_bus\":\"uart\"}"));
        json_object_array_add(
            fields,
            json_tokener_parse("{\"key\":\"comm_read\",\"label\":\"Quantidade exata a receber "
                               "(bytes)\",\"type\":\"integer\",\"default\":1,\"help\":\"Timeout ou "
                               "resposta incompleta interrompem a "
                               "sequência.\",\"optional\":true,\"min\":0,\"max\":4096,\"when\":"
                               "\"communication\"}"));
        json_object_array_add(
            fields, json_tokener_parse("{\"key\":\"comm_address\",\"label\":\"Endereço I²C de 7 "
                                       "bits\",\"type\":\"integer\",\"default\":72,\"help\":"
                                       "\"Decimal ou 0x hexadecimal. Endereços reservados "
                                       "excluídos.\",\"optional\":true,\"min\":8,\"max\":119,"
                                       "\"when\":\"communication\",\"when_bus\":\"i2c\"}"));
        json_object_array_add(
            fields, json_tokener_parse(
                        "{\"key\":\"comm_expect\",\"label\":\"Resposta esperada (hexadecimal "
                        "opcional)\",\"type\":\"text\",\"default\":\"\",\"help\":\"Comprimento "
                        "igual à leitura. Divergência interrompe a "
                        "sequência.\",\"optional\":true,\"when\":\"communication\"}"));
        json_object_array_add(
            fields, json_tokener_parse(
                        "{\"key\":\"comm_mask\",\"label\":\"Máscara da comparação (hex "
                        "opcional)\",\"type\":\"text\",\"default\":\"\",\"help\":\"FF compara "
                        "todos os bits; 00 ignora o byte correspondente. Exige resposta "
                        "esperada.\",\"optional\":true,\"when\":\"communication\"}"));
        json_object_array_add(
            fields,
            json_tokener_parse("{\"key\":\"comm_register\",\"label\":\"Registrador "
                               "JTAG\",\"type\":\"choice\",\"default\":\"dr\",\"help\":\"IR e DR "
                               "compartilham a sessão durante o "
                               "teste.\",\"optional\":true,\"options\":[{\"id\":\"dr\",\"label\":"
                               "\"DR\"},{\"id\":\"ir\",\"label\":\"IR\"},{\"id\":\"reset\","
                               "\"label\":\"Reset TAP\"},{\"id\":\"idle\",\"label\":\"Idle "
                               "TCK\"}],\"when\":\"communication\",\"when_bus\":\"jtag\"}"));
        json_object_array_add(
            fields, json_tokener_parse("{\"key\":\"comm_bits\",\"label\":\"Quantidade de bits "
                                       "JTAG\",\"type\":\"integer\",\"default\":8,\"help\":\"A "
                                       "leitura deve ter ceil(bits/8) bytes. Reset e Idle exigem "
                                       "zero.\",\"optional\":true,\"min\":0,\"max\":32768,\"when\":"
                                       "\"communication\",\"when_bus\":\"jtag\"}"));
        json_object_array_add(
            fields, json_tokener_parse("{\"key\":\"comm_idle\",\"label\":\"Ciclos Idle "
                                       "TCK\",\"type\":\"integer\",\"default\":0,\"help\":\"Idle "
                                       "explícito exige 1..4096; scans podem terminar com ciclos "
                                       "adicionais.\",\"optional\":true,\"min\":0,\"max\":4096,"
                                       "\"when\":\"communication\",\"when_bus\":\"jtag\"}"));
        json_object_array_add(
            fields,
            json_tokener_parse("{\"key\":\"comm_ir_bits\",\"label\":\"IR antes de DR (bits; zero "
                               "desativa)\",\"type\":\"integer\",\"default\":0,\"help\":\"Prefixo "
                               "opcional executado na mesma "
                               "transação.\",\"optional\":true,\"min\":0,\"max\":32768,\"when\":"
                               "\"communication\",\"when_bus\":\"jtag\"}"));
        json_object_array_add(
            fields,
            json_tokener_parse(
                "{\"key\":\"comm_ir_write\",\"label\":\"IR antes de DR "
                "(hexadecimal)\",\"type\":\"text\",\"default\":\"\",\"help\":\"Exige o comprimento "
                "exato e bits altos não usados em "
                "zero.\",\"optional\":true,\"when\":\"communication\",\"when_bus\":\"jtag\"}"));
        json_object_array_add(
            fields,
            json_tokener_parse(
                "{\"key\":\"comm_decode\",\"label\":\"Valor numérico da "
                "resposta\",\"type\":\"choice\",\"default\":\"count\",\"help\":\"A variável recebe "
                "contagem de bytes ou um valor inteiro. Bytes completos ficam na "
                "evidência.\",\"optional\":true,\"options\":[{\"id\":\"count\",\"label\":"
                "\"Quantidade recebida\"},{\"id\":\"u8\",\"label\":\"Inteiro 8 bits sem "
                "sinal\"},{\"id\":\"u16be\",\"label\":\"Inteiro 16 bits big "
                "endian\"},{\"id\":\"u16le\",\"label\":\"Inteiro 16 bits little "
                "endian\"},{\"id\":\"s16be\",\"label\":\"Inteiro 16 bits com sinal big "
                "endian\"},{\"id\":\"s16le\",\"label\":\"Inteiro 16 bits com sinal little "
                "endian\"},{\"id\":\"u32be\",\"label\":\"Inteiro 32 bits big "
                "endian\"},{\"id\":\"u32le\",\"label\":\"Inteiro 32 bits little "
                "endian\"}],\"when\":\"communication\"}"));
        json_object_array_add(
            fields, json_tokener_parse(
                        "{\"key\":\"comm_simulated\",\"label\":\"Resposta de exemplo na simulação "
                        "(hex)\",\"type\":\"multiline\",\"default\":\"00\",\"help\":\"Deve ter o "
                        "comprimento exato da operação; não acessa USB e também testa "
                        "comparações/máscaras.\",\"optional\":true,\"when\":\"communication\"}"));
    }
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        json_object *f = json_object_array_get_idx(fields, i);
        const char *key = bc_str(f, "key");
        if (!strcmp(kind, "step") && !strcmp(key, "type")) {
            json_object *o = json_object_new_object();
            json_object_object_add(o, "id", json_object_new_string("communication"));
            json_object_object_add(o, "label",
                                   json_object_new_string("Transferência I²C / SPI / UART / JTAG"));
            json_object_array_add(bc_get(f, "options"), o);
        }
        if (!strcmp(kind, "step") && !strcmp(key, "variable"))
            json_object_object_add(
                f, "when", json_object_new_string("manual,read,calculate,wait,communication"));
        if (!strcmp(kind, "device") && !strcmp(key, "transport")) {
            json_object *o = json_object_new_object();
            json_object_object_add(o, "id", json_object_new_string("selected"));
            json_object_object_add(o, "label",
                                   json_object_new_string("Adaptador I²C escolhido no BPCB"));
            json_object_array_add(bc_get(f, "options"), o);
        }
    }
}

json_object *bs_communication(json_object *t, bc_error *e) {
    if (!strcmp(bc_str(t, "comm_mode"), "multi")) {
        if (!json_object_get_boolean(bc_get(t, "comm_enabled"))) {
            bc_fail(e, "Ative a comunicação selecionável.");
            return NULL;
        }
        json_object *rows = bc_get(t, "comm_channels"), *d = json_object_new_object(),
                    *channels = json_object_new_object();
        json_object_object_add(d, "api", json_object_new_int(2));
        json_object_object_add(d, "channels", channels);
        if (!json_object_is_type(rows, json_type_array) || json_object_array_length(rows) > 8) {
            json_object_put(d);
            bc_fail(e, "Lista de canais inválida.");
            return NULL;
        }
        for (size_t i = 0; i < json_object_array_length(rows); i++) {
            json_object *row = json_object_array_get_idx(rows, i);
            const char *id = bc_str(row, "id");
            if (!bc_lab_id(id) || bc_get(channels, id)) {
                json_object_put(d);
                bc_fail(e, "Nome de canal inválido/repetido.");
                return NULL;
            }
            json_object *ch = bs_lab_test_channel(t, id, e),
                        *decl = ch ? bs_communication(ch, e) : NULL;
            json_object_put(ch);
            if (!decl) {
                json_object_put(d);
                return NULL;
            }
            json_object_object_add(channels, id, decl);
        }
        if (bc_comm_declaration(d, e) < 0) {
            json_object_put(d);
            return NULL;
        }
        return d;
    }

    if (!json_object_get_boolean(bc_get(t, "comm_enabled"))) {
        bc_fail(e, "Ative comunicação selecionável no cadastro do teste.");
        return NULL;
    }
    json_object *d = json_object_new_object(), *a = json_object_new_array();
    json_object_object_add(d, "api", json_object_new_int(1));
    json_object_object_add(d, "bus", json_object_new_string(bc_str(t, "comm_bus")));
    json_object_object_add(d, "adapters", a);
    char *text = strdup(bc_str(t, "comm_adapters")), *save = NULL;
    for (char *token = text ? strtok_r(text, ",", &save) : NULL; token;
         token = strtok_r(NULL, ",", &save)) {
        while (*token == ' ')
            token++;
        size_t n = strlen(token);
        while (n && token[n - 1] == ' ')
            token[--n] = 0;
        json_object_array_add(a, json_object_new_string(token));
    }
    free(text);
    const char *src[] = {"comm_min_hz", "comm_max_hz", "comm_max_transfer", "comm_timeout_ms"},
               *dst[] = {"min_hz", "max_hz", "max_transfer", "max_timeout_ms"};
    for (unsigned i = 0; i < 4; i++)
        json_object_object_add(d, dst[i], bc_clone(bc_get(t, src[i])));
    if (bc_comm_declaration(d, e) < 0) {
        json_object_put(d);
        return NULL;
    }
    return d;
}

json_object *bs_communication_operation(json_object *t, json_object *s, bc_error *e) {
    if (!strcmp(bc_str(t, "comm_mode"), "multi") ||
        (*bc_str(s, "lab_mode") && strcmp(bc_str(s, "lab_mode"), "raw")))
        return bs_lab_operation(t, s, e);
    const char *keys[] = {"comm_read", "comm_address", "comm_bits", "comm_idle", "comm_ir_bits"};
    const int low[] = {0, 8, 0, 0, 0}, high[] = {4096, 119, 32768, 4096, 32768};
    for (unsigned i = 0; i < 5; i++) {
        if (i == 1 && strcmp(bc_str(t, "comm_bus"), "i2c"))
            continue;
        if (i >= 2 && strcmp(bc_str(t, "comm_bus"), "jtag"))
            continue;
        json_object *v = bc_get(s, keys[i]);
        if (!json_object_is_type(v, json_type_int) || json_object_get_int64(v) < low[i] ||
            json_object_get_int64(v) > high[i]) {
            bc_fail(e, "Campo inteiro de comunicação inválido: %s.", keys[i]);
            return NULL;
        }
    }
    json_object *d = bs_communication(t, e);
    if (!d)
        return NULL;
    const char *bus = bc_str(t, "comm_bus");
    if (strcmp(bus, bc_str(s, "comm_bus"))) {
        bc_fail(e, "O bloco usa um barramento diferente do teste.");
        json_object_put(d);
        return NULL;
    }
    unsigned read = (unsigned)bc_num(s, "comm_read");
    json_object *o = NULL;
    int control = !strcmp(bus, "jtag") && (!strcmp(bc_str(s, "comm_register"), "reset") ||
                                           !strcmp(bc_str(s, "comm_register"), "idle"));
    if (control) {
        if (*bc_str(s, "comm_write") || read) {
            json_object_put(d);
            bc_fail(e, "Reset/Idle JTAG exigem escrita vazia e leitura zero.");
            return NULL;
        }
        o = json_object_new_object();
        json_object_object_add(o, "write", json_object_new_string(""));
        json_object_object_add(o, "read", json_object_new_int(0));
    } else
        o = bc_comm_text_operation(
            bc_str(s, "comm_write"), !strcmp(bc_str(s, "comm_encoding"), "hex"),
            !strcmp(bus, "uart") ? bc_str(s, "comm_ending") : "none", read, e);
    if (!o) {
        json_object_put(d);
        return NULL;
    }
    if (!strcmp(bus, "i2c"))
        json_object_object_add(o, "address", bc_clone(bc_get(s, "comm_address")));
    if (!strcmp(bus, "jtag")) {
        json_object_object_add(o, "register", json_object_new_string(bc_str(s, "comm_register")));
        json_object_object_add(o, "bits", bc_clone(bc_get(s, "comm_bits")));
        json_object_object_add(o, "idle_cycles", bc_clone(bc_get(s, "comm_idle")));
        if (bc_num(s, "comm_ir_bits")) {
            json_object_object_add(o, "ir_bits", bc_clone(bc_get(s, "comm_ir_bits")));
            json_object_object_add(o, "ir_write",
                                   json_object_new_string(bc_str(s, "comm_ir_write")));
        }
    }
    if (*bc_str(s, "comm_expect"))
        json_object_object_add(o, "expect", json_object_new_string(bc_str(s, "comm_expect")));
    if (*bc_str(s, "comm_mask"))
        json_object_object_add(o, "mask", json_object_new_string(bc_str(s, "comm_mask")));
    json_object_object_add(o, "label", json_object_new_string(bc_str(s, "title")));
    int rc = bc_comm_operation_validate(bus, o, e);
    uint8_t bytes[BC_COMM_MAX];
    size_t n = 0;
    bc_comm_hex(bc_str(o, "write"), bytes, sizeof bytes, &n, e);
    size_t received = !strcmp(bus, "spi") ? n : read;
    if (!rc &&
        (n > (size_t)bc_num(d, "max_transfer") || received > (size_t)bc_num(d, "max_transfer") ||
         (bc_num(s, "comm_ir_bits") + 7) / 8 > bc_num(d, "max_transfer")))
        rc = bc_fail(e, "Bloco excede o limite de transferência publicado.");
    json_object_put(d);
    if (rc < 0) {
        json_object_put(o);
        return NULL;
    }
    return o;
}

json_object *bs_communication_result_prepared(json_object *t, json_object *s, json_object *result,
                                              int simulate, json_object *prepared, bc_error *e) {
    if (!strcmp(bc_str(t, "comm_mode"), "multi") || bc_get(s, "lab_capture") ||
        (*bc_str(s, "lab_mode") && strcmp(bc_str(s, "lab_mode"), "raw")))
        return bs_lab_result_prepared(t, s, result, simulate, prepared, e);
    json_object *op = prepared ? bc_clone(prepared) : bs_communication_operation(t, s, e);
    if (!op)
        return NULL;
    uint8_t rx[BC_COMM_MAX], tx[BC_COMM_MAX], expected[BC_COMM_MAX], mask[BC_COMM_MAX];
    size_t n = 0, nt = 0, ne = 0;
    bc_comm_hex(bc_str(op, "write"), tx, sizeof tx, &nt, e);
    if (bc_comm_hex(simulate ? bc_str(s, "comm_simulated") : bc_str(result, "received"), rx,
                    sizeof rx, &n, e) < 0)
        goto bad;
    size_t required = !strcmp(bc_str(t, "comm_bus"), "spi") ? nt : (size_t)bc_num(op, "read");
    if (n != required) {
        bc_fail(e, "Resposta da comunicação tem %zu bytes; esperado %zu.", n, required);
        goto bad;
    }
    int matched = 1;
    if (*bc_str(op, "expect")) {
        bc_comm_hex(bc_str(op, "expect"), expected, sizeof expected, &ne, e);
        memset(mask, 255, ne);
        if (*bc_str(op, "mask"))
            bc_comm_hex(bc_str(op, "mask"), mask, sizeof mask, &ne, e);
        if (!strcmp(bc_str(t, "comm_bus"), "jtag") && (unsigned)bc_num(op, "bits") % 8 && n)
            mask[n - 1] &= (uint8_t)((1u << ((unsigned)bc_num(op, "bits") % 8)) - 1);
        for (size_t i = 0; i < n; i++)
            if ((rx[i] & mask[i]) != (expected[i] & mask[i]))
                matched = 0;
    }
    const char *decode = bc_str(s, "comm_decode");
    double value = (double)n;
    if (strcmp(decode, "count")) {
        size_t width = !strcmp(decode, "u8")                                      ? 1
                       : !strncmp(decode, "u16", 3) || !strncmp(decode, "s16", 3) ? 2
                       : !strncmp(decode, "u32", 3)                               ? 4
                                                                                  : 0;
        if (!width || width != n) {
            bc_fail(e, "Decodificação exige 1, 2 ou 4 bytes conforme o formato.");
            goto bad;
        }
        uint32_t raw = 0;
        int little = strstr(decode, "le") != NULL;
        for (size_t i = 0; i < width; i++)
            raw = (raw << 8) | rx[little ? width - i - 1 : i];
        value = decode[0] == 's' ? (double)(int16_t)(uint16_t)raw : (double)raw;
    }
    json_object *r = result ? bc_clone(result) : json_object_new_object();
    char *hex = bc_comm_format_bytes(rx, n, 1);
    json_object_object_add(r, "received", json_object_new_string(hex));
    free(hex);
    json_object_object_add(r, "value", json_object_new_double(value));
    json_object_object_add(r, "matched", json_object_new_boolean(matched));
    json_object_object_add(r, "simulation", json_object_new_boolean(simulate));
    json_object_object_add(r, "operation", op);
    return r;
bad:
    json_object_put(op);
    return NULL;
}

json_object *bs_communication_result(json_object *t, json_object *s, json_object *result,
                                     int simulate, bc_error *e) {
    json_object *normalized = NULL;
    if (bs_form("step", s, &normalized, e) < 0)
        return NULL;
    json_object *r = bs_communication_result_prepared(t, normalized, result, simulate, NULL, e);
    json_object_put(normalized);
    return r;
}
