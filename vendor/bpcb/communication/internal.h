#pragma once
#include "../communication.h"
#include <time.h>

typedef enum { BC_CH341, BC_FT232H, BC_MCP2221, BC_CP2112, BC_SERIAL, BC_CH347 } bc_comm_kind;

typedef struct {
    void *context;
    int (*write)(void *, const uint8_t *, size_t, unsigned, bc_error *);
    int (*read)(void *, uint8_t *, size_t, unsigned, bc_error *);
    int (*feature)(void *, uint8_t *, size_t, int, unsigned, bc_error *);
    int (*control)(void *, unsigned, unsigned, unsigned, bc_error *);
    void (*close)(void *);
} bc_comm_io;

struct bc_comm {
    bc_comm_kind kind;
    json_object *config;
    bc_comm_io io;
    int (*cancel)(void *);
    void *cancel_context;
    int lost;
    double deadline;
    unsigned actual_hz;
    unsigned packet_size;
    int ch347_hid;
    uint8_t uart_pending[BC_COMM_MAX];
    size_t uart_pending_size;
};

double bc_comm_now(void);
int bc_comm_check(bc_comm *s, bc_error *e);
unsigned bc_comm_remaining(bc_comm *s, bc_error *e);
void bc_comm_deadline(bc_comm *s);
int bc_comm_write(bc_comm *s, const uint8_t *data, size_t size, bc_error *e);
int bc_comm_read(bc_comm *s, uint8_t *data, size_t size, bc_error *e);
int bc_comm_feature(bc_comm *s, uint8_t *data, size_t size, int get, bc_error *e);
int bc_comm_protocol_init(bc_comm *s, bc_error *e);
int bc_comm_protocol_i2c(bc_comm *s, bc_comm_i2c_message *m, unsigned n, bc_error *e);
int bc_comm_protocol_spi(bc_comm *s, const uint8_t *tx, uint8_t *rx, size_t n, bc_error *e);
int bc_comm_usb_list(json_object *list, int (*cancel)(void *), void *context, bc_error *e);
int bc_comm_usb_open(bc_comm *s, int probe, bc_error *e);
int bc_comm_hid_list(json_object *list, int (*cancel)(void *), void *context, bc_error *e);
int bc_comm_hid_open(bc_comm *s, bc_error *e);
int bc_comm_serial_list(json_object *list, bc_error *e);
int bc_comm_serial_open(bc_comm *s, bc_error *e);
int bc_comm_serial_probe(json_object *config, bc_error *e);
void bc_comm_device(json_object *list, const char *adapter, const char *device, const char *serial,
                    const char *label, const char *access);
int bc_comm_select(json_object *item, json_object *config);
int bc_comm_lock(const char *identity, bc_error *e);
void bc_comm_unlock(int descriptor);

int bc_comm_hid_normalize(bc_comm_kind kind, uint8_t *buffer, size_t actual, size_t requested,
                          bc_error *error);

int bc_comm_ch347_init(bc_comm *s, bc_error *e);
int bc_comm_ch347_i2c(bc_comm *s, bc_comm_i2c_message *m, unsigned n, bc_error *e);
int bc_comm_ch347_spi(bc_comm *s, const uint8_t *tx, uint8_t *rx, size_t n, bc_error *e);
const char *bc_comm_serial_adapter(unsigned vid, unsigned pid);

int bc_comm_ch347_mode(unsigned vid, unsigned pid);
int bc_comm_ch347_supports(int mode, const char *bus);
int bc_comm_ch347_open(bc_comm *s, int probe, bc_error *e);
int bc_comm_ch347_jtag(bc_comm *s, const char *reg, const uint8_t *tx, uint8_t *rx, unsigned bits,
                       unsigned idle, bc_error *e);
int bc_comm_ch347_hid_encode(const uint8_t *data, size_t n, uint8_t packet[513], bc_error *e);
int bc_comm_ch347_hid_decode(const uint8_t *packet, size_t n, int windows, uint8_t data[510],
                             bc_error *e);
