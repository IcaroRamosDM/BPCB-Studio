#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "../lab.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
#include <fcntl.h>
#include <unistd.h>

static const struct {
    const char *id, *label, *buses, *wiring;
} catalog[] = {
    {"ch341t", "CH341T", "i2c",
     "I²C: SCL, SDA e GND. Pull-ups e níveis devem corresponder à PCB. Frequências: 20, 100 e 400 "
     "kHz."},
    {"ft232h", "FT232H", "i2c,spi,uart",
     "I²C: AD0=SCL; AD1 e AD2 unidos=SDA; GND comum e pull-ups externos. SPI: AD0=SCK, AD1=MOSI, "
     "AD2=MISO, AD3…AD7=CS. UART: TX/RX via porta VCP. Confira os níveis do módulo; o software não "
     "altera sua tensão. I²C MPSSE sem clock stretching nesta versão."},
    {"ch340", "CH340 ou CH341 em modo UART", "uart",
     "UART assíncrona: TX→RX, RX←TX e GND. Se houver RTS/CTS, conecte conforme o procedimento. "
     "Confira o nível lógico; não conecte RS-232 diretamente."},
    {"cp2102", "CP210x", "uart",
     "UART assíncrona: TX→RX, RX←TX e GND. Confira o nível lógico do módulo."},
    {"ft232rl", "FT232R (FT232RL)", "uart",
     "UART assíncrona: TX→RX, RX←TX e GND. Confira o nível lógico do módulo."},
    {"mcp2221a", "MCP2221A", "i2c,uart",
     "I²C: SCL, SDA e GND, com pull-ups. UART: TX/RX pela porta CDC. Confira níveis e ligações na "
     "placa."},
    {"cp2112", "CP2112", "i2c",
     "SMBus/I²C: SCL, SDA e GND, com pull-ups. Escrita ≤61 bytes; prefixo de leitura combinada ≤16 "
     "bytes; leitura ≤512 bytes."},
    {"serial", "Porta serial genérica", "uart",
     "UART assíncrona pela porta indicada. O procedimento da PCB define o enquadramento e os "
     "comandos. USART síncrona não é oferecida por esta interface."},
    {"ch347", "Waveshare CH347T", "i2c,spi,uart,jtag",
     "CH347T: M0=UART0+UART1; M1=UART1+I²C+SPI; M2=UART1+I²C+SPI via HID; "
     "M3=UART1+I²C+JTAG. Ajuste as chaves ANTES de conectar o USB. I²C: SCL/SDA/GND, "
     "pull-ups; 20/100/400/750 kHz. SPI: SCK, SDO=MOSI, SDI=MISO, CS0/CS1 e GND; "
     "468750 Hz…30 MHz. JTAG: TCK/TMS/TDI/TDO/GND; TRST não é acionado. "
     "Confira pinout e chave 3.3/5 V. O BPCB detecta o modo; não muda as chaves nem a tensão. "
     "UART HID M2: 8 bits; fluxo e linhas de modem não são configurados nesta versão."}};

static int adapter_index(const char *name) {
    for (unsigned i = 0; i < sizeof catalog / sizeof *catalog; i++)
        if (!strcmp(catalog[i].id, name))
            return (int)i;
    return -1;
}

static int supports(const char *adapter, const char *bus) {
    int i = adapter_index(adapter);
    return i >= 0 &&
           (!strcmp(bus, "i2c") || !strcmp(bus, "spi") || !strcmp(bus, "uart") ||
            !strcmp(bus, "jtag")) &&
           strstr(catalog[i].buses, bus) != NULL;
}

static void string(json_object *o, const char *key, const char *value) {
    json_object_object_add(o, key, json_object_new_string(value));
}

static void integer(json_object *o, const char *key, int64_t value) {
    json_object_object_add(o, key, json_object_new_int64(value));
}

