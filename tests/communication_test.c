#include "studio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "STUDIO COMM FAIL %d: %s (%s)\n", __LINE__, #x, e.text);               \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static void str(json_object *o, const char *k, const char *v) {
    json_object_object_add(o, k, json_object_new_string(v));
}

static void num(json_object *o, const char *k, int n) {
    json_object_object_add(o, k, json_object_new_int(n));
}

typedef struct {
    double time, value;
    int calls, results, cancel, confirm;
} fixture;

static int stopped(void *p) {
    return ((fixture *)p)->cancel;
}

static double clock_(void *p) {
    return ((fixture *)p)->time;
}

static int sleep_(void *p, unsigned ms) {
    ((fixture *)p)->time += (double)ms / 1000;
    return 0;
}

static int emit_(void *p, json_object *ev) {
    fixture *f = p;
    if (!strcmp(bc_str(ev, "type"), "measurement")) {
        f->results++;
        f->value = bc_num(ev, "value");
    }
    return 0;
}

static int confirm_(void *p, const char *id, const char *text, int number, double min, double max,
                    double timeout, double *value) {
    (void)id;
    (void)text;
    (void)number;
    (void)min;
    (void)max;
    (void)timeout;
    (void)value;
    ((fixture *)p)->confirm++;
    return 0;
}

static json_object *communicate(void *p, json_object *op, bc_error *e) {
    (void)op;
    (void)e;
    ((fixture *)p)->calls++;
    return json_tokener_parse("{\"received\":\"12 14\",\"bytes_read\":2}");
}

static json_object *project(const char *bus) {
    json_object *p = bs_new(), *b = bc_get(p, "board");
    str(b, "connections", "Fixture de software; sem hardware.");
    json_object *t = bs_defaults("test");
    str(t, "id", "comm");
    str(t, "title", "Comunicação");
    str(t, "instructions", "Sem hardware nos testes automatizados.");
    str(t, "expected", "Conferir os bytes.");
    num(t, "comm_enabled", 1);
    str(t, "comm_bus", bus);
    str(t, "comm_adapters",
        !strcmp(bus, "i2c")    ? "ch341t,ft232h,mcp2221a,cp2112,ch347"
        : !strcmp(bus, "spi")  ? "ft232h,ch347"
        : !strcmp(bus, "uart") ? "serial,ch340,cp2102,ft232rl,mcp2221a,ch347"
                               : "ch347");
    num(t, "comm_min_hz", !strcmp(bus, "jtag") ? 1875000 : 20000);
    num(t, "comm_max_hz", !strcmp(bus, "jtag") ? 30000000 : 1000000);
    json_object_object_add(t, "comm_enabled", json_object_new_boolean(1));
    json_object_object_add(t, "parameters", json_object_new_array());
    json_object_object_add(t, "formulas", json_object_new_array());
    json_object *steps = json_object_new_array();
    json_object_object_add(t, "steps", steps);
    json_object_array_add(bc_get(p, "tests"), t);
    json_object *s = bs_defaults("step");
    str(s, "id", "exchange");
    str(s, "title", "Bytes");
    str(s, "type", "communication");
    str(s, "comm_bus", bus);
    str(s, "comm_write", "00 00");
    num(s, "comm_read", 2);
    num(s, "comm_bits", 13);
    str(s, "comm_expect", "12 14");
    str(s, "comm_simulated", "12 14");
    str(s, "comm_decode", "u16be");
    json_object_array_add(steps, s);
    return p;
}

