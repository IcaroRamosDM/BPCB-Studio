#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
static int integer(json_object *o, const char *k, int64_t lo, int64_t hi) {
    json_object *v = bc_get(o, k);
    return json_object_is_type(v, json_type_int) && json_object_get_int64(v) >= lo &&
           json_object_get_int64(v) <= hi;
}

static int profile_name(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 80 || !bc_utf8((const unsigned char *)s, n) || s[0] == ' ' || s[n - 1] == ' ')
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32 || strchr("/\\", s[i]))
            return 0;
    return 1;
}

json_object *bc_comm_profiles(const char *root, bc_error *e) {
    char path[4096];
    if (bc_path(path, sizeof path, root, "communication-profiles.json", e) < 0)
        return NULL;
    struct stat st;
    if (lstat(path, &st) < 0 && errno == ENOENT)
        return json_object_new_array();
    json_object *a = bc_read_json(path, e);
    if (!a)
        return NULL;
    if (bc_comm_profiles_validate(a, e) < 0) {
        json_object_put(a);
        return NULL;
    }
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *row = json_object_array_get_idx(a, i),
                    *c = bc_comm_config(bc_get(row, "config"), e);
        json_object_object_add(c, "connections_checked", json_object_new_boolean(0));
        json_object_object_add(c, "allow_kernel_detach", json_object_new_boolean(0));
        json_object_object_add(row, "config", c);
    }
    return a;
}

int bc_comm_profiles_validate(json_object *a, bc_error *e) {
    if (!json_object_is_type(a, json_type_array) || json_object_array_length(a) > 64) {
        bc_fail(e, "Lista de perfis inválida (máximo 64).");
        return -1;
    }
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *row = json_object_array_get_idx(a, i);
        if (!profile_name(bc_str(row, "name"))) {
            bc_fail(e, "Nome de perfil inválido.");
            return -1;
        }
        for (size_t j = 0; j < i; j++)
            if (!strcmp(bc_str(row, "name"), bc_str(json_object_array_get_idx(a, j), "name"))) {
                bc_fail(e, "Nomes de perfis repetidos.");
                return -1;
            }
        if (!json_object_is_type(bc_get(row, "config"), json_type_object) ||
            !json_object_is_type(bc_get(row, "name"), json_type_string) ||
            (size_t)json_object_get_string_len(bc_get(row, "name")) !=
                strlen(bc_str(row, "name"))) {
            bc_fail(e, "Perfil incompleto ou com NUL embutido.");
            return -1;
        }
        json_object *c = bc_comm_config(bc_get(row, "config"), e);
        if (!c)
            return -1;
        json_object_put(c);
    }
    return 0;
}

static int profile_change(const char *root, const char *name, json_object *config, int replace,
                          int remove, bc_error *e) {
    if (!profile_name(name))
        return bc_fail(e, "Use um nome de perfil com 1..80 bytes, sem barras ou controles.");
    json_object *c = remove ? NULL : bc_comm_config(config, e);
    if (!remove && !c)
        return -1;
    char lockpath[4096], path[4096];
    int fd = -1, rc = -1;
    json_object *a = NULL;
    if (bc_path(path, sizeof path, root, "communication-profiles.json", e) < 0 ||
        bc_path(lockpath, sizeof lockpath, root, "storage.lock", e) < 0)
        goto done;
    fd = open(lockpath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0) {
        bc_fail(e, "Armazenamento ocupado ou indisponível.");
        goto done;
    }
    a = bc_comm_profiles(root, e);
    if (!a)
        goto done;
    size_t at = json_object_array_length(a);
    for (size_t i = 0; i < json_object_array_length(a); i++)
        if (!strcmp(name, bc_str(json_object_array_get_idx(a, i), "name")))
            at = i;
    if (remove) {
        if (at == json_object_array_length(a)) {
            bc_fail(e, "Perfil não encontrado.");
            goto done;
        }
        json_object_array_del_idx(a, at, 1);
    } else {
        if (at < json_object_array_length(a) && !replace) {
            bc_fail(e, "Este nome já existe. Confirme a substituição.");
            goto done;
        }
        if (at == json_object_array_length(a) && at >= 64) {
            bc_fail(e, "Limite de 64 perfis atingido.");
            goto done;
        }
        json_object_object_add(c, "connections_checked", json_object_new_boolean(0));
        json_object_object_add(c, "allow_kernel_detach", json_object_new_boolean(0));
        json_object *row = json_object_new_object();
        json_object_object_add(row, "name", json_object_new_string(name));
        json_object_object_add(row, "config", c);
        c = NULL;
        json_object_array_put_idx(a, at, row);
    }
    rc = bc_write_json(path, a, e);
done:
    if (fd >= 0)
        close(fd);
    json_object_put(c);
    json_object_put(a);
    return rc;
}

