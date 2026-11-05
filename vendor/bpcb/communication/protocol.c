#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fault(bc_error *e, int code, const char *text) {
    errno = code;
    return bc_fail(e, "%s", text);
}

double bc_comm_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

void bc_comm_deadline(bc_comm *s) {
    s->deadline = bc_comm_now() + bc_num(s->config, "timeout_ms") / 1000.0;
}

int bc_comm_check(bc_comm *s, bc_error *e) {
    if (!s)
        return fault(e, ENOTCONN, "Sessão de comunicação não está aberta.");
    if (s->lost)
        return fault(e, s->lost,
                     "Sessão interrompida; encerre e confira o dispositivo antes de abrir outra. "
                     "Não houve reconexão automática.");
    if (s->cancel && s->cancel(s->cancel_context))
        return fault(e, ECANCELED, "Comunicação cancelada.");
    if (bc_comm_now() >= s->deadline)
        return fault(e, ETIMEDOUT, "Tempo limite da transação excedido.");
    return 0;
}

unsigned bc_comm_remaining(bc_comm *s, bc_error *e) {
    if (bc_comm_check(s, e) < 0)
        return 0;
    double ms = (s->deadline - bc_comm_now()) * 1000;
    return ms < 1 ? 1 : (unsigned)ms;
}

int bc_comm_write(bc_comm *s, const uint8_t *data, size_t n, bc_error *e) {
    unsigned timeout = bc_comm_remaining(s, e);
    if (!timeout)
        return -1;
    int got = s->io.write(s->io.context, data, n, timeout, e);
    if (got < 0 || got != (int)n) {
        if (got >= 0)
            fault(e, EIO, "Escrita incompleta; a transação não será repetida.");
        if (errno != ENXIO && errno != ECANCELED)
            s->lost = errno ? errno : EIO;
        return -1;
    }
    return 0;
}

int bc_comm_read(bc_comm *s, uint8_t *data, size_t n, bc_error *e) {
    size_t done = 0;
    while (done < n) {
        unsigned timeout = bc_comm_remaining(s, e);
        if (!timeout) {
            s->lost = errno;
            return -1;
        }
        int got = s->io.read(s->io.context, data + done, n - done, timeout, e);
        if (got <= 0 || got > (int)(n - done)) {
            if (got == 0)
                fault(e, ETIMEDOUT, "Resposta incompleta ou ausente.");
            else if (got > 0)
                fault(e, EIO, "Resposta maior que o buffer.");
            s->lost = errno ? errno : EIO;
            return -1;
        }
        done += (size_t)got;
    }
    return 0;
}

int bc_comm_feature(bc_comm *s, uint8_t *data, size_t size, int get, bc_error *e) {
    unsigned t = bc_comm_remaining(s, e);
    if (!t)
        return -1;
    int n = s->io.feature(s->io.context, data, size, get, t, e);
    if (n != (int)size) {
        if (n >= 0)
            fault(e, EIO, "Relatório HID incompleto.");
        s->lost = errno ? errno : EIO;
        return -1;
    }
    return bc_comm_check(s, e);
}

