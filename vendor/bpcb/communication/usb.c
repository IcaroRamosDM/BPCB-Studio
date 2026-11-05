#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bc_comm *session;
    libusb_context *context;
    libusb_device_handle *handle;
    int claimed, detached, lock, interface;
    unsigned char in, out;
    unsigned packet;
    uint8_t pending[16384];
    size_t at, used;
} usb_io;

static int usb_fail(bc_error *e, int code) {
    errno = code == LIBUSB_ERROR_ACCESS      ? EACCES
            : code == LIBUSB_ERROR_NO_DEVICE ? ENODEV
            : code == LIBUSB_ERROR_BUSY      ? EBUSY
            : code == LIBUSB_ERROR_TIMEOUT   ? ETIMEDOUT
                                             : EIO;
    return bc_fail(e, "USB: %s. Confira conexão, permissão e driver da interface.",
                   libusb_error_name(code));
}

static const char *adapter(unsigned vid, unsigned pid) {
    return vid == 0x1a86 && pid == 0x5512   ? "ch341t"
           : vid == 0x0403 && pid == 0x6014 ? "ft232h"
           : vid == 0x1a86 && (pid == 0x55d9 || pid == 0x55db || pid == 0x55dc || pid == 0x55dd)
               ? "ch347"
               : NULL;
}

static void identity(libusb_device *dev, struct libusb_device_descriptor *d, char key[128]) {
    uint8_t ports[8];
    int n = libusb_get_port_numbers(dev, ports, sizeof ports);
    int pos = snprintf(key, 128, "usb:%04x:%04x:%u", d->idVendor, d->idProduct,
                       libusb_get_bus_number(dev));
    if (n > 0) {
        for (int i = 0; i < n && pos < 120; i++)
            pos += snprintf(key + pos, 128 - (size_t)pos, "%c%u", i ? '.' : '-', ports[i]);
    } else
        snprintf(key + pos, 128 - (size_t)pos, ":address-%u", libusb_get_device_address(dev));
}

static void info(libusb_device *dev, struct libusb_device_descriptor *d, char key[128],
                 char serial[256], char label[512], int *access) {
    identity(dev, d, key);
    serial[0] = 0;
    snprintf(label, 512, "%s · %s", adapter(d->idVendor, d->idProduct), key);
    libusb_device_handle *h = NULL;
    *access = libusb_open(dev, &h);
    if (!*access) {
        unsigned char product[256] = {0};
        if (d->iSerialNumber)
            libusb_get_string_descriptor_ascii(h, d->iSerialNumber, (unsigned char *)serial, 255);
        if (d->iProduct)
            libusb_get_string_descriptor_ascii(h, d->iProduct, product, 255);
        if (*product)
            snprintf(label, 512, "%.200s · %s%s%.160s", product, key, *serial ? " · série " : "",
                     serial);
        libusb_close(h);
    }
}

int bc_comm_usb_list(json_object *list, int (*cancel)(void *), void *ctx, bc_error *e) {
    libusb_context *usb = NULL;
    libusb_device **devices = NULL;
    int rc = libusb_init(&usb);
    if (rc < 0)
        return usb_fail(e, rc);
    ssize_t n = libusb_get_device_list(usb, &devices);
    if (n < 0) {
        libusb_exit(usb);
        return usb_fail(e, (int)n);
    }
    for (ssize_t i = 0; i < n; i++) {
        if (cancel && cancel(ctx)) {
            errno = ECANCELED;
            rc = bc_fail(e, "Enumeração cancelada.");
            break;
        }
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(devices[i], &d) < 0 || !adapter(d.idVendor, d.idProduct))
            continue;
        char key[128], serial[256] = {0}, label[512];
        int access;
        info(devices[i], &d, key, serial, label, &access);
        bc_comm_device(list, adapter(d.idVendor, d.idProduct), key, serial, label,
                       !strcmp(adapter(d.idVendor, d.idProduct), "ch347") &&
                               d.idProduct != 0x55db && d.idProduct != 0x55dd
                           ? "CH347T: M0 UART ou M2 HID; escolha a interface correspondente"
                       : access == 0                   ? "USB acessível"
                       : access == LIBUSB_ERROR_ACCESS ? "Sem permissão/driver compatível"
                                                       : "Falha de abertura USB");
        if (!strcmp(adapter(d.idVendor, d.idProduct), "ch347")) {
            json_object *item = json_object_array_get_idx(list, json_object_array_length(list) - 1);
            json_object_object_add(
                item, "mode", json_object_new_int(bc_comm_ch347_mode(d.idVendor, d.idProduct)));
            json_object_object_add(item, "interface", json_object_new_int(2));
        }
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(usb);
    return rc;
}

