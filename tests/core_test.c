#include "studio.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void str(json_object *o, const char *k, const char *v) {
    json_object_object_add(o, k, json_object_new_string(v));
}

static void num(json_object *o, const char *k, double v) {
    json_object_object_add(o, k, json_object_new_double(v));
}

static json_object *step(const char *id, const char *type) {
    json_object *s = bs_defaults("step");
    str(s, "id", id);
    str(s, "type", type);
    str(s, "title", id);
    return s;
}

typedef struct {
    double time, values[8], measured;
    int writes, reads, confirmations, fail_on_write, cancel_after_write, stopped, results, init;
    double commands[64];
    unsigned channels[64];
    int reject_confirm, samples, states, acquisitions;
    double last_sample, external;
} mock;

static int stopped(void *p) {
    return ((mock *)p)->stopped;
}

static double clock_now(void *p) {
    return ((mock *)p)->time;
}

static int sleep_ms(void *p, unsigned n) {
    ((mock *)p)->time += (double)n / 1000;
    return ((mock *)p)->stopped ? -1 : 0;
}

static int event(void *p, json_object *v) {
    mock *m = p;
    if (bc_get(v, "variables"))
        m->states++;
    if (bc_get(v, "acquisition"))
        m->acquisitions++;
    if (!strcmp(bc_str(v, "type"), "sample")) {
        m->samples++;
        json_object *values = bc_get(v, "values");
        json_object_object_foreach(values, key, value) {
            (void)key;
            m->last_sample = json_object_get_double(value);
        }
    }
    if (!strcmp(bc_str(v, "type"), "measurement")) {
        m->results++;
        m->measured = bc_num(v, "value");
    }
    return 0;
}

static int confirm(void *p, const char *id, const char *message, int number, double min, double max,
                   double timeout, double *v) {
    (void)message;
    (void)min;
    (void)max;
    (void)timeout;
    mock *m = p;
    m->confirmations++;
    if (m->reject_confirm)
        return -1;
    if (number)
        *v = !strcmp(id, "entrada") ? 5 : 2.5;
    return 0;
}

static int read_value(void *p, json_object *d, json_object *s, double *v, bc_error *e) {
    (void)d;
    (void)e;
    mock *m = p;
    m->reads++;
    *v = m->values[(unsigned)bc_num(s, "channel")];
    return 0;
}

static int write_value(void *p, json_object *d, json_object *s, double v, bc_error *e) {
    (void)d;
    mock *m = p;
    int at = m->writes++;
    m->commands[at] = v;
    m->channels[at] = (unsigned)bc_num(s, "channel");
    m->values[m->channels[at]] = v;
    if (m->cancel_after_write == m->writes)
        m->stopped = 1;
    if (m->fail_on_write == m->writes)
        return bc_fail(e, "Injected failure");
    return 0;
}

static int init(void *p, json_object *d, json_object *project, bc_error *e) {
    (void)d;
    (void)project;
    (void)e;
    ((mock *)p)->init++;
    return 0;
}

static bs_host host(mock *m, int sim) {
    return (bs_host){m,          sim,         stopped, clock_now, sleep_ms, event, confirm,
                     read_value, write_value, init,    NULL,      NULL,     NULL,  NULL};
}

static json_object *hardware(void) {
    json_object *p = bs_example(), *d = bs_defaults("device");
    str(d, "id", "io");
    str(d, "driver", "tca9534a");
    num(d, "output_mask", 3);
    num(d, "initial_output", 0);
    json_object_array_add(bc_get(p, "devices"), d);
    for (unsigned i = 0; i < 2; i++) {
        json_object *s = bs_defaults("signal");
        str(s, "id", i ? "b" : "a");
        str(s, "device", "io");
        str(s, "label", i ? "Saída B" : "Saída A");
        str(s, "location", i ? "TP2" : "TP1");
        str(s, "return", "GND");
        str(s, "direction", "output");
        str(s, "unit", "nível");
        str(s, "interlock", "grupo");
        num(s, "channel", i);
        num(s, "min", 0);
        num(s, "max", 1);
        num(s, "safe", 0);
        num(s, "simulate", 0);
        num(s, "stop_order", i ? 1 : 0);
        json_object_array_add(bc_get(p, "signals"), s);
    }
    json_object *t = bs_find(bc_get(p, "tests"), "divisor"), *steps = json_object_new_array();
    str(t, "mode", "hardware");
    json_object_object_add(t, "steps", steps);
    json_object *s = step("on", "write");
    str(s, "signal", "a");
    str(s, "expression", "1");
    json_object_array_add(steps, s);
    s = step("wait", "delay");
    num(s, "seconds", .1);
    json_object_array_add(steps, s);
    s = step("off", "write");
    str(s, "signal", "a");
    str(s, "expression", "0");
    json_object_array_add(steps, s);
    return p;
}