static void pause_short(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

static int command(bc_comm *s, const uint8_t *p, size_t n, bc_error *e) {
    return bc_comm_write(s, p, n, e);
}

static int ch_command(bc_comm *s, uint8_t c, bc_error *e) {
    uint8_t p[] = {0xaa, c, 0};
    return command(s, p, sizeof p, e);
}

static int ch_byte(bc_comm *s, uint8_t b, bc_error *e) {
    uint8_t p[] = {0xaa, 0x80, b, 0}, ack;
    if (command(s, p, sizeof p, e) < 0 || bc_comm_read(s, &ack, 1, e) < 0)
        return -1;
    return ack & 0x80 ? fault(e, ENXIO, "I²C: dispositivo respondeu NACK.") : 0;
}

static int ft_gpio(bc_comm *s, uint8_t value, uint8_t direction, bc_error *e) {
    uint8_t p[] = {0x80, value, direction};
    return command(s, p, sizeof p, e);
}

/* Repeated GPIO commands bound START/STOP hold times even at the fastest
 * MPSSE command consumption (60 MB/s). 256 * 3 bytes gives >=12.8 us.
 * This is deliberately independent of host scheduling and USB packet splits. */
static int ft_lines(bc_comm *s, const uint8_t *states, size_t count, bc_error *e) {
    uint8_t p[4 * 256 * 3];
    size_t at = 0;
    for (size_t k = 0; k < count; k++)
        for (unsigned i = 0; i < 256; i++) {
            p[at++] = 0x80;
            p[at++] = states[k];
            p[at++] = 3;
        }
    return command(s, p, at, e);
}

static int ft_start(bc_comm *s, bc_error *e) {
    const uint8_t states[] = {2, 3, 1, 0};
    return ft_lines(s, states, 4, e);
}

static int ft_stop(bc_comm *s, bc_error *e) {
    const uint8_t states[] = {0, 1, 3};
    return ft_lines(s, states, 3, e);
}

static int ft_byte(bc_comm *s, uint8_t value, bc_error *e) {
    uint8_t p[] = {0x80, 0, 3, 0x11, 0, 0, value, 0x80, 0, 1, 0x22, 0, 0x87}, ack;
    if (command(s, p, sizeof p, e) < 0 || bc_comm_read(s, &ack, 1, e) < 0)
        return -1;
    return ack & 0x01 ? fault(e, ENXIO, "I²C: dispositivo respondeu NACK.") : 0;
}

static int ft_read_byte(bc_comm *s, uint8_t *value, int last, bc_error *e) {
    uint8_t p[] = {0x80, 0, 1, 0x20, 0, 0, 0x87};
    if (command(s, p, sizeof p, e) < 0 || bc_comm_read(s, value, 1, e) < 0)
        return -1;
    uint8_t ack[] = {0x80, 0, 3, 0x13, 0, (uint8_t)(last ? 0xff : 0), 0x80, 0, 1};
    return command(s, ack, sizeof ack, e);
}

static int mcp_command(bc_comm *s, uint8_t p[64], uint8_t r[64], bc_error *e) {
    if (command(s, p, 64, e) < 0 || bc_comm_read(s, r, 64, e) < 0)
        return -1;
    if (r[0] != p[0])
        return fault(e, EIO, "MCP2221A: resposta não corresponde ao comando.");
    return 0;
}

static int mcp_status(bc_comm *s, uint8_t r[64], bc_error *e) {
    uint8_t p[64] = {0x10};
    if (mcp_command(s, p, r, e) < 0)
        return -1;
    if (r[1])
        return fault(e, EIO, "MCP2221A: falha ao consultar estado.");
    if (r[20] & 0x40)
        return fault(e, ENXIO, "MCP2221A: endereço/dado não confirmado (NACK).");
    if (r[8] == 0x25)
        return fault(e, ENXIO, "MCP2221A: NACK no endereço.");
    switch (r[8]) {
        case 0x12:
        case 0x23:
        case 0x44:
        case 0x62:
            return fault(e, ETIMEDOUT, "MCP2221A: timeout do barramento I²C.");
        default:
            return 0;
    }
}

static int mcp_wait(bc_comm *s, int no_stop, bc_error *e) {
    for (;;) {
        uint8_t r[64];
        if (bc_comm_check(s, e) < 0 || mcp_status(s, r, e) < 0)
            return -1;
        if (r[8] == (no_stop ? 0x45 : 0))
            return 0;
        pause_short();
    }
}

static int mcp_i2c(bc_comm *s, bc_comm_i2c_message *m, unsigned n, bc_error *e) {
    for (unsigned i = 0; i < n; i++) {
        uint8_t p[64] = {0}, r[64];
        int read = (m[i].flags & BC_COMM_I2C_READ) != 0;
        p[0] = (uint8_t)(read ? (i ? 0x93 : 0x91) : (n == 2 ? 0x94 : 0x90));
        p[1] = (uint8_t)m[i].size;
        p[2] = (uint8_t)(m[i].size >> 8);
        p[3] = (uint8_t)(((unsigned)m[i].address << 1) | (read ? 1u : 0u));
        if (!read)
            memcpy(p + 4, m[i].data, m[i].size);
        if (mcp_command(s, p, r, e) < 0)
            return -1;
        if (r[1])
            return fault(e, EBUSY, "MCP2221A: motor I²C ocupado; escrita não repetida.");
        if (!read) {
            if (mcp_wait(s, n == 2, e) < 0)
                return -1;
        } else {
            size_t at = 0;
            while (at < m[i].size) {
                memset(p, 0, sizeof p);
                p[0] = 0x40;
                if (bc_comm_check(s, e) < 0 || mcp_command(s, p, r, e) < 0)
                    return -1;
                if (r[1] || r[3] == 127 || r[3] > 60 || r[3] > m[i].size - at)
                    return fault(e, EIO, "MCP2221A: resposta I²C inválida/incompleta.");
                if (r[3]) {
                    memcpy(m[i].data + at, r + 4, r[3]);
                    at += r[3];
                } else {
                    if (mcp_status(s, r, e) < 0)
                        return -1;
                    pause_short();
                }
            }
            if (mcp_wait(s, 0, e) < 0)
                return -1;
        }
    }
    return 0;
}

static int cp_read_report(bc_comm *s, uint8_t r[64], uint8_t expected, bc_error *e) {
    if (bc_comm_read(s, r, 64, e) < 0)
        return -1;
    return r[0] == expected ? 0 : fault(e, EIO, "CP2112: identificador de resposta inesperado.");
}

static int cp_status(bc_comm *s, size_t expected, bc_error *e) {
    for (;;) {
        uint8_t p[64] = {0x15, 1}, r[64];
        if (bc_comm_check(s, e) < 0 || command(s, p, 2, e) < 0 || cp_read_report(s, r, 0x16, e) < 0)
            return -1;
        if (r[1] == 2) {
            if (r[2] != 5)
                return fault(e, EIO, "CP2112: conclusão sem confirmação de sucesso.");
            if (expected && (((unsigned)r[5] << 8) | r[6]) != expected)
                return fault(e, EIO, "CP2112: quantidade recebida difere da solicitada.");
            return 0;
        }
        if (r[1] == 3)
            return fault(e,
                         r[2] == 0   ? ENXIO
                         : r[2] == 1 ? ETIMEDOUT
                                     : EIO,
                         r[2] == 0   ? "CP2112: NACK no endereço."
                         : r[2] == 1 ? "CP2112: SCL preso/timeout."
                                     : "CP2112: arbitragem perdida ou transferência incompleta.");
        if (r[1] == 0 && (r[2] & 0xc0))
            return fault(e, EBUSY, "CP2112: SCL/SDA presos desde a inicialização.");
        if (r[1] > 3)
            return fault(e, EIO, "CP2112: estado de transferência inválido.");
        pause_short();
    }
}

static int cp_i2c(bc_comm *s, bc_comm_i2c_message *m, unsigned n, bc_error *e) {
    uint8_t p[64] = {0}, r[64];
    bc_comm_i2c_message *read = NULL;
    if (n == 2) {
        read = m + 1;
        p[0] = 0x11;
        p[1] = (uint8_t)(m[0].address << 1);
        p[2] = (uint8_t)(read->size >> 8);
        p[3] = (uint8_t)read->size;
        p[4] = (uint8_t)m[0].size;
        memcpy(p + 5, m[0].data, m[0].size);
    } else if (m[0].flags & BC_COMM_I2C_READ) {
        read = m;
        p[0] = 0x10;
        p[1] = (uint8_t)(m[0].address << 1);
        p[2] = (uint8_t)(m[0].size >> 8);
        p[3] = (uint8_t)m[0].size;
    } else {
        p[0] = 0x14;
        p[1] = (uint8_t)(m[0].address << 1);
        p[2] = (uint8_t)m[0].size;
        memcpy(p + 3, m[0].data, m[0].size);
    }
    size_t request_size = n == 2 ? 21 : read ? 4 : m[0].size + 3;
    if (command(s, p, request_size, e) < 0 || cp_status(s, read ? read->size : 0, e) < 0)
        return -1;
    if (read) {
        size_t at = 0;
        while (at < read->size) {
            size_t want = read->size - at;
            if (want > 61)
                want = 61;
            memset(p, 0, sizeof p);
            p[0] = 0x12;
            p[1] = (uint8_t)(want >> 8);
            p[2] = (uint8_t)want;
            if (command(s, p, 3, e) < 0 || cp_read_report(s, r, 0x13, e) < 0)
                return -1;
            if (r[1] == 3 || r[1] > 3 || r[2] == 0 || r[2] > want)
                return fault(e, EIO, "CP2112: leitura incompleta/inválida.");
            memcpy(read->data + at, r + 3, r[2]);
            at += r[2];
        }
    }
    return 0;
}

int bc_comm_protocol_init(bc_comm *s, bc_error *e) {
    if (s->kind == BC_CH347)
        return bc_comm_ch347_init(s, e);
    bc_comm_deadline(s);
    unsigned hz = (unsigned)bc_num(s->config, "frequency_hz");
    s->actual_hz = hz;
    if (s->kind == BC_SERIAL) {
        s->actual_hz = 0;
        return 0;
    }
    if (s->kind == BC_CH341)
        return ch_command(s, (uint8_t)(0x60u | (hz == 20000 ? 0u : hz == 100000 ? 1u : 2u)), e);
    if (s->kind == BC_FT232H) {
        if (!s->io.control)
            return fault(e, ENOSYS, "FT232H: controle USB indisponível.");
        /* SIO reset/purge, low latency and MPSSE mode. No EEPROM writes. */
        const unsigned requests[][3] = {{0, 0, 0}, {0, 1, 0},  {0, 2, 0},
                                        {9, 2, 0}, {11, 0, 0}, {11, 0x0200, 0}};
        for (unsigned i = 0; i < sizeof requests / sizeof *requests; i++)
            if (s->io.control(s->io.context, requests[i][0], requests[i][1], requests[i][2], e) < 0)
                return -1;
        uint8_t sync[] = {0xaa, 0x87}, answer[2];
        if (command(s, sync, sizeof sync, e) < 0 || bc_comm_read(s, answer, 2, e) < 0)
            return -1;
        if (answer[0] != 0xfa || answer[1] != 0xaa)
            return fault(e, EIO, "FT232H: sincronização MPSSE falhou.");
        int i2c = !strcmp(bc_str(s->config, "bus"), "i2c");
        unsigned base = i2c ? 20000000u : 30000000u, divider = (base + hz - 1) / hz;
        if (!divider)
            divider = 1;
        if (divider > 65536)
            return fault(e, EINVAL, "Frequência MPSSE fora da faixa.");
        s->actual_hz = base / divider;
        --divider;
        uint8_t p[] = {0x8a,
                       0x85,
                       0x97,
                       (uint8_t)(i2c ? 0x8c : 0x8d),
                       0x9e,
                       (uint8_t)(i2c ? 3 : 0),
                       0,
                       0x86,
                       (uint8_t)divider,
                       (uint8_t)(divider >> 8)};
        if (command(s, p, sizeof p, e) < 0)
            return -1;
        if (i2c)
            return ft_gpio(s, 3, 3, e);
        unsigned cs = (unsigned)bc_num(s->config, "chip_select");
        uint8_t idle =
            (uint8_t)((1u << cs) | (((unsigned)bc_num(s->config, "spi_mode") & 2u) ? 1u : 0u));
        return ft_gpio(s, idle, (uint8_t)(3u | (1u << cs)), e);
    }
    if (s->kind == BC_MCP2221) {
        uint8_t p[64] = {0x10, 0, 0, 0x20}, r[64];
        p[4] = (uint8_t)((12000000u + hz - 1u) / hz - 2u);
        s->actual_hz = 12000000u / ((unsigned)p[4] + 2u);
        if (mcp_command(s, p, r, e) < 0)
            return -1;
        return r[1] == 0 && r[3] == 0x20
                   ? 0
                   : fault(e, EBUSY,
                           "MCP2221A: não foi possível configurar o clock; motor ocupado.");
    }
    if (s->kind == BC_CP2112) {
        uint8_t version[3] = {5};
        if (bc_comm_feature(s, version, sizeof version, 1, e) < 0)
            return -1;
        if (version[1] != 0x0c)
            return fault(e, ENODEV, "O dispositivo HID não confirmou ser um CP2112.");
        unsigned timeout = (unsigned)bc_num(s->config, "timeout_ms");
        if (timeout > 1000)
            timeout = 1000;
        uint8_t p[14] = {6,
                         (uint8_t)(hz >> 24),
                         (uint8_t)(hz >> 16),
                         (uint8_t)(hz >> 8),
                         (uint8_t)hz,
                         2,
                         0,
                         (uint8_t)(timeout >> 8),
                         (uint8_t)timeout,
                         (uint8_t)(timeout >> 8),
                         (uint8_t)timeout,
                         1,
                         0,
                         1},
                r[14] = {6};
        if (bc_comm_feature(s, p, sizeof p, 0, e) < 0 || bc_comm_feature(s, r, sizeof r, 1, e) < 0)
            return -1;
        if (memcmp(p, r, sizeof p))
            return fault(e, EIO, "CP2112: configuração SMBus não confirmada.");
        return 0;
    }
    return fault(e, ENOSYS, "Driver de comunicação indisponível.");
}

int bc_comm_protocol_i2c(bc_comm *s, bc_comm_i2c_message *m, unsigned n, bc_error *e) {
    if (s->kind == BC_CH347)
        return bc_comm_ch347_i2c(s, m, n, e);
    int rc = -1, started = 0;
    if (s->kind == BC_MCP2221)
        return mcp_i2c(s, m, n, e);
    if (s->kind == BC_CP2112)
        return cp_i2c(s, m, n, e);
    for (unsigned i = 0; i < n; i++) {
        started = 1;
        if ((s->kind == BC_CH341 ? ch_command(s, 0x74, e) : ft_start(s, e)) < 0)
            goto end;
        int reading = (m[i].flags & BC_COMM_I2C_READ) != 0;
        uint8_t address = (uint8_t)(((unsigned)m[i].address << 1) | (reading ? 1u : 0u));
        if ((s->kind == BC_CH341 ? ch_byte(s, address, e) : ft_byte(s, address, e)) < 0)
            goto end;
        for (size_t k = 0; k < m[i].size; k++) {
            if (!reading) {
                if ((s->kind == BC_CH341 ? ch_byte(s, m[i].data[k], e)
                                         : ft_byte(s, m[i].data[k], e)) < 0)
                    goto end;
            } else if (s->kind == BC_CH341) {
                uint8_t p[] = {0xaa, (uint8_t)(k + 1 == m[i].size ? 0xc0 : 0xc1), 0};
                if (command(s, p, sizeof p, e) < 0 || bc_comm_read(s, m[i].data + k, 1, e) < 0)
                    goto end;
            } else if (ft_read_byte(s, m[i].data + k, k + 1 == m[i].size, e) < 0)
                goto end;
        }
    }
    rc = 0;
end:
    if (started && !s->lost) {
        int saved = errno;
        bc_error stop = {{0}}; /* A STOP remains bounded even after an operator cancellation. */
        int (*cancel)(void *) = s->cancel;
        s->cancel = NULL;
        s->deadline = bc_comm_now() + 0.1;
        int stopped = s->kind == BC_CH341 ? ch_command(s, 0x75, &stop) : ft_stop(s, &stop);
        s->cancel = cancel;
        if (stopped < 0 && rc == 0) {
            *e = stop;
            return -1;
        }
        errno = saved;
    }
    return rc;
}

int bc_comm_protocol_spi(bc_comm *s, const uint8_t *tx, uint8_t *rx, size_t n, bc_error *e) {
    if (s->kind == BC_CH347)
        return bc_comm_ch347_spi(s, tx, rx, n, e);
    unsigned mode = (unsigned)bc_num(s->config, "spi_mode"),
             cs = (unsigned)bc_num(s->config, "chip_select");
    uint8_t idle = (uint8_t)((1u << cs) | ((mode & 2u) ? 1u : 0u)),
            direction = (uint8_t)(3u | (1u << cs));
    if (ft_gpio(s, (uint8_t)(idle & ~(1u << cs)), direction, e) < 0)
        return -1;
    uint8_t p[BC_COMM_MAX + 4];
    p[0] = (uint8_t)(0x30u | ((mode == 0 || mode == 3) ? 1u : 4u) |
                     (json_object_get_boolean(bc_get(s->config, "lsb_first")) ? 8u : 0u));
    p[1] = (uint8_t)(n - 1);
    p[2] = (uint8_t)((n - 1) >> 8);
    memcpy(p + 3, tx, n);
    p[n + 3] = 0x87;
    int rc = command(s, p, n + 4, e);
    if (!rc)
        rc = bc_comm_read(s, rx, n, e);
    if (!s->lost) {
        bc_error stop = {{0}};
        int saved = errno;
        int (*cancel)(void *) = s->cancel;
        s->cancel = NULL;
        s->deadline = bc_comm_now() + 0.1;
        int stopped = ft_gpio(s, idle, direction, &stop);
        s->cancel = cancel;
        if (stopped < 0 && !rc) {
            *e = stop;
            return -1;
        }
        errno = saved;
    }
    return rc;
}