int bc_comm_profile_save(const char *root, const char *name, json_object *c, int replace,
                         bc_error *e) {
    return profile_change(root, name, c, replace, 0, e);
}

int bc_comm_profile_delete(const char *root, const char *name, bc_error *e) {
    return profile_change(root, name, NULL, 0, 1, e);
}

char *bc_comm_format_bytes(const uint8_t *b, size_t n, int hex) {
    if (n > BC_COMM_MAX || (!b && n))
        return NULL;
    char *s = malloc(n * 4 + 1);
    if (!s)
        return NULL;
    size_t at = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned v = b[i];
        if (hex)
            at += (size_t)sprintf(s + at, i + 1 < n ? "%02X " : "%02X", v);
        else if (v >= 32 && v <= 126 && v != '\\')
            s[at++] = (char)v;
        else if (v == '\n' || v == '\r' || v == '\t' || v == '\\') {
            s[at++] = '\\';
            s[at++] = v == '\n' ? 'n' : v == '\r' ? 'r' : v == '\t' ? 't' : '\\';
        } else
            at += (size_t)sprintf(s + at, "\\x%02X", v);
    }
    s[at] = 0;
    return s;
}

json_object *bc_comm_text_operation(const char *text, int hex, const char *delimiter, unsigned read,
                                    bc_error *e) {
    uint8_t b[BC_COMM_MAX];
    size_t n = 0;
    if (!text || !delimiter || read > BC_COMM_MAX) {
        bc_fail(e, "Terminal: tamanho inválido.");
        return NULL;
    }
    if (hex) {
        if (bc_comm_hex(text, b, sizeof b, &n, e) < 0)
            return NULL;
    } else {
        n = strlen(text);
        if (n > sizeof b) {
            bc_fail(e, "Terminal: máximo 4096 bytes.");
            return NULL;
        }
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)text[i] > 127) {
                bc_fail(e, "ASCII aceita bytes 0..127; use hexadecimal para outros bytes.");
                return NULL;
            }
        memcpy(b, text, n);
    }
    const char *d = !strcmp(delimiter, "none")   ? ""
                    : !strcmp(delimiter, "lf")   ? "\n"
                    : !strcmp(delimiter, "cr")   ? "\r"
                    : !strcmp(delimiter, "crlf") ? "\r\n"
                                                 : NULL;
    if (!d || n + strlen(d) > sizeof b || (!n && !read)) {
        bc_fail(e, "Terminal: delimitador ou quantidade inválida.");
        return NULL;
    }
    if (n) {
        memcpy(b + n, d, strlen(d));
        n += strlen(d);
    }
    char *s = bc_comm_format_bytes(b, n, 1);
    if (!s)
        return NULL;
    json_object *o = json_object_new_object();
    json_object_object_add(o, "write", json_object_new_string(s));
    free(s);
    json_object_object_add(o, "read", json_object_new_int64(read));
    return o;
}

int bc_comm_uart_unread(bc_comm *s, const uint8_t *data, size_t size, bc_error *e) {
    if (!s || strcmp(bc_str(s->config, "bus"), "uart") || !data ||
        size > BC_COMM_MAX - s->uart_pending_size)
        return bc_fail(e, "Fila UART excedida.");
    memmove(s->uart_pending + size, s->uart_pending, s->uart_pending_size);
    memcpy(s->uart_pending, data, size);
    s->uart_pending_size += size;
    return 0;
}

