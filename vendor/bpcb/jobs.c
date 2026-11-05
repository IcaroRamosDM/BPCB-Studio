#define _POSIX_C_SOURCE 200809L
#include "bancada.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#ifndef _WIN32
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
extern char **environ;
#endif
static void (*wakeup_callback)(void);

void bc_set_wakeup(void (*callback)(void)) {
    wakeup_callback = callback;
}

void bc_wakeup(void) {
    if (wakeup_callback)
        wakeup_callback();
}

typedef struct event {
    json_object *o;
    struct event *next;
} event;

struct bc_job {
    pthread_t thread;
    pthread_mutex_t mutex;
    atomic_int stop, finished;
    event *first, *last;
    unsigned queued;
    int notify_fd[2];
    char *operation, *root, *id, *path, *extra;
    json_object *values;
    char reply_id[128];
    int reply, reply_ready, reply_has_value;
    double reply_value;
};

static void notify(bc_job *j) {
#ifndef _WIN32
    const unsigned char byte = 1;
    /* Pipe nao bloqueante: varios eventos podem compartilhar o mesmo aviso. */
    while (write(j->notify_fd[1], &byte, 1) < 0 && errno == EINTR) {
    }
#else
    (void)j;
    bc_wakeup();
#endif
}

static void emit(bc_job *j, json_object *o) {
    event *ev = calloc(1, sizeof *ev);
    if (!ev) {
        json_object_put(o);
        atomic_store(&j->stop, 1);
        return;
    }
    ev->o = o;
    pthread_mutex_lock(&j->mutex);
    if (j->queued > 512) {
        pthread_mutex_unlock(&j->mutex);
        free(ev);
        json_object_put(o);
        atomic_store(&j->stop, 1);
        return;
    }
    if (j->last)
        j->last->next = ev;
    else
        j->first = ev;
    j->last = ev;
    j->queued++;
    pthread_mutex_unlock(&j->mutex);
    notify(j);
}

static json_object *message(const char *type, const char *text) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "type", json_object_new_string(type));
    json_object_object_add(o, "message", json_object_new_string(text));
    return o;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int sample_valid(json_object *event) {
    json_object *values = bc_get(event, "values");
    if (!json_object_is_type(values, json_type_object) || json_object_object_length(values) > 16 ||
        !*bc_str(event, "unit") || strlen(bc_str(event, "unit")) > 32)
        return 0;
    json_object_object_foreach(values, key, value) {
        if (!*key || strlen(key) > 160 ||
            (!json_object_is_type(value, json_type_double) &&
             !json_object_is_type(value, json_type_int)) ||
            !isfinite(json_object_get_double(value)))
            return 0;
    }
    const char *keys[] = {"elapsed", "target", "limit"};
    for (unsigned i = 0; i < 3; i++) {
        json_object *v = bc_get(event, keys[i]);
        if (v &&
            ((!json_object_is_type(v, json_type_double) &&
              !json_object_is_type(v, json_type_int)) ||
             !isfinite(json_object_get_double(v)) || (i == 0 && json_object_get_double(v) < 0)))
            return 0;
    }
    return 1;
}

