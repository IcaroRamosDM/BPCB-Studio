#include "ads7924_c.h"
#include "studio.h"
#ifndef _WIN32
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <unistd.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct bs_io {
    bs_host *host;
    bc_comm *session;
    bc_lab *lab;
    json_object *config, *definition;
};

bs_io *bs_io_new(bs_host *host) {
    if (!host)
        return NULL;
    bs_io *io = calloc(1, sizeof *io);
    if (io)
        io->host = host;
    return io;
}

void bs_io_free(bs_io *io) {
    if (io) {
        bc_comm_close(io->session);
        bc_lab_free(io->lab);
        json_object_put(io->config);
        json_object_put(io->definition);
        free(io);
    }
}

static int io_cancelled(void *context) {
    bs_io *io = context;
    return io->host->stopped && io->host->stopped(io->host->context);
}

int bs_io_communication_setup(bs_io *io, json_object *config, json_object *definition,
                              bc_error *e) {
    if (!io || io->host->simulation)
        return bc_fail(e, "Simulação não abre comunicação.");
    json_object *test = json_object_new_object();
    if (definition)
        json_object_object_add(test, "communication", bc_clone(definition));
    json_object *c = bc_comm_resolve(NULL, test, config, e);
    json_object_put(test);
    if (!c)
        return -1;
    bc_comm_close(io->session);
    io->session = NULL;
    json_object_put(io->config);
    json_object_put(io->definition);
    io->config = c;
    io->definition = bc_clone(definition);
    bc_lab_free(io->lab);
    io->lab = bc_lab_new(c, definition, io_cancelled, io, e);
    return io->lab ? 0 : -1;
}

json_object *bs_io_communicate(void *context, json_object *operation, bc_error *e) {
    bs_io *io = context;
    if (!io || !io->lab)
        return bc_fail(e, "Comunicação não foi preparada."), NULL;
    json_object *r = bc_lab_execute(io->lab, operation, e);
    if (r && bc_get(r, "transport")) {
        json_object *event = json_object_new_object();
        json_object_object_add(event, "type", json_object_new_string("status"));
        json_object_object_add(
            event, "message",
            json_object_new_string("Operação concluída no canal/dispositivo selecionado."));
        json_object_object_add(event, "transport", bc_clone(bc_get(r, "transport")));
        io->host->emit(io->host->context, event);
        json_object_put(event);
    }
    return r;
}

static int io_integer(json_object *o, const char *key, double min, double max) {
    json_object *v = bc_get(o, key);
    double n = json_object_get_double(v);
    return (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double)) &&
           isfinite(n) && n >= min && n <= max && floor(n) == n;
}

static int io_signal(json_object *d, json_object *signal, bc_error *e) {
    if (!io_integer(d, "address", 8, 119) || !io_integer(signal, "channel", 0, 7))
        return bc_fail(e, "Endereço ou canal inválido.");
    const char *driver = bc_str(d, "driver");
    if (!strcmp(driver, "register") &&
        (!io_integer(signal, "register", 0, 255) || !io_integer(signal, "width", 1, 4) ||
         !io_integer(signal, "bits", 1, 32) || !io_integer(signal, "shift", 0, 31)))
        return bc_fail(e, "Formato inteiro de registrador inválido.");
    if ((!strcmp(driver, "ads7924") || !strcmp(driver, "mcp4728")) &&
        (!isfinite(bc_num(d, "vref")) || bc_num(d, "vref") < 0.1 || bc_num(d, "vref") > 5.5))
        return bc_fail(e, "Referência ADC/DAC inválida.");
    return 0;
}

