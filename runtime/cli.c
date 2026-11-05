#include "studio.h"
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Always exports a temporary simulation-only package, even for hardware projects. */
static int simulate_samples(json_object *project, const char *path, const char *test,
                            const char *worker, bc_error *e) {
    if (!bs_find(bc_get(project, "tests"), test))
        return bc_fail(e, "Teste inexistente: %s", test);
    char root[4096], package[4096];
    if (bs_temp_dir(root, "cli", e) < 0)
        return -1;
    int rc = -1;
    if (bc_path(package, sizeof package, root, "simulation.bancada", e) < 0 ||
        bs_export(project, path, worker, package, 1, e) < 0 ||
        bc_install(root, package, NULL, e) < 0)
        goto done;
    bc_job *job = bc_job_start("run", root, bc_str(bc_get(project, "board"), "id"), test, "", NULL);
    if (!job) {
        bc_fail(e, "Não foi possível iniciar a simulação.");
        goto done;
    }
    int finished = 0;
    while (!finished) {
        bc_job_acknowledge(job);
        json_object *event;
        while ((event = bc_job_next(job))) {
            puts(json_object_to_json_string_ext(event, JSON_C_TO_STRING_PLAIN));
            if (!strcmp(bc_str(event, "type"), "confirm")) {
                if (json_object_get_boolean(bc_get(event, "number"))) {
                    if (!bc_get(event, "sample")) {
                        bc_job_cancel(job);
                        bc_fail(e, "Medição simulada sem valor de exemplo.");
                    } else
                        bc_job_reply_value(job, bc_str(event, "id"), 1, bc_num(event, "sample"));
                } else
                    bc_job_reply(job, bc_str(event, "id"), 1);
            } else if (!strcmp(bc_str(event, "type"), "finished")) {
                finished = 1;
                if (!strcmp(bc_str(event, "status"), "ok"))
                    rc = 0;
                else
                    bc_fail(e, "%s", bc_str(event, "message"));
            }
            json_object_put(event);
        }
        if (!finished) {
            struct timespec pause = {0, 20000000};
            nanosleep(&pause, NULL);
        }
    }
    bc_job_free(job);
done:
    bs_remove_temp(root);
    return rc;
}

int main(int argc, char **argv) {
    bs_platform_init();
#ifdef _WIN32
    char **unicode_args = g_win32_get_command_line();
    argv = unicode_args;
    argc = (int)g_strv_length(unicode_args);
#endif
    bc_error e = {{0}};
    json_object *p = NULL;
    int rc = -1;
    char worker[4096];
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("BPCB Studio " BS_VERSION " · " BS_PLATFORM_LABEL);
        rc = 0;
    } else if (argc == 3 && !strcmp(argv[1], "catalog")) {
        p = bs_catalog(argv[2]);
        if (p) {
            puts(json_object_to_json_string_ext(p, JSON_C_TO_STRING_PRETTY));
            rc = 0;
        }
    } else if (argc == 3 && (!strcmp(argv[1], "new") || !strcmp(argv[1], "example"))) {
        p = !strcmp(argv[1], "new") ? bs_new() : bs_example();
        rc = bs_save(argv[2], p, &e);
    } else if (argc == 3 && !strcmp(argv[1], "validate")) {
        p = bs_load(argv[2], &e);
        if (p) {
            json_object *a = bs_validate(p);
            puts(json_object_to_json_string_ext(a, JSON_C_TO_STRING_PRETTY));
            rc = json_object_array_length(a) ? -1 : 0;
            json_object_put(a);
        }
    } else if ((argc == 4 || argc == 5) && !strcmp(argv[1], "export")) {
        const char *executor = argc == 5 ? argv[3] : worker;
        if (argc == 5 || bs_worker_path(worker, &e) == 0) {
            p = bs_load(argv[2], &e);
            if (p)
                rc = bs_export(p, argv[2], executor, argv[argc - 1], 0, &e);
        }
    } else if ((argc == 4 || argc == 5) && !strcmp(argv[1], "simulate-samples")) {
        const char *executor = argc == 5 ? argv[4] : worker;
        if (argc == 5 || bs_worker_path(worker, &e) == 0) {
            p = bs_load(argv[2], &e);
            if (p)
                rc = simulate_samples(p, argv[2], argv[3], executor, &e);
        }
    } else if (argc == 4 && (!strcmp(argv[1], "backup") || !strcmp(argv[1], "manual"))) {
        p = bs_load(argv[2], &e);
        if (p)
            rc = !strcmp(argv[1], "backup") ? bs_backup(p, argv[2], argv[3], &e)
                                            : bs_manual(p, argv[3], &e);
    } else if (argc == 3 && !strcmp(argv[1], "backup-preview")) {
        p = bs_backup_preview(argv[2], NULL, &e);
        if (p) {
            puts(json_object_to_json_string_ext(p, JSON_C_TO_STRING_PRETTY));
            rc = 0;
        }
    } else if (argc == 5 && !strcmp(argv[1], "restore")) {
        p = bs_restore(argv[2], argv[3], argv[4], &e);
        rc = p ? 0 : -1;
    } else if (argc == 6 && !strcmp(argv[1], "reuse")) {
        p = bs_load(argv[2], &e);
        json_object *donor = bs_load(argv[3], &e),
                    *merged = p && donor ? bs_reuse_test(p, donor, argv[4], &e) : NULL;
        if (merged)
            rc = bs_save(argv[5], merged, &e);
        json_object_put(donor);
        json_object_put(merged);
    } else if (argc == 3 && !strcmp(argv[1], "inspect")) {
        p = bc_inspect(argv[2], &e);
        if (p) {
            puts(json_object_to_json_string_ext(p, JSON_C_TO_STRING_PRETTY));
            rc = 0;
        }
    } else if (argc == 4 && !strcmp(argv[1], "import")) {
        p = bs_import(argv[2], argv[3], &e);
        rc = p ? 0 : -1;
    } else {
        fputs("Uso: studio_cli --version\n"
              "     studio_cli new|example|validate PROJETO.bpcb\n"
              "     studio_cli export PROJETO.bpcb [EXECUTOR] SAIDA.bancada\n"
              "     studio_cli simulate-samples PROJETO.bpcb TESTE [EXECUTOR]\n"
              "     studio_cli backup PROJETO.bpcb COPIA.bpcbstudio\n"
              "     studio_cli backup-preview COPIA.bpcbstudio\n"
              "     studio_cli restore COPIA.bpcbstudio NOVA_PASTA SHA256_DA_PREVIA\n"
              "     studio_cli manual PROJETO.bpcb MANUAL.pdf\n"
              "     studio_cli reuse PROJETO.bpcb ORIGEM.bpcb TESTE SAIDA.bpcb\n"
              "     studio_cli inspect PACOTE.bancada\n"
              "     studio_cli import PACOTE.bancada PROJETO.bpcb\n"
              "     studio_cli catalog board|component|device|signal|test|parameter|formula|step\n"
              "simulate-samples usa os exemplos e aceita as confirmações; nunca acessa hardware.\n",
              stderr);
    }
    if (rc && *e.text)
        fprintf(stderr, "%s\n", e.text);
    json_object_put(p);
#ifdef _WIN32
    g_strfreev(unicode_args);
#endif
    return rc ? 1 : 0;
}
