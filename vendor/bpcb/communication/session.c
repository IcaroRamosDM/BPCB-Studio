#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "../lab.h"
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

json_object *bc_comm_list(int (*cancel)(void *), void *context, bc_error *e) {
    json_object *list = json_object_new_array();
    if (bc_comm_usb_list(list, cancel, context, e) < 0 ||
        bc_comm_hid_list(list, cancel, context, e) < 0 || bc_comm_serial_list(list, e) < 0) {
        json_object_put(list);
        return NULL;
    }
    return list;
}

static bc_comm *allocate(json_object *config, int (*cancel)(void *), void *context, bc_error *e) {
    json_object *c = bc_comm_config(config, e);
    if (!c)
        return NULL;
    bc_comm *s = calloc(1, sizeof *s);
    if (!s) {
        json_object_put(c);
        bc_fail(e, "Memória insuficiente.");
        return NULL;
    }
    s->config = c;
    s->cancel = cancel;
    s->cancel_context = context;
    const char *a = bc_str(c, "adapter");
    int hid_uart =
        !strcmp(a, "ch347") &&
        (!strcmp(bc_str(c, "device"), "") || !strncmp(bc_str(c, "device"), "/dev/hidraw", 11) ||
         !strncmp(bc_str(c, "device"), "\\\\?\\hid#", 8) || bc_num(c, "ch347_mode") == 2);
    s->kind = !strcmp(bc_str(c, "bus"), "uart") && !hid_uart ? BC_SERIAL
              : !strcmp(a, "ch341t")                         ? BC_CH341
              : !strcmp(a, "ft232h")                         ? BC_FT232H
              : !strcmp(a, "ch347")                          ? BC_CH347
              : !strcmp(a, "mcp2221a")                       ? BC_MCP2221
                                                             : BC_CP2112;
    bc_comm_deadline(s);
    return s;
}

int bc_comm_open(bc_comm **out, json_object *config, int (*cancel)(void *), void *context,
                 bc_error *e) {
    *out = NULL;
    bc_comm *s = allocate(config, cancel, context, e);
    if (!s)
        return -1;
    int rc = s->kind == BC_CH347    ? bc_comm_ch347_open(s, 0, e)
             : s->kind == BC_SERIAL ? bc_comm_serial_open(s, e)
             : s->kind == BC_CH341 || s->kind == BC_FT232H || s->kind == BC_CH347
                 ? bc_comm_usb_open(s, 0, e)
                 : bc_comm_hid_open(s, e);
    if (!rc)
        rc = bc_comm_protocol_init(s, e);
    if (rc < 0) {
        int saved = errno;
        bc_comm_close(s);
        errno = saved;
        return -1;
    }
    *out = s;
    return 0;
}

json_object *bc_comm_information(bc_comm *s) {
    if (!s)
        return NULL;
    json_object *info = json_object_new_object();
    json_object_object_add(info, "configuration", bc_clone(s->config));
    json_object_object_add(info, "actual_hz", json_object_new_int64(s->actual_hz));
    return info;
}

void bc_comm_close(bc_comm *s) {
    if (!s)
        return;
    if (s->io.close)
        s->io.close(s->io.context);
    json_object_put(s->config);
    free(s);
}

json_object *bc_comm_probe(json_object *config, int (*cancel)(void *), void *context, bc_error *e) {
    bc_comm *s = allocate(config, cancel, context, e);
    if (!s)
        return NULL;
    int rc = s->kind == BC_CH347    ? bc_comm_ch347_open(s, 1, e)
             : s->kind == BC_SERIAL ? bc_comm_serial_probe(s->config, e)
             : s->kind == BC_CH341 || s->kind == BC_FT232H || s->kind == BC_CH347
                 ? bc_comm_usb_open(s, 1, e)
                 : bc_comm_hid_open(s, e);
    if (rc < 0) {
        bc_comm_close(s);
        return NULL;
    }
    json_object *result = json_object_new_object();
    json_object_object_add(result, "configuration", bc_clone(s->config));
    json_object_object_add(
        result, "scope",
        json_object_new_string(
            s->kind == BC_SERIAL
                ? "Porta e permissão conferidas sem abrir/configurar UART; abertura pode alterar "
                  "DTR/RTS e depende de uma operação explícita."
                : "Acesso ao dispositivo conferido. Nenhum comando I²C/SPI, configuração MPSSE ou "
                  "GPIO foi enviado. Não comprova conexões ou resposta da PCB."));
    bc_comm_close(s);
    return result;
}

