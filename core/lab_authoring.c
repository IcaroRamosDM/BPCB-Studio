#define _POSIX_C_SOURCE 200809L
#include "studio.h"
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

static json_object *field(const char *key, const char *label, const char *type, json_object *def,
                          const char *help) {
    json_object *r = json_object_new_object();
    text(r, "key", key);
    text(r, "label", label);
    text(r, "type", type);
    text(r, "help", help);
    json_object_object_add(r, "default", def);
    json_object_object_add(r, "optional", json_object_new_boolean(1));
    return r;
}

static void option(json_object *f, const char *id, const char *label) {
    json_object *options = bc_get(f, "options");
    if (!options) {
        options = json_object_new_array();
        json_object_object_add(f, "options", options);
    }
    json_object *o = json_object_new_object();
    text(o, "id", id);
    text(o, "label", label);
    json_object_array_add(options, o);
}

static void add(json_object *fields, json_object *f, const char *mode) {
    if (mode)
        text(f, "when_lab", mode);
    text(f, "when", "communication");
    json_object_array_add(fields, f);
}

static json_object *number(const char *key, const char *label, double def, double min, double max,
                           int integer, const char *help) {
    json_object *f =
        field(key, label, integer ? "integer" : "number",
              integer ? json_object_new_int64((int64_t)def) : json_object_new_double(def), help);
    json_object_object_add(f, "min", json_object_new_double(min));
    json_object_object_add(f, "max", json_object_new_double(max));
    return f;
}

