#include "studio.h"
#ifndef _WIN32
#include <sys/file.h>
#include <poll.h>
#endif
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdatomic.h>
static atomic_int cancelled;

typedef struct {
    bs_io *io;
    char input[1024 * 1024];
    size_t used;
    int simulation;
} context;

static void stop_signal(int sig) {
    (void)sig;
    cancelled = 1;
}

static int stopped(void *ctx) {
    (void)ctx;
    return cancelled != 0;
}

static double now(void *ctx) {
    (void)ctx;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int wait_ms(void *ctx, unsigned ms) {
    (void)ctx;
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) < 0) {
        if (errno != EINTR || cancelled)
            return -1;
    }
    return cancelled ? -1 : 0;
}

static int emit(void *ctx, json_object *v) {
    (void)ctx;
    const char *s = json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN);
    if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF || fflush(stdout) == EOF)
        return -1;
    return 0;
}

static json_object *receive(context *ctx, double deadline) {
    for (;;) {
        char *end = memchr(ctx->input, '\n', ctx->used);
        if (end) {
            size_t size = (size_t)(end - ctx->input);
            bc_error e = {{0}};
            json_object *o = bc_parse_json(ctx->input, size, &e);
            ctx->used -= size + 1;
            memmove(ctx->input, end + 1, ctx->used);
            return o;
        }
        if (cancelled || now(ctx) >= deadline || ctx->used == sizeof ctx->input)
            return NULL;
        struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
        double remaining = (deadline - now(ctx)) * 1000;
        int timeout = remaining > 1000 ? 1000 : (int)remaining;
        if (timeout < 1)
            timeout = 1;
        int rc = poll(&fd, 1, timeout);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }
        if (!rc)
            continue;
        ssize_t n = read(STDIN_FILENO, ctx->input + ctx->used, sizeof ctx->input - ctx->used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return NULL;
        ctx->used += (size_t)n;
    }
}

static int checkpoint(void *c, json_object *step, size_t index, unsigned visit,
                      json_object *variables, bc_error *e) {
    context *ctx = c;
    if (!ctx->simulation)
        return bc_fail(e, "Pausa de depuração bloqueada no hardware.");
    char id[96];
    snprintf(id, sizeof id, "debug_%llu_%u", (unsigned long long)index, visit);
    json_object *ev = json_object_new_object(), *state = json_object_new_object();
    size_t bytes = 0, count = 0;
    int truncated = 0;
    json_object_object_foreach(variables, k, v) {
        size_t n =
            strlen(k) + strlen(json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN)) + 8;
        if (count >= 256 || bytes + n > 20000) {
            truncated = 1;
            continue;
        }
        json_object_object_add(state, k, bc_clone(v));
        bytes += n;
        count++;
    }
    json_object_object_add(ev, "type", json_object_new_string("confirm"));
    json_object_object_add(ev, "id", json_object_new_string(id));
    json_object_object_add(ev, "debug", json_object_new_boolean(1));
    json_object_object_add(ev, "step_id", json_object_new_string(bc_str(step, "id")));
    json_object_object_add(ev, "step_index", json_object_new_int64((int64_t)index));
    json_object_object_add(ev, "visit", json_object_new_int64(visit));
    json_object_object_add(ev, "variables", state);
    json_object_object_add(ev, "variables_truncated", json_object_new_boolean(truncated));
    json_object_object_add(ev, "message", json_object_new_string(bc_str(step, "title")));
    int rc = emit(ctx, ev);
    json_object_put(ev);
    if (rc < 0)
        return bc_fail(e, "Canal de depuração fechado.");
    json_object *reply = receive(ctx, now(ctx) + 3600);
    rc = reply && !strcmp(bc_str(reply, "reply_to"), id) &&
         json_object_is_type(bc_get(reply, "accepted"), json_type_boolean) &&
         json_object_get_boolean(bc_get(reply, "accepted"));
    json_object_put(reply);
    return rc ? 0 : bc_fail(e, "Depuração interrompida, resposta inválida ou pausa expirada.");
}