int main(void) {
    bc_error e = {{0}};
    const char *buses[] = {"i2c", "spi", "uart", "jtag"};
    for (unsigned i = 0; i < 4; i++) {
        json_object *p = project(buses[i]), *t = json_object_array_get_idx(bc_get(p, "tests"), 0),
                    *s = json_object_array_get_idx(bc_get(t, "steps"), 0);
        if (bs_check(p, &e))
            fprintf(stderr, "%s\n", e.text);
        CHECK(bs_check(p, &e) == 0);
        json_object *m = bs_manifest(p, 0, &e);
        CHECK(m && !strcmp(bc_str(m, "min_app"), "0.14.0"));
        CHECK(!strcmp(bc_str(bc_get(bc_test(m, "comm"), "communication"), "bus"), buses[i]));
        json_object_put(m);
        fixture f = {0};
        bs_host h = {.context = &f,
                     .simulation = 1,
                     .stopped = stopped,
                     .clock = clock_,
                     .sleep_ms = sleep_,
                     .emit = emit_,
                     .confirm = confirm_,
                     .communicate = communicate};
        CHECK(bs_execute(p, "comm", NULL, NULL, &h, &e) == 0 && f.value == 4628 && f.results == 1 &&
              f.calls == 0);
        h.simulation = 0;
        f = (fixture){0};
        CHECK(bs_execute(p, "comm", NULL, NULL, &h, &e) == 0 && f.calls == 1 && f.results == 1 &&
              f.value == 4628);
        str(s, "comm_expect", "00 00");
        f = (fixture){0};
        CHECK(bs_execute(p, "comm", NULL, NULL, &h, &e) < 0 && f.results == 1 && f.calls == 1);
        str(s, "comm_mask", "00 00");
        f = (fixture){0};
        CHECK(bs_execute(p, "comm", NULL, NULL, &h, &e) == 0);
        str(s, "comm_expect", "12 14");
        str(s, "comm_mask", "");
        str(s, "comm_bus", i ? "i2c" : "uart");
        CHECK(bs_check(p, &e) < 0);
        str(s, "comm_bus", buses[i]);
        num(s, "comm_read", 4097);
        CHECK(bs_check(p, &e) < 0);
        num(s, "comm_read", 2);
        str(s, "comm_decode", "u32be");
        CHECK(bs_check(p, &e) < 0);
        str(s, "comm_decode", "u16le");
        h.simulation = 1;
        f = (fixture){0};
        CHECK(bs_execute(p, "comm", NULL, NULL, &h, &e) == 0 && f.value == 5138);
        str(s, "comm_simulated", "12");
        CHECK(bs_check(p, &e) < 0);
        str(s, "comm_simulated", "12 14");
        str(t, "comm_adapters", "unknown");
        CHECK(bs_check(p, &e) < 0);
        json_object_put(p);
    }
    json_object *p = bs_example(), *m = bs_manifest(p, 1, &e);
    CHECK(m && !strcmp(bc_str(m, "min_app"), "0.11.0"));
    json_object_put(m);
    json_object_put(p);
    p = project("uart");
    json_object *t = json_object_array_get_idx(bc_get(p, "tests"), 0),
                *s = json_object_array_get_idx(bc_get(t, "steps"), 0);
    str(s, "comm_encoding", "ascii");
    str(s, "comm_write", "AT");
    str(s, "comm_ending", "crlf");
    json_object *o = bs_communication_operation(t, s, &e);
    CHECK(o && !strcmp(bc_str(o, "write"), "41 54 0D 0A"));
    json_object_put(o);
    json_object_object_add(s, "comm_read", json_object_new_double(1e300));
    CHECK(!bs_communication_operation(t, s, &e));
    json_object_put(p);
    p = project("i2c");
    json_object *invalid_device = bs_defaults("device");
    json_object_object_add(invalid_device, "output_mask", json_object_new_double(INFINITY));
    json_object_array_add(bc_get(p, "devices"), invalid_device);
    CHECK(bs_check(p, &e) < 0);
    json_object_put(p);
    p = project("jtag");
    t = json_object_array_get_idx(bc_get(p, "tests"), 0);
    s = json_object_array_get_idx(bc_get(t, "steps"), 0);
    str(s, "comm_register", "reset");
    str(s, "comm_write", "");
    str(s, "comm_expect", "");
    str(s, "comm_simulated", "");
    str(s, "comm_decode", "count");
    num(s, "comm_read", 0);
    num(s, "comm_bits", 0);
    o = bs_communication_operation(t, s, &e);
    CHECK(o);
    json_object_put(o);
    str(s, "comm_write", "zz");
    CHECK(!bs_communication_operation(t, s, &e));
    str(s, "comm_write", "");
    str(s, "comm_register", "idle");
    num(s, "comm_idle", 1);
    o = bs_communication_operation(t, s, &e);
    CHECK(o);
    json_object_put(o);
    str(s, "comm_register", "ir");
    num(s, "comm_idle", 0);
    num(s, "comm_bits", 5);
    num(s, "comm_read", 1);
    str(s, "comm_write", "09");
    str(s, "comm_simulated", "01");
    o = bs_communication_operation(t, s, &e);
    CHECK(o);
    json_object_put(o);
    json_object_put(p);
    const char *formats[] = {"u8", "s16be", "s16le", "u32be", "u32le"},
               *bytes[] = {"FF", "FF FE", "FE FF", "01 23 45 67", "67 45 23 01"};
    const int sizes[] = {1, 2, 2, 4, 4};
    const double values[] = {255, -2, -2, 19088743, 19088743};
    for (unsigned i = 0; i < 5; i++) {
        p = project("uart");
        t = json_object_array_get_idx(bc_get(p, "tests"), 0);
        s = json_object_array_get_idx(bc_get(t, "steps"), 0);
        num(s, "comm_read", sizes[i]);
        str(s, "comm_expect", bytes[i]);
        str(s, "comm_simulated", bytes[i]);
        str(s, "comm_decode", formats[i]);
        json_object *result = bs_communication_result(t, s, NULL, 1, &e);
        CHECK(result && bc_num(result, "value") == values[i] &&
              json_object_get_boolean(bc_get(result, "matched")));
        json_object_put(result);
        json_object_put(p);
    }
    fixture f = {0};
    bs_host h = {.context = &f,
                 .simulation = 1,
                 .stopped = stopped,
                 .clock = clock_,
                 .sleep_ms = sleep_,
                 .emit = emit_,
                 .confirm = confirm_};
    bs_io *io = bs_io_new(&h);
    CHECK(io);
    json_object *dev = bs_defaults("device"), *sig = bs_defaults("signal");
    double value = 0;
    CHECK(bs_io_read(io, dev, sig, &value, &e) < 0);
    json_object_object_add(sig, "channel", json_object_new_double(1e300));
    CHECK(bs_io_read(io, dev, sig, &value, &e) < 0);
    num(sig, "channel", 0);
    json_object_object_add(dev, "address", json_object_new_double(NAN));
    CHECK(bs_io_read(io, dev, sig, &value, &e) < 0);
    num(dev, "address", 56);
    CHECK(bs_io_write(io, dev, sig, NAN, &e) < 0);
    num(dev, "output_mask", -1);
    CHECK(bs_io_init(io, dev, NULL, &e) < 0);
    CHECK(!bs_io_new(NULL));
    bs_io_free(io);
    json_object_put(dev);
    json_object_put(sig);
    json_object *bad_text = bs_defaults("step"), *normalized = NULL;
    json_object_object_add(bad_text, "comm_write", json_object_new_string_len("AT\0HIDDEN", 9));
    CHECK(bs_form("step", bad_text, &normalized, &e) < 0);
    json_object_put(bad_text);
    printf("%u verificações Studio: quatro barramentos, contrato, simulação, máscara, falha, "
           "decodificação e limites\n",
           checks);
    return 0;
}
