#include "studio.h"
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

struct bs_task {
    pthread_t thread;
    atomic_int done;
    int pipe[2];
    char *op, *path, *extra, *worker;
    json_object *project, *result;
    bc_error error;
};

static json_object *success(void) {
    json_object *v = json_object_new_object();
    json_object_object_add(v, "ok", json_object_new_boolean(1));
    return v;
}

static void *work(void *ptr) {
    bs_task *t = ptr;
    int rc = -1;
    if (!strcmp(t->op, "render"))
        t->result = bs_document_preview(t->path, atoi(t->extra), &t->error);
    else if (!strcmp(t->op, "browse"))
        t->result = bs_browse(t->path, t->extra, &t->error);
    else if (!strcmp(t->op, "load"))
        t->result = bs_load(t->path, &t->error);
    else if (!strcmp(t->op, "backup"))
        rc = bs_backup(t->project, t->path, t->extra, &t->error);
    else if (!strcmp(t->op, "backup-preview"))
        t->result = bs_backup_preview(t->extra, t->project, &t->error);
    else if (!strcmp(t->op, "restore"))
        t->result = bs_restore(t->extra, t->path, bc_str(t->project, "restore_digest"), &t->error);
    else if (!strcmp(t->op, "manual"))
        rc = bs_manual(t->project, t->extra, &t->error);
    else if (!strcmp(t->op, "save"))
        rc = bs_save(t->path, t->project, &t->error);
    else if (!strcmp(t->op, "export"))
        rc = bs_export(t->project, t->path, t->worker, t->extra, 0, &t->error);
    else if (!strcmp(t->op, "import"))
        t->result = bs_import(t->extra, t->path, &t->error);
    else if (!strcmp(t->op, "document")) {
        rc = bs_document(t->project, t->path, t->extra,
                         bc_str(bc_get(t->project, "pending_document"), "title"),
                         bc_str(bc_get(t->project, "pending_document"), "role"), &t->error);
        json_object_object_del(t->project, "pending_document");
        if (!rc)
            t->result = bc_clone(t->project);
    } else if (!strcmp(t->op, "simulate")) {
        char temp[4096];
        if (bs_temp_dir(temp, "sim", &t->error) == 0) {
            char package[4096];
            if (bc_path(package, sizeof package, temp, "simulation.bancada", &t->error) == 0 &&
                bs_export(t->project, t->path, t->worker, package, 1, &t->error) == 0 &&
                bc_install(temp, package, NULL, &t->error) == 0) {
                t->result = success();
                json_object_object_add(t->result, "root", json_object_new_string(temp));
            } else
                bs_remove_temp(temp);
        } else
            bc_fail(&t->error, "Criar simulação temporária.");
    } else if (!strcmp(t->op, "validate"))
        t->result = bs_validate(t->project);
    else
        bc_fail(&t->error, "Operação desconhecida.");
    if (!t->result && rc == 0)
        t->result = success();
    atomic_store(&t->done, 1);
#ifdef _WIN32
    bc_wakeup();
#else
    char c = 1;
    ssize_t notified;
    do {
        notified = write(t->pipe[1], &c, 1);
    } while (notified < 0 && errno == EINTR);
    (void)notified;
#endif
    return NULL;
}

bs_task *bs_task_start(const char *op, json_object *p, const char *path, const char *extra,
                       const char *worker) {
    bs_task *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->op = strdup(op ? op : "");
    t->path = strdup(path ? path : "");
    t->extra = strdup(extra ? extra : "");
    t->worker = strdup(worker ? worker : "");
    t->project = bc_clone(p);
    if (!t->op || !t->path || !t->extra || !t->worker)
        goto fail;
#ifdef _WIN32
    t->pipe[0] = t->pipe[1] = -1;
#else
    if (pipe(t->pipe) < 0)
        goto fail;
    fcntl(t->pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(t->pipe[1], F_SETFD, FD_CLOEXEC);
#endif
    if (pthread_create(&t->thread, NULL, work, t) != 0) {
        close(t->pipe[0]);
        close(t->pipe[1]);
        goto fail;
    }
    return t;
fail:
    free(t->op);
    free(t->path);
    free(t->extra);
    free(t->worker);
    json_object_put(t->project);
    free(t);
    return NULL;
}

int bs_task_fd(bs_task *t) {
    return t ? t->pipe[0] : -1;
}

int bs_task_done(bs_task *t) {
    return t ? atomic_load(&t->done) : 1;
}

json_object *bs_task_result(bs_task *t, bc_error *e) {
    if (!t || !atomic_load(&t->done))
        return NULL;
    if (e)
        *e = t->error;
    return t->result ? json_object_get(t->result) : NULL;
}

void bs_task_free(bs_task *t) {
    if (!t)
        return;
    pthread_join(t->thread, NULL);
    close(t->pipe[0]);
    close(t->pipe[1]);
    free(t->op);
    free(t->path);
    free(t->extra);
    free(t->worker);
    json_object_put(t->project);
    json_object_put(t->result);
    free(t);
}
