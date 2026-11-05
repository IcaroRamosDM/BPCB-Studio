#define _POSIX_C_SOURCE 200809L
#include "../lab.h"
#include <glib.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdlib.h>
#include <string.h>

static void text(json_object *o, const char *k, const char *s) {
    json_object_object_add(o, k, json_object_new_string(s));
}

static void check(json_object *a, const char *id, const char *label, const char *type) {
    json_object *r = json_object_new_object();
    text(r, "id", id);
    text(r, "label", label);
    text(r, "type", type);
    json_object_array_add(a, r);
}

static char *fingerprint(json_object *config, const char *procedure) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "configuration", bc_clone(config));
    text(o, "procedure", procedure);
    char *digest = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), -1);
    json_object_put(o);
    return digest;
}

json_object *bc_qualification_plan(json_object *config, const char *procedure, bc_error *e) {
    json_object *c = bc_comm_config(config, e);
    if (!c)
        return NULL;
    const char *bus = bc_str(c, "bus");
    if (!strcmp(bc_str(c, "adapter"), "ch347") && bc_num(c, "ch347_mode") < 0) {
        json_object_put(c);
        bc_fail(e, "Para qualificar CH347T escolha explicitamente seu modo M0/M1/M2/M3.");
        return NULL;
    }
    int valid = (!strcmp(procedure, "uart_loopback") && !strcmp(bus, "uart")) ||
                (!strcmp(procedure, "spi_loopback") && !strcmp(bus, "spi")) ||
                (!strcmp(procedure, "i2c_component") && !strcmp(bus, "i2c")) ||
                (!strcmp(procedure, "jtag_chain") && !strcmp(bus, "jtag"));
    if (!valid) {
        json_object_put(c);
        bc_fail(e, "Roteiro de qualificação incompatível com a seleção.");
        return NULL;
    }
    /* A plan is not stored and does not carry forward permission to transmit. */
    json_object_object_add(c, "connections_checked", json_object_new_boolean(0));
    json_object_object_add(c, "allow_kernel_detach", json_object_new_boolean(0));
    json_object *r = json_object_new_object(), *checks = json_object_new_array();
    json_object_object_add(r, "schema", json_object_new_int(1));
    text(r, "procedure", procedure);
    json_object_object_add(r, "configuration", c);
    char *digest = fingerprint(c, procedure);
    text(r, "fingerprint", digest);
    g_free(digest);
    json_object_object_add(r, "checks", checks);
    check(checks, "model_revision", "Modelo/revisão e pinagem conferidos no manual do módulo",
          "boolean");
    check(checks, "connections", "GND, pinos, pull-ups e polaridade conferidos", "boolean");
    check(checks, "levels", "Tensão lógica/alimentação compatível com PCB e adaptador", "boolean");
    check(checks, "native_os", "Sistema e driver usados no ensaio identificados", "text");
    if (!strcmp(procedure, "uart_loopback"))
        check(checks, "functional", "Loopback TX–RX com resposta exata A5 5A 00 FF", "evidence");
    if (!strcmp(procedure, "spi_loopback")) {
        check(checks, "functional", "Loopback MOSI–MISO com resposta exata A5 5A 00 FF",
              "evidence");
        check(checks, "timing", "CPOL/CPHA/CS e clock conferidos com instrumento", "boolean");
    }
    if (!strcmp(procedure, "i2c_component")) {
        check(checks, "functional",
              "Componente de modelo/endereço conhecido devolveu identificação/medida coerente",
              "evidence");
        check(checks, "known_component",
              "Modelo e endereço do periférico confirmados, sem inferir por ACK", "boolean");
    }
    if (!strcmp(procedure, "jtag_chain")) {
        check(checks, "functional", "Cadeia JTAG identificada sem TDO fixo/truncamento",
              "evidence");
        check(checks, "known_chain", "Quantidade e IDs conferidos com documentação da placa",
              "boolean");
    }

    text(r, "status", "pending");
    text(r, "scope",
         "Roteiro por dispositivo/modo. Registro do operador, sem certificação elétrica "
         "automática. Simulação, PTY e Xvfb não qualificam hardware.");
    return r;
}