int bc_comm_i2c(bc_comm *s, bc_comm_i2c_message *m, unsigned n, bc_error *e) {
    if (!s || strcmp(bc_str(s->config, "bus"), "i2c"))
        return bc_fail(e, "Sessão não está em I²C.");
    if (!m || n < 1 || n > 2)
        return bc_fail(
            e, "Use uma mensagem I²C ou uma escrita seguida de leitura com repeated START.");
    for (unsigned i = 0; i < n; i++) {
        size_t maximum = s->kind == BC_CH341     ? 32
                         : s->kind == BC_MCP2221 ? (m[i].flags & BC_COMM_I2C_READ ? 4096 : 60)
                         : s->kind == BC_CP2112  ? (m[i].flags & BC_COMM_I2C_READ ? 512
                                                    : n == 2                      ? 16
                                                                                  : 61)
                                                 : BC_COMM_MAX;
        if (m[i].address < 8 || m[i].address > 0x77 ||
            (m[i].flags != 0 && m[i].flags != BC_COMM_I2C_READ) || !m[i].data || !m[i].size ||
            m[i].size > maximum)
            return bc_fail(e,
                           "Mensagem I²C fora dos limites do adaptador: endereço 08…77, tamanho "
                           "máximo %zu bytes.",
                           maximum);
    }
    if (n == 2 && (m[0].flags || m[1].flags != BC_COMM_I2C_READ || m[0].address != m[1].address))
        return bc_fail(e, "Leitura combinada requer escrita e leitura no mesmo endereço I²C.");
    bc_comm_deadline(s);
    if (bc_comm_check(s, e) < 0)
        return -1;
    int rc = bc_comm_protocol_i2c(s, m, n, e);
    if (rc < 0 && (s->kind == BC_MCP2221 || s->kind == BC_CP2112)) { /* Do not leave an unfinished
                                              HID transfer running or reuse stale reports. */
        int saved = errno;
        bc_error ignored = {{0}};
        int (*cancel)(void *) = s->cancel;
        s->cancel = NULL;
        s->deadline = bc_comm_now() + 0.1;
        if (!s->lost) {
            uint8_t p[64] = {0}, r[64];
            p[0] = s->kind == BC_MCP2221 ? 0x10 : 0x17;
            p[s->kind == BC_MCP2221 ? 2 : 1] = s->kind == BC_MCP2221 ? 0x10 : 1;
            if (bc_comm_write(s, p, s->kind == BC_MCP2221 ? sizeof p : 2, &ignored) == 0 &&
                s->kind == BC_MCP2221)
                bc_comm_read(s, r, sizeof r, &ignored);
        }
        s->cancel = cancel;
        s->lost = saved ? saved : EIO;
        errno = saved;
    }
    return rc;
}

int bc_comm_spi(bc_comm *s, const uint8_t *tx, uint8_t *rx, size_t n, bc_error *e) {
    if (!s || (s->kind != BC_FT232H && s->kind != BC_CH347) ||
        strcmp(bc_str(s->config, "bus"), "spi"))
        return bc_fail(e, "SPI exige FT232H ou CH347T em modo SPI.");
    if (!tx || !rx || n < 1 || n > BC_COMM_MAX)
        return bc_fail(e, "SPI: use 1…4096 bytes por transação.");
    bc_comm_deadline(s);
    if (bc_comm_check(s, e) < 0)
        return -1;
    return bc_comm_protocol_spi(s, tx, rx, n, e);
}