void bs_lab_catalog(const char *kind, json_object *fields) {
    if (!strcmp(kind, "channel")) {
        json_object_array_add(
            fields, field("id", "Nome do canal", "id", json_object_new_string("main"),
                          "Até 40 letras, números ou sublinhado. Usado nas etapas e no BPCB."));
        json_object *test = bs_catalog("test");
        for (size_t i = 0; i < json_object_array_length(test); i++) {
            json_object *f = json_object_array_get_idx(test, i);
            const char *key = bc_str(f, "key");
            if (!strncmp(key, "comm_", 5) && strcmp(key, "comm_enabled") &&
                strcmp(key, "comm_mode") && strcmp(key, "comm_channels"))
                json_object_array_add(fields, bc_clone(f));
        }
        json_object_put(test);
        return;
    }
    if (!strcmp(kind, "test")) {
        json_object *f = field(
            "comm_mode", "Organização da comunicação", "choice", json_object_new_string("single"),
            "Canal único mantém compatibilidade. Multicanal permite até 8 interfaces/adaptadores "
            "na mesma sequência, com seleção explícita no BPCB.");
        option(f, "single", "Um barramento");
        option(f, "multi", "Vários canais/barramentos");
        json_object_array_add(fields, f);
        f = field("comm_channels", "Canais desta sequência", "channels", json_object_new_array(),
                  "Adicione canais, seu barramento, adaptadores permitidos e limites. "
                  "Portas/séries e consentimento não são gravados no projeto.");
        text(f, "when_comm_mode", "multi");
        json_object_array_add(fields, f);
        for (size_t i = 0; i < json_object_array_length(fields); i++) {
            f = json_object_array_get_idx(fields, i);
            const char *k = bc_str(f, "key");
            if (!strncmp(k, "comm_", 5) && strcmp(k, "comm_enabled") && strcmp(k, "comm_mode") &&
                strcmp(k, "comm_channels"))
                text(f, "when_comm_mode", "single");
            if (!strcmp(k, "comm_enabled"))
                text(f, "help",
                     "Exige seleção explícita de cada adaptador no BPCB. Nenhuma porta ou "
                     "permissão é salva no projeto.");
        }
    }
    if (!strcmp(kind, "device"))
        json_object_array_add(fields,
                              field("comm_channel", "Canal I²C no teste multicanal", "text",
                                    json_object_new_string("main"),
                                    "Nome do canal definido no teste. Em canal único, use main."));
    if (strcmp(kind, "step"))
        return;
    json_object *f =
        field("comm_channel", "Canal desta etapa", "channel", json_object_new_string("main"),
              "Escolha um canal declarado no teste; no modo único use main.");
    add(fields, f, NULL);
    f = field("lab_mode", "Tipo de operação", "choice", json_object_new_string("raw"),
              "Escolha bytes brutos, componente IoT ou protocolo UART. Captura e falhas só afetam "
              "a simulação.");
    option(f, "raw", "Transferência de bytes");
    option(f, "peripheral", "Componente IoT");
    option(f, "protocol", "Protocolo UART");
    add(fields, f, NULL);
    f = field("lab_device", "Componente", "choice", json_object_new_string("bme280"),
              "Modelo exato. BME/BMP280 aplicam compensação de fábrica; INA219 usa o shunt "
              "informado; MPU-6050 lê a escala configurada.");
    json_object *catalog = bc_peripheral_catalog();
    for (size_t i = 0; i < json_object_array_length(catalog); i++) {
        json_object *row = json_object_array_get_idx(catalog, i);
        option(f, bc_str(row, "id"), bc_str(row, "label"));
    }
    json_object_put(catalog);
    add(fields, f, "peripheral");
    f = field("lab_action", "Ação no componente", "choice", json_object_new_string("read"),
              "Ler usa a configuração atual. Medir inicializa/configura e pode alterar "
              "registradores. OLED: padrão ou desligar; ACK não comprova a imagem exibida.");
    option(f, "read", "Ler configuração/medição existente");
    option(f, "measure", "Inicializar e medir");
    option(f, "pattern", "Exibir padrão OLED");
    option(f, "off", "Desligar OLED");
    add(fields, f, "peripheral");
    f = field("lab_metric", "Grandeza desejada", "choice", json_object_new_string("temperature"),
              "A variável recebe esta grandeza em unidade física. Use uma etapa Resultado para "
              "definir referência/tolerância.");
    const char *metrics[] = {"temperature", "pressure", "humidity", "bus_voltage", "shunt_voltage",
                             "current",     "power",    "accel_x",  "accel_y",     "accel_z",
                             "gyro_x",      "gyro_y",   "gyro_z",   "acknowledged"};
    for (unsigned i = 0; i < sizeof metrics / sizeof *metrics; i++)
        option(f, metrics[i], bc_lab_metric_label(metrics[i]));
    add(fields, f, "peripheral");
    add(fields,
        number("lab_address", "Endereço I²C (7 bits)", 0x76, 8, 119, 1,
               "Confira o endereço do módulo; ACK não identifica seu modelo."),
        "peripheral");
    add(fields,
        number("lab_shunt", "Resistor shunt INA219 (ohm)", 0.1, 0.000001, 100, 0,
               "Valor real do resistor instalado; necessário para corrente/potência."),
        "peripheral");
    add(fields,
        number("lab_current_lsb", "LSB de corrente INA219 (A)", 0.0001, 1e-9, 1, 0,
               "Usado apenas ao inicializar; a leitura usa a calibração efetivamente lida."),
        "peripheral");
    f = field("lab_accel_range", "Escala do acelerômetro (g)", "choice",
              json_object_new_string("2"), "Aplicada ao inicializar MPU-6050.");
    option(f, "2", "±2 g");
    option(f, "4", "±4 g");
    option(f, "8", "±8 g");
    option(f, "16", "±16 g");
    add(fields, f, "peripheral");
    f = field("lab_gyro_range", "Escala do giroscópio (°/s)", "choice",
              json_object_new_string("250"), "Aplicada ao inicializar MPU-6050.");
    option(f, "250", "±250");
    option(f, "500", "±500");
    option(f, "1000", "±1000");
    option(f, "2000", "±2000");
    add(fields, f, "peripheral");
    f = field("lab_oled_height", "Altura SSD1306 (pixels)", "choice", json_object_new_string("64"),
              "Largura 128 pixels. A geometria precisa corresponder ao módulo.");
    option(f, "32", "128 × 32");
    option(f, "64", "128 × 64");
    add(fields, f, "peripheral");
    f = field("lab_pattern", "Padrão OLED", "choice", json_object_new_string("checkerboard"),
              "A inspeção visual é do operador; a resposta I²C confirma somente comunicação.");
    option(f, "checkerboard", "Quadriculado");
    option(f, "bars", "Barras");
    option(f, "white", "Branco");
    option(f, "black", "Preto");
    add(fields, f, "peripheral");
    f = field("lab_protocol", "Protocolo UART", "choice", json_object_new_string("at"),
              "AT entende eco, OK/ERROR e eventos ESP-AT. Modbus RTU monta/verifica CRC; RS-485 "
              "exige transceptor com direção automática.");
    option(f, "at", "AT textual");
    option(f, "delimited", "Resposta delimitada");
    option(f, "length", "Comprimento fixo");
    option(f, "modbus_rtu", "Modbus RTU");
    add(fields, f, "protocol");
    add(fields,
        field("lab_success", "Linha de sucesso AT", "text", json_object_new_string("OK"),
              "Compare exatamente a linha indicada pelo firmware do módulo."),
        "protocol");
    add(fields,
        number("lab_max_bytes", "Máximo de bytes da resposta", 1024, 1, 4096, 1,
               "Também deve caber no limite publicado pelo canal."),
        "protocol");
    add(fields,
        number("lab_timeout_ms", "Tempo total da resposta (ms)", 1000, 50, 10000, 1,
               "Sem repetição automática de comandos."),
        "protocol");
    add(fields,
        number("lab_length", "Comprimento fixo da resposta", 1, 1, 4096, 1,
               "Usado somente no protocolo Comprimento fixo."),
        "protocol");
    add(fields,
        number("lab_slave", "Endereço Modbus", 1, 1, 247, 1,
               "Sem broadcast; escrita exige resposta do escravo."),
        "protocol");
    f = field("lab_function", "Função Modbus", "choice", json_object_new_string("3"),
              "Leituras 1/2/3/4; escritas 5/6/15/16. Os endereços são offsets de base zero.");
    const char *functions[] = {"1", "2", "3", "4", "5", "6", "15", "16"};
    for (unsigned i = 0; i < 8; i++)
        option(f, functions[i], functions[i]);
    add(fields, f, "protocol");
    add(fields,
        number("lab_start", "Primeiro endereço Modbus", 0, 0, 65535, 1,
               "Offset de registrador/bobina, sem prefixos 3xxxx/4xxxx."),
        "protocol");
    add(fields,
        number("lab_quantity", "Quantidade Modbus", 1, 1, 2000, 1,
               "A faixa depende da função; limites são validados antes de transmitir."),
        "protocol");
    add(fields,
        field("lab_values", "Dados de escrita Modbus (hex)", "text", json_object_new_string(""),
              "Função 5: FF 00 ou 00 00. Função 6: um registrador big endian. Funções 15/16: dados "
              "completos."),
        "protocol");
    add(fields,
        number("lab_sample", "Valor numérico na simulação sem captura", 25.08, -1e12, 1e12, 0,
               "Exemplo declarado pelo autor; não comprova a medição física."),
        NULL);
    add(fields,
        field("lab_sim_success", "Resposta simulada válida?", "boolean", json_object_new_boolean(1),
              "Desative para testar a interrupção por resposta inválida."),
        NULL);
    add(fields,
        field("lab_capture", "Captura de reprodução (opcional)", "replay", NULL,
              "Importe captura UART/trace IoT ou roteiro de transações. Somente a simulação usa "
              "esses dados; importação não acessa hardware."),
        NULL);
    f = field("lab_fault", "Falha na reprodução", "choice", json_object_new_string("none"),
              "Injetada exclusivamente na captura simulada.");
    const char *faults[] = {"none",     "fragment", "delay",      "timeout",
                            "truncate", "corrupt",  "disconnect", "nack"};
    for (unsigned i = 0; i < 8; i++)
        option(f, faults[i], faults[i]);
    add(fields, f, NULL);
    add(fields,
        number("lab_fault_at", "Transação onde injetar a falha", 0, 0, 511, 1,
               "Índice base zero na captura."),
        NULL);
    add(fields,
        number("lab_fragment", "Fragmento recebido (bytes)", 1, 1, 4096, 1,
               "Tamanho dos blocos UART na reprodução."),
        NULL);
    add(fields,
        number("lab_fault_offset", "Byte a corromper (base zero)", 0, 0, 4095, 1,
               "Índice dentro da resposta da transação escolhida."),
        NULL);
    add(fields,
        number("lab_fault_xor", "Máscara XOR da corrupção", 1, 1, 255, 1,
               "Bits a inverter no byte escolhido."),
        NULL);
    add(fields,
        number("lab_delay_ms", "Atraso simulado (ms)", 1000, 0, 60000, 1,
               "Tempo virtual: não bloqueia a interface por esse período."),
        NULL);
    for (size_t i = 0; i < json_object_array_length(fields); i++) {
        f = json_object_array_get_idx(fields, i);
        const char *key = bc_str(f, "key");
        if (!strncmp(key, "comm_", 5) && strcmp(key, "comm_channel") && strcmp(key, "comm_bus") &&
            strcmp(key, "comm_write") && strcmp(key, "comm_encoding") && strcmp(key, "comm_ending"))
            text(f, "when_lab", "raw");
        if (!strcmp(key, "plot") || !strcmp(key, "plot_target") || !strcmp(key, "plot_limit"))
            text(f, "when", "manual,read,wait,calculate,result,communication");
    }
}