int bc_comm_uart_poll(bc_comm *s, uint8_t *data, size_t n, unsigned wait, bc_error *e) {
    if (!s || strcmp(bc_str(s->config, "bus"), "uart") || !data || !n || n > BC_COMM_MAX ||
        wait < 10 || wait > 1000)
        return bc_fail(e, "Leitura de terminal inválida.");
    if (s->uart_pending_size) {
        bc_comm_deadline(s);
        if (bc_comm_check(s, e) < 0)
            return -1;
        size_t got = s->uart_pending_size < n ? s->uart_pending_size : n;
        memcpy(data, s->uart_pending, got);
        s->uart_pending_size -= got;
        memmove(s->uart_pending, s->uart_pending + got, s->uart_pending_size);
        return (int)got;
    }
    s->deadline = bc_comm_now() + (double)wait / 1000.;
    if (bc_comm_check(s, e) < 0)
        return -1;
    errno = 0;
    int got = s->io.read(s->io.context, data, n, wait, e);
    if (got < 0 && errno == ETIMEDOUT && !(s->cancel && s->cancel(s->cancel_context))) {
        e->text[0] = 0;
        return 0;
    }
    if (got < 0 || got > (int)n) {
        s->lost = errno ? errno : EIO;
        return -1;
    }
    return got;
}

json_object *bc_comm_scan(bc_comm **slot, unsigned first, unsigned last,
                          int (*progress)(void *, json_object *), void *ctx, bc_error *e) {
    bc_comm *s = slot ? *slot : NULL;
    if (!s || strcmp(bc_str(s->config, "bus"), "i2c") || first < 8 || last > 119 || first > last) {
        bc_fail(e, "Varredura: use um intervalo I²C entre 0x08 e 0x77.");
        return NULL;
    }
    json_object *a = json_object_new_array();
    double scan_deadline = bc_comm_now() + 120;
    for (unsigned address = first; address <= last; address++) {
        if (bc_comm_now() > scan_deadline) {
            bc_fail(e, "Varredura excedeu o limite global de 120 segundos.");
            goto bad;
        }
        uint8_t byte;
        bc_comm_i2c_message msg = {(uint16_t)address, BC_COMM_I2C_READ, 1, &byte};
        errno = 0;
        int rc = bc_comm_i2c(s, &msg, 1, e);
        if (rc < 0 && errno != ENXIO)
            goto bad;
        json_object *r = json_object_new_object();
        json_object_object_add(r, "address", json_object_new_int((int)address));
        json_object_object_add(r, "ack", json_object_new_boolean(rc == 0));
        json_object_object_add(r, "method", json_object_new_string("read-one-byte"));
        if (!rc)
            json_object_object_add(r, "value", json_object_new_int(byte));
        json_object_array_add(a, r);
        if (progress && progress(ctx, r) < 0) {
            bc_fail(e, "Varredura cancelada.");
            goto bad;
        }
        /* HID engines are closed on NACK: do not retry/reopen a disconnected session. */
        if (rc < 0 && s->lost && address < last) {
            json_object *c = bc_clone(s->config);
            int (*cancel)(void *) = s->cancel;
            void *context = s->cancel_context;
            bc_comm_close(s);
            *slot = NULL;
            int opened = bc_comm_open(slot, c, cancel, context, e);
            json_object_put(c);
            s = *slot;
            if (opened < 0)
                goto bad;
        }
        e->text[0] = 0;
    }
    return a;
bad:
    json_object_put(a);
    return NULL;
}