int bc_comm_uart(bc_comm *s, const uint8_t *tx, size_t tx_size, uint8_t *rx, size_t rx_size,
                 bc_error *e) {
    if (!s || (s->kind != BC_SERIAL && s->kind != BC_CH347) ||
        strcmp(bc_str(s->config, "bus"), "uart"))
        return bc_fail(e, "Sessão não está em UART.");
    if (tx_size > BC_COMM_MAX || rx_size > BC_COMM_MAX || (!tx_size && !rx_size) ||
        (tx_size && !tx) || (rx_size && !rx))
        return bc_fail(e, "UART: transferência inválida; máximo 4096 bytes.");
    bc_comm_deadline(s);
    if (bc_comm_check(s, e) < 0)
        return -1;
    if (tx_size && bc_comm_write(s, tx, tx_size, e) < 0)
        return -1;
    if (tx_size && s->kind == BC_CH347) {
        unsigned frame = 1u + (unsigned)bc_num(s->config, "data_bits") +
                         (unsigned)bc_num(s->config, "stop_bits") +
                         (strcmp(bc_str(s->config, "parity"), "none") ? 1u : 0u);
        double until = bc_comm_now() + (double)(tx_size * frame) / bc_num(s->config, "baud");
        while (bc_comm_now() < until) {
            if (bc_comm_check(s, e) < 0)
                return -1;
            struct timespec t = {0, 1000000};
            nanosleep(&t, NULL);
        }
    }
    if (rx_size && s->uart_pending_size) {
        size_t n = s->uart_pending_size < rx_size ? s->uart_pending_size : rx_size;
        memcpy(rx, s->uart_pending, n);
        s->uart_pending_size -= n;
        memmove(s->uart_pending, s->uart_pending + n, s->uart_pending_size);
        rx += n;
        rx_size -= n;
    }
    if (rx_size && bc_comm_read(s, rx, rx_size, e) < 0)
        return -1;
    return 0;
}

