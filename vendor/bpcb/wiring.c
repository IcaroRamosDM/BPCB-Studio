#define _POSIX_C_SOURCE 200809L
#include "workbench.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static int text_field(json_object *o, const char *key, int required, size_t max, bc_error *e) {
    json_object *v = bc_get(o, key);
    if (!v)
        return required ? bc_fail(e, "Campo obrigatório no mapa: %s.", key) : 0;
    if (!json_object_is_type(v, json_type_string) || (required && !*bc_str(o, key)) ||
        strlen(bc_str(o, key)) > max ||
        !bc_utf8((const unsigned char *)bc_str(o, key), strlen(bc_str(o, key))))
        return bc_fail(e, "Texto inválido no mapa: %s.", key);
    return 0;
}

static const struct {
    const char *id, *bus;
} roles[] = {
    {"manual", ""},       {"ground", ""},       {"power", ""},        {"i2c_sda", "i2c"},
    {"i2c_scl", "i2c"},   {"spi_mosi", "spi"},  {"spi_miso", "spi"},  {"spi_sck", "spi"},
    {"spi_cs", "spi"},    {"uart_tx", "uart"},  {"uart_rx", "uart"},  {"jtag_tck", "jtag"},
    {"jtag_tms", "jtag"}, {"jtag_tdi", "jtag"}, {"jtag_tdo", "jtag"}, {"jtag_reset", "jtag"}};

int bc_wiring_validate(json_object *rows, json_object *definition, bc_error *e) {
    if (!rows)
        return 0;
    if (!json_object_is_type(rows, json_type_array) || json_object_array_length(rows) > 128)
        return bc_fail(e, "Mapa exige até 128 conexões.");
    for (size_t i = 0; i < json_object_array_length(rows); i++) {
        json_object *r = json_object_array_get_idx(rows, i);
        if (bc_lab_object(r,
                          "|id||channel||connector||pin||adapter_pin||signal||role||voltage_v||pcb_"
                          "limit_v||adapter_limit_v||notes|",
                          e) < 0 ||
            !bc_lab_id(bc_str(r, "id")))
            return bc_fail(e, "Conexão %u inválida.", (unsigned)(i + 1));
        const char *keys[] = {"id",          "channel", "connector", "pin",
                              "adapter_pin", "signal",  "role",      "notes"};
        size_t max[] = {40, 40, 40, 24, 40, 80, 24, 1000};
        for (unsigned k = 0; k < 8; k++)
            if (text_field(r, keys[k], k < 7, max[k], e) < 0)
                return -1;
        if (!bc_lab_id(bc_str(r, "channel")))
            return bc_fail(e, "Canal inválido na conexão.");
        const char *bus = NULL;
        for (unsigned k = 0; k < sizeof roles / sizeof *roles; k++)
            if (!strcmp(roles[k].id, bc_str(r, "role")))
                bus = roles[k].bus;
        if (!bus)
            return bc_fail(e, "Função de pino desconhecida.");
        if (*bus) {
            json_object *d = bc_lab_channel_definition(definition, bc_str(r, "channel"));
            if (!d || strcmp(bus, bc_str(d, "bus")))
                return bc_fail(e, "Conexão %s exige um canal %s declarado.", bc_str(r, "id"), bus);
        } else if (strcmp(bc_str(r, "channel"), "manual") &&
                   !bc_lab_channel_definition(definition, bc_str(r, "channel")))
            return bc_fail(e, "Canal ausente no mapa.");
        double voltage, pcb, adapter;
        if (!bc_get(r, "voltage_v") || !bc_get(r, "pcb_limit_v") || !bc_get(r, "adapter_limit_v") ||
            bc_lab_number(r, "voltage_v", 0, 0, 1000, &voltage, e) < 0 ||
            bc_lab_number(r, "pcb_limit_v", 0, 0, 1000, &pcb, e) < 0 ||
            bc_lab_number(r, "adapter_limit_v", 0, 0, 1000, &adapter, e) < 0 || voltage > pcb ||
            voltage > adapter)
            return bc_fail(e, "Tensão declarada excede o limite da PCB/adaptador (%s).",
                           bc_str(r, "id"));
        if (!strcmp(bus, "") && !strcmp(bc_str(r, "role"), "ground") && voltage != 0)
            return bc_fail(e, "GND deve declarar 0 V.");
        for (size_t j = 0; j < i; j++) {
            json_object *old = json_object_array_get_idx(rows, j);
            if (!strcmp(bc_str(old, "id"), bc_str(r, "id")))
                return bc_fail(e, "Identificador repetido no mapa.");
            if (!strcmp(bc_str(old, "connector"), bc_str(r, "connector")) &&
                !strcmp(bc_str(old, "pin"), bc_str(r, "pin")))
                return bc_fail(e, "Pino da PCB declarado mais de uma vez.");
            if (strcmp(bc_str(r, "role"), "ground") && strcmp(bc_str(r, "role"), "power") &&
                strcmp(bc_str(r, "channel"), "manual") &&
                !strcmp(bc_str(old, "channel"), bc_str(r, "channel")) &&
                !strcmp(bc_str(old, "adapter_pin"), bc_str(r, "adapter_pin")))
                return bc_fail(e, "Pino do adaptador associado a dois sinais no mesmo canal.");
        }
    }
    return 0;
}

char *bc_wiring_text(json_object *rows, bc_error *e) {
    if (!rows || !json_object_is_type(rows, json_type_array) ||
        json_object_array_length(rows) > 128) {
        bc_fail(e, "Mapa de conexões inválido.");
        return NULL;
    }
    GString *s = g_string_new("MAPA DE CONEXÕES DECLARADO\nTensões e pinagem precisam de "
                              "conferência física; o software não as mede automaticamente.\n\n");
    for (size_t i = 0; i < json_object_array_length(rows); i++) {
        json_object *r = json_object_array_get_idx(rows, i);
        g_string_append_printf(
            s, "%s · PCB %s/%s ← %s/%s · %s · %.6g V (máximos PCB %.6g V; adaptador %.6g V)\n%s%s",
            bc_str(r, "id"), bc_str(r, "connector"), bc_str(r, "pin"), bc_str(r, "channel"),
            bc_str(r, "adapter_pin"), bc_str(r, "signal"), bc_num(r, "voltage_v"),
            bc_num(r, "pcb_limit_v"), bc_num(r, "adapter_limit_v"), bc_str(r, "notes"),
            *bc_str(r, "notes") ? "\n" : "");
    }
    return g_string_free(s, FALSE);
}