json_object *bs_lab_test_channel(json_object *t, const char *id, bc_error *e) {
    if (strcmp(bc_str(t, "comm_mode"), "multi")) {
        if (strcmp(id, "main")) {
            bc_fail(e, "Canal único usa main.");
            return NULL;
        }
        return bc_clone(t);
    }
    json_object *a = bc_get(t, "comm_channels");
    if (!json_object_is_type(a, json_type_array) || !json_object_array_length(a) ||
        json_object_array_length(a) > 8) {
        bc_fail(e, "Teste multicanal exige 1…8 canais.");
        return NULL;
    }
    json_object *row = bs_find(a, id);
    if (!row) {
        bc_fail(e, "Canal %s ausente.", id);
        return NULL;
    }
    json_object *out = bc_clone(row);
    text(out, "comm_mode", "single");
    json_object_object_add(out, "comm_enabled", json_object_new_boolean(1));
    return out;
}

json_object *bs_lab_operation(json_object *t, json_object *s, bc_error *e) {
    json_object *normalized = NULL;
    if (bs_form("step", s, &normalized, e) < 0)
        return NULL;
    s = normalized;
    const char *id = *bc_str(s, "comm_channel") ? bc_str(s, "comm_channel") : "main";
    json_object *channel = bs_lab_test_channel(t, id, e);
    if (!channel) {
        json_object_put(normalized);
        return NULL;
    }
    json_object *decl = bs_communication(channel, e);
    if (!decl) {
        json_object_put(channel);
        json_object_put(normalized);
        return NULL;
    }
    const char *mode = bc_str(s, "lab_mode"), *bus = bc_str(channel, "comm_bus");
    json_object *op = NULL;
    if (!*mode || !strcmp(mode, "raw")) {
        json_object *copy = bc_clone(s);
        text(copy, "comm_bus", bus);
        op = bs_communication_operation(channel, copy, e);
        json_object_put(copy);
    } else if (!strcmp(mode, "peripheral")) {
        op = json_object_new_object();
        text(op, "kind", "peripheral");
        text(op, "device", bc_str(s, "lab_device"));
        text(op, "action", bc_str(s, "lab_action"));
        text(op, "metric", bc_str(s, "lab_metric"));
        json_object_object_add(op, "address", bc_clone(bc_get(s, "lab_address")));
        json_object_object_add(op, "shunt_ohms", bc_clone(bc_get(s, "lab_shunt")));
        json_object_object_add(op, "current_lsb", bc_clone(bc_get(s, "lab_current_lsb")));
        json_object_object_add(op, "range",
                               json_object_new_int(atoi(bc_str(s, "lab_accel_range"))));
        json_object_object_add(op, "gyro_range",
                               json_object_new_int(atoi(bc_str(s, "lab_gyro_range"))));
        json_object_object_add(op, "height",
                               json_object_new_int(atoi(bc_str(s, "lab_oled_height"))));
        text(op, "pattern", bc_str(s, "lab_pattern"));
    } else if (!strcmp(mode, "protocol")) {
        op = json_object_new_object();
        text(op, "kind", "uart_protocol");
        text(op, "protocol", bc_str(s, "lab_protocol"));
        const char *src[] = {"lab_max_bytes", "lab_timeout_ms", "lab_length",
                             "lab_slave",     "lab_start",      "lab_quantity"},
                   *dst[] = {"max_bytes", "timeout_ms", "length", "slave", "start", "quantity"};
        for (unsigned i = 0; i < 6; i++)
            json_object_object_add(op, dst[i], bc_clone(bc_get(s, src[i])));
        json_object_object_add(op, "function",
                               json_object_new_int(atoi(bc_str(s, "lab_function"))));
        if (!strcmp(bc_str(s, "lab_protocol"), "modbus_rtu"))
            text(op, "values", bc_str(s, "lab_values"));
        else {
            json_object *raw = bc_comm_text_operation(
                bc_str(s, "comm_write"), !strcmp(bc_str(s, "comm_encoding"), "hex"), "none", 1, e);
            if (!raw) {
                json_object_put(op);
                op = NULL;
            } else {
                text(op, "write", bc_str(raw, "write"));
                text(op, "delimiter", bc_str(s, "comm_ending"));
                text(op, "success", bc_str(s, "lab_success"));
                json_object_put(raw);
            }
        }
    } else
        bc_fail(e, "Tipo de bloco de laboratório inválido.");
    if (op && !strcmp(bc_str(s, "lab_mode"), "protocol") &&
        !strcmp(bc_str(s, "lab_protocol"), "frame")) {
        const char *remove[] = {"length", "slave", "start", "quantity", "function", "success"};
        for (unsigned i = 0; i < 6; i++)
            json_object_object_del(op, remove[i]);
        json_object_object_add(op, "frame", bc_clone(bc_get(s, "lab_frame")));
        text(op, "metric", bc_str(s, "lab_frame_metric"));
    }
    if (op && bc_comm_operation_validate(bus, op, e) < 0) {
        json_object_put(op);
        op = NULL;
    }
    if (op) {
        json_object *config = json_object_new_object();
        text(config, "bus", bus);
        if (bc_get(op, "kind") && bc_lab_operation_check(config, decl, op, e) < 0) {
            json_object_put(op);
            op = NULL;
        }
        json_object_put(config);
    }
    if (op && !strcmp(bc_str(t, "comm_mode"), "multi")) {
        json_object *wrapper = json_object_new_object();
        text(wrapper, "channel", id);
        json_object_object_add(wrapper, "operation", op);
        op = wrapper;
    }
    json_object_put(channel);
    json_object_put(decl);
    json_object_put(normalized);
    return op;
}