int bc_comm_operation_validate(const char *bus, json_object *o, bc_error *e) {
    if (bc_get(o, "kind"))
        return bc_lab_operation_validate(bus, o, e);
    if (!json_object_is_type(o, json_type_object))
        return bc_fail(e, "Operação de comunicação inválida.");
    json_object_object_foreach(o, field, item) {
        if (json_object_is_type(item, json_type_string) &&
            (size_t)json_object_get_string_len(item) != strlen(json_object_get_string(item)))
            return bc_fail(e, "Operação contém NUL embutido: %s.", field);
    }
    const char *allowed[] = {"write",    "read", "address",     "expect",  "mask",    "label",
                             "register", "bits", "idle_cycles", "ir_bits", "ir_write"};
    json_object_object_foreach(o, key, value) {
        (void)value;
        int found = 0;
        for (unsigned i = 0; i < sizeof allowed / sizeof *allowed; i++)
            if (!strcmp(key, allowed[i]))
                found = 1;
        if (!found ||
            (strcmp(bus, "jtag") &&
             (!strcmp(key, "register") || !strcmp(key, "bits") || !strcmp(key, "idle_cycles") ||
              !strcmp(key, "ir_bits") || !strcmp(key, "ir_write"))))
            return bc_fail(e, "Campo de operação desconhecido neste barramento: %s.", key);
    }
    if (strcmp(bus, "i2c") && strcmp(bus, "spi") && strcmp(bus, "uart") && strcmp(bus, "jtag"))
        return bc_fail(e, "Barramento inválido.");
    uint8_t buffer[BC_COMM_MAX];
    size_t tx = 0, expected = 0, mask = 0;
    if ((bc_get(o, "write") && !json_object_is_type(bc_get(o, "write"), json_type_string)) ||
        bc_comm_hex(bc_str(o, "write"), buffer, sizeof buffer, &tx, e) < 0)
        return bc_fail(e, "Escrita hexadecimal inválida.");
    if (!strcmp(bus, "jtag") && bc_get(o, "bits") &&
        (!json_object_is_type(bc_get(o, "bits"), json_type_int) ||
         json_object_get_int64(bc_get(o, "bits")) < 0 ||
         json_object_get_int64(bc_get(o, "bits")) > BC_COMM_MAX * 8))
        return bc_fail(e, "JTAG: bits deve ser inteiro de 0 a 32768.");
    json_object *read = bc_get(o, "read");
    int64_t n = read                   ? json_object_get_int64(read)
                : !strcmp(bus, "spi")  ? (int64_t)tx
                : !strcmp(bus, "jtag") ? (json_object_get_int64(bc_get(o, "bits")) + 7) / 8
                                       : 0;
    if ((read && !json_object_is_type(read, json_type_int)) || n < 0 || n > BC_COMM_MAX ||
        (!tx && !n && strcmp(bus, "jtag")))
        return bc_fail(e, "Quantidade de bytes inválida.");
    if (!strcmp(bus, "jtag")) {
        const char *reg = bc_get(o, "register") ? bc_str(o, "register") : "dr";
        int scan = !strcmp(reg, "dr") || !strcmp(reg, "ir");
        int64_t bits = bc_get(o, "bits") ? json_object_get_int64(bc_get(o, "bits")) : 0;
        json_object *idle = bc_get(o, "idle_cycles");
        if ((!scan && strcmp(reg, "reset") && strcmp(reg, "idle")) ||
            (bc_get(o, "register") &&
             !json_object_is_type(bc_get(o, "register"), json_type_string)) ||
            (scan && (!json_object_is_type(bc_get(o, "bits"), json_type_int) || bits < 1 ||
                      bits > BC_COMM_MAX * 8)) ||
            (!scan && ((bc_get(o, "bits") &&
                        (!json_object_is_type(bc_get(o, "bits"), json_type_int) || bits)) ||
                       tx || n)) ||
            (scan && (n != (bits + 7) / 8 || (tx && tx != (size_t)n))) || bc_get(o, "address") ||
            (idle && (!json_object_is_type(idle, json_type_int) || bc_num(o, "idle_cycles") < 0 ||
                      bc_num(o, "idle_cycles") > 4096)) ||
            (!strcmp(reg, "idle") && bc_num(o, "idle_cycles") < 1))
            return bc_fail(e, "JTAG: use IR/DR com 1…32768 bits e bytes correspondentes, reset TAP "
                              "ou idle_cycles de 1…4096.");
        if (scan && tx && (bits % 8) && (buffer[tx - 1] >> (bits % 8)))
            return bc_fail(e,
                           "JTAG: bits não utilizados no último byte de escrita devem ser zero.");
    }
    if (!strcmp(bus, "jtag") && (bc_get(o, "ir_bits") || bc_get(o, "ir_write"))) {
        int64_t bits = json_object_get_int64(bc_get(o, "ir_bits"));
        size_t ir_size = 0;
        if ((bc_get(o, "register") && strcmp(bc_str(o, "register"), "dr")) ||
            !json_object_is_type(bc_get(o, "ir_bits"), json_type_int) || bits < 1 ||
            bits > BC_COMM_MAX * 8 ||
            !json_object_is_type(bc_get(o, "ir_write"), json_type_string) ||
            bc_comm_hex(bc_str(o, "ir_write"), buffer, sizeof buffer, &ir_size, e) < 0 ||
            ir_size != (size_t)(bits + 7) / 8 ||
            ((bits % 8) && (buffer[ir_size - 1] >> (bits % 8))))
            return bc_fail(e, "JTAG: IR prévia exige DR, ir_bits de 1…32768 e ir_write hexadecimal "
                              "com os bits superiores não utilizados zerados.");
    }
    if (!strcmp(bus, "spi") && (!tx || (size_t)n != tx))
        return bc_fail(
            e, "SPI é full duplex: envie um byte (00/FF se necessário) para cada byte recebido.");
    if (!strcmp(bus, "i2c") && (!json_object_is_type(bc_get(o, "address"), json_type_int) ||
                                bc_num(o, "address") < 8 || bc_num(o, "address") > 0x77))
        return bc_fail(e, "Endereço I²C deve ser de 7 bits, entre 8 e 119.");
    if (bc_get(o, "expect")) {
        if (!json_object_is_type(bc_get(o, "expect"), json_type_string) ||
            bc_comm_hex(bc_str(o, "expect"), buffer, sizeof buffer, &expected, e) < 0 || !n ||
            expected != (size_t)n)
            return bc_fail(e, "Comparação deve ter exatamente os bytes da leitura.");
    }
    if (bc_get(o, "mask")) {
        if (!bc_get(o, "expect") || !json_object_is_type(bc_get(o, "mask"), json_type_string) ||
            bc_comm_hex(bc_str(o, "mask"), buffer, sizeof buffer, &mask, e) < 0 || mask != expected)
            return bc_fail(e, "Máscara exige comparação e o mesmo tamanho.");
    }
    if (bc_get(o, "label") && (!json_object_is_type(bc_get(o, "label"), json_type_string) ||
                               strlen(bc_str(o, "label")) > 160))
        return bc_fail(e, "Rótulo de operação inválido.");
    return 0;
}

