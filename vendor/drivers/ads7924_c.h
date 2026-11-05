#pragma once
#include <stdint.h>
typedef struct {
    void *context;
    int (*write_reg)(void *context,uint8_t reg,uint8_t value);
    int (*read_reg)(void *context,uint8_t reg,uint8_t *bytes,unsigned count);
    int (*wait_ms)(void *context,unsigned ms);
} ads_bus;
int ads7924_read_all(const ads_bus *bus,unsigned raw[4]);