static int transfer(bs_io *io, json_object *dev, uint8_t *w, unsigned nw, uint8_t *r, unsigned nr,
                    bc_error *e) {
    if (!io || !io->host || io->host->simulation)
        return bc_fail(e, "Simulação/contexto inválido: acesso I²C recusado.");
    if (!io_integer(dev, "address", 8, 119))
        return bc_fail(e, "Endereço I²C inválido.");
    unsigned count = (nw ? 1u : 0u) + (nr ? 1u : 0u);
    unsigned addr = (unsigned)bc_num(dev, "address");
    if (addr < 8 || addr > 119 || nw > 32 || nr > 32 || (!nw && !nr))
        return bc_fail(e, "Transferência I²C inválida.");
#ifndef _WIN32
    unsigned index = 0;
    struct i2c_msg msg[2];
    if (nw)
        msg[index++] =
            (struct i2c_msg){.addr = (uint16_t)addr, .flags = 0, .len = (uint16_t)nw, .buf = w};
    if (nr)
        msg[index++] = (struct i2c_msg){
            .addr = (uint16_t)addr, .flags = I2C_M_RD, .len = (uint16_t)nr, .buf = r};
#endif
    int result;
    if (!strcmp(bc_str(dev, "transport"), "selected") ||
        !strcmp(bc_str(dev, "transport"), "ch341")) {
        json_object *op = json_object_new_object();
        char *hex = bc_comm_format_bytes(w, nw, 1);
        json_object_object_add(op, "write", json_object_new_string(hex));
        free(hex);
        json_object_object_add(op, "read", json_object_new_int((int)nr));
        json_object_object_add(op, "address", json_object_new_int((int)addr));
        if (bc_num(io->definition, "api") == 2) {
            json_object *wrapper = json_object_new_object();
            json_object_object_add(wrapper, "channel",
                                   json_object_new_string(*bc_str(dev, "comm_channel")
                                                              ? bc_str(dev, "comm_channel")
                                                              : "main"));
            json_object_object_add(wrapper, "operation", op);
            op = wrapper;
        }
        json_object *response = bs_io_communicate(io, op, e);
        json_object_put(op);
        if (!response)
            return -1;
        uint8_t bytes[32];
        size_t n = 0;
        int rc = bc_comm_hex(bc_str(response, "received"), bytes, sizeof bytes, &n, e);
        if (!rc && n == nr) {
            if (nr)
                memcpy(r, bytes, nr);
        } else
            rc = bc_fail(e, "Comprimento I²C divergente.");
        json_object_put(response);
        return rc;
    } else {
#ifdef _WIN32
        return bc_fail(e, "O adaptador /dev/i2c-N só existe no Linux. No Windows use USB CH341.");
#else
        const char *path = bc_str(dev, "path");
        if (strncmp(path, "/dev/i2c-", 9) || !path[9])
            return bc_fail(e, "Use o caminho exato /dev/i2c-N.");
        for (const char *c = path + 9; *c; c++)
            if (*c < '0' || *c > '9')
                return bc_fail(e, "Caminho I²C inválido.");
        int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            return bc_fail(e, "Abrir %s: %s", bc_str(dev, "path"), strerror(errno));
        struct stat st;
        if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode)) {
            close(fd);
            return bc_fail(e, "Adaptador I²C deve ser um dispositivo de caracteres.");
        }
        struct i2c_rdwr_ioctl_data data = {.msgs = msg, .nmsgs = count};
        result = ioctl(fd, I2C_RDWR, &data);
        int saved = errno;
        close(fd);
        errno = saved;
#endif
    }
    if (result != (int)count)
        return bc_fail(e, "I²C %s em 0x%02X: %s", bc_str(dev, "name"), addr,
                       strerror(errno ? errno : EIO));
    return 0;
}

static int regread(bs_io *io, json_object *d, unsigned reg, uint8_t *out, unsigned n, bc_error *e) {
    uint8_t r = (uint8_t)reg;
    return transfer(io, d, &r, 1, out, n, e);
}

static int regwrite(bs_io *io, json_object *d, unsigned reg, const uint8_t *bytes, unsigned n,
                    bc_error *e) {
    uint8_t b[33];
    if (n > 31)
        return bc_fail(e, "Registro muito longo.");
    b[0] = (uint8_t)reg;
    memcpy(b + 1, bytes, n);
    return transfer(io, d, b, n + 1, NULL, 0, e);
}

static int checked(bs_io *io, json_object *d, unsigned reg, uint8_t value, bc_error *e) {
    uint8_t observed = 0;
    if (regwrite(io, d, reg, &value, 1, e) < 0 || regread(io, d, reg, &observed, 1, e) < 0)
        return -1;
    return observed == value ? 0 : bc_fail(e, "Registro 0x%02X não confirmou a escrita.", reg);
}

typedef struct {
    bs_io *io;
    json_object *dev;
    bc_error *err;
} adcctx;

static int adcwrite(void *p, uint8_t reg, uint8_t val) {
    adcctx *a = p;
    return regwrite(a->io, a->dev, reg, &val, 1, a->err);
}

static int adcread(void *p, uint8_t reg, uint8_t *buf, unsigned n) {
    adcctx *a = p;
    return regread(a->io, a->dev, reg, buf, n, a->err);
}