int bc_comm_operation_check(json_object *config, json_object *declaration, json_object *operation,
                            bc_error *e) {
    if (bc_num(declaration, "api") == 2 || bc_get(operation, "kind"))
        return bc_lab_operation_check(config, declaration, operation, e);
    const char *bus = bc_str(config, "bus"), *adapter = bc_str(config, "adapter");
    if (bc_comm_operation_validate(bus, operation, e) < 0)
        return -1;
    uint8_t buffer[BC_COMM_MAX];
    size_t tx = 0;
    if (bc_comm_hex(bc_str(operation, "write"), buffer, sizeof buffer, &tx, e) < 0)
        return -1;
    size_t rx = bc_get(operation, "read") ? (size_t)bc_num(operation, "read")
                : !strcmp(bus, "spi")     ? tx
                : !strcmp(bus, "jtag")    ? ((size_t)bc_num(operation, "bits") + 7) / 8
                                          : 0;
    size_t max_tx = BC_COMM_MAX, max_rx = BC_COMM_MAX;
    if (!strcmp(bus, "i2c")) {
        if (!strcmp(adapter, "ch341t"))
            max_tx = max_rx = 32;
        else if (!strcmp(adapter, "mcp2221a"))
            max_tx = 60;
        else if (!strcmp(adapter, "cp2112")) {
            max_tx = rx ? 16 : 61;
            max_rx = 512;
        }
    }
    size_t declared = bc_get(declaration, "max_transfer")
                          ? (size_t)bc_num(declaration, "max_transfer")
                          : BC_COMM_MAX;
    if (!strcmp(bus, "jtag") && bc_get(operation, "ir_bits") &&
        ((size_t)bc_num(operation, "ir_bits") + 7) / 8 > declared)
        return bc_fail(e, "Instrução IR excede o limite declarado no pacote.");
    if (tx > declared || rx > declared)
        return bc_fail(e, "Operação excede o limite de %zu bytes declarado no pacote.", declared);
    if (tx > max_tx || rx > max_rx)
        return bc_fail(
            e, "Operação excede os limites de %s: escrita até %zu bytes; leitura até %zu bytes.",
            adapter, max_tx, max_rx);
    return 0;
}