static const char *unit(json_object *s) {
    if (!strcmp(bc_str(s, "lab_mode"), "protocol"))
        return !strcmp(bc_str(s, "lab_protocol"), "at") ? "resposta" : "código";
    return bc_lab_metric_unit(bc_str(s, "lab_metric"));
}

json_object *bs_lab_result_prepared(json_object *t, json_object *s, json_object *result,
                                    int simulate, json_object *prepared, bc_error *e) {
    json_object *op = prepared ? bc_clone(prepared) : bs_communication_operation(t, s, e);
    if (!op)
        return NULL;
    json_object *raw = bc_get(op, "operation");
    if (!raw)
        raw = op;
    const char *id = *bc_str(s, "comm_channel") ? bc_str(s, "comm_channel") : "main";
    json_object *channel = NULL, *r = NULL;
    if (simulate && bc_get(s, "lab_capture")) {
        json_object *fault = json_object_new_object();
        text(fault, "type", bc_str(s, "lab_fault"));
        json_object_object_add(fault, "exchange", bc_clone(bc_get(s, "lab_fault_at")));
        json_object_object_add(fault, "fragment_size", bc_clone(bc_get(s, "lab_fragment")));
        json_object_object_add(fault, "delay_ms", bc_clone(bc_get(s, "lab_delay_ms")));
        json_object_object_add(fault, "offset", bc_clone(bc_get(s, "lab_fault_offset")));
        json_object_object_add(fault, "xor", bc_clone(bc_get(s, "lab_fault_xor")));
        r = bc_replay_execute(bc_get(s, "lab_capture"), raw, fault, e);
        json_object_put(fault);
        if (r) {
            json_object_object_add(r, "matched",
                                   json_object_new_boolean(!strcmp(bc_str(r, "status"), "pass")));
            if (!bc_get(r, "value"))
                json_object_object_add(r, "value", json_object_new_double(0));
        }
    } else if (!*bc_str(s, "lab_mode") || !strcmp(bc_str(s, "lab_mode"), "raw")) {
        channel = bs_lab_test_channel(t, id, e);
        if (!channel)
            goto done;
        json_object *copy = bc_clone(s);
        text(copy, "comm_bus", bc_str(channel, "comm_bus"));
        json_object_object_del(copy, "lab_capture");
        r = bs_communication_result_prepared(channel, copy, result, simulate, raw, e);
        json_object_put(copy);
    } else if (simulate && !strcmp(bc_str(s, "lab_protocol"), "frame") &&
               !strcmp(bc_str(s, "lab_mode"), "protocol")) {
        uint8_t bytes[4096];
        size_t n = 0;
        if (bc_comm_hex(bc_str(s, "lab_frame_sample"), bytes, sizeof bytes, &n, e) < 0)
            goto done;
        r = bc_frame_decode(bc_get(s, "lab_frame"), bytes, n, bc_str(s, "lab_frame_metric"), e);
        if (r)
            text(r, "simulation_source", "author-frame");
    } else if (simulate) {
        double sample;
        if (bc_lab_number(s, "lab_sample", 25.08, -1e12, 1e12, &sample, e) < 0)
            goto done;
        r = json_object_new_object();
        json_object_object_add(r, "value", json_object_new_double(sample));
        json_object_object_add(
            r, "matched",
            json_object_new_boolean(json_object_get_boolean(bc_get(s, "lab_sim_success"))));
        text(r, "unit", unit(s));
        text(r, "simulation_source", "author-declared");
    } else if (result && bc_get(result, "value") && isfinite(bc_num(result, "value")))
        r = bc_clone(result);
    else
        bc_fail(e, "Componente/protocolo não retornou valor válido.");
    if (r) {
        json_object_object_add(r, "simulation", json_object_new_boolean(simulate));
        json_object_object_add(r, "operation", bc_clone(raw));
        text(r, "channel", id);
        if (!bc_get(r, "unit"))
            text(r, "unit", !strcmp(bc_str(s, "comm_decode"), "count") ? "bytes" : "código");
    }
done:
    json_object_put(channel);
    json_object_put(op);
    return r;
}

json_object *bs_lab_result(json_object *t, json_object *s, json_object *result, int simulate,
                           bc_error *e) {
    json_object *normalized = NULL;
    if (bs_form("step", s, &normalized, e) < 0)
        return NULL;
    json_object *r = bs_lab_result_prepared(t, normalized, result, simulate, NULL, e);
    json_object_put(normalized);
    return r;
}