static int adcwait(void *p, unsigned ms) {
    adcctx *a = p;
    return a->io->host->sleep_ms(a->io->host->context, ms);
}

static uint64_t decode(const uint8_t *b, unsigned n, int little) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v = (v << 8) | b[little ? n - i - 1 : i];
    return v;
}

int bs_io_read(void *context, json_object *d, json_object *s, double *value, bc_error *e) {
    if (io_signal(d, s, e) < 0)
        return -1;
    bs_io *io = context;
    const char *driver = bc_str(d, "driver");
    unsigned ch = (unsigned)bc_num(s, "channel");
    uint8_t b[32] = {0};
    if (!isfinite(bc_num(s, "channel")) || bc_num(s, "channel") < 0 || bc_num(s, "channel") > 7 ||
        floor(bc_num(s, "channel")) != bc_num(s, "channel"))
        return bc_fail(e, "Canal inválido.");
    if ((!strcmp(driver, "ads7924") || !strcmp(driver, "mcp4728")) && ch > 3)
        return bc_fail(e, "Canal deve ser 0..3.");
    if (!strcmp(driver, "tca9534a")) {
        if (regread(io, d, !strcmp(bc_str(s, "direction"), "output") ? 1 : 0, b, 1, e) < 0)
            return -1;
        *value = ((unsigned)b[0] >> ch) & 1u;
    } else if (!strcmp(driver, "mcp4728")) {
        if (transfer(io, d, NULL, 0, b, 24, e) < 0)
            return -1;
        unsigned off = ch * 6;
        unsigned raw = ((unsigned)(b[off + 1] & 15u) << 8) | b[off + 2];
        *value = (double)raw * bc_num(d, "vref") / 4096.;
    } else if (!strcmp(driver, "ads7924")) {
        unsigned raw[4];
        adcctx a = {io, d, e};
        ads_bus bus = {&a, adcwrite, adcread, adcwait};
        if (ads7924_read_all(&bus, raw) < 0) {
            if (!*e->text)
                bc_fail(e, "Falha ADS7924: %s", strerror(errno));
            return -1;
        }
        if (raw[ch] == 4095)
            return bc_fail(e, "ADS7924 no código máximo; leitura saturada.");
        *value = (double)raw[ch] * bc_num(d, "vref") / 4096.;
    } else if (!strcmp(driver, "register")) {
        unsigned n = (unsigned)bc_num(s, "width"), bits = (unsigned)bc_num(s, "bits"),
                 shift = (unsigned)bc_num(s, "shift");
        if (n < 1 || n > 4 || bits < 1 || bits > 32 || bits + shift > 8 * n)
            return bc_fail(e, "Formato do registro inválido.");
        if (regread(io, d, (unsigned)bc_num(s, "register"), b, n, e) < 0)
            return -1;
        uint64_t raw = (decode(b, n, !strcmp(bc_str(s, "endian"), "little")) >> shift) &
                       ((UINT64_C(1) << bits) - 1);
        *value = (double)raw * bc_num(s, "gain") + bc_num(s, "offset");
    } else
        return bc_fail(e, "Dispositivo não suportado.");
    return 0;
}