json_object *bc_qualification_assess(json_object *plan, json_object *answers, bc_error *e) {
    if (bc_lab_object(answers,
                      "|fingerprint||model_revision||connections||levels||native_os||functional||"
                      "timing||known_component||known_chain||"
                      "expected_voltage||measured_voltage||display_visual|",
                      e) < 0)
        return NULL;
    json_object *expected =
        bc_qualification_plan(bc_get(plan, "configuration"), bc_str(plan, "procedure"), e);
    if (!expected)
        return NULL;
    if (strcmp(bc_str(plan, "fingerprint"), bc_str(expected, "fingerprint")) ||
        strcmp(bc_str(answers, "fingerprint"), bc_str(expected, "fingerprint"))) {
        json_object_put(expected);
        bc_fail(e, "Seleção/roteiro mudou. Inicie nova qualificação.");
        return NULL;
    }
    json_object *r = bc_clone(expected), *details = json_object_new_array();
    json_object_object_add(r, "assessment", details);
    int failed = 0, pending = 0;
    json_object *checks = bc_get(expected, "checks");
    for (size_t i = 0; i < json_object_array_length(checks); i++) {
        json_object *item = json_object_array_get_idx(checks, i),
                    *answer = bc_get(answers, bc_str(item, "id")), *row = bc_clone(item);
        const char *type = bc_str(item, "type");
        int state = 0;
        if (answer) {
            if (!strcmp(type, "boolean")) {
                if (!json_object_is_type(answer, json_type_boolean)) {
                    json_object_put(row);
                    json_object_put(r);
                    json_object_put(expected);
                    bc_fail(e, "Conferência exige booleano.");
                    return NULL;
                }
                state = json_object_get_boolean(answer) ? 1 : -1;
            } else if (!strcmp(type, "text"))
                state =
                    json_object_is_type(answer, json_type_string) && *json_object_get_string(answer)
                        ? 1
                        : -1;
            else if (!strcmp(type, "evidence")) {
                state = -1;
                json_object *transport = bc_get(answer, "transport"),
                            *configuration = bc_get(transport, "configuration");
                int physical = !json_object_get_boolean(bc_get(answer, "simulation")) &&
                               (!bc_get(answer, "hardware_accessed") ||
                                json_object_get_boolean(bc_get(answer, "hardware_accessed"))) &&
                               configuration;
                json_object *normalized = configuration ? bc_comm_config(configuration, e) : NULL;
                if (normalized) {
                    json_object_object_add(normalized, "connections_checked",
                                           json_object_new_boolean(0));
                    json_object_object_add(normalized, "allow_kernel_detach",
                                           json_object_new_boolean(0));
                    json_object *selected = bc_get(expected, "configuration");
                    int identity =
                        !strcmp(bc_str(selected, "adapter"), bc_str(normalized, "adapter")) &&
                        !strcmp(bc_str(selected, "bus"), bc_str(normalized, "bus")) &&
                        (!*bc_str(selected, "device") ||
                         !strcmp(bc_str(selected, "device"), bc_str(normalized, "device"))) &&
                        (!*bc_str(selected, "serial") ||
                         !strcmp(bc_str(selected, "serial"), bc_str(normalized, "serial")));
                    if (!strncmp(bc_str(normalized, "device"), "/dev/pts/", 9))
                        physical = 0;
                    if (!*bc_str(selected, "device"))
                        text(normalized, "device", "");
                    if (!*bc_str(selected, "serial"))
                        text(normalized, "serial", "");
                    char *digest = fingerprint(normalized, bc_str(plan, "procedure"));
                    int same = identity && !strcmp(digest, bc_str(plan, "fingerprint"));
                    g_free(digest);
                    json_object_put(normalized);
                    if (physical && same) {
                        const char *procedure = bc_str(plan, "procedure");
                        if (!strcmp(procedure, "jtag_chain"))
                            state = json_object_get_boolean(bc_get(answer, "complete")) ? 1 : -1;
                        else if (!strcmp(procedure, "i2c_component"))
                            state = json_object_get_boolean(bc_get(answer, "matched")) ? 1 : -1;
                        else {
                            uint8_t data[4], tx[4];
                            size_t n = 0, nt = 0;
                            bc_error parse = {{0}};
                            static const uint8_t pattern[] = {0xa5, 0x5a, 0, 0xff};
                            state = bc_comm_hex(bc_str(answer, "received"), data, sizeof data, &n,
                                                &parse) == 0 &&
                                            bc_comm_hex(bc_str(answer, "transmitted"), tx,
                                                        sizeof tx, &nt, &parse) == 0 &&
                                            n == 4 && nt == 4 && !memcmp(data, pattern, 4) &&
                                            !memcmp(tx, pattern, 4) &&
                                            json_object_get_boolean(bc_get(answer, "matched"))
                                        ? 1
                                        : -1;
                        }
                    }
                }
            }
        }
        if (state < 0)
            failed = 1;
        if (!state)
            pending = 1;
        text(row, "status", state > 0 ? "pass" : state < 0 ? "fail" : "pending");
        if (answer)
            json_object_object_add(row, "answer", bc_clone(answer));
        json_object_array_add(details, row);
    }
    if (json_object_get_boolean(
            bc_get(bc_get(answers, "functional"), "requires_visual_confirmation"))) {
        if (!bc_get(answers, "display_visual"))
            pending = 1;
        else if (!json_object_is_type(bc_get(answers, "display_visual"), json_type_boolean) ||
                 !json_object_get_boolean(bc_get(answers, "display_visual")))
            failed = 1;
    }
    if (bc_get(answers, "expected_voltage") || bc_get(answers, "measured_voltage")) {
        double a, b;
        if (bc_lab_number(answers, "expected_voltage", 0, 1, 5.5, &a, e) < 0 ||
            bc_lab_number(answers, "measured_voltage", 0, 0, 5.5, &b, e) < 0) {
            json_object_put(r);
            json_object_put(expected);
            return NULL;
        }
        if (fabs(a - b) > a * 0.1)
            failed = 1;
    }
    text(r, "status", failed ? "fail" : pending ? "pending" : "pass");
    text(r, "acceptance", "operator-recorded");
    json_object_object_add(r, "answers", bc_clone(answers));
    json_object_put(expected);
    return r;
}