static int confirm(void *c, const char *id, const char *message, int number, double min, double max,
                   double timeout, double *value) {
    context *ctx = c;
    json_object *ev = json_object_new_object();
    json_object_object_add(ev, "type", json_object_new_string("confirm"));
    json_object_object_add(ev, "id", json_object_new_string(id));
    json_object_object_add(ev, "message", json_object_new_string(message));
    json_object_object_add(ev, "number", json_object_new_boolean(number));
    json_object_object_add(ev, "min", json_object_new_double(min));
    json_object_object_add(ev, "max", json_object_new_double(max));
    json_object_object_add(ev, "timeout", json_object_new_double(timeout));
    if (number && ctx->simulation)
        json_object_object_add(ev, "sample", json_object_new_double(*value));
    int rc = emit(ctx, ev);
    json_object_put(ev);
    if (rc < 0)
        return -1;
    json_object *reply = receive(ctx, now(ctx) + timeout);
    int ok = reply && !strcmp(bc_str(reply, "reply_to"), id) &&
             json_object_get_boolean(bc_get(reply, "accepted"));
    if (ok && number) {
        json_object *v = bc_get(reply, "value");
        *value = json_object_get_double(v);
        ok = (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double)) &&
             isfinite(*value) && *value >= min && *value <= max;
    }
    json_object_put(reply);
    return ok ? 0 : -1;
}

static int confirm_detail(void *c, json_object *step, double *value, json_object **acquired) {
    context *ctx = c;
    json_object *ev = bc_clone(step);
    json_object_object_add(ev, "type", json_object_new_string("confirm"));
    json_object_object_add(ev, "number", json_object_new_boolean(1));
    json_object_object_add(ev, "measurement", json_object_new_boolean(1));
    if (!ctx->simulation)
        json_object_object_del(ev, "sample");
    else
        json_object_object_add(ev, "sample", json_object_new_double(*value));
    if (!ctx->simulation && !strcmp(bc_str(step, "source"), "rigol")) {
        char field[64];
        snprintf(field, sizeof field, "read:%s:%s", bc_str(step, "rigol_channel"),
                 bc_str(step, "rigol_metric"));
        json_object_object_add(ev, "instrument_request", json_object_new_string(field));
    }
    int rc = emit(ctx, ev);
    json_object_put(ev);
    if (rc < 0)
        return -1;
    json_object *reply = receive(ctx, now(ctx) + bc_num(step, "timeout"));
    int ok = reply && !strcmp(bc_str(reply, "reply_to"), bc_str(step, "id")) &&
             json_object_get_boolean(bc_get(reply, "accepted"));
    if (ok) {
        json_object *v = bc_get(reply, "value");
        *value = json_object_get_double(v);
        ok = (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double)) &&
             isfinite(*value);
    }
    if (ok && bc_get(reply, "acquisition"))
        *acquired = bc_clone(bc_get(reply, "acquisition"));
    json_object_put(reply);
    return ok ? 0 : -1;
}

static int read_value(void *ctx, json_object *d, json_object *s, double *v, bc_error *e) {
    return bs_io_read(((context *)ctx)->io, d, s, v, e);
}

static int write_value(void *ctx, json_object *d, json_object *s, double v, bc_error *e) {
    return bs_io_write(((context *)ctx)->io, d, s, v, e);
}

static int initialize(void *ctx, json_object *d, json_object *p, bc_error *e) {
    return bs_io_init(((context *)ctx)->io, d, p, e);
}

static json_object *communicate(void *ctx, json_object *operation, bc_error *e) {
    return bs_io_communicate(((context *)ctx)->io, operation, e);
}