static int native_run(bc_job *j, json_object **result, bc_error *e) {
    char folder[4096], exe[4096], lockfile[4096];
    json_object *m = bc_profile(j->root, j->id, folder, e);
    if (!m)
        return -1;
    json_object *profile = bc_apply(m, j->values, e);
    json_object_put(m);
    if (!profile)
        return -1;
    json_object *t = bc_test(profile, j->path);
    int rc = -1, lock = -1, in[2] = {-1, -1}, out[2] = {-1, -1}, status = 0, reaped = 0, ended = 0,
        pending = 0, eof = 0;
#ifdef _WIN32
    bc_win_child child = {.diagnostic_fd = -1};
#else
    pid_t pid = -1;
#endif
    char pending_id[128] = "";
    char *send = NULL;
    size_t send_at = 0, send_size = 0;
    json_object *snapshot = NULL, *request = NULL;
    double stop_at = 0;
    if (!t || !json_object_get_boolean(bc_get(t, "enabled"))) {
        bc_fail(e, "Esta rotina ainda nao esta habilitada no pacote.");
        goto done;
    }
    if (bc_verify(profile, folder, e) < 0 ||
        bc_path(exe, sizeof exe, folder, bc_str(t, "entry"), e) < 0 ||
        bc_path(lockfile, sizeof lockfile, j->root, "execution.lock", e) < 0)
        goto done;
    lock = open(lockfile, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        bc_fail(e, "Ja existe um teste em execucao.");
        goto done;
    }
    if (atomic_load(&j->stop)) {
        rc = 1;
        goto done;
    }
    request = json_object_new_object();
    json_object_object_add(request, "api", json_object_new_int(BC_API));
    json_object_object_add(request, "test", json_object_new_string(j->path));
    json_object_object_add(request, "package_dir", json_object_new_string(folder));
    json_object_object_add(request, "components", json_object_get(bc_get(profile, "components")));
    json_object_object_add(request, "mode", json_object_new_string(bc_str(t, "mode")));
    json_object *parameters = json_object_new_object(), *params = bc_get(t, "parameters");
    for (size_t i = 0; i < json_object_array_length(params); i++) {
        json_object *p = json_object_array_get_idx(params, i);
        json_object_object_add(parameters, bc_str(p, "id"), json_object_get(bc_get(p, "default")));
    }
    json_object_object_add(request, "parameters", parameters);
    if (bc_get(j->values, "studio_debug") &&
        !json_object_is_type(bc_get(j->values, "studio_debug"), json_type_boolean)) {
        bc_fail(e, "Opção de depuração deve ser booleana.");
        goto done;
    }
    int studio_debug = json_object_get_boolean(bc_get(j->values, "studio_debug"));
    if (studio_debug) {
        if (strcmp(bc_str(t, "mode"), "simulation") || !bc_get(profile, "studio")) {
            bc_fail(e, "Depuração exige simulação do Studio.");
            goto done;
        }
        json_object_object_add(request, "studio_debug", json_object_new_boolean(1));
    }
    snapshot = bc_clone(request);
    json_object_object_add(snapshot, "pcb", json_object_new_string(bc_str(profile, "pcb")));
    json_object_object_add(snapshot, "revision",
                           json_object_new_string(bc_str(profile, "revision")));
    json_object_object_add(snapshot, "version", json_object_new_string(bc_str(profile, "version")));
    json_object *start = message("snapshot", bc_str(t, "title"));
    json_object_object_add(start, "data", json_object_get(snapshot));
    emit(j, start);
    const char *text = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
    send_size = strlen(text) + 1;
    send = malloc(send_size + 1);
    if (!send) {
        bc_fail(e, "Memoria insuficiente.");
        goto done;
    }
    snprintf(send, send_size + 1, "%s\n", text);
#ifdef _WIN32
    if (bc_win_spawn(exe, &in[1], &out[0], &child, e->text, sizeof e->text))
        goto done;
#else
    if (pipe(in) < 0 || pipe(out) < 0) {
        bc_fail(e, "Criar canal da rotina: %s", strerror(errno));
        goto done;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(in[i], F_SETFD, FD_CLOEXEC);
        fcntl(out[i], F_SETFD, FD_CLOEXEC);
    }
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out[1], STDERR_FILENO);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    char *argv[] = {exe, NULL};
    int spawn = posix_spawn(&pid, exe, &fa, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    in[0] = -1;
    close(out[1]);
    out[1] = -1;
    if (spawn) {
        pid = -1;
        bc_fail(e, "Executar rotina: %s", strerror(spawn));
        goto done;
    }
    fcntl(in[1], F_SETFL, O_NONBLOCK);
    fcntl(out[0], F_SETFL, O_NONBLOCK);
#endif
    double timeout = bc_num(t, "timeout");
    if (timeout <= 0 || timeout > 3600)
        timeout = 60;
    double deadline = now() + timeout, debug_last = now(), debug_hard = now() + 7200;
    int debug_pending = 0;
    char buffer[65536];
    size_t used = 0;
    unsigned measures = 0;
    while (!(reaped && eof)) {
#ifdef _WIN32
        char diagnostic[1024];
        int bytes = child.diagnostic_fd >= 0
                        ? bc_win_read(child.diagnostic_fd, diagnostic, sizeof diagnostic - 1)
                        : 0;
        if (bytes > 0) {
            diagnostic[bytes] = 0;
            emit(j, message("status", diagnostic));
        }
#endif
        double instant = now();
        if (debug_pending)
            deadline += instant - debug_last;
        debug_last = instant;
        if (studio_debug && instant > debug_hard) {
            bc_fail(e, "Limite de duas horas da sessão de depuração atingido.");
            atomic_store(&j->stop, 1);
        }
        if (instant > deadline && !stop_at) {
            bc_fail(e, "Tempo limite da rotina excedido; estado fisico nao confirmado.");
            atomic_store(&j->stop, 1);
        }
        if (atomic_load(&j->stop) && !stop_at) {
            stop_at = instant;
#ifdef _WIN32
            bc_win_stop(&child, 0);
#else
            kill(-pid, SIGTERM);
#endif
            emit(j, message("status", "Parada solicitada. Estado fisico nao confirmado."));
        }
#ifdef _WIN32
        if (stop_at && instant - stop_at > 8.0)
            bc_win_stop(&child, 1);
#else
        if (stop_at && instant - stop_at > 8.0)
            kill(-pid, SIGKILL);
#endif
        if (stop_at && instant - stop_at > 9.0)
            break;
        if (send_at < send_size && in[1] >= 0) {
            ssize_t n = write(in[1], send + send_at, send_size - send_at);
            if (n > 0)
                send_at += (size_t)n;
            else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                close(in[1]);
                in[1] = -1;
            }
        }
        pthread_mutex_lock(&j->mutex);
        if (j->reply_ready) {
            if (pending && !strcmp(j->reply_id, pending_id) && send_at == send_size) {
                json_object *reply = json_object_new_object();
                json_object_object_add(reply, "reply_to", json_object_new_string(j->reply_id));
                json_object_object_add(reply, "accepted", json_object_new_boolean(j->reply));
                if (j->reply_has_value)
                    json_object_object_add(reply, "value", json_object_new_double(j->reply_value));
                const char *s = json_object_to_json_string_ext(reply, JSON_C_TO_STRING_PLAIN);
                free(send);
                send_size = strlen(s) + 1;
                send = malloc(send_size + 1);
                send_at = 0;
                if (send)
                    snprintf(send, send_size + 1, "%s\n", s);
                else {
                    send_size = 0;
                    atomic_store(&j->stop, 1);
                }
                json_object_put(reply);
                pending = 0;
                debug_pending = 0;
                if (!j->reply)
                    atomic_store(&j->stop, 1);
            }
            j->reply_ready = 0;
        }
        pthread_mutex_unlock(&j->mutex);
        struct pollfd poller = {out[0], POLLIN, 0};
        poll(&poller, 1, 20);
        if (!eof) {
            ssize_t n = read(out[0], buffer + used, sizeof buffer - 1 - used);
            if (n > 0)
                used += (size_t)n;
            else if (n == 0)
                eof = 1;
            else if (errno != EAGAIN && errno != EINTR)
                eof = 1;
            buffer[used] = 0;
            char *line;
            while ((line = memchr(buffer, '\n', used))) {
                size_t count = (size_t)(line - buffer);
                *line = 0;
                json_object *ev = json_tokener_parse(buffer);
                if (!ev || !json_object_is_type(ev, json_type_object) || ended) {
                    if (ev)
                        json_object_put(ev);
                    bc_fail(e, "Protocolo de rotina invalido.");
                    atomic_store(&j->stop, 1);
                } else {
                    const char *kind = bc_str(ev, "type");
                    if (!strcmp(kind, "measurement")) {
                        json_object *value = bc_get(ev, "value");
                        if (++measures > 256 ||
                            (!json_object_is_type(value, json_type_double) &&
                             !json_object_is_type(value, json_type_int)) ||
                            !isfinite(json_object_get_double(value)) || !*bc_str(ev, "label") ||
                            !*bc_str(ev, "unit")) {
                            bc_fail(e, "Medicao invalida recebida.");
                            atomic_store(&j->stop, 1);
                        } else {
                            if (!strcmp(bc_str(t, "mode"), "simulation"))
                                json_object_object_add(ev, "source",
                                                       json_object_new_string("Simulação"));
                            bc_assess(ev);
                            time_t wall = time(NULL);
                            struct tm local;
                            localtime_r(&wall, &local);
                            char stamp[64];
                            strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S %z", &local);
                            json_object_object_add(ev, "timestamp", json_object_new_string(stamp));
                            emit(j, json_object_get(ev));
                        }
                    } else if (!strcmp(kind, "confirm")) {
                        if (pending || !*bc_str(ev, "id") ||
                            strlen(bc_str(ev, "id")) >= sizeof pending_id) {
                            bc_fail(e, "Confirmacao externa invalida.");
                            atomic_store(&j->stop, 1);
                        } else if (json_object_get_boolean(bc_get(ev, "debug")) && !studio_debug) {
                            bc_fail(e, "Pausa de depuração não autorizada para esta execução.");
                            atomic_store(&j->stop, 1);
                        } else {
                            debug_pending = json_object_get_boolean(bc_get(ev, "debug"));
                            pending = 1;
                            snprintf(pending_id, sizeof pending_id, "%s", bc_str(ev, "id"));
                            emit(j, json_object_get(ev));
                        }
                    } else if (!strcmp(kind, "sample") && sample_valid(ev))
                        emit(j, json_object_get(ev));
                    else if (!strcmp(kind, "status"))
                        emit(j, json_object_get(ev));
                    else if (!strcmp(kind, "done")) {
                        ended = 1;
                        json_object_object_add(snapshot, "message",
                                               json_object_new_string(bc_str(ev, "message")));
                    } else if (!strcmp(kind, "error")) {
                        bc_fail(e, "%s", bc_str(ev, "message"));
                        atomic_store(&j->stop, 1);
                    } else {
                        bc_fail(e, "Evento de rotina desconhecido.");
                        atomic_store(&j->stop, 1);
                    }
                    json_object_put(ev);
                }
                size_t consumed = count + 1;
                used -= consumed;
                memmove(buffer, buffer + consumed, used);
                buffer[used] = 0;
            }
            if (used == sizeof buffer - 1) {
                bc_fail(e, "Mensagem de rotina grande demais.");
                atomic_store(&j->stop, 1);
            }
        }
#ifdef _WIN32
        if (!reaped)
            reaped = bc_win_exited(&child, &status);
#else
        if (!reaped) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid)
                reaped = 1;
            else if (w < 0 && errno != EINTR) {
                bc_fail(e, "Falha ao aguardar a rotina.");
                break;
            }
        }