json_object *bc_comm_jtag_identify(bc_comm *s, unsigned maximum, bc_error *e) {
    if (!s || strcmp(bc_str(s->config, "bus"), "jtag") || maximum < 1 || maximum > 32) {
        bc_fail(e, "Cadeia JTAG: máximo de 1..32 TAPs.");
        return NULL;
    }
    uint8_t tx[132], rx[132];
    memset(tx, 255, sizeof tx);
    memset(rx, 0, sizeof rx);
    bc_comm_deadline(s);
    if (bc_comm_ch347_jtag(s, "reset", NULL, NULL, 0, 0, e) < 0 ||
        bc_comm_ch347_jtag(s, "dr", tx, rx, (maximum + 1) * 32, 0, e) < 0)
        return NULL;
    json_object *r = json_object_new_object(), *a = json_object_new_array();
    json_object_object_add(r, "taps", a);
    unsigned at = 0, limit = (maximum + 1) * 32;
    int end = 0;
    for (unsigned index = 0; index < maximum && at < limit; index++) {
        uint32_t id = 0;
        if (((unsigned)rx[at / 8] >> (at % 8)) & 1u) {
            if (at + 32 > limit)
                break;
            for (unsigned b = 0; b < 32; b++)
                id |= (uint32_t)(((unsigned)rx[(at + b) / 8] >> ((at + b) % 8)) & 1u) << b;
            if (id == UINT32_MAX) {
                end = 1;
                break;
            }
            at += 32;
        } else
            at++;
        json_object *tap = json_object_new_object();
        json_object_object_add(tap, "index", json_object_new_int((int)index));
        json_object_object_add(tap, "bypass", json_object_new_boolean(!id));
        char text[16];
        snprintf(text, sizeof text, "0x%08X", id);
        json_object_object_add(tap, "idcode", json_object_new_string(text));
        json_object_object_add(tap, "manufacturer", json_object_new_int((int)((id >> 1) & 0x7ff)));
        json_object_object_add(tap, "part", json_object_new_int((int)((id >> 12) & 0xffff)));
        json_object_object_add(tap, "version", json_object_new_int((int)(id >> 28)));
        json_object_array_add(a, tap);
    }
    if (!end && at + 32 <= limit) {
        uint32_t tail = 0;
        for (unsigned b = 0; b < 32; b++)
            tail |= (uint32_t)(((unsigned)rx[(at + b) / 8] >> ((at + b) % 8)) & 1u) << b;
        end = tail == UINT32_MAX;
    }
    json_object_object_add(r, "complete",
                           json_object_new_boolean(end && json_object_array_length(a) > 0));
    json_object_object_add(
        r, "scope",
        json_object_new_string(
            "IDCODE/BYPASS após reset TAP; não identifica comprimento IR nem garante modelo/estado "
            "elétrico. TDO fixo baixo/alto ou cadeia maior podem ser inconclusivos."));
    return r;
}

static int map_label(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 160 || !bc_utf8((const unsigned char *)s, n))
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32)
            return 0;
    return 1;
}

int bc_comm_register_map_validate(json_object *m, bc_error *e) {
    if (!json_object_is_type(m, json_type_object) || !integer(m, "schema", 1, 1) ||
        !map_label(bc_str(m, "name")))
        return bc_fail(e, "Mapa de registradores inválido.");
    json_object_object_foreach(m, key, value) {
        (void)key;
        if (json_object_is_type(value, json_type_string) &&
            (size_t)json_object_get_string_len(value) != strlen(json_object_get_string(value)))
            return bc_fail(e, "Mapa contém NUL embutido.");
    }
    json_object *a = bc_get(m, "registers");
    if (!json_object_is_type(a, json_type_array) || !json_object_array_length(a) ||
        json_object_array_length(a) > 256)
        return bc_fail(e, "Mapa: use 1..256 entradas.");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *r = json_object_array_get_idx(a, i);
        if (!json_object_is_type(r, json_type_object))
            return bc_fail(e, "Entrada de mapa deve ser um objeto.");
        json_object_object_foreach(r, key, value) {
            (void)key;
            if (json_object_is_type(value, json_type_string) &&
                (size_t)json_object_get_string_len(value) != strlen(json_object_get_string(value)))
                return bc_fail(e, "Entrada contém NUL embutido.");
        }
        const char *access = bc_str(r, "access");
        if (!map_label(bc_str(r, "name")) || !integer(r, "register", 0, 65535) ||
            !integer(r, "register_bytes", 0, 2) || !integer(r, "width", 1, 32) ||
            (strcmp(access, "ro") && strcmp(access, "rw") && strcmp(access, "wo")) ||
            (!strcmp(access, "wo") && !bc_get(r, "command")))
            return bc_fail(e, "Mapa: entrada %zu inválida.", i + 1);
        if (bc_num(r, "register_bytes") == 1 && bc_num(r, "register") > 255)
            return bc_fail(e, "Registrador não cabe em 1 byte.");
        if (bc_get(r, "command")) {
            uint8_t b[64];
            size_t n;
            if (bc_comm_hex(bc_str(r, "command"), b, sizeof b, &n, e) < 0 || !n)
                return bc_fail(e, "Comando de mapa inválido.");
        }
    }
    return 0;
}