static void test_model(void) {
    bc_error e = {{0}};
    json_object *p = bs_new();
    CHECK(bs_check(p, &e) < 0);
    json_object_put(p);
    p = bs_example();
    CHECK(bs_check(p, &e) == 0);
    json_object *m = bs_manifest(p, 1, &e);
    if (!m)
        fprintf(stderr, "%s\n", e.text);
    CHECK(m != NULL);
    CHECK(bc_validate(m, &e) == 0);
    json_object *test = bc_test(m, "divisor"), *preview = bc_preview(m, test, &e);
    CHECK(preview != NULL);
    CHECK(fabs(bc_num(bc_get(preview, "values"), "expected_mid") - 2.5) < 1e-9);
    json_object_put(preview);
    json_object *vars = json_object_new_object();
    num(vars, "x", 123.45);
    char *r = bs_render("Valor {x:.2f}", vars, &e);
    CHECK(r && strstr(r, "123.45"));
    free(r);
    json_object_put(vars);
    double v;
    CHECK(bs_quantity("4,4 kΩ", "resistor", &v) == 0 && v == 4400);
    CHECK(bs_quantity("3u3", "capacitor", &v) < 0);
    CHECK(bs_quantity("4k7", "resistor", &v) < 0);
    CHECK(bs_quantity("3.3 µF", "capacitor", &v) == 0 && fabs(v - 3.3e-6) < 1e-15);
    CHECK(bs_quantity("NaN", "resistor", &v) < 0);
    CHECK(bs_quantity("1 kV", "resistor", &v) < 0);
    json_object *form = bs_defaults("parameter"), *normalized = NULL;
    str(form, "default", "4,4");
    num(form, "max", 10);
    CHECK(bs_form("parameter", form, &normalized, &e) == 0 && bc_num(normalized, "default") == 4.4);
    json_object_put(normalized);
    str(form, "default", "nan");
    CHECK(bs_form("parameter", form, &normalized, &e) < 0);
    json_object_put(form);
    json_object *comp = json_object_array_get_idx(bc_get(p, "components"), 0);
    str(comp, "value", "4k7");
    CHECK(bs_check(p, &e) < 0);
    str(comp, "value", "1k");
    json_object_put(m);
    json_object_put(p);
    p = hardware();
    if (bs_check(p, &e))
        fprintf(stderr, "%s\n", e.text);
    CHECK(bs_check(p, &e) == 0);
    m = bs_manifest(p, 0, &e);
    if (!m)
        fprintf(stderr, "%s\n", e.text);
    CHECK(m != NULL);
    CHECK(json_object_array_length(bc_get(m, "tests")) == 3);
    CHECK(bs_find(bc_get(bc_get(m, "studio"), "tests"), "specific_a") != NULL);
    json_object_put(m);
    str(bs_find(bc_get(p, "tests"), "divisor"), "mode", "simulation");
    m = bs_manifest(p, 0, &e);
    CHECK(m != NULL);
    json_object *all = bc_get(m, "tests");
    for (size_t i = 0; i < json_object_array_length(all); i++)
        CHECK(!strcmp(bc_str(json_object_array_get_idx(all, i), "mode"), "simulation"));
    num(json_object_array_get_idx(bc_get(p, "signals"), 0), "safe", 2);
    CHECK(bs_check(p, &e) < 0);
    json_object_put(m);
    json_object_put(p);
}