#endif
    }
    if (e->text[0])
        rc = -1;
    else if (atomic_load(&j->stop))
        rc = 1;
    else if (!reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || !ended || pending || used)
        bc_fail(e, "Rotina terminou sem confirmar a conclusao; estado fisico nao confirmado.");
    else {
        rc = 0;
        *result = json_object_get(snapshot);
    }
done:
#ifdef _WIN32
    bc_win_close_child(&child);
#else
    if (pid > 0) {
        kill(-pid, SIGKILL);
        if (!reaped)
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
            }
    }
#endif
    for (int i = 0; i < 2; i++) {
        if (in[i] >= 0)
            close(in[i]);
        if (out[i] >= 0)
            close(out[i]);
    }
    if (lock >= 0)
        close(lock);
    free(send);
    if (request)
        json_object_put(request);
    if (snapshot)
        json_object_put(snapshot);
    json_object_put(profile);
    return rc;
}

static int browse_cancelled(void *arg) {
    return atomic_load(&((bc_job *)arg)->stop);
}

static void *work(void *arg) {
    bc_job *j = arg;
    bc_error e = {{0}};
    json_object *result = NULL;
    int rc = 0;
    if (!strcmp(j->operation, "boot")) {
        rc = bc_boot(j->root, j->path, &e);
        if (!rc)
            result = bc_list(j->root, &e);
    } else if (!strcmp(j->operation, "list"))
        result = bc_list(j->root, &e);
    else if (!strcmp(j->operation, "browse")) {
        result = bc_browse(j->path, browse_cancelled, j, &e);
        if (atomic_load(&j->stop))
            rc = 1;
    } else if (!strcmp(j->operation, "inspect"))
        result = bc_inspect(j->path, &e);
    else if (!strcmp(j->operation, "install"))
        rc = bc_install(j->root, j->path, j->extra, &e);
    else if (!strcmp(j->operation, "save"))
        rc = bc_save(j->root, j->id, j->values, &e);
    else if (!strcmp(j->operation, "export"))
        rc = bc_export(j->root, j->id, j->path, j->values, &e);
    else if (!strcmp(j->operation, "render_document"))
        result = bc_render_document(j->root, j->id, j->path, j->values, &e);
    else if (!strcmp(j->operation, "document"))
        rc = bc_open_document(j->root, j->id, j->path, &e);
    else if (!strcmp(j->operation, "run"))
        rc = native_run(j, &result, &e);
    else
        rc = bc_fail(&e, "Operacao desconhecida.");
    if (e.text[0])
        rc = -1;
    json_object *end =
        message("finished", rc < 0    ? e.text
                            : rc == 1 ? "Execução interrompida. Estado físico não confirmado."
                                      : "Operação concluída.");
    json_object_object_add(end, "status",
                           json_object_new_string(rc < 0    ? "error"
                                                  : rc == 1 ? "cancelled"
                                                            : "ok"));
    if (result)
        json_object_object_add(end, "data", result);
    emit(j, end);
    atomic_store(&j->finished, 1);
    notify(j);
    return NULL;
}

