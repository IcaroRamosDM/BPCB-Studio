#pragma once
#include "bancada.h"
#include "communication.h"
#include "lab.h"
#include "workbench.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BS_SCHEMA 3
#define BS_VERSION "0.6.0"
#ifdef _WIN32
#define BS_WORKER_NAME "bpcb_sequence.exe"
#define BS_PLATFORM_LABEL "Windows x64"
#else
#define BS_WORKER_NAME "bpcb_sequence"
#define BS_PLATFORM_LABEL "Linux"
#endif
int bs_temp_dir(char out[4096], const char *purpose, bc_error *e);
int bs_executable_path(char out[4096], bc_error *e);
int bs_worker_path(char out[4096], bc_error *e);
void bs_platform_init(void);
void bs_catalog_extend(const char *kind, json_object *fields);
json_object *bs_catalog(const char *kind);
json_object *bs_document_preview(const char *source, int page, bc_error *e);
int bs_quantity(const char *text, const char *kind, double *out);
json_object *bs_simulation_values(const char *text, bc_error *e);
int bs_check_calibration(json_object *project, json_object *test, json_object *components,
                         json_object *cal, int simulation, bc_error *e);
int bs_identifier(const char *s);
void bs_lab_catalog(const char *, json_object *);
json_object *bs_lab_test_channel(json_object *, const char *, bc_error *);
json_object *bs_lab_operation(json_object *, json_object *, bc_error *);
json_object *bs_lab_result(json_object *, json_object *, json_object *, int, bc_error *);
/* Prepared variants are internal to a validated execution; never accept external operations here.
 */
json_object *bs_communication_result_prepared(json_object *, json_object *, json_object *, int,
                                              json_object *, bc_error *);
json_object *bs_lab_result_prepared(json_object *, json_object *, json_object *, int, json_object *,
                                    bc_error *);
void bs_communication_catalog(const char *kind, json_object *fields);
json_object *bs_communication(json_object *test, bc_error *e);
json_object *bs_communication_operation(json_object *test, json_object *step, bc_error *e);
json_object *bs_communication_result(json_object *test, json_object *step, json_object *result,
                                     int simulate, bc_error *e);

int bs_field_visible(json_object *field, json_object *values);
json_object *bs_defaults(const char *kind);
int bs_form(const char *kind, json_object *raw, json_object **out, bc_error *e);
const char *bs_kind_label(const char *kind);
json_object *bs_new(void);
json_object *bs_example(void);
json_object *bs_find(json_object *array, const char *id);
json_object *bs_rows(json_object *project, const char *kind, const char *test_id);
int bs_put(json_object *project, const char *kind, const char *test_id, int index,
           json_object *item, bc_error *e);
int bs_delete(json_object *project, const char *kind, const char *test_id, int index, bc_error *e);
int bs_move(json_object *array, int index, int direction);
void bs_refactor(json_object *project, const char *kind, const char *test, const char *before,
                 const char *after);
int bs_duplicate(json_object *project, const char *kind, const char *test, int index, bc_error *e);
json_object *bs_reuse_test(json_object *project, json_object *source, const char *test,
                           bc_error *e);
json_object *bs_session_order(json_object *project, bc_error *e);
int bs_backup(json_object *project, const char *project_path, const char *output, bc_error *e);
json_object *bs_backup_preview(const char *backup, json_object *current, bc_error *e);
json_object *bs_restore(const char *backup, const char *directory, const char *digest, bc_error *e);
int bs_manual(json_object *project, const char *output, bc_error *e);
void bs_workbench_catalog(const char *, json_object *);
int bs_project_schema(json_object *);
int bs_shape(json_object *project, bc_error *e);
json_object *bs_validate(json_object *project);
int bs_check(json_object *project, bc_error *e);
json_object *bs_manifest(json_object *project, int simulate, bc_error *e);
char *bs_render(const char *text, json_object *vars, bc_error *e);
int bs_save(const char *path, json_object *project, bc_error *e);
json_object *bs_load(const char *path, bc_error *e);
int bs_copy(const char *source, const char *target, bc_error *e);
int bs_document(json_object *project, const char *project_path, const char *source,
                const char *title, const char *role, bc_error *e);
int bs_export(json_object *project, const char *project_path, const char *worker,
              const char *output, int simulate, bc_error *e);
json_object *bs_import(const char *package, const char *project_path, bc_error *e);
json_object *bs_browse(const char *path, const char *filter, bc_error *e);
int bs_asset_pdf(const char *source, const char *destination, bc_error *e);
int bs_notes_pdf(const char *text, const char *destination, bc_error *e);
void bs_remove_temp(const char *path);
typedef struct bs_task bs_task;
bs_task *bs_task_start(const char *op, json_object *project, const char *path, const char *extra,
                       const char *worker);
int bs_task_fd(bs_task *task);
int bs_task_done(bs_task *task);
json_object *bs_task_result(bs_task *task, bc_error *e);
void bs_task_free(bs_task *task);

typedef struct {
    void *context;
    int simulation;
    int (*stopped)(void *);
    double (*clock)(void *);
    int (*sleep_ms)(void *, unsigned);
    int (*emit)(void *, json_object *);
    int (*confirm)(void *, const char *, const char *, int, double, double, double, double *);
    int (*read)(void *, json_object *, json_object *, double *, bc_error *);
    int (*write)(void *, json_object *, json_object *, double, bc_error *);
    int (*initialize)(void *, json_object *, json_object *, bc_error *);
    json_object *calibration;
    int (*confirm_detail)(void *, json_object *, double *, json_object **);
    json_object *(*communicate)(void *, json_object *, bc_error *);
    /* Simulation-only; a pause must not consume the execution deadline. */
    int (*checkpoint)(void *, json_object *, size_t, unsigned, json_object *, bc_error *);
} bs_host;

int bs_execute(json_object *project, const char *test_id, json_object *parameters,
               json_object *components, bs_host *host, bc_error *e);
typedef struct bs_io bs_io;
int bs_io_communication_setup(bs_io *, json_object *, json_object *, bc_error *);
json_object *bs_io_communicate(void *, json_object *, bc_error *);
bs_io *bs_io_new(bs_host *host);
void bs_io_free(bs_io *);
int bs_io_read(void *, json_object *, json_object *, double *, bc_error *);
int bs_io_write(void *, json_object *, json_object *, double, bc_error *);
int bs_io_init(void *, json_object *, json_object *, bc_error *);
#ifdef __cplusplus
}
#endif
