#define _POSIX_C_SOURCE 200809L
#include "studio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static bc_error e;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "STUDIO WORKBENCH %d: %s (%s)\n", __LINE__, #x, e.text);               \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static void text(json_object *o, const char *k, const char *v) {
    json_object_object_add(o, k, json_object_new_string(v));
}

typedef struct {
    double time, pause;
    unsigned visits, results;
    int stop, reject;
    double count;
} state;

static int stopped(void *p) {
    return ((state *)p)->stop;
}

static double clock_(void *p) {
    return ((state *)p)->time;
}

static int sleep_(void *p, unsigned ms) {
    ((state *)p)->time += ms / 1000.0;
    return 0;
}

static int emit_(void *p, json_object *ev) {
    state *s = p;
    if (!strcmp(bc_str(ev, "type"), "measurement")) {
        s->results++;
        s->count = bc_num(ev, "value");
    }
    return 0;
}

static int checkpoint(void *p, json_object *step, size_t index, unsigned visit, json_object *vars,
                      bc_error *error) {
    state *s = p;
    CHECK(index < 4 && visit == ++s->visits && *bc_str(step, "id"));
    if (visit == 1)
        CHECK(!bc_get(vars, "count"));
    s->time += s->pause;
    return s->reject ? bc_fail(error, "Injected cancellation") : 0;
}

static json_object *step(const char *id, const char *type) {
    json_object *r = bs_defaults("step");
    text(r, "id", id);
    text(r, "type", type);
    text(r, "title", id);
    return r;
}

static void debugger(void) {
    json_object *p = bs_new();
    text(bc_get(p, "board"), "connections", "Sem hardware: fixture de depuração.");
    json_object *t = bs_defaults("test");
    text(t, "id", "debug");
    text(t, "instructions", "Fixture sem hardware.");
    json_object_object_add(t, "parameters", json_object_new_array());
    json_object_object_add(t, "formulas", json_object_new_array());
    json_object *a = json_object_new_array();
    json_object_object_add(t, "steps", a);
    json_object_array_add(bc_get(p, "tests"), t);
    json_object *s = step("init", "calculate");
    text(s, "variable", "count");
    text(s, "expression", "0");
    json_object_array_add(a, s);
    s = step("increment", "calculate");
    text(s, "variable", "count");
    text(s, "expression", "count+1");
    json_object_array_add(a, s);
    s = step("loop", "branch");
    text(s, "expression", "count");
    text(s, "operator", "lt");
    text(s, "right", "3");
    text(s, "target", "increment");
    json_object_array_add(a, s);
    s = step("result", "result");
    text(s, "expression", "count");
    text(s, "reference", "3");
    json_object_array_add(a, s);
    CHECK(bs_check(p, &e) == 0);
    state st = {.pause = 3600};
    bs_host host = {.context = &st,
                    .simulation = 1,
                    .stopped = stopped,
                    .clock = clock_,
                    .sleep_ms = sleep_,
                    .emit = emit_,
                    .checkpoint = checkpoint};
    CHECK(bs_execute(p, "debug", NULL, NULL, &host, &e) == 0 && st.visits == 8 && st.results == 1 &&
          st.count == 3);
    st = (state){.reject = 1};
    CHECK(bs_execute(p, "debug", NULL, NULL, &host, &e) < 0 && st.visits == 1 && st.results == 0);
    st = (state){.stop = 1};
    CHECK(bs_execute(p, "debug", NULL, NULL, &host, &e) < 0 && st.visits == 0);
    st = (state){.pause = -1};
    CHECK(bs_execute(p, "debug", NULL, NULL, &host, &e) < 0 && st.visits == 1);
    st = (state){.pause = 7201};
    CHECK(bs_execute(p, "debug", NULL, NULL, &host, &e) < 0 && st.visits == 1);
    st = (state){0};
    host.simulation = 0;
    CHECK(bs_execute(p, "debug", NULL, NULL, &host, &e) < 0 && st.visits == 0 && st.results == 0);
    host.simulation = 1;
    host.checkpoint = NULL;
    CHECK(bs_execute(p, "debug", NULL, NULL, &host, &e) == 0 && st.count == 3);
    json_object_put(p);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    debugger();
    char file[4096];
    snprintf(file, sizeof file, "%s/examples/Laboratorio/AT.bpcb", argv[1]);
    json_object *p = bs_load(file, &e);
    CHECK(p);
    json_object *t = json_object_array_get_idx(bc_get(p, "tests"), 0),
                *s = json_object_array_get_idx(bc_get(t, "steps"), 0);
    text(s, "lab_protocol", "frame");
    json_object_object_add(s, "lab_frame", bc_frame_example());
    text(s, "lab_frame_metric", "temperature");
    text(s, "lab_frame_sample", "AA 55 C4 09 E4 0C 01 BD");
    CHECK(bs_check(p, &e) == 0);
    json_object *op = bs_communication_operation(t, s, &e);
    CHECK(op && !strcmp(bc_str(op, "protocol"), "frame"));
    json_object_put(op);
    json_object *r = bs_communication_result(t, s, NULL, 1, &e);
    CHECK(r && json_object_get_boolean(bc_get(r, "matched")) && bc_num(r, "value") == 25);
    json_object_put(r);
    text(s, "lab_frame_sample", "AA 55 C4 09 E4 0C 01 BC");
    r = bs_communication_result(t, s, NULL, 1, &e);
    CHECK(r && !json_object_get_boolean(bc_get(r, "matched")));
    json_object_put(r);
    text(s, "lab_frame_sample", "AA 55 C4 09 E4 0C 01 BD");
    json_object_object_add(p, "schema", json_object_new_int(2));
    CHECK(bs_project_schema(p) == 3);
    json_object *m = bs_manifest(p, 0, &e);
    CHECK(m && !strcmp(bc_str(m, "min_app"), "0.16.0"));
    json_object_put(m);
    json_object *rows = json_object_new_array(), *pin = bs_defaults("connection");
    json_object_array_add(rows, pin);
    json_object_object_add(t, "wiring", rows);
    CHECK(bs_check(p, &e) == 0);
    m = bs_manifest(p, 0, &e);
    CHECK(m && json_object_array_length(
                   bc_get(json_object_array_get_idx(bc_get(m, "tests"), 0), "wiring")) == 1);
    CHECK(strstr(bc_str(json_object_array_get_idx(bc_get(m, "tests"), 0), "instructions"),
                 "MAPA DE CONEX"));
    json_object_put(m);
    json_object_array_add(rows, bc_clone(pin));
    CHECK(bs_check(p, &e) < 0);
    json_object_put(p);
    printf(
        "%u verificações do depurador, prazo, cancelamento, isolamento físico, quadros e mapas.\n",
        checks);
    return 0;
}
