#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <string.h>

/* CH347T M1 wire format, independently implemented from the WCH-published
 * interface: see docs/COMUNICACAO.md. No vendor library or kernel driver is linked. */
static int fail(bc_error *e, int code, const char *message) {
    errno = code;
    return bc_fail(e, "CH347T: %s", message);
}

static unsigned little16(const uint8_t *p) {
    return p[0] | ((unsigned)p[1] << 8);
}

static void put16(uint8_t *p, unsigned value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static int reply(bc_comm *s, uint8_t cmd, uint8_t *payload, size_t size, bc_error *e) {
    uint8_t header[3];
    if (bc_comm_read(s, header, sizeof header, e) < 0)
        return -1;
    if (header[0] != cmd || little16(header + 1) != size)
        return fail(e, EIO, "resposta USB com comando ou tamanho inesperado; encerre a sessão.");
    return bc_comm_read(s, payload, size, e);
}

static int request(bc_comm *s, uint8_t cmd, uint8_t value, uint8_t *payload, size_t size,
                   bc_error *e) {
    const uint8_t packet[] = {cmd, 1, 0, value};
    if (bc_comm_write(s, packet, sizeof packet, e) < 0)
        return -1;
    return reply(s, cmd, payload, size, e);
}

static int stream(bc_comm *s, uint8_t command, bc_error *e) {
    const uint8_t packet[] = {0xaa, command, 0};
    return bc_comm_write(s, packet, sizeof packet, e);
}

/* CS controls contain two five-byte records: enable/state, active delay LE16,
 * deactivation delay LE16. A zero enable leaves the other CS untouched. */
static int chip_select(bc_comm *s, unsigned cs, int active, bc_error *e) {
    uint8_t packet[13] = {0xc1, 10, 0};
    for (unsigned k = 0; k < 2; k++)
        if (k == cs || cs == 2)
            packet[3 + 5 * k] = (uint8_t)(active ? 0x80 : 0xc0);
    return bc_comm_write(s, packet, sizeof packet, e);
}

static int jtag_init(bc_comm *s, bc_error *e);

int bc_comm_ch347_init(bc_comm *s, bc_error *e) {
    if (!strcmp(bc_str(s->config, "bus"), "uart")) {
        unsigned baud = (unsigned)bc_num(s->config, "baud");
        uint8_t cfg[12] = {0,
                           0xcb,
                           8,
                           0,
                           (uint8_t)baud,
                           (uint8_t)(baud >> 8),
                           (uint8_t)(baud >> 16),
                           (uint8_t)(baud >> 24),
                           (uint8_t)(bc_num(s->config, "stop_bits") == 2 ? 2 : 0),
                           (uint8_t)(!strcmp(bc_str(s->config, "parity"), "odd")    ? 1
                                     : !strcmp(bc_str(s->config, "parity"), "even") ? 2
                                                                                    : 0),
                           8,
                           32};
        s->actual_hz = 0;
        return bc_comm_feature(s, cfg, sizeof cfg, 0, e);
    }
    if (!strcmp(bc_str(s->config, "bus"), "jtag"))
        return jtag_init(s, e);
    uint8_t info[4];
    if (request(s, 0xca, 0, info, sizeof info, e) < 0)
        return -1;
    unsigned firmware = little16(info);
    unsigned hz = (unsigned)bc_num(s->config, "frequency_hz");
    if (!strcmp(bc_str(s->config, "bus"), "i2c")) {
        unsigned speed = hz == 20000 ? 0 : hz == 100000 ? 1 : hz == 400000 ? 2 : 3;
        if (stream(s, (uint8_t)(0x60u | speed), e) < 0)
            return -1;
        if (firmware >= 0x0341 && stream(s, 0x16, e) < 0)
            return -1; /* Explicitly disable clock stretching where supported. */
        s->actual_hz = hz;
        return 0;
    }
    /* 60 MHz / 2^divider. Use the legacy base on old firmware, and explicitly
     * select that same base on firmware supporting the extended clock selector. */
    unsigned divider = 0, actual = 60000000;
    while (actual > hz && divider < 7) {
        actual /= 2;
        divider++;
    }
    if (actual > hz)
        return fail(e, EINVAL, "clock SPI abaixo de 468750 Hz.");
    if (firmware >= 0x0341) {
        uint8_t status;
        if (request(s, 0xe1, 4, &status, 1, e) < 0)
            return -1;
        if (status)
            return fail(e, EIO, "a configuração da base do clock SPI foi recusada.");
    }
    uint8_t cfg[26], packet[29] = {0xc0, 26, 0}, status;
    if (request(s, 0xca, 1, cfg, sizeof cfg, e) < 0)
        return -1;
    unsigned mode = (unsigned)bc_num(s->config, "spi_mode");
    put16(cfg, 0);          /* Full duplex, two wires. */
    put16(cfg + 2, 0x0104); /* Master. */
    put16(cfg + 4, 0);      /* 8 data bits. */
    put16(cfg + 6, mode & 2u);
    put16(cfg + 8, mode & 1u);
    put16(cfg + 10, 0x0200); /* Software NSS. */
    put16(cfg + 12, divider * 8u);
    put16(cfg + 14, json_object_get_boolean(bc_get(s->config, "lsb_first")) ? 0x80 : 0);
    put16(cfg + 16, 7); /* CRC polynomial, CRC not enabled. */
    put16(cfg + 18, 0); /* No extra inter-byte delay. */
    cfg[20] = 0xff;
    cfg[21] &= 0x3f; /* Both chip selects active low; preserve unrelated bits. */
    memcpy(packet + 3, cfg, sizeof cfg);
    if (bc_comm_write(s, packet, sizeof packet, e) < 0 || reply(s, 0xc0, &status, 1, e) < 0)
        return -1;
    if (status)
        return fail(e, EIO, "a inicialização SPI foi recusada.");
    s->actual_hz = actual;
    return chip_select(s, 2, 0, e);
}

/* END ends a command stream, not an I2C transaction; only 0x75 emits STOP.
 * Short streams avoid the CH347's 512-byte USB command boundary entirely. */
static int output(bc_comm *s, const uint8_t *data, size_t size, bc_error *e) {
    uint8_t packet[67] = {0xaa}, ack[63];
    while (size) {
        size_t n = size > 63 ? 63 : size;
        packet[1] = (uint8_t)(0x80u | n);
        memcpy(packet + 2, data, n);
        packet[n + 2] = 0;
        if (bc_comm_write(s, packet, n + 3, e) < 0 || bc_comm_read(s, ack, n, e) < 0)
            return -1;
        for (size_t k = 0; k < n; k++) {
            if (ack[k] == 0)
                return fail(e, ENXIO, "I²C recebeu NACK; o comando não será repetido.");
            if (ack[k] != 1)
                return fail(e, EIO, "resposta de ACK I²C inválida.");
        }
        data += n;
        size -= n;
    }
    return 0;
}

static int finish(bc_comm *s, int rc, int i2c, bc_error *e) {
    int saved = errno;
    if (!s->lost) {
        int (*cancel)(void *) = s->cancel;
        double deadline = s->deadline;
        bc_error stop = {{0}};
        s->cancel = NULL;
        s->deadline = bc_comm_now() + 0.1;
        int stopped = i2c ? stream(s, 0x75, &stop)
                          : chip_select(s, (unsigned)bc_num(s->config, "chip_select"), 0, &stop);
        s->cancel = cancel;
        s->deadline = deadline;
        if (stopped < 0 && !rc) {
            *e = stop;
            return -1;
        }
    }
    if (rc < 0 && saved != ENXIO && saved != ECANCELED)
        s->lost = saved ? saved : EIO;
    errno = saved;
    return rc;
}

int bc_comm_ch347_i2c(bc_comm *s, bc_comm_i2c_message *messages, unsigned count, bc_error *e) {
    for (unsigned i = 0; i < count; i++) {
        bc_comm_i2c_message *m = messages + i;
        int reading = (m->flags & BC_COMM_I2C_READ) != 0;
        uint8_t address = (uint8_t)(((unsigned)m->address << 1) | (reading ? 1u : 0u));
        if (stream(s, 0x74, e) < 0 || output(s, &address, 1, e) < 0)
            return finish(s, -1, 1, e);
        if (!reading) {
            if (output(s, m->data, m->size, e) < 0)
                return finish(s, -1, 1, e);
        } else {
            size_t at = 0;
            while (at < m->size) {
                size_t remaining = m->size - at;
                size_t n = remaining == 1 ? 1 : remaining - 1 > 63 ? 63 : remaining - 1;
                uint8_t cmd = remaining == 1 ? 0xc0 : (uint8_t)(0xc0u | n);
                if (stream(s, cmd, e) < 0 || bc_comm_read(s, m->data + at, n, e) < 0)
                    return finish(s, -1, 1, e);
                at += n;
            }
        }
    }
    return finish(s, 0, 1, e);
}

int bc_comm_ch347_spi(bc_comm *s, const uint8_t *tx, uint8_t *rx, size_t size, bc_error *e) {
    if (chip_select(s, (unsigned)bc_num(s->config, "chip_select"), 1, e) < 0)
        return -1;
    size_t at = 0;
    while (at < size) {
        size_t n = size - at;
        size_t maximum = s->ch347_hid ? 507 : 4093;
        if (n > maximum)
            n = maximum; /* Header + data <=4096; CS remains asserted across chunks. */
        uint8_t packet[4096];
        packet[0] = 0xc2;
        put16(packet + 1, (unsigned)n);
        memcpy(packet + 3, tx + at, n);
        if (bc_comm_write(s, packet, n + 3, e) < 0 || reply(s, 0xc2, rx + at, n, e) < 0)
            return finish(s, -1, 0, e);
        at += n;
    }
    return finish(s, 0, 0, e);
}

int bc_comm_ch347_mode(unsigned vid, unsigned pid) {
    if (vid != 0x1a86)
        return -1;
    return pid == 0x55d9 ? 0 : pid == 0x55db ? 1 : pid == 0x55dc ? 2 : pid == 0x55dd ? 3 : -1;
}

int bc_comm_ch347_supports(int mode, const char *bus) {
    return !strcmp(bus, "uart")   ? mode >= 0 && mode <= 3
           : !strcmp(bus, "i2c")  ? mode >= 1 && mode <= 3
           : !strcmp(bus, "spi")  ? mode == 1 || mode == 2
           : !strcmp(bus, "jtag") ? mode == 3
                                  : 0;
}

int bc_comm_ch347_open(bc_comm *s, int probe, bc_error *e) {
    json_object *list = bc_comm_list(s->cancel, s->cancel_context, e), *selected = NULL;
    if (!list)
        return -1;
    unsigned found = 0;
    const char *bus = bc_str(s->config, "bus");
    int requested = (int)bc_num(s->config, "ch347_mode");
    for (size_t k = 0; k < json_object_array_length(list); k++) {
        json_object *item = json_object_array_get_idx(list, k);
        if (!bc_comm_select(item, s->config) || !bc_get(item, "mode"))
            continue;
        int mode = (int)bc_num(item, "mode"), iface = (int)bc_num(item, "interface");
        if (!bc_comm_ch347_supports(mode, bus) || (requested != -1 && requested != mode))
            continue;
        if (mode == 2 && iface != (!strcmp(bus, "uart") ? 0 : 1))
            continue;
        if (mode != 2 && (!strcmp(bus, "uart") || strncmp(bc_str(item, "device"), "usb:", 4)))
            continue;
        found++;
        selected = item;
    }
    if (found != 1) {
        json_object_put(list);
        errno = found ? EEXIST : ENODEV;
        return bc_fail(
            e, found ? "Mais de uma interface CH347T corresponde. Escolha caminho/série e modo."
                     : "Interface CH347T compatível ausente: M0=UART; M1/M2=UART+I²C+SPI; "
                       "M3=UART+I²C+JTAG. Confira modo, caminho, permissão e driver; nenhum "
                       "substituto foi escolhido.");
    }
    int mode = (int)bc_num(selected, "mode");
    json_object_object_add(s->config, "device", json_object_get(bc_get(selected, "device")));
    json_object_object_add(s->config, "serial", json_object_get(bc_get(selected, "serial")));
    json_object_object_add(s->config, "ch347_mode", json_object_new_int(mode));
    s->ch347_hid = mode == 2;
    int rc = mode == 2 ? bc_comm_hid_open(s, e) : bc_comm_usb_open(s, probe, e);
    json_object_put(list);
    return rc;
}

int bc_comm_ch347_hid_encode(const uint8_t *data, size_t n, uint8_t packet[513], bc_error *e) {
    if (!data || n > 510 || !n)
        return fail(e, EINVAL, "payload HID deve ter 1…510 bytes.");
    memset(packet, 0, 513);
    put16(packet + 1, (unsigned)n);
    memcpy(packet + 3, data, n);
    return 513; /* Report ID zero followed by LE16 length and a 510-byte payload. */
}

int bc_comm_ch347_hid_decode(const uint8_t *packet, size_t n, int windows, uint8_t data[510],
                             bc_error *e) {
    size_t offset = windows ? 1 : 0;
    if (n < offset + 2 || n > 512 + offset || (windows && packet[0]))
        return fail(e, EIO, "relatório HID sem cabeçalho válido.");
    unsigned size = little16(packet + offset);
    if (size > 510 || size > n - offset - 2)
        return fail(e, EIO, "comprimento HID inconsistente.");
    memcpy(data, packet + offset + 2, size);
    return (int)size;
}

/* Bit operations sample TDO on each rising edge and return one byte per edge,
 * bit zero is the sampled value. Packets remain below the legacy 512-byte limit. */
static int tms_clocks(bc_comm *s, unsigned sequence, unsigned count, bc_error *e) {
    uint8_t packet[3 + 2 * 32 + 1] = {0xd1};
    if (count > 32)
        return fail(e, EINVAL, "sequência TMS interna inválida.");
    put16(packet + 1, count * 2 + 1);
    for (unsigned k = 0; k < count; k++) {
        packet[3 + 2 * k] = (sequence & (1u << k)) ? 2 : 0;
        packet[4 + 2 * k] = (uint8_t)(packet[3 + 2 * k] | 1);
    }
    packet[3 + count * 2] = count && (sequence & (1u << (count - 1))) ? 2 : 0;
    return bc_comm_write(s, packet, 4 + 2 * count, e);
}

static int jtag_reset(bc_comm *s, bc_error *e) {
    return tms_clocks(s, 0x3f, 7, e); /* Six TMS=1 clocks -> Reset, then TMS=0 -> Idle. */
}

static int jtag_init(bc_comm *s, bc_error *e) {
    uint8_t packet[9] = {0xd0, 6, 0, 0, 9, 0, 0, 0, 0}, support;
    if (bc_comm_write(s, packet, sizeof packet, e) < 0 || reply(s, 0xd0, &support, 1, e) < 0)
        return -1;
    unsigned hz = (unsigned)bc_num(s->config, "frequency_hz");
    unsigned actual = 60000000, index = support ? 7 : 5;
    while (actual > hz && index) {
        actual /= 2;
        index--;
    }
    if (actual > hz)
        return fail(e, EINVAL, "clock JTAG solicitado indisponível no firmware.");
    packet[4] = (uint8_t)index;
    if (bc_comm_write(s, packet, sizeof packet, e) < 0 || reply(s, 0xd0, &support, 1, e) < 0)
        return -1;
    s->actual_hz = actual;
    return jtag_reset(s, e);
}

int bc_comm_ch347_jtag(bc_comm *s, const char *reg, const uint8_t *tx, uint8_t *rx, unsigned bits,
                       unsigned idle, bc_error *e) {
    if (bc_comm_check(s, e) < 0)
        return -1;
    int rc = 0;
    if (!strcmp(reg, "reset"))
        rc = jtag_reset(s, e);
    else if (strcmp(reg, "idle")) {
        /* All scans start/end in Run-Test/Idle; IR persists across following DR scans. */
        rc = tms_clocks(s, !strcmp(reg, "ir") ? 3 : 1, !strcmp(reg, "ir") ? 4 : 3, e);
        if (!rc)
            memset(rx, 0, (bits + 7) / 8);
        for (unsigned at = 0; !rc && at < bits;) {
            unsigned n = bits - at;
            if (n > 128)
                n = 128;
            uint8_t packet[260] = {0xd2}, samples[128];
            put16(packet + 1, 2 * n + 1);
            for (unsigned k = 0; k < n; k++) {
                unsigned bit = at + k;
                uint8_t value = (uint8_t)((tx[bit / 8] & (1u << (bit % 8))) ? 0x10 : 0);
                if (bit + 1 == bits)
                    value |= 2;
                packet[3 + 2 * k] = value;
                packet[4 + 2 * k] = (uint8_t)(value | 1);
            }
            packet[3 + 2 * n] = (uint8_t)(packet[3 + 2 * (n - 1)] & ~1u);
            if (bc_comm_write(s, packet, 4 + 2 * n, e) < 0 || reply(s, 0xd2, samples, n, e) < 0) {
                rc = -1;
                break;
            }
            for (unsigned k = 0; k < n; k++)
                if (samples[k] & 1)
                    rx[(at + k) / 8] |= (uint8_t)(1u << ((at + k) % 8));
            at += n;
        }
        if (!rc)
            rc = tms_clocks(s, 1, 2, e); /* Exit1 -> Update -> Idle. */
    }
    for (unsigned at = 0; !rc && at < idle;) {
        unsigned n = idle - at;
        if (n > 32)
            n = 32;
        rc = tms_clocks(s, 0, n, e);
        at += n;
    }
    if (rc < 0) {
        int saved = errno;
        if (!s->lost) {
            int (*cancel)(void *) = s->cancel;
            s->cancel = NULL;
            s->deadline = bc_comm_now() + 0.1;
            bc_error ignored = {{0}};
            jtag_reset(s, &ignored);
            s->cancel = cancel;
        }
        s->lost = saved ? saved : EIO;
        errno = saved;
    }
    return rc;
}