json_object *bc_comm_register_operation(json_object *m, size_t index, unsigned address,
                                        const char *write_hex, bc_error *e) {
    if (bc_comm_register_map_validate(m, e) < 0)
        return NULL;
    json_object *a = bc_get(m, "registers");
    if (index >= json_object_array_length(a) || address < 8 || address > 119) {
        bc_fail(e, "Selecione uma entrada e endereço 0x08..0x77.");
        return NULL;
    }
    json_object *r = json_object_array_get_idx(a, index);
    int writing = write_hex && *write_hex;
    const char *access = bc_str(r, "access");
    if ((writing && !strcmp(access, "ro")) || (!writing && !strcmp(access, "wo"))) {
        bc_fail(e, "Acesso ao registrador não permitido pelo mapa.");
        return NULL;
    }
    uint8_t b[BC_COMM_MAX];
    size_t n = 0;
    if (bc_get(r, "command")) {
        if (bc_comm_hex(bc_str(r, "command"), b, sizeof b, &n, e) < 0)
            return NULL;
    } else {
        unsigned reg = (unsigned)bc_num(r, "register"),
                 width = (unsigned)bc_num(r, "register_bytes");
        if (width == 2)
            b[n++] = (uint8_t)(reg >> 8);
        if (width)
            b[n++] = (uint8_t)reg;
    }
    if (writing) {
        size_t got;
        if (bc_comm_hex(write_hex, b + n, sizeof b - n, &got, e) < 0)
            return NULL;
        if (got != (size_t)bc_num(r, "width")) {
            bc_fail(e, "Informe exatamente %.0f bytes de dados.", bc_num(r, "width"));
            return NULL;
        }
        n += got;
    }
    json_object *o = json_object_new_object();
    char *hex = bc_comm_format_bytes(b, n, 1);
    json_object_object_add(o, "write", json_object_new_string(hex));
    free(hex);
    json_object_object_add(o, "read", json_object_new_int(writing ? 0 : (int)bc_num(r, "width")));
    json_object_object_add(o, "address", json_object_new_int((int)address));
    json_object_object_add(o, "label", json_object_new_string(bc_str(r, "name")));
    return o;
}