static void usb_close(void *context) {
    usb_io *u = context;
    if (u->handle) {
        if (u->claimed)
            libusb_release_interface(u->handle, u->interface);
        if (u->detached)
            libusb_attach_kernel_driver(u->handle, u->interface);
        libusb_close(u->handle);
    }
    if (u->context)
        libusb_exit(u->context);
    bc_comm_unlock(u->lock);
    free(u);
}

static int usb_write(void *context, const uint8_t *data, size_t size, unsigned timeout,
                     bc_error *e) {
    usb_io *u = context;
    int n = 0,
        rc = libusb_bulk_transfer(u->handle, u->out, (unsigned char *)data, (int)size, &n, timeout);
    if (rc < 0)
        return usb_fail(e, rc);
    return n;
}

static int usb_read(void *context, uint8_t *data, size_t size, unsigned timeout, bc_error *e) {
    usb_io *u = context;
    double deadline = bc_comm_now() + (double)timeout / 1000;
    for (;;) {
        if (u->at < u->used) {
            size_t n = u->used - u->at;
            if (n > size)
                n = size;
            memcpy(data, u->pending + u->at, n);
            u->at += n;
            return (int)n;
        }
        if (bc_comm_check(u->session, e) < 0)
            return -1;
        double remaining = (deadline - bc_comm_now()) * 1000;
        if (remaining <= 0)
            return usb_fail(e, LIBUSB_ERROR_TIMEOUT);
        unsigned slice = (unsigned)remaining;
        if (!slice)
            slice = 1;
        if (slice > 50)
            slice = 50;
        uint8_t buffer[16384];
        int n = 0, rc = libusb_bulk_transfer(u->handle, u->in, buffer, sizeof buffer, &n, slice);
        if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT)
            return usb_fail(e, rc);
        if (!n)
            continue;
        u->at = u->used = 0;
        if (u->session->kind == BC_FT232H) {
            for (int at = 0; at < n;) {
                unsigned packet = (unsigned)(n - at);
                if (packet > u->packet)
                    packet = u->packet;
                if (packet < 2) {
                    errno = EIO;
                    return bc_fail(e, "FT232H: pacote USB sem os bytes de estado.");
                } /* FTDI inserts two status bytes at EVERY USB packet boundary. */
                if (packet > 2) {
                    memcpy(u->pending + u->used, buffer + at + 2, packet - 2);
                    u->used += packet - 2;
                }
                at += (int)packet;
            }
        } else {
            memcpy(u->pending, buffer, (size_t)n);
            u->used = (size_t)n;
        }
    }
}

static int usb_control(void *context, unsigned request, unsigned value, unsigned index,
                       bc_error *e) {
    usb_io *u = context;
    unsigned timeout = bc_comm_remaining(u->session, e);
    if (!timeout)
        return -1;
    int rc = libusb_control_transfer(u->handle, 0x40, (uint8_t)request, (uint16_t)value,
                                     (uint16_t)(index + 1), NULL, 0, timeout);
    return rc < 0 ? usb_fail(e, rc) : 0;
}