json_object *bc_comm_catalog(void) {
    json_object *a = json_object_new_array();
    for (unsigned i = 0; i < sizeof catalog / sizeof *catalog; i++) {
        json_object *o = json_object_new_object(), *b = json_object_new_array();
        string(o, "id", catalog[i].id);
        string(o, "label", catalog[i].label);
        string(o, "wiring", catalog[i].wiring);
        const char *names[] = {"i2c", "spi", "uart", "jtag"};
        for (unsigned k = 0; k < 4; k++)
            if (supports(catalog[i].id, names[k]))
                json_object_array_add(b, json_object_new_string(names[k]));
        json_object_object_add(o, "buses", b);
        json_object_array_add(a, o);
    }
    return a;
}

static int number(json_object *in, json_object *out, const char *key, int def, int min, int max,
                  bc_error *e) {
    json_object *v = bc_get(in, key);
    int64_t n = v ? json_object_get_int64(v) : def;
    if (v && (!json_object_is_type(v, json_type_int) || n < min || n > max))
        return bc_fail(e, "Configuração de comunicação inválida: %s (%d…%d).", key, min, max);
    integer(out, key, n);
    return 0;
}

json_object *bc_comm_config(json_object *in, bc_error *e) {
    if (json_object_is_type(in, json_type_object)) {
        json_object_object_foreach(in, key, value) {
            if (json_object_is_type(value, json_type_string) &&
                (size_t)json_object_get_string_len(value) !=
                    strlen(json_object_get_string(value))) {
                bc_fail(e, "Configuração contém NUL embutido: %s.", key);
                return NULL;
            }
        }
    }
    if (in && !json_object_is_type(in, json_type_object)) {
        bc_fail(e, "Comunicação deve ser um objeto.");
        return NULL;
    }
    const char *adapter = bc_get(in, "adapter") ? bc_str(in, "adapter") : "ch341t",
               *bus = bc_get(in, "bus") ? bc_str(in, "bus") : "i2c";
    if (!supports(adapter, bus)) {
        bc_fail(e, "O adaptador %s não oferece %s nesta versão.", adapter, bus);
        return NULL;
    }
    const char *allowed[] = {"api",
                             "adapter",
                             "bus",
                             "device",
                             "serial",
                             "frequency_hz",
                             "timeout_ms",
                             "spi_mode",
                             "chip_select",
                             "lsb_first",
                             "baud",
                             "data_bits",
                             "stop_bits",
                             "parity",
                             "flow_control",
                             "dtr",
                             "rts",
                             "allow_kernel_detach",
                             "connections_checked",
                             "ch347_mode"};
    if (in) {
        json_object_object_foreach(in, key, value) {
            (void)value;
            int found = 0;
            for (unsigned i = 0; i < sizeof allowed / sizeof *allowed; i++)
                if (!strcmp(key, allowed[i]))
                    found = 1;
            if (!found) {
                bc_fail(e, "Opção de comunicação desconhecida: %s.", key);
                return NULL;
            }
        }
    }
    if (bc_get(in, "api") && (!json_object_is_type(bc_get(in, "api"), json_type_int) ||
                              bc_num(in, "api") != BC_COMM_API)) {
        bc_fail(e, "API de comunicação incompatível.");
        return NULL;
    }
    json_object *o = json_object_new_object();
    if (number(in, o, "ch347_mode", -1, -1, 3, e) < 0) {
        json_object_put(o);
        return NULL;
    }
    int mode = (int)bc_num(o, "ch347_mode");
    if (strcmp(adapter, "ch347") && mode != -1) {
        bc_fail(e, "Modo M0…M3 pertence somente ao CH347T.");
        goto fail;
    }
    if (!strcmp(adapter, "ch347") && mode != -1 && !bc_comm_ch347_supports(mode, bus)) {
        bc_fail(e, "O modo M%d do CH347T não oferece %s. Confira as chaves do módulo.", mode, bus);
        goto fail;
    }
    /* Keep the API 1 configuration byte-for-byte compatible in shape for
     * pre-0.13 native workers. Only CH347 configurations carry the new key. */
    if (strcmp(adapter, "ch347"))
        json_object_object_del(o, "ch347_mode");
    integer(o, "api", BC_COMM_API);
    string(o, "adapter", adapter);
    string(o, "bus", bus);
    const char *texts[] = {"device", "serial"};
    for (unsigned i = 0; i < 2; i++) {
        const char *s = bc_str(in, texts[i]);
        if ((bc_get(in, texts[i]) &&
             !json_object_is_type(bc_get(in, texts[i]), json_type_string)) ||
            strlen(s) > 1023) {
            bc_fail(e, "Identificação de comunicação inválida.");
            goto fail;
        }
        for (const unsigned char *p = (const unsigned char *)s; *p; p++)
            if (*p < 32) {
                bc_fail(e, "Identificação de comunicação contém controle.");
                goto fail;
            }
        string(o, texts[i], s);
    }
    if (number(in, o, "timeout_ms", 1000, 50, 10000, e) < 0 ||
        number(in, o, "frequency_hz",
               !strcmp(bus, "jtag")         ? 1875000
               : !strcmp(adapter, "ch341t") ? 20000
                                            : 100000,
               1000, 30000000, e) < 0 ||
        number(in, o, "spi_mode", 0, 0, 3, e) < 0 ||
        number(in, o, "chip_select", !strcmp(adapter, "ch347") ? 0 : 3,
               !strcmp(adapter, "ch347") ? 0 : 3, !strcmp(adapter, "ch347") ? 1 : 7, e) < 0 ||
        number(in, o, "baud", 115200, 300, 3000000, e) < 0 ||
        number(in, o, "data_bits", 8, 7, 8, e) < 0 || number(in, o, "stop_bits", 1, 1, 2, e) < 0)
        goto fail;
    const char *booleans[] = {"lsb_first",           "dtr",       "rts", "allow_kernel_detach",
                              "connections_checked", "ch347_mode"};
    for (unsigned i = 0; i < 5; i++) {
        json_object *v = bc_get(in, booleans[i]);
        if (v && !json_object_is_type(v, json_type_boolean)) {
            bc_fail(e, "Opção booleana inválida: %s.", booleans[i]);
            goto fail;
        }
        json_object_object_add(o, booleans[i], json_object_new_boolean(json_object_get_boolean(v)));
    }
    const char *parity = bc_get(in, "parity") ? bc_str(in, "parity") : "none",
               *flow = bc_get(in, "flow_control") ? bc_str(in, "flow_control") : "none";
    if (strcmp(parity, "none") && strcmp(parity, "even") && strcmp(parity, "odd")) {
        bc_fail(e, "Paridade deve ser none, even ou odd.");
        goto fail;
    }
    if (strcmp(flow, "none") && strcmp(flow, "rtscts")) {
        bc_fail(e, "Controle de fluxo deve ser none ou rtscts.");
        goto fail;
    }
    string(o, "parity", parity);
    string(o, "flow_control", flow);
    unsigned hz = (unsigned)bc_num(o, "frequency_hz");
    if (!strcmp(bus, "i2c")) {
        if (((!strcmp(adapter, "ch341t") || !strcmp(adapter, "ch347")) && hz != 20000 &&
             hz != 100000 && hz != 400000 && (!strcmp(adapter, "ch341t") || hz != 750000)) ||
            (!strcmp(adapter, "mcp2221a") && (hz < 50000 || hz > 400000)) ||
            (!strcmp(adapter, "cp2112") && (hz < 10000 || hz > 400000)) ||
            (!strcmp(adapter, "ft232h") && (hz < 1000 || hz > 400000))) {
            bc_fail(e, "Frequência I²C não suportada pelo adaptador selecionado.");
            goto fail;
        }
    }
    if (!strcmp(adapter, "ch347") && !strcmp(bus, "spi") && hz < 468750) {
        bc_fail(e, "CH347T: SPI requer 468750 Hz a 30 MHz; o clock é arredondado para baixo.");
        goto fail;
    }
    if (!strcmp(adapter, "ch347") && !strcmp(bus, "uart") &&
        (bc_num(o, "baud") < 1200 || bc_num(o, "data_bits") != 8)) {
        bc_fail(e, "CH347T: UART VCP exige 8 bits e 1200…3000000 baud nesta versão.");
        goto fail;
    }
    if (!strcmp(bus, "jtag") && hz < 1875000) {
        bc_fail(e, "JTAG CH347T: solicite 1875000…30000000 Hz.");
        goto fail;
    }
    int hid_uart = !strcmp(adapter, "ch347") && !strcmp(bus, "uart") &&
                   (mode == 2 || !strncmp(bc_str(o, "device"), "/dev/hidraw", 11) ||
                    !strncmp(bc_str(o, "device"), "\\\\?\\hid#", 8));
    if (hid_uart &&
        (strcmp(bc_str(o, "flow_control"), "none") || json_object_get_boolean(bc_get(o, "dtr")) ||
         json_object_get_boolean(bc_get(o, "rts")))) {
        bc_fail(e, "CH347T UART HID M2: fluxo e linhas DTR/RTS não são configurados; use as opções "
                   "padrão.");
        goto fail;
    }
    if (!strcmp(bus, "uart") && !*bc_str(o, "device") && !hid_uart) {
        bc_fail(e, "Selecione a porta serial (COM no Windows ou /dev no Linux).");
        goto fail;
    }
    if (!strcmp(adapter, "mcp2221a") && !strcmp(bus, "uart") && bc_num(o, "baud") > 460800) {
        bc_fail(e, "MCP2221A: UART limitada a 460800 baud.");
        goto fail;
    }
    return o;
fail:
    json_object_put(o);
    return NULL;
}