json_object *bc_comm_register_maps(void) {
    json_object *maps = json_tokener_parse(
        "[{\"schema\":1,\"name\":\"TCA9534A\",\"source\":\"https://www.ti.com/lit/ds/symlink/"
        "tca9534a.pdf\",\"registers\":["
        "{\"name\":\"Entrada\",\"register\":0,\"register_bytes\":1,\"width\":1,\"access\":\"ro\"},"
        "{\"name\":\"Latch de "
        "saída\",\"register\":1,\"register_bytes\":1,\"width\":1,\"access\":\"rw\"},"
        "{\"name\":\"Inversão de "
        "polaridade\",\"register\":2,\"register_bytes\":1,\"width\":1,\"access\":\"rw\"},"
        "{\"name\":\"Direções "
        "(1=entrada)\",\"register\":3,\"register_bytes\":1,\"width\":1,\"access\":\"rw\"}]},"
        "{\"schema\":1,\"name\":\"ADS7924\",\"source\":\"https://www.ti.com/lit/gpn/"
        "ads7924\",\"registers\":["
        "{\"name\":\"Modo\",\"register\":0,\"register_bytes\":1,\"width\":1,\"access\":\"rw\"},"
        "{\"name\":\"ADC0 (12 bits alinhados à "
        "esquerda)\",\"register\":130,\"register_bytes\":1,\"width\":2,\"access\":\"ro\"},"
        "{\"name\":\"ADC1\",\"register\":132,\"register_bytes\":1,\"width\":2,\"access\":\"ro\"},"
        "{\"name\":\"ADC2\",\"register\":134,\"register_bytes\":1,\"width\":2,\"access\":\"ro\"},"
        "{\"name\":\"ADC3\",\"register\":136,\"register_bytes\":1,\"width\":2,\"access\":\"ro\"}]},"
        "{\"schema\":1,\"name\":\"BME280 / BMP280 (somente "
        "leitura)\",\"source\":\"https://www.bosch-sensortec.com/products/environmental-sensors/"
        "humidity-sensors-bme280/\",\"registers\":["
        "{\"name\":\"Chip ID (BME=60, "
        "BMP=58)\",\"register\":208,\"register_bytes\":1,\"width\":1,\"access\":\"ro\"},"
        "{\"name\":\"Status\",\"register\":243,\"register_bytes\":1,\"width\":1,\"access\":\"ro\"},"
        "{\"name\":\"Pressão/temperatura brutas (sem "
        "compensação)\",\"register\":247,\"register_bytes\":1,\"width\":6,\"access\":\"ro\"}]},"
        "{\"schema\":1,\"name\":\"MPU-6050 (somente "
        "leitura)\",\"source\":\"https://invensense.tdk.com/products/motion-tracking/6-axis/"
        "mpu-6050/\",\"registers\":["
        "{\"name\":\"WHO_AM_I\",\"register\":117,\"register_bytes\":1,\"width\":1,\"access\":"
        "\"ro\"},"
        "{\"name\":\"Acelerômetro/temperatura/giroscópio "
        "brutos\",\"register\":59,\"register_bytes\":1,\"width\":14,\"access\":\"ro\"}]},"
        "{\"schema\":1,\"name\":\"INA219 (somente "
        "leitura)\",\"source\":\"https://www.ti.com/lit/ds/symlink/ina219.pdf\",\"registers\":["
        "{\"name\":\"Configuração\",\"register\":0,\"register_bytes\":1,\"width\":2,\"access\":"
        "\"ro\"},"
        "{\"name\":\"Tensão shunt "
        "bruta\",\"register\":1,\"register_bytes\":1,\"width\":2,\"access\":\"ro\"},"
        "{\"name\":\"Tensão barramento "
        "bruta\",\"register\":2,\"register_bytes\":1,\"width\":2,\"access\":\"ro\"}]},"
        "{\"schema\":1,\"name\":\"MCP4728 (leitura volátil, sem "
        "EEPROM)\",\"source\":\"https://www.microchip.com/en-us/product/MCP4728\",\"registers\":["
        "{\"name\":\"Todos os canais (24 bytes, formato "
        "datasheet)\",\"register\":0,\"register_bytes\":0,\"width\":24,\"access\":\"ro\"}]},"
        "{\"schema\":1,\"name\":\"SSD1306 OLED (comandos, sem leitura "
        "I²C)\",\"source\":\"https://www.solomon-systech.com/product/ssd1306/\",\"registers\":["
        "{\"name\":\"Comando de controle (AE=off, AF=on; requer inicialização "
        "própria)\",\"register\":0,\"register_bytes\":1,\"width\":1,\"access\":\"wo\",\"command\":"
        "\"00\"}]}]");
    const int addresses[] = {0x38, 0x48, 0x76, 0x68, 0x40, 0x60, 0x3c};
    for (size_t i = 0; i < json_object_array_length(maps); i++)
        json_object_object_add(json_object_array_get_idx(maps, i), "default_address",
                               json_object_new_int(addresses[i]));
    return maps;
}
