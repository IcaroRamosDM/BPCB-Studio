#include "studio.h"
#include <stdio.h>
#include <string.h>

static json_object *create_field(const char *k, const char *l, const char *t, json_object *d) {
    json_object *r = json_object_new_object();
    json_object_object_add(r, "key", json_object_new_string(k));
    json_object_object_add(r, "label", json_object_new_string(l));
    json_object_object_add(r, "type", json_object_new_string(t));
    json_object_object_add(r, "default", d);
    json_object_object_add(
        r, "help",
        json_object_new_string(
            "Valores declarados pelo autor; confira a documentação do dispositivo."));
    return r;
}

static void add_text_field(json_object *a, const char *k, const char *l, const char *d) {
    json_object_array_add(a, create_field(k, l, "text", json_object_new_string(d)));
}

static void add_number_field(json_object *a, const char *k, const char *l, double d, double lo,
                             double hi) {
    json_object *r = create_field(k, l, "number", json_object_new_double(d));
    json_object_object_add(r, "min", json_object_new_double(lo));
    json_object_object_add(r, "max", json_object_new_double(hi));
    json_object_array_add(a, r);
}

static void add_choice_field(json_object *a, const char *k, const char *l, const char *d,
                             const char *csv) {
    json_object *r = create_field(k, l, "choice", json_object_new_string(d)),
                *o = json_object_new_array();
    char copy[512];
    snprintf(copy, sizeof copy, "%s", csv);
    char *save = NULL;
    for (char *v = strtok_r(copy, ",", &save); v; v = strtok_r(NULL, ",", &save)) {
        json_object *x = json_object_new_object();
        json_object_object_add(x, "id", json_object_new_string(v));
        json_object_object_add(x, "label", json_object_new_string(v));
        json_object_array_add(o, x);
    }
    json_object_object_add(r, "options", o);
    json_object_array_add(a, r);
}

void bs_workbench_catalog(const char *kind, json_object *a) {
    if (!strcmp(kind, "connection")) {
        add_text_field(a, "id", "Identificador da conexão", "conexao1");
        add_text_field(a, "channel", "Canal (main, nome ou manual)", "manual");
        add_text_field(a, "connector", "Conector da PCB", "J1");
        add_text_field(a, "pin", "Pino da PCB", "1");
        add_text_field(a, "adapter_pin", "Pino do adaptador ou instrumento", "GND");
        add_text_field(a, "signal", "Sinal", "GND");
        add_choice_field(
            a, "role", "Função do sinal", "ground",
            "manual,ground,power,i2c_sda,i2c_scl,spi_mosi,spi_miso,spi_sck,spi_cs,uart_tx,uart_"
            "rx,jtag_tck,jtag_tms,jtag_tdi,jtag_tdo,jtag_reset");
        add_number_field(a, "voltage_v", "Tensão esperada (V)", 0, 0, 1000);
        add_number_field(a, "pcb_limit_v", "Máximo admitido pela PCB (V)", 3.3, 0, 1000);
        add_number_field(a, "adapter_limit_v", "Máximo admitido pelo adaptador (V)", 3.3, 0, 1000);
        add_text_field(a, "notes", "Observações (polaridade, pull-ups, direção…)", "");
        return;
    }
    if (!strcmp(kind, "test")) {
        json_object_array_add(a, create_field("wiring", "Mapa de conexões deste teste", "wiring",
                                              json_object_new_array()));
        return;
    }
    if (!strcmp(kind, "step")) {
        for (size_t i = 0; i < json_object_array_length(a); i++) {
            json_object *x = json_object_array_get_idx(a, i);
            if (!strcmp(bc_str(x, "key"), "lab_protocol")) {
                json_object *o = json_object_new_object();
                json_object_object_add(o, "id", json_object_new_string("frame"));
                json_object_object_add(o, "label",
                                       json_object_new_string("Quadro binário configurável"));
                json_object_array_add(bc_get(x, "options"), o);
            }
        }
        json_object *x = create_field("lab_frame", "Formato do quadro binário", "frame", NULL);
        json_object_object_add(x, "when", json_object_new_string("communication"));
        json_object_object_add(x, "when_lab", json_object_new_string("protocol"));
        json_object_object_add(x, "when_protocol", json_object_new_string("frame"));
        json_object_array_add(a, x);
        x = create_field("lab_frame_metric", "Campo que alimenta a variável da etapa", "text",
                         json_object_new_string("temperature"));
        json_object_object_add(x, "when", json_object_new_string("communication"));
        json_object_object_add(x, "when_lab", json_object_new_string("protocol"));
        json_object_object_add(x, "when_protocol", json_object_new_string("frame"));
        json_object_array_add(a, x);
        x = create_field("lab_frame_sample", "Quadro de exemplo na simulação (hex)", "text",
                         json_object_new_string("AA 55 C4 09 E4 0C 01 BD"));
        json_object_object_add(x, "when", json_object_new_string("communication"));
        json_object_object_add(x, "when_lab", json_object_new_string("protocol"));
        json_object_object_add(x, "when_protocol", json_object_new_string("frame"));
        json_object_array_add(a, x);
    }
}