int bc_comm_usb_open(bc_comm *s, int probe, bc_error *e) {
    usb_io *u = calloc(1, sizeof *u);
    if (!u)
        return bc_fail(e, "Memória insuficiente.");
    u->session = s;
    u->interface = s->kind == BC_CH347 ? 2 : 0;
    u->lock = -1;
    int rc = libusb_init(&u->context);
    if (rc < 0) {
        usb_fail(e, rc);
        goto fail;
    }
    libusb_device **devices = NULL, *candidate = NULL;
    ssize_t n = libusb_get_device_list(u->context, &devices);
    if (n < 0) {
        usb_fail(e, (int)n);
        goto fail;
    }
    unsigned found = 0;
    char selected[128] = "";
    for (ssize_t i = 0; i < n; i++) {
        if (bc_comm_check(s, e) < 0)
            break;
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(devices[i], &d) < 0 || !adapter(d.idVendor, d.idProduct) ||
            strcmp(adapter(d.idVendor, d.idProduct), bc_str(s->config, "adapter")))
            continue;
        char key[128], serial[256] = {0};
        identity(devices[i], &d, key);
        if (*bc_str(s->config, "device") && strcmp(key, bc_str(s->config, "device")))
            continue;
        if (*bc_str(s->config, "serial")) {
            libusb_device_handle *h = NULL;
            if (libusb_open(devices[i], &h) == 0) {
                if (d.iSerialNumber)
                    libusb_get_string_descriptor_ascii(h, d.iSerialNumber, (unsigned char *)serial,
                                                       255);
                libusb_close(h);
            }
            if (strcmp(serial, bc_str(s->config, "serial")))
                continue;
        }
        found++;
        if (candidate)
            libusb_unref_device(candidate);
        candidate = libusb_ref_device(devices[i]);
        snprintf(selected, sizeof selected, "%s", key);
    }
    libusb_free_device_list(devices, 1);
    if (bc_comm_check(s, e) < 0) {
        if (candidate)
            libusb_unref_device(candidate);
        goto fail;
    }
    if (found != 1) {
        if (candidate)
            libusb_unref_device(candidate);
        errno = found ? EEXIST : ENODEV;
        bc_fail(e,
                found
                    ? "Mais de um adaptador corresponde à seleção. Escolha o dispositivo USB exato."
                    : "O adaptador selecionado está ausente. Nenhum outro dispositivo será usado.");
        goto fail;
    }
    if (!probe) {
        u->lock = bc_comm_lock(selected, e);
        if (u->lock < 0) {
            libusb_unref_device(candidate);
            goto fail;
        }
    }
    rc = libusb_open(candidate, &u->handle);
    if (rc < 0) {
        libusb_unref_device(candidate);
        usb_fail(e, rc);
        goto fail;
    }
    struct libusb_device_descriptor actual_descriptor = {0};
    char actual_serial[256] = {0};
    if (libusb_get_device_descriptor(candidate, &actual_descriptor) == 0 &&
        actual_descriptor.iSerialNumber)
        libusb_get_string_descriptor_ascii(u->handle, actual_descriptor.iSerialNumber,
                                           (unsigned char *)actual_serial, 255);
    if (*bc_str(s->config, "serial") && strcmp(actual_serial, bc_str(s->config, "serial"))) {
        libusb_unref_device(candidate);
        bc_fail(e, "A série USB mudou desde a enumeração.");
        goto fail;
    }
    if (s->kind == BC_CH347) {
        int mode = bc_comm_ch347_mode(actual_descriptor.idVendor, actual_descriptor.idProduct);
        if ((mode != 1 && mode != 3) || !bc_comm_ch347_supports(mode, bc_str(s->config, "bus")) ||
            (bc_num(s->config, "ch347_mode") != -1 && bc_num(s->config, "ch347_mode") != mode)) {
            libusb_unref_device(candidate);
            errno = ENOTSUP;
            bc_fail(e,
                    "CH347T: modo USB incompatível com %s ou com o modo selecionado. Ajuste as "
                    "chaves com o USB desconectado.",
                    bc_str(s->config, "bus"));
            goto fail;
        }
        json_object_object_add(s->config, "ch347_mode", json_object_new_int(mode));
    }
    json_object_object_add(s->config, "device", json_object_new_string(selected));
    json_object_object_add(s->config, "serial", json_object_new_string(actual_serial));
    if (probe) {
        libusb_unref_device(candidate);
        usb_close(u);
        return 0;
    }
    struct libusb_config_descriptor *config = NULL;
    rc = libusb_get_active_config_descriptor(candidate, &config);
    libusb_unref_device(candidate);
    if (rc < 0) {
        usb_fail(e, rc);
        goto fail;
    }
    int endpoints = 0;
    for (unsigned i = 0; i < config->bNumInterfaces; i++)
        for (int k = 0; k < config->interface[i].num_altsetting; k++) {
            const struct libusb_interface_descriptor *d = &config->interface[i].altsetting[k];
            if (d->bInterfaceNumber != u->interface || d->bAlternateSetting != 0)
                continue;
            for (unsigned q = 0; q < d->bNumEndpoints; q++) {
                const struct libusb_endpoint_descriptor *ep = &d->endpoint[q];
                if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if (ep->bEndpointAddress & 0x80) {
                    u->in = ep->bEndpointAddress;
                    u->packet = ep->wMaxPacketSize;
                } else
                    u->out = ep->bEndpointAddress;
            }
        }
    endpoints = u->in && u->out && u->packet;
    libusb_free_config_descriptor(config);
    if (!endpoints) {
        bc_fail(e, "Interface USB não possui os endpoints esperados.");
        goto fail;
    }
    int active = libusb_kernel_driver_active(u->handle, u->interface);
    if (active == 1) {
        if (!json_object_get_boolean(bc_get(s->config, "allow_kernel_detach"))) {
            errno = EBUSY;
            bc_fail(e,
                    "Interface ocupada pelo driver do sistema. No Linux, autorize explicitamente a "
                    "liberação em Comunicação. Somente a interface selecionada será liberada.");
            goto fail;
        }
        rc = libusb_detach_kernel_driver(u->handle, u->interface);
        if (rc < 0) {
            usb_fail(e, rc);
            goto fail;
        }
        u->detached = 1;
    }
    rc = libusb_claim_interface(u->handle, u->interface);
    if (rc < 0) {
        usb_fail(e, rc);
        goto fail;
    }
    u->claimed = 1;
    s->packet_size = u->packet;
    s->io = (bc_comm_io){u, usb_write, usb_read, NULL, usb_control, usb_close};
    return 0;
fail:
    usb_close(u);
    return -1;
}