int bc_comm_declaration(json_object *d, bc_error *e) {
    if (d && bc_num(d, "api") == 2)
        return bc_lab_declaration(d, e);
    if (!d)
        return 0;
    if (!json_object_is_type(d, json_type_object) ||
        !json_object_is_type(bc_get(d, "api"), json_type_int) || bc_num(d, "api") != BC_COMM_API)
        return bc_fail(e, "Declaração de comunicação: API incompatível.");
    json_object_object_foreach(d, field, item) {
        if (json_object_is_type(item, json_type_string) &&
            (size_t)json_object_get_string_len(item) != strlen(json_object_get_string(item)))
            return bc_fail(e, "Declaração contém NUL embutido: %s.", field);
    }
    const char *allowed[] = {"api",           "bus",          "adapters",         "min_hz",
                             "max_hz",        "max_transfer", "clock_stretching", "operations",
                             "max_timeout_ms"};
    json_object_object_foreach(d, key, value) {
        (void)value;
        int found = 0;
        for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++)
            if (!strcmp(key, allowed[i]))
                found = 1;
        if (!found)
            return bc_fail(e, "Campo de declaração de comunicação desconhecido: %s.", key);
    }
    const char *bus = bc_str(d, "bus");
    if (strcmp(bus, "i2c") && strcmp(bus, "spi") && strcmp(bus, "uart") && strcmp(bus, "jtag"))
        return bc_fail(e, "Barramento de comunicação inválido.");
    json_object *adapters = bc_get(d, "adapters");
    if (adapters &&
        (!json_object_is_type(adapters, json_type_array) || !json_object_array_length(adapters) ||
         json_object_array_length(adapters) > sizeof catalog / sizeof *catalog))
        return bc_fail(e, "Adaptadores declarados inválidos.");
    for (size_t i = 0;
         json_object_is_type(adapters, json_type_array) && i < json_object_array_length(adapters);
         i++) {
        const char *a = json_object_get_string(json_object_array_get_idx(adapters, i));
        if (!a || !json_object_is_type(json_object_array_get_idx(adapters, i), json_type_string) ||
            (size_t)json_object_get_string_len(json_object_array_get_idx(adapters, i)) !=
                strlen(a) ||
            !supports(a, bus))
            return bc_fail(e, "Adaptador incompatível com o barramento declarado.");
    }
    const char *limits[] = {"min_hz", "max_hz", "max_transfer"};
    for (unsigned i = 0; i < 3; i++) {
        json_object *v = bc_get(d, limits[i]);
        if (v && (!json_object_is_type(v, json_type_int) || json_object_get_int64(v) < 1 ||
                  json_object_get_int64(v) > (i == 2 ? BC_COMM_MAX : 30000000)))
            return bc_fail(e, "Limite de comunicação inválido: %s.", limits[i]);
    }
    if (bc_get(d, "min_hz") && bc_get(d, "max_hz") && bc_num(d, "min_hz") > bc_num(d, "max_hz"))
        return bc_fail(e, "Faixa de frequência inválida.");
    json_object *max_timeout = bc_get(d, "max_timeout_ms");
    if (max_timeout &&
        (!json_object_is_type(max_timeout, json_type_int) ||
         json_object_get_int64(max_timeout) < 50 || json_object_get_int64(max_timeout) > 10000))
        return bc_fail(e, "max_timeout_ms deve estar entre 50 e 10000 ms.");
    json_object *stretch = bc_get(d, "clock_stretching");
    if (stretch && !json_object_is_type(stretch, json_type_boolean))
        return bc_fail(e, "clock_stretching deve ser booleano.");
    json_object *operations = bc_get(d, "operations");
    if (operations) {
        if (!json_object_is_type(operations, json_type_array) ||
            !json_object_array_length(operations) || json_object_array_length(operations) > 64)
            return bc_fail(e, "Sequência de comunicação inválida.");
        for (size_t i = 0; json_object_is_type(operations, json_type_array) &&
                           i < json_object_array_length(operations);
             i++)
            if (bc_comm_operation_validate(bus, json_object_array_get_idx(operations, i), e) < 0)
                return -1;
    }
    return 0;
}