static void test_engine(void) {
    bc_error e = {{0}};
    json_object *p = bs_example(), *params = json_object_new_object(),
                *components = json_object_new_object();
    mock m = {0};
    bs_host h = host(&m, 1);
    CHECK(bs_execute(p, "divisor", params, components, &h, &e) == 0);
    CHECK(m.results == 1 && fabs(m.measured - 1000) < 1e-9);
    CHECK(m.writes == 0 && m.reads == 0);
    str(components, "R1", "2.2 kΩ");
    m = (mock){0};
    CHECK(bs_execute(p, "divisor", params, components, &h, &e) == 0 && m.measured == 2200);
    num(params, "voltage", 100);
    m = (mock){0};
    CHECK(bs_execute(p, "divisor", params, components, &h, &e) < 0 && m.confirmations == 0);
    json_object_put(params);
    json_object_put(components);
    json_object_put(p);
    p = hardware();
    h = host(&m, 0);
    m = (mock){0};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) == 0);
    CHECK(m.writes == 3 && m.commands[0] == 1 && m.commands[1] == 0 && m.commands[2] == 0);
    m = (mock){.fail_on_write = 1};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0);
    CHECK(m.writes == 2 && m.commands[1] == 0);
    m = (mock){.cancel_after_write = 1};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0);
    CHECK(m.writes == 2 && m.commands[1] == 0);
    m = (mock){.reject_confirm = 1};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && m.writes == 0);
    m = (mock){0};
    m.values[1] = 1;
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && m.writes == 0);
    json_object *t = bs_find(bc_get(p, "tests"), "divisor"), *ss = bc_get(t, "steps");
    json_object *wait = step("feedback", "wait");
    str(wait, "signal", "b");
    str(wait, "variable", "feedback");
    num(wait, "min", 1);
    num(wait, "max", 1);
    num(wait, "timeout", .1);
    json_object_array_put_idx(ss, 1, wait);
    m = (mock){0};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && strstr(e.text, "Tempo esgotado"));
    CHECK(m.commands[m.writes - 1] == 0);
    json_object_put(p);
    p = bs_example();
    t = bs_find(bc_get(p, "tests"), "divisor");
    ss = json_object_new_array();
    json_object_object_add(t, "steps", ss);
    json_object *s = step("start", "calculate");
    str(s, "variable", "counter");
    str(s, "expression", "0");
    json_object_array_add(ss, s);
    s = step("increment", "calculate");
    str(s, "variable", "counter");
    str(s, "expression", "counter + 1");
    json_object_array_add(ss, s);
    s = step("loop", "branch");
    str(s, "expression", "counter");
    str(s, "operator", "lt");
    str(s, "right", "3");
    str(s, "target", "increment");
    num(s, "repeat_limit", 5);
    json_object_array_add(ss, s);
    s = step("value", "result");
    str(s, "expression", "counter");
    str(s, "unit", "vezes");
    json_object_array_add(ss, s);
    CHECK(bs_check(p, &e) == 0);
    h = host(&m, 1);
    m = (mock){0};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) == 0 && m.measured == 3);
    num(json_object_array_get_idx(ss, 2), "repeat_limit", 1);
    m = (mock){0};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && strstr(e.text, "repetições"));
    json_object_put(p);
}

static void job_wait(const char *root, const char *id, int cancel) {
    bc_job *j = bc_job_start("run", root, id, "divisor", "", NULL);
    CHECK(j != NULL);
    int results = 0, finished = 0, ok = 0;
    double measure = 0;
    time_t deadline = time(NULL) + 20;
    while (!bc_job_finished(j) && time(NULL) < deadline) {
#ifdef _WIN32
        Sleep(20);
#else
        struct pollfd pfd = {bc_job_notification_fd(j), POLLIN, 0};
        poll(&pfd, 1, 1000);
#endif
        bc_job_acknowledge(j);
        json_object *v;
        while ((v = bc_job_next(j))) {
            if (!strcmp(bc_str(v, "type"), "confirm")) {
                if (cancel)
                    bc_job_cancel(j);
                else if (json_object_get_boolean(bc_get(v, "number")))
                    bc_job_reply_value(j, bc_str(v, "id"), 1,
                                       !strcmp(bc_str(v, "id"), "entrada") ? 5 : 2.5);
                else
                    bc_job_reply(j, bc_str(v, "id"), 1);
            }
            if (!strcmp(bc_str(v, "type"), "measurement")) {
                results++;
                measure = bc_num(v, "value");
                CHECK(!strcmp(bc_str(v, "source"), "Simulação"));
                CHECK(!strcmp(bc_str(v, "verdict"), "pass"));
            }
            if (!strcmp(bc_str(v, "type"), "finished")) {
                finished = 1;
                ok = !strcmp(bc_str(v, "status"), "ok");
            }
            json_object_put(v);
        }
    }
    json_object *v;
    while ((v = bc_job_next(j))) {
        if (!strcmp(bc_str(v, "type"), "finished")) {
            finished = 1;
            ok = !strcmp(bc_str(v, "status"), "ok");
        }
        json_object_put(v);
    }
    CHECK(bc_job_finished(j) && finished);
    if (cancel)
        CHECK(!ok && results == 0);
    else
        CHECK(ok && results == 1 && measure == 1000);
    bc_job_free(j);
}