bc_job *bc_job_start(const char *op, const char *root, const char *id, const char *path,
                     const char *extra, json_object *values) {
    bc_job *j = calloc(1, sizeof *j);
    if (!j)
        return NULL;
#ifdef _WIN32
    j->notify_fd[0] = j->notify_fd[1] = -1;
#else
    if (pipe(j->notify_fd) < 0) {
        free(j);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (fcntl(j->notify_fd[i], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(j->notify_fd[i], F_SETFL, O_NONBLOCK) < 0) {
            close(j->notify_fd[0]);
            close(j->notify_fd[1]);
            free(j);
            return NULL;
        }
    }
#endif
    j->operation = strdup(op);
    j->root = strdup(root);
    j->id = strdup(id ? id : "");
    j->path = strdup(path ? path : "");
    j->extra = strdup(extra ? extra : "");
    j->values = values ? bc_clone(values) : json_object_new_object();
    pthread_mutex_init(&j->mutex, NULL);
    if (!j->operation || !j->root || !j->id || !j->path || !j->extra || !j->values ||
        pthread_create(&j->thread, NULL, work, j) != 0) {
        free(j->operation);
        free(j->root);
        free(j->id);
        free(j->path);
        free(j->extra);
        if (j->values)
            json_object_put(j->values);
        close(j->notify_fd[0]);
        close(j->notify_fd[1]);
        pthread_mutex_destroy(&j->mutex);
        free(j);
        return NULL;
    }
    return j;
}

json_object *bc_job_next(bc_job *j) {
    pthread_mutex_lock(&j->mutex);
    event *ev = j->first;
    if (ev) {
        j->first = ev->next;
        if (!j->first)
            j->last = NULL;
        j->queued--;
    }
    pthread_mutex_unlock(&j->mutex);
    if (!ev)
        return NULL;
    json_object *o = ev->o;
    free(ev);
    return o;
}

int bc_job_notification_fd(bc_job *j) {
    return j ? j->notify_fd[0] : -1;
}

void bc_job_acknowledge(bc_job *j) {
#ifndef _WIN32
    unsigned char bytes[256];
    for (;;) {
        ssize_t n = read(j->notify_fd[0], bytes, sizeof bytes);
        if (n > 0)
            continue;
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
#else
    (void)j;
#endif
}

int bc_job_finished(bc_job *j) {
    return atomic_load(&j->finished);
}

void bc_job_cancel(bc_job *j) {
    if (j)
        atomic_store(&j->stop, 1);
}

void bc_job_reply(bc_job *j, const char *id, int accepted) {
    pthread_mutex_lock(&j->mutex);
    snprintf(j->reply_id, sizeof j->reply_id, "%s", id);
    j->reply = accepted;
    j->reply_has_value = 0;
    j->reply_ready = 1;
    pthread_mutex_unlock(&j->mutex);
}

void bc_job_reply_value(bc_job *j, const char *id, int accepted, double value) {
    pthread_mutex_lock(&j->mutex);
    snprintf(j->reply_id, sizeof j->reply_id, "%s", id);
    j->reply = accepted;
    j->reply_has_value = 1;
    j->reply_value = value;
    j->reply_ready = 1;
    pthread_mutex_unlock(&j->mutex);
}

void bc_job_free(bc_job *j) {
    if (!j)
        return;
    bc_job_cancel(j);
    pthread_join(j->thread, NULL);
    json_object *ev;
    while ((ev = bc_job_next(j)))
        json_object_put(ev);
    json_object_put(j->values);
    free(j->operation);
    free(j->root);
    free(j->id);
    free(j->path);
    free(j->extra);
    close(j->notify_fd[0]);
    close(j->notify_fd[1]);
    pthread_mutex_destroy(&j->mutex);
    free(j);
}