int main(void) {
    bs_platform_init();
#ifdef _WIN32
    bc_win_watch_parent(stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
#else
    struct sigaction sa = {0};
    sa.sa_handler = stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#endif
    context *ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return 1;
    bc_error e = {{0}};
    int rc = -1, lock = -1;
    json_object *compiled = NULL, *manifest = NULL, *request = receive(ctx, now(ctx) + 10);
    if (!request || bc_num(request, "api") != 2) {
        bc_fail(&e, "Solicitação BPCB inválida.");
        goto done;
    }
    char path[4096];
    if (bc_path(path, sizeof path, bc_str(request, "package_dir"), "manifest.json", &e) < 0)
        goto done;
    manifest = bc_read_json(path, &e);
    if (!manifest || bc_validate(manifest, &e) < 0)
        goto done;
    json_object *test = bc_test(manifest, bc_str(request, "test"));
    json_object *p = bc_get(manifest, "studio_source");
    if (!p || !test) {
        bc_fail(&e, "Projeto ou teste ausente no pacote.");
        goto done;
    }
    int simulation = !strcmp(bc_str(request, "mode"), "simulation");
    ctx->simulation = simulation;
    if (bc_get(request, "studio_debug") &&
        (!json_object_is_type(bc_get(request, "studio_debug"), json_type_boolean) ||
         (!simulation && json_object_get_boolean(bc_get(request, "studio_debug"))))) {
        bc_fail(&e, "Depuração exige uma solicitação de simulação.");
        goto done;
    }
    if (!simulation && !strcmp(bc_str(test, "mode"), "simulation")) {
        bc_fail(&e, "Este teste foi publicado somente para simulação.");
        goto done;
    }
    compiled = bs_manifest(p, simulation, &e);
    if (!compiled)
        goto done;
    p = bc_get(compiled, "studio");
    if (!simulation) {
        lock = open(bc_bench_lock_path(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
            bc_fail(&e, "Outra ferramenta está usando o barramento da bancada.");
            goto done;
        }
    }
    bs_host host = {ctx,
                    simulation,
                    stopped,
                    now,
                    wait_ms,
                    emit,
                    confirm,
                    read_value,
                    write_value,
                    initialize,
                    bc_get(request, "calibration"),
                    confirm_detail,
                    communicate,
                    json_object_get_boolean(bc_get(request, "studio_debug")) ? checkpoint : NULL};
    ctx->io = bs_io_new(&host);
    if (!ctx->io) {
        bc_fail(&e, "Memória insuficiente.");
        goto done;
    }
    if (!simulation) {
        json_object *definition =
            bc_get(bc_test(compiled, bc_str(request, "test")), "communication");
        if (bs_io_communication_setup(ctx->io, bc_get(request, "communication"), definition, &e) <
            0)
            goto done;
        json_object *steps = bc_get(bs_find(bc_get(p, "tests"), bc_str(request, "test")), "steps");
        for (size_t i = 0; i < json_object_array_length(steps); i++) {
            json_object *s = json_object_array_get_idx(steps, i);
            if (!strcmp(bc_str(s, "type"), "communication")) {
                json_object *op = bs_communication_operation(
                    bs_find(bc_get(p, "tests"), bc_str(request, "test")), s, &e);
                if (!op)
                    goto done;
                int check =
                    bc_comm_operation_check(bc_get(request, "communication"), definition, op, &e);
                json_object_put(op);
                if (check < 0)
                    goto done;
            }
        }
    }
    rc = bs_execute(p, bc_str(request, "test"), bc_get(request, "parameters"),
                    bc_get(request, "components"), &host, &e);
done:
    {
        json_object *ev = json_object_new_object();
        json_object_object_add(ev, "type", json_object_new_string(rc ? "error" : "done"));
        json_object_object_add(
            ev, "message",
            json_object_new_string(
                rc ? e.text : "Sequência concluída. Resultados identificam se houve simulação."));
        emit(ctx, ev);
        json_object_put(ev);
    }
    bs_io_free(ctx->io);
    if (lock >= 0)
        close(lock);
    json_object_put(request);
    json_object_put(manifest);
    json_object_put(compiled);
    free(ctx);
    return rc ? 1 : 0;
}