static void test_package(const char *worker) {
    bc_error e = {{0}};
    char root[4096];
    CHECK(bs_temp_dir(root, "test", &e) == 0);
    char project_path[4096], package[4096], svg[4096], installed[4096], imported[4096];
    CHECK(bc_path(project_path, sizeof project_path, root, "project.bpcb", &e) == 0);
    CHECK(bc_path(package, sizeof package, root, "example.bancada", &e) == 0);
    CHECK(bc_path(svg, sizeof svg, root, "drawing.svg", &e) == 0);
    CHECK(bc_path(installed, sizeof installed, root, "installed", &e) == 0);
    CHECK(bc_path(imported, sizeof imported, root, "reimport/project.bpcb", &e) == 0);
    FILE *f = fopen(svg, "w");
    CHECK(f != NULL);
    fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"200\"><rect x=\"20\" "
          "y=\"20\" width=\"360\" height=\"160\" fill=\"none\" stroke=\"black\"/><text x=\"60\" "
          "y=\"100\">BPCB Studio - esquema</text></svg>",
          f);
    fclose(f);
    json_object *p = bs_example();
    CHECK(bs_document(p, project_path, svg, "Esquema de exemplo", "schematic", &e) == 0);
    CHECK(bs_save(project_path, p, &e) == 0);
    json_object *loaded = bs_load(project_path, &e);
    CHECK(loaded != NULL);
    CHECK(json_object_equal(p, loaded));
    json_object_put(loaded);
    json_object *preview = bs_document_preview(svg, 0, &e);
    CHECK(preview != NULL);
    CHECK(bc_num(preview, "pages") == 1);
    bs_remove_temp(bc_str(preview, "root"));
    json_object_put(preview);
    CHECK(bs_export(p, project_path, worker, package, 1, &e) == 0);
    json_object *m = bc_inspect(package, &e);
    if (!m)
        fprintf(stderr, "%s\n", e.text);
    CHECK(m != NULL);
    CHECK(json_object_array_length(bc_get(m, "documents")) == 2);
    CHECK(bc_install(installed, package, bc_str(m, "package_digest"), &e) == 0);
    char folder[4096];
    json_object *profile = bc_profile(installed, "exemplo_divisor", folder, &e);
    CHECK(profile != NULL && bc_verify(profile, folder, &e) == 0);
    job_wait(installed, "exemplo_divisor", 0);
    job_wait(installed, "exemplo_divisor", 1);
    json_object *r = bs_import(package, imported, &e);
    CHECK(r != NULL && json_object_equal(p, r));
    char package2[4096];
    CHECK(bc_path(package2, sizeof package2, root, "repacked.bancada", &e) == 0);
    CHECK(bs_export(r, imported, worker, package2, 0, &e) == 0);
    json_object *again = bc_inspect(package2, &e);
    CHECK(again != NULL);
    json_object_put(again);
    json_object_put(r);
    json_object_put(profile);
    json_object_put(m);
    json_object_put(p);
    bs_remove_temp(root);
}

#include "evolution.inc"

int main(int argc, char **argv) {
    bs_platform_init();
    CHECK(argc == 2);
    test_model();
    test_engine();
    test_evolution();
    test_continuity();
    test_package(argv[1]);
    printf("PASS: %u verificações do núcleo, executor e pacote.\n", checks);
    return 0;
}