json_object *bc_comm_resolve(json_object *profile, json_object *test, json_object *selected,
                             bc_error *e) {
    json_object *d = bc_get(test, "communication");
    if (!d)
        d = bc_get(profile, "communication");
    if (bc_comm_declaration(d, e) < 0)
        return NULL;
    if (d && bc_num(d, "api") == 2)
        return bc_lab_resolve(d, selected, e);
    json_object *c = bc_comm_config(selected, e);
    if (!c)
        return NULL;
    if (d) {
        const char *bus = bc_str(d, "bus"), *adapter = bc_str(c, "adapter");
        if (!strcmp(adapter, "ch347") && bc_get(profile, "min_app")) {
            unsigned major = 0, minor = 0;
            sscanf(bc_str(profile, "min_app"), "%u.%u", &major, &minor);
            if (!major && minor < 13) {
                bc_fail(e, "CH347T exige pacote atualizado para BPCB 0.13.0 ou superior; o "
                           "executor anterior não conhece este adaptador.");
                goto fail;
            }
        }
        json_object *a = bc_get(d, "adapters");
        int found = !a;
        for (size_t i = 0;
             json_object_is_type(a, json_type_array) && i < json_object_array_length(a); i++)
            if (!strcmp(adapter, json_object_get_string(json_object_array_get_idx(a, i))))
                found = 1;
        double hz = bc_num(c, "frequency_hz");
        if (strcmp(bus, bc_str(c, "bus")) || !found) {
            bc_fail(e, "Este teste exige %s e um adaptador autorizado pelo pacote.", bus);
            goto fail;
        }
        if (strcmp(bus, "uart") && ((bc_get(d, "min_hz") && hz < bc_num(d, "min_hz")) ||
                                    (bc_get(d, "max_hz") && hz > bc_num(d, "max_hz")))) {
            bc_fail(e, "Frequência fora da faixa declarada pelo teste.");
            goto fail;
        }
        if (!strcmp(adapter, "ch347") && (!strcmp(bus, "spi") || !strcmp(bus, "jtag"))) {
            unsigned effective = 60000000;
            while (effective > (unsigned)hz)
                effective /= 2;
            if (bc_get(d, "min_hz") && effective < bc_num(d, "min_hz")) {
                bc_fail(e,
                        "Clock CH347T efetivo %u Hz ficaria abaixo do mínimo declarado no teste.",
                        effective);
                goto fail;
            }
        }
        if (bc_get(d, "max_timeout_ms") && bc_num(c, "timeout_ms") > bc_num(d, "max_timeout_ms")) {
            bc_fail(e, "Timeout excede o máximo de %.0f ms permitido pelo procedimento.",
                    bc_num(d, "max_timeout_ms"));
            goto fail;
        }
        if (json_object_get_boolean(bc_get(d, "clock_stretching")) &&
            (!strcmp(adapter, "ft232h") || !strcmp(adapter, "ch341t") ||
             !strcmp(adapter, "ch347"))) {
            bc_fail(e, "Este teste exige clock stretching, não declarado pelo driver selecionado.");
            goto fail;
        }
    } else if (strcmp(bc_str(c, "adapter"), "ch341t") || strcmp(bc_str(c, "bus"), "i2c") ||
               *bc_str(c, "device") || *bc_str(c, "serial") || bc_num(c, "frequency_hz") != 20000) {
        bc_fail(e, "Pacote legado não declara a seleção de adaptador. Atualize o .bancada; seus "
                   "executores continuam fixos no CH341T.");
        goto fail;
    }
    if (selected && !json_object_get_boolean(bc_get(c, "connections_checked"))) {
        bc_fail(e, "Confira as conexões e os níveis no diálogo Comunicação antes de executar.");
        goto fail;
    }
    json_object *operations = bc_get(d, "operations");
    for (size_t i = 0; json_object_is_type(operations, json_type_array) &&
                       i < json_object_array_length(operations);
         i++)
        if (bc_comm_operation_check(c, d, json_object_array_get_idx(operations, i), e) < 0)
            goto fail;
    return c;
fail:
    json_object_put(c);
    return NULL;
}

