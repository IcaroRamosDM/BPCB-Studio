#pragma once
#include "bancada.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Transport API 1. Sessions never reconnect or select a different device. */
#define BC_COMM_API 1
#define BC_COMM_MAX 4096
#define BC_COMM_I2C_READ 1u
typedef struct bc_comm bc_comm;

typedef struct {
    uint16_t address, flags;
    size_t size;
    uint8_t *data;
} bc_comm_i2c_message;

json_object *bc_comm_catalog(void);
int bc_comm_ch347_supports(int mode, const char *bus);
json_object *bc_comm_config(json_object *input, bc_error *error);
int bc_comm_declaration(json_object *declaration, bc_error *error);
json_object *bc_comm_resolve(json_object *profile, json_object *test, json_object *selected,
                             bc_error *error);
json_object *bc_comm_list(int (*cancel)(void *), void *context, bc_error *error);
json_object *bc_comm_probe(json_object *config, int (*cancel)(void *), void *context,
                           bc_error *error);
int bc_comm_open(bc_comm **session, json_object *config, int (*cancel)(void *), void *context,
                 bc_error *error);
json_object *bc_comm_information(bc_comm *session);
void bc_comm_close(bc_comm *session);
int bc_comm_i2c(bc_comm *session, bc_comm_i2c_message *messages, unsigned count, bc_error *error);
int bc_comm_spi(bc_comm *session, const uint8_t *output, uint8_t *input, size_t size,
                bc_error *error);
int bc_comm_uart(bc_comm *session, const uint8_t *output, size_t output_size, uint8_t *input,
                 size_t input_size, bc_error *error);
int bc_comm_operation_check(json_object *config, json_object *declaration, json_object *operation,
                            bc_error *error);
int bc_comm_operation_validate(const char *bus, json_object *operation, bc_error *error);
json_object *bc_comm_exchange(json_object *config, json_object *operation, int (*cancel)(void *),
                              void *context, bc_error *error);
int bc_comm_hex(const char *text, uint8_t *data, size_t capacity, size_t *size, bc_error *error);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C" {
#endif
json_object *bc_comm_transaction(bc_comm *session, json_object *operation, bc_error *error);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* Manual tools. Storage occurs only through an explicit save/export call. */
int bc_comm_profiles_validate(json_object *profiles, bc_error *e);
json_object *bc_comm_profiles(const char *root, bc_error *e);
int bc_comm_profile_save(const char *root, const char *name, json_object *config, int replace,
                         bc_error *e);
int bc_comm_profile_delete(const char *root, const char *name, bc_error *e);
json_object *bc_comm_register_maps(void);
int bc_comm_register_map_validate(json_object *map, bc_error *e);
json_object *bc_comm_register_operation(json_object *map, size_t index, unsigned address,
                                        const char *write_hex, bc_error *e);
json_object *bc_comm_scan(bc_comm **session, unsigned first, unsigned last,
                          int (*progress)(void *, json_object *), void *context, bc_error *e);
json_object *bc_comm_jtag_identify(bc_comm *session, unsigned maximum, bc_error *e);
int bc_comm_uart_unread(bc_comm *, const uint8_t *, size_t, bc_error *);
int bc_comm_uart_poll(bc_comm *session, uint8_t *data, size_t size, unsigned wait_ms, bc_error *e);
json_object *bc_comm_text_operation(const char *text, int hex, const char *delimiter, unsigned read,
                                    bc_error *e);
char *bc_comm_format_bytes(const uint8_t *data, size_t size, int hex);
json_object *bc_openocd_targets(void);
json_object *bc_openocd_plan(json_object *request, bc_error *e);
json_object *bc_openocd_run(json_object *request, int (*cancel)(void *), void *context,
                            bc_error *e);
#ifdef __cplusplus
}
#endif