int bs_io_write(void *context, json_object *d, json_object *s, double v, bc_error *e) {
    if (!isfinite(v))
        return bc_fail(e, "Comando não finito.");
    if (io_signal(d, s, e) < 0)
        return -1;
    bs_io *io = context;
    const char *driver = bc_str(d, "driver");
    unsigned ch = (unsigned)bc_num(s, "channel");
    uint8_t b[32] = {0};
    if (!isfinite(bc_num(s, "channel")) || bc_num(s, "channel") < 0 || bc_num(s, "channel") > 7 ||
        floor(bc_num(s, "channel")) != bc_num(s, "channel"))
        return bc_fail(e, "Canal inválido.");
    if ((!strcmp(driver, "ads7924") || !strcmp(driver, "mcp4728")) && ch > 3)
        return bc_fail(e, "Canal deve ser 0..3.");
    if (!strcmp(driver, "tca9534a")) {
        if (v != 0 && v != 1)
            return bc_fail(e, "Saída digital exige 0 ou 1.");
        uint8_t config;
        if (regread(io, d, 3, &config, 1, e) < 0)
            return -1;
        if (config != (uint8_t)~(unsigned)bc_num(d, "output_mask"))
            return bc_fail(e, "Direções inesperadas: inclua a etapa Preparar dispositivo.");
        if (regread(io, d, 1, b, 1, e) < 0)
            return -1;
        b[0] = (uint8_t)((b[0] & ~(1u << ch)) | ((unsigned)v << ch));
        return checked(io, d, 1, b[0], e);
    } else if (!strcmp(driver, "mcp4728")) {
        double ref = bc_num(d, "vref");
        if (v < 0 || v > ref || ref <= 0 || ch > 3)
            return bc_fail(e, "Comando DAC inválido.");
        unsigned code = (unsigned)lround(v * 4096 / ref);
        if (code > 4095)
            code = 4095;
        b[0] = (uint8_t)(0x40u | (ch << 1));
        b[1] = (uint8_t)((code >> 8) & 15u);
        b[2] = (uint8_t)(code & 255u);
        if (transfer(io, d, b, 3, NULL, 0, e) < 0)
            return -1;
        /* Lê os registros voláteis; não grava EEPROM. */
        if (transfer(io, d, NULL, 0, b, 24, e) < 0)
            return -1;
        unsigned off = ch * 6;
        if (((unsigned)(b[off + 1] & 15u) << 8 | b[off + 2]) != code || (b[off + 1] & 0xf0u) != 0)
            return bc_fail(e, "DAC não confirmou código, VDD como referência e modo normal.");
        return 0;
    } else if (!strcmp(driver, "register")) {
        unsigned n = (unsigned)bc_num(s, "width"), bits = (unsigned)bc_num(s, "bits"),
                 shift = (unsigned)bc_num(s, "shift");
        double gain = bc_num(s, "gain");
        if (n < 1 || n > 4 || bits < 1 || bits > 32 || bits + shift > 8 * n || gain <= 0)
            return bc_fail(e, "Formato do registro inválido.");
        uint64_t mask = (UINT64_C(1) << bits) - 1;
        double raw = (v - bc_num(s, "offset")) / gain;
        if (!isfinite(raw) || raw < 0 || raw > (double)mask)
            return bc_fail(e, "Valor não representável no registro.");
        int little = !strcmp(bc_str(s, "endian"), "little");
        if (regread(io, d, (unsigned)bc_num(s, "register"), b, n, e) < 0)
            return -1;
        uint64_t full = decode(b, n, little);
        full = (full & ~(mask << shift)) | ((uint64_t)llround(raw) << shift);
        for (unsigned i = 0; i < n; i++)
            b[little ? i : n - 1 - i] = (uint8_t)(full >> (8 * i));
        if (regwrite(io, d, (unsigned)bc_num(s, "register"), b, n, e) < 0)
            return -1;
        if (regread(io, d, (unsigned)bc_num(s, "register"), b, n, e) < 0)
            return -1;
        if (decode(b, n, little) != full)
            return bc_fail(e, "Registro não confirmou a escrita.");
        return 0;
    } else
        return bc_fail(e, "Este dispositivo não permite saída.");
}

int bs_io_init(void *context, json_object *d, json_object *project, bc_error *e) {
    bs_io *io = context;
    const char *driver = bc_str(d, "driver");
    if (!strcmp(driver, "tca9534a") &&
        (!io_integer(d, "output_mask", 0, 255) || !io_integer(d, "initial_output", 0, 255)))
        return bc_fail(e, "Configuração TCA inválida.");
    if (!strcmp(driver, "tca9534a")) {
        uint8_t conf = (uint8_t)~(unsigned)bc_num(d, "output_mask"),
                out = (uint8_t)bc_num(d, "initial_output");
        /* Primeiro o latch, depois a direção. Mantém os níveis de encerramento cadastrados. */
        if (checked(io, d, 1, out, e) < 0 || checked(io, d, 2, 0, e) < 0 ||
            checked(io, d, 3, conf, e) < 0)
            return -1;
        return 0;
    }
    if (!strcmp(driver, "mcp4728")) {
        json_object *ss = bc_get(project, "signals");
        for (size_t i = 0; i < json_object_array_length(ss); i++) {
            json_object *s = json_object_array_get_idx(ss, i);
            if (!strcmp(bc_str(s, "device"), bc_str(d, "id")) &&
                !strcmp(bc_str(s, "direction"), "output"))
                if (bs_io_write(context, d, s, bc_num(s, "safe"), e) < 0)
                    return -1;
        }
        return 0;
    }
    return 0;
}