int bc_comm_hex(const char *text, uint8_t *out, size_t cap, size_t *size, bc_error *e) {
    int high = -1;
    size_t n = 0;
    if (!text)
        text = "";
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isspace(*p)) {
            if (high >= 0)
                return bc_fail(e, "Hexadecimal incompleto; use pares como 01 A0 FF.");
            continue;
        }
        int v = isdigit(*p)              ? *p - '0'
                : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10
                : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10
                                         : -1;
        if (v < 0)
            return bc_fail(e, "Dados devem conter somente pares hexadecimais.");
        if (high < 0)
            high = v;
        else {
            if (n >= cap)
                return bc_fail(e, "Transferência excede %zu bytes.", cap);
            out[n++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }
    if (high >= 0)
        return bc_fail(e, "Hexadecimal incompleto.");
    *size = n;
    return 0;
}

void bc_comm_device(json_object *list, const char *adapter, const char *device, const char *serial,
                    const char *label, const char *access) {
    json_object *o = json_object_new_object();
    string(o, "adapter", adapter);
    string(o, "device", device);
    string(o, "serial", serial);
    string(o, "label", label);
    string(o, "access", access);
    json_object_array_add(list, o);
}

int bc_comm_select(json_object *item, json_object *c) {
    return !strcmp(bc_str(item, "adapter"), bc_str(c, "adapter")) &&
           (!*bc_str(c, "device") || !strcmp(bc_str(item, "device"), bc_str(c, "device"))) &&
           (!*bc_str(c, "serial") || !strcmp(bc_str(item, "serial"), bc_str(c, "serial")));
}

int bc_comm_lock(const char *identity, bc_error *e) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)identity; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    char path[4096];
#ifdef _WIN32
    char temp[MAX_PATH];
    if (!GetTempPathA(sizeof temp, temp)) {
        bc_fail(e, "Pasta temporária indisponível.");
        return -1;
    }
    snprintf(path, sizeof path, "%sbpcb-comm-%016llx.lock", temp, (unsigned long long)hash);
#else
    snprintf(path, sizeof path, "/tmp/bpcb-comm-%lu-%016llx.lock", (unsigned long)getuid(),
             (unsigned long long)hash);
#endif
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (fd >= 0)
            close(fd);
        bc_fail(e, "Adaptador ocupado por outra operação BPCB.");
        return -1;
    }
    return fd;
}

void bc_comm_unlock(int fd) {
    if (fd >= 0)
        close(fd);
}
