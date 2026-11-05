#define _POSIX_C_SOURCE 200809L
#include "studio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double monotonic(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int stopped_(void *context) {
    (void)context;
    return 0;
}

static double clock_(void *context) {
    (void)context;
    return 0;
}

static int sleep_(void *context, unsigned ms) {
    (void)context;
    (void)ms;
    return 0;
}

static int emit_(void *context, json_object *event) {
    if (!strcmp(bc_str(event, "type"), "measurement"))
        (*(unsigned *)context)++;
    return 0;
}

static int confirm_(void *context, const char *id, const char *text, int number, double min,
                    double max, double timeout, double *value) {
    (void)context;
    (void)id;
    (void)text;
    (void)number;
    (void)min;
    (void)max;
    (void)timeout;
    (void)value;
    return 0;
}

static void text(json_object *o, const char *k, const char *v) {
    json_object_object_add(o, k, json_object_new_string(v));
}

static json_object *step(const char *id, const char *type, const char *variable,
                         const char *expression) {
    json_object *s = bs_defaults("step");
    text(s, "id", id);
    text(s, "type", type);
    text(s, "title", id);
    text(s, "variable", variable);
    text(s, "expression", expression);
    return s;
}

int main(void) {
    bc_error e = {{0}};
    json_object *p = bs_new(), *board = bc_get(p, "board");
    text(board, "connections", "Benchmark simulado; sem hardware.");
    json_object *test = bs_defaults("test");
    text(test, "id", "benchmark");
    text(test, "instructions", "Benchmark sem renderização e sem hardware.");
    json_object_object_add(test, "comm_enabled", json_object_new_boolean(1));
    json_object_object_add(test, "parameters", json_object_new_array());
    json_object_object_add(test, "formulas", json_object_new_array());
    json_object *steps = json_object_new_array();
    json_object_object_add(test, "steps", steps);
    json_object_array_add(bc_get(p, "tests"), test);
    json_object_array_add(steps, step("init", "calculate", "counter", "0"));
    json_object *read = step("sensor", "communication", "medicao", "0");
    text(read, "lab_mode", "peripheral");
    text(read, "lab_device", "bme280");
    text(read, "lab_action", "measure");
    text(read, "lab_metric", "temperature");
    json_object_array_add(steps, read);
    json_object_array_add(steps, step("increment", "calculate", "counter", "counter + 1"));
    json_object *branch = step("loop", "branch", "counter", "counter");
    text(branch, "operator", "lt");
    text(branch, "right", "500");
    text(branch, "target", "sensor");
    json_object_object_add(branch, "repeat_limit", json_object_new_int(500));
    json_object_array_add(steps, branch);
    double times[5];
    for (unsigned i = 0; i < 5; i++) {
        unsigned readings = 0;
        bs_host host = {.context = &readings,
                        .simulation = 1,
                        .stopped = stopped_,
                        .clock = clock_,
                        .sleep_ms = sleep_,
                        .emit = emit_,
                        .confirm = confirm_};
        double start = monotonic();
        int rc = bs_execute(p, "benchmark", NULL, NULL, &host, &e);
        times[i] = monotonic() - start;
        if (rc || readings != 500) {
            fprintf(stderr, "%s; leituras=%u\n", e.text, readings);
            return 1;
        }
    }
    for (unsigned i = 0; i < 5; i++)
        for (unsigned j = i + 1; j < 5; j++)
            if (times[j] < times[i]) {
                double t = times[i];
                times[i] = times[j];
                times[j] = t;
            }
    printf("{\"samples\":5,\"sensor_operations_per_sample\":500,\"median_seconds\":%.9f,\"minimum_"
           "seconds\":%.9f,\"maximum_seconds\":%.9f,\"simulation\":true,\"hardware_accessed\":"
           "false}\n",
           times[2], times[0], times[4]);
    json_object_put(p);
    return 0;
}
