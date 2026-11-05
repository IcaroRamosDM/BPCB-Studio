#define _POSIX_C_SOURCE 200809L
#include "studio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned checks;
static bc_error e;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "LAB STUDIO FAIL %d: %s (%s)\n", __LINE__, #x, e.text);                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static void text(json_object *o, const char *k, const char *s) {
    json_object_object_add(o, k, json_object_new_string(s));
}

static void number(json_object *o, const char *k, double n) {
    json_object_object_add(o, k, json_object_new_double(n));
}

typedef struct {
    unsigned calls, readings;
    int matched;
} state;

static int stopped(void *p) {
    (void)p;
    return 0;
}

static double tick(void *p) {
    (void)p;
    return 0;
}

static int wait_(void *p, unsigned ms) {
    (void)p;
    (void)ms;
    return 0;
}

static int emit_(void *p, json_object *event) {
    state *s = p;
    if (!strcmp(bc_str(event, "type"), "measurement"))
        s->readings++;
    return 0;
}

static int confirm_(void *p, const char *id, const char *message, int n, double min, double max,
                    double timeout, double *value) {
    (void)p;
    (void)id;
    (void)message;
    (void)n;
    (void)min;
    (void)max;
    (void)timeout;
    (void)value;
    return 0;
}

static json_object *communicate(void *p, json_object *op, bc_error *error) {
    (void)error;
    state *s = p;
    s->calls++;
    CHECK(!strcmp(bc_str(op, "device"), "bme280"));
    CHECK(bc_num(op, "address") == 118);
    text(op, "device", "callback_mutated_argument");
    json_object *r = json_object_new_object();
    number(r, "value", 25.08);
    text(r, "unit", "°C");
    json_object_object_add(r, "matched", json_object_new_boolean(s->matched));
    return r;
}

static json_object *step(const char *id, const char *type) {
    json_object *s = bs_defaults("step");
    text(s, "id", id);
    text(s, "title", id);
    text(s, "type", type);
    return s;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const char *names[] = {"BME280", "BMP280",     "INA219",     "MPU6050",   "OLED",
                           "AT",     "AT_Captura", "Modbus_RTU", "Multicanal"};
    for (unsigned i = 0; i < 9; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/examples/Laboratorio/%s.bpcb", argv[1], names[i]);
        json_object *p = bs_load(path, &e);
        CHECK(p);
        CHECK(bs_check(p, &e) == 0);
        json_object *m = bs_manifest(p, 0, &e);
        CHECK(m && !strcmp(bc_str(m, "min_app"), "0.15.0"));
        json_object_put(m);
        json_object *t = json_object_array_get_idx(bc_get(p, "tests"), 0),
                    *s = json_object_array_get_idx(bc_get(t, "steps"), 0);
        json_object_object_add(p, "schema", json_object_new_int(1));
        CHECK(bs_project_schema(p) == 2);
        json_object *upgraded = bs_manifest(p, 0, &e);
        CHECK(upgraded && bc_num(bc_get(upgraded, "studio_source"), "schema") == 2);
        json_object_put(upgraded);
        json_object *op = bs_communication_operation(t, s, &e);
        CHECK(op);
        json_object *r = bs_communication_result(t, s, NULL, 1, &e);
        CHECK(r && json_object_get_boolean(bc_get(r, "matched")));
        CHECK(*bc_str(r, "unit"));
        json_object_put(r);
        json_object_put(op);
        if (i == 8) {
            text(s, "comm_channel", "missing");
            CHECK(bs_check(p, &e) < 0);
            text(s, "comm_channel", "sensores");
            json_object *channels = bc_get(t, "comm_channels");
            json_object_array_add(channels, bc_clone(json_object_array_get_idx(channels, 0)));
            CHECK(bs_check(p, &e) < 0);
        }
        json_object_put(p);
    }
    json_object *p = bs_new();
    CHECK(bc_num(p, "schema") == BS_SCHEMA);
    json_object_object_add(p, "schema", json_object_new_int(1));
    CHECK(bs_shape(p, &e) == 0);
    char tmp[] = "/tmp/bpcb-studio-schema-XXXXXX";
    int fd = mkstemp(tmp);
    CHECK(fd >= 0);
    close(fd);
    CHECK(bs_save(tmp, p, &e) == 0);
    json_object *saved = bc_read_json(tmp, &e);
    CHECK(saved && bc_num(saved, "schema") == 1);
    CHECK(bc_num(p, "schema") == 1);
    json_object_put(saved);
    unlink(tmp);
    json_object_object_add(p, "schema", json_object_new_int(BS_SCHEMA + 1));
    CHECK(bs_shape(p, &e) < 0);
    json_object_object_add(p, "schema", json_object_new_double(2));
    CHECK(bs_shape(p, &e) < 0);
    json_object_put(p);
    p = bs_new();
    text(bc_get(p, "board"), "connections", "Fixture sem hardware.");
    json_object *t = bs_defaults("test");
    text(t, "id", "cache");
    json_object_object_add(t, "comm_enabled", json_object_new_boolean(1));
    json_object_object_add(t, "parameters", json_object_new_array());
    json_object_object_add(t, "formulas", json_object_new_array());
    json_object *steps = json_object_new_array();
    json_object_object_add(t, "steps", steps);
    json_object_array_add(bc_get(p, "tests"), t);
    json_object *s = step("init", "calculate");
    text(s, "variable", "count");
    text(s, "expression", "0");
    json_object_array_add(steps, s);
    s = step("sensor", "communication");
    text(s, "variable", "temperature");
    text(s, "lab_mode", "peripheral");
    text(s, "lab_device", "bme280");
    text(s, "lab_metric", "temperature");
    json_object_array_add(steps, s);
    json_object *sensor = s;
    s = step("increment", "calculate");
    text(s, "variable", "count");
    text(s, "expression", "count+1");
    json_object_array_add(steps, s);
    s = step("repeat", "branch");
    text(s, "expression", "count");
    text(s, "operator", "lt");
    text(s, "right", "10");
    text(s, "target", "sensor");
    number(s, "repeat_limit", 10);
    json_object_array_add(steps, s);
    state f = {.matched = 1};
    bs_host host = {.context = &f,
                    .simulation = 0,
                    .clock = tick,
                    .stopped = stopped,
                    .sleep_ms = wait_,
                    .emit = emit_,
                    .confirm = confirm_,
                    .communicate = communicate};
    CHECK(bs_execute(p, "cache", NULL, NULL, &host, &e) == 0 && f.calls == 10 && f.readings == 10);
    CHECK(!strcmp(bc_str(sensor, "lab_device"), "bme280"));
    f = (state){.matched = 0};
    CHECK(bs_execute(p, "cache", NULL, NULL, &host, &e) < 0 && f.calls == 1 && f.readings == 1);
    f = (state){.matched = 1};
    host.simulation = 1;
    CHECK(bs_execute(p, "cache", NULL, NULL, &host, &e) == 0 && f.calls == 0 && f.readings == 10);
    number(sensor, "lab_address", 1e300);
    f = (state){.matched = 1};
    host.simulation = 0;
    CHECK(bs_execute(p, "cache", NULL, NULL, &host, &e) < 0 && f.calls == 0);
    json_object_put(p);
    printf("%u verificações avançadas Studio: nove exemplos, canais, unidades, versões, defaults, "
           "cache isolado, falhas e validação prévia.\n",
           checks);
    return 0;
}
