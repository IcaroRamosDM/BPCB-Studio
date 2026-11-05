#pragma once
#include "communication.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BC_LAB_CHANNELS 8
#define BC_LAB_TRACE 256

/* All I/O has one explicit operation, a bound, and the selected device identity. */
typedef struct {
    void *context;
    json_object *(*transaction)(void *, json_object *, bc_error *);
    int (*poll)(void *, uint8_t *, size_t, unsigned, bc_error *);
    double (*clock)(void *);
    int (*wait_ms)(void *, unsigned, bc_error *);
    int (*cancelled)(void *);
    int (*unread)(void *, const uint8_t *, size_t, bc_error *);
} bc_lab_io;
typedef struct bc_lab bc_lab;
int bc_lab_object(json_object *, const char *, bc_error *);
int bc_lab_int(json_object *, const char *, int, int, int, int *, bc_error *);
int bc_lab_number(json_object *, const char *, double, double, double, double *, bc_error *);
int bc_lab_id(const char *);
int bc_lab_declaration(json_object *, bc_error *);
json_object *bc_lab_resolve(json_object *, json_object *, bc_error *);
int bc_lab_operation_validate(const char *, json_object *, bc_error *);
int bc_lab_operation_check(json_object *, json_object *, json_object *, bc_error *);
json_object *bc_lab_channel_config(json_object *, const char *);
json_object *bc_lab_channel_definition(json_object *, const char *);
bc_lab *bc_lab_new(json_object *, json_object *, int (*)(void *), void *, bc_error *);
void bc_lab_free(bc_lab *);
json_object *bc_lab_execute(bc_lab *, json_object *, bc_error *);
json_object *bc_lab_execute_io(const char *, json_object *, bc_lab_io *, bc_error *);
json_object *bc_peripheral_catalog(void);
const char *bc_lab_metric_label(const char *);
const char *bc_lab_metric_unit(const char *);
int bc_peripheral_validate(const char *, json_object *, bc_error *);
json_object *bc_peripheral_execute(json_object *, bc_lab_io *, bc_error *);
int bc_uart_protocol_validate(json_object *, bc_error *);
json_object *bc_uart_protocol_execute(json_object *, bc_lab_io *, bc_error *);
uint16_t bc_modbus_crc(const uint8_t *, size_t);
int bc_replay_validate(json_object *, bc_error *);
json_object *bc_replay_from_capture(json_object *, bc_error *);
json_object *bc_replay_execute(json_object *, json_object *, json_object *, bc_error *);
json_object *bc_qualification_plan(json_object *, const char *, bc_error *);
json_object *bc_qualification_assess(json_object *, json_object *, bc_error *);
json_object *bc_firmware_manifest(json_object *, const char *, bc_error *);
json_object *bc_firmware_plan(json_object *, bc_error *);
json_object *bc_firmware_run(json_object *, int (*)(void *), void *, bc_error *);
json_object *bc_external_process(json_object *, int, int (*)(void *), void *, bc_error *);
#ifdef __cplusplus
}
#endif