json_object *bc_comm_transaction(bc_comm *s, json_object *o, bc_error *e) {
    if (bc_get(o, "kind")) {
        bc_fail(e, "Operação de alto nível exige executor de laboratório.");
        return NULL;
    }
    if (!s) {
        bc_fail(e, "Sessão de comunicação ausente.");
        return NULL;
    }
    const char *bus = bc_str(s->config, "bus");
    if (bc_comm_operation_check(s->config, NULL, o, e) < 0)
        return NULL;
    uint8_t tx[BC_COMM_MAX], rx[BC_COMM_MAX], expected[BC_COMM_MAX], mask[BC_COMM_MAX];
    size_t size = 0;
    if (bc_comm_hex(bc_str(o, "write"), tx, sizeof tx, &size, e) < 0)
        return NULL;
    size_t n = bc_get(o, "read")      ? (size_t)bc_num(o, "read")
               : !strcmp(bus, "spi")  ? size
               : !strcmp(bus, "jtag") ? ((size_t)bc_num(o, "bits") + 7) / 8
                                      : 0;
    int rc;
    if (!strcmp(bus, "i2c")) {
        bc_comm_i2c_message m[2];
        unsigned count = 0;
        if (size)
            m[count++] = (bc_comm_i2c_message){(uint16_t)bc_num(o, "address"), 0, size, tx};
        if (n)
            m[count++] =
                (bc_comm_i2c_message){(uint16_t)bc_num(o, "address"), BC_COMM_I2C_READ, n, rx};
        rc = bc_comm_i2c(s, m, count, e);
    } else if (!strcmp(bus, "spi"))
        rc = bc_comm_spi(s, tx, rx, size, e);
    else if (!strcmp(bus, "jtag")) {
        if (!size)
            memset(tx, 0, n);
        size = n;
        bc_comm_deadline(s);
        rc = 0;
        if (bc_get(o, "ir_bits")) {
            uint8_t ir_tx[BC_COMM_MAX], ir_rx[BC_COMM_MAX];
            size_t ir_size = 0;
            bc_comm_hex(bc_str(o, "ir_write"), ir_tx, sizeof ir_tx, &ir_size, e);
            rc = bc_comm_ch347_jtag(s, "ir", ir_tx, ir_rx, (unsigned)bc_num(o, "ir_bits"), 0, e);
        }
        if (!rc)
            rc = bc_comm_ch347_jtag(s, bc_get(o, "register") ? bc_str(o, "register") : "dr", tx, rx,
                                    (unsigned)bc_num(o, "bits"), (unsigned)bc_num(o, "idle_cycles"),
                                    e);
    } else
        rc = bc_comm_uart(s, tx, size, rx, n, e);
    if (rc < 0)
        return NULL;
    json_object *r = json_object_new_object();
    char hex[BC_COMM_MAX * 3 + 1];
    for (size_t i = 0; i < n; i++)
        snprintf(hex + i * 3, sizeof hex - i * 3, "%02X%s", rx[i], i + 1 < n ? " " : "");
    if (!n)
        hex[0] = 0;
    json_object_object_add(r, "received", json_object_new_string(hex));
    json_object_object_add(r, "bytes_read", json_object_new_int64((int64_t)n));
    size_t written = size;
    if (!strcmp(bus, "jtag") && bc_get(o, "ir_bits"))
        written += ((size_t)bc_num(o, "ir_bits") + 7) / 8;
    json_object_object_add(r, "bytes_written", json_object_new_int64((int64_t)written));
    if (!strcmp(bus, "jtag")) {
        json_object_object_add(r, "bits", json_object_new_int64((int64_t)bc_num(o, "bits")));
        if (bc_get(o, "ir_bits"))
            json_object_object_add(r, "ir_bits", json_object_get(bc_get(o, "ir_bits")));
    }
    json_object_object_add(r, "actual_hz",
                           json_object_new_int(s->kind == BC_SERIAL ? 0 : (int)s->actual_hz));
    json_object_object_add(r, "configuration", bc_clone(s->config));
    if (bc_get(o, "expect")) {
        size_t unused = 0;
        bc_comm_hex(bc_str(o, "expect"), expected, sizeof expected, &unused, e);
        memset(mask, 255, n);
        if (bc_get(o, "mask"))
            bc_comm_hex(bc_str(o, "mask"), mask, sizeof mask, &unused, e);
        if (!strcmp(bus, "jtag") && (unsigned)bc_num(o, "bits") % 8u && n)
            mask[n - 1] &= (uint8_t)((1u << ((unsigned)bc_num(o, "bits") % 8u)) - 1u);
        int matched = 1;
        for (size_t i = 0; i < n; i++)
            if ((rx[i] & mask[i]) != (expected[i] & mask[i]))
                matched = 0;
        json_object_object_add(r, "matched", json_object_new_boolean(matched));
    }
    return r;
}

json_object *bc_comm_exchange(json_object *config, json_object *operation, int (*cancel)(void *),
                              void *context, bc_error *e) {
    json_object *c = bc_comm_config(config, e);
    if (!c)
        return NULL;
    if (!json_object_get_boolean(bc_get(c, "connections_checked"))) {
        json_object_put(c);
        bc_fail(e, "Confirme as conexões e níveis antes de transmitir.");
        return NULL;
    }
    if (bc_comm_operation_check(c, NULL, operation, e) < 0) {
        json_object_put(c);
        return NULL;
    }
    int lock = open(bc_bench_lock_path(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        if (lock >= 0)
            close(lock);
        json_object_put(c);
        bc_fail(e, "Outra operação está usando a bancada.");
        return NULL;
    }
    if (bc_get(operation, "kind")) {
        json_object *definition = json_object_new_object();
        json_object_object_add(definition, "api", json_object_new_int(1));
        json_object_object_add(definition, "bus", bc_clone(bc_get(c, "bus")));
        json_object_object_add(definition, "max_transfer", json_object_new_int(4096));
        bc_lab *lab = bc_lab_new(c, definition, cancel, context, e);
        json_object *result = lab ? bc_lab_execute(lab, operation, e) : NULL;
        bc_lab_free(lab);
        json_object_put(definition);
        json_object_put(c);
        close(lock);
        return result;
    }
    bc_comm *s = NULL;
    int rc = bc_comm_open(&s, c, cancel, context, e);
    json_object_put(c);
    if (rc < 0) {
        close(lock);
        return NULL;
    }
    json_object *r = bc_comm_transaction(s, operation, e);
    bc_comm_close(s);
    close(lock);
    return r;
}
