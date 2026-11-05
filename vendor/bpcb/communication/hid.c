#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#else
#include <hidsdi.h>
#include <setupapi.h>
#endif
typedef struct {
    bc_comm *session;
    int lock;
    uint8_t pending[510];
    size_t pending_at, pending_size;
#ifndef _WIN32
    int fd;
#else
    HANDLE handle;
    unsigned input_size, output_size, feature_size;
#endif
} hid_io;

static const char *adapter(unsigned vid, unsigned pid) {
    return vid == 0x04d8 && pid == 0x00dd   ? "mcp2221a"
           : vid == 0x10c4 && pid == 0xea90 ? "cp2112"
           : vid == 0x1a86 && pid == 0x55dc ? "ch347"
                                            : NULL;
}

static int hid_fail(bc_error *e, const char *operation) {
#ifdef _WIN32
    DWORD code = GetLastError();
    errno = code == ERROR_ACCESS_DENIED                                          ? EACCES
            : code == ERROR_DEVICE_NOT_CONNECTED || code == ERROR_INVALID_HANDLE ? ENODEV
            : code == ERROR_SEM_TIMEOUT                                          ? ETIMEDOUT
                                                                                 : EIO;
    return bc_fail(e, "HID: %s falhou (Windows %lu).", operation, (unsigned long)code);
#else
    int code = errno;
    return bc_fail(e, "HID: %s: %s.", operation, strerror(code));
#endif
}

static void ch347_item(json_object *list, const char *adapter_id, int interface) {
    if (strcmp(adapter_id, "ch347"))
        return;
    json_object *item = json_object_array_get_idx(list, json_object_array_length(list) - 1);
    json_object_object_add(item, "mode", json_object_new_int(2));
    json_object_object_add(item, "interface", json_object_new_int(interface));
    json_object_object_add(item, "bus",
                           json_object_new_string(interface == 0 ? "uart" : "i2c,spi"));
}
#ifndef _WIN32
static int interface_number(const char *name) {
    char path[512], real[4096];
    snprintf(path, sizeof path, "/sys/class/hidraw/%.240s/device", name);
    if (!realpath(path, real))
        return -1;
    for (unsigned k = 0; k < 5; k++) {
        char file[4200];
        snprintf(file, sizeof file, "%s/bInterfaceNumber", real);
        FILE *f = fopen(file, "r");
        if (f) {
            unsigned v = 255;
            int ok = fscanf(f, "%x", &v);
            fclose(f);
            return ok == 1 ? (int)v : -1;
        }
        char *slash = strrchr(real, '/');
        if (!slash)
            break;
        *slash = 0;
    }
    return -1;
}
#else
static int interface_number(const char *path) {
    const char *mi = strstr(path, "mi_");
    if (!mi)
        mi = strstr(path, "MI_");
    unsigned value = 255;
    return mi && sscanf(mi + 3, "%2x", &value) == 1 ? (int)value : -1;
}
#endif
int bc_comm_hid_list(json_object *list, int (*cancel)(void *), void *context, bc_error *e) {
#ifndef _WIN32
    DIR *d = opendir("/sys/class/hidraw");
    if (!d)
        return 0;
    struct dirent *item;
    while ((item = readdir(d))) {
        if (cancel && cancel(context)) {
            closedir(d);
            return bc_fail(e, "Enumeração HID cancelada.");
        }
        if (strncmp(item->d_name, "hidraw", 6))
            continue;
        char file[512], device[256], line[2048], serial[1024] = "", name[512] = "";
        unsigned bus = 0, vid = 0, pid = 0;
        snprintf(file, sizeof file, "/sys/class/hidraw/%s/device/uevent", item->d_name);
        FILE *f = fopen(file, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3)
                continue;
            if (!strncmp(line, "HID_UNIQ=", 9))
                snprintf(serial, sizeof serial, "%.1023s", line + 9);
            if (!strncmp(line, "HID_NAME=", 9))
                snprintf(name, sizeof name, "%.511s", line + 9);
        }
        fclose(f);
        const char *a = bus == 3 ? adapter(vid, pid) : NULL;
        if (!a)
            continue;
        int interface = !strcmp(a, "ch347") ? interface_number(item->d_name) : -1;
        if (!strcmp(a, "ch347") && interface != 0 && interface != 1)
            continue;
        snprintf(device, sizeof device, "/dev/%.240s", item->d_name);
        char label[1024];
        snprintf(label, sizeof label, "%.350s · %s%s%.350s", *name ? name : a, device,
                 *serial ? " · série " : "", serial);
        if (!strcmp(a, "ch347"))
            snprintf(label, sizeof label, "CH347T M2 HID %s · %s%s%.350s",
                     interface == 0 ? "UART1" : "I²C/SPI", device, *serial ? " · série " : "",
                     serial);
        bc_comm_device(list, a, device, serial, label,
                       access(device, R_OK | W_OK) ? "Sem permissão HID" : "HID acessível");
        ch347_item(list, a, interface);
    }
    closedir(d);
    (void)e;
#else
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO devices =
        SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE)
        return hid_fail(e, "enumerar");
    SP_DEVICE_INTERFACE_DATA item = {0};
    item.cbSize = sizeof item;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devices, NULL, &guid, i, &item); i++) {
        if (cancel && cancel(context)) {
            SetupDiDestroyDeviceInfoList(devices);
            return bc_fail(e, "Enumeração HID cancelada.");
        }
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &item, NULL, 0, &needed, NULL);
        if (!needed)
            continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = calloc(1, needed);
        if (!detail)
            continue;
        detail->cbSize = sizeof *detail;
        if (SetupDiGetDeviceInterfaceDetailW(devices, &item, detail, needed, NULL, NULL)) {
            HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
            HIDD_ATTRIBUTES attributes = {0};
            attributes.Size = sizeof attributes;
            if (h != INVALID_HANDLE_VALUE && HidD_GetAttributes(h, &attributes)) {
                const char *a = adapter(attributes.VendorID, attributes.ProductID);
                if (a) {
                    wchar_t serial_w[256] = {0}, product_w[256] = {0};
                    HidD_GetSerialNumberString(h, serial_w, sizeof serial_w);
                    HidD_GetProductString(h, product_w, sizeof product_w);
                    char path[1024] = {0}, serial[1024] = {0}, product[1024] = {0}, label[2048];
                    int ok = WideCharToMultiByte(CP_UTF8, 0, detail->DevicePath, -1, path,
                                                 sizeof path, NULL, NULL);
                    WideCharToMultiByte(CP_UTF8, 0, serial_w, -1, serial, sizeof serial, NULL,
                                        NULL);
                    WideCharToMultiByte(CP_UTF8, 0, product_w, -1, product, sizeof product, NULL,
                                        NULL);
                    int interface = !strcmp(a, "ch347") ? interface_number(path) : -1;
                    if (ok && (strcmp(a, "ch347") || interface == 0 || interface == 1)) {
                        snprintf(label, sizeof label, "%.1000s%s%.1000s", *product ? product : a,
                                 *serial ? " · série " : "", serial);
                        if (!strcmp(a, "ch347"))
                            snprintf(label, sizeof label, "CH347T M2 HID %s%s%.1000s",
                                     interface == 0 ? "UART1" : "I²C/SPI",
                                     *serial ? " · série " : "", serial);
                        bc_comm_device(list, a, path, serial, label, "HID detectado");
                        ch347_item(list, a, interface);
                    }
                }
            }
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devices);
#endif
    return 0;
}

static void hid_close(void *context) {
    hid_io *h = context;
#ifndef _WIN32
    if (h->fd >= 0)
        close(h->fd);
#else
    if (h->handle != INVALID_HANDLE_VALUE)
        CloseHandle(h->handle);
#endif
    bc_comm_unlock(h->lock);
    free(h);
}
#ifdef _WIN32
static int hid_transfer(hid_io *h, void *data, size_t n, unsigned timeout, int output,
                        bc_error *e) {
    OVERLAPPED operation = {0};
    operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!operation.hEvent)
        return hid_fail(e, "criar evento");
    DWORD actual = 0;
    BOOL ok = output ? WriteFile(h->handle, data, (DWORD)n, &actual, &operation)
                     : ReadFile(h->handle, data, (DWORD)n, &actual, &operation);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        double deadline = bc_comm_now() + (double)timeout / 1000;
        for (;;) {
            DWORD wait = WaitForSingleObject(operation.hEvent, 25);
            if (wait == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(h->handle, &operation, &actual, FALSE);
                break;
            }
            if (wait == WAIT_FAILED || bc_comm_check(h->session, e) < 0 ||
                bc_comm_now() >= deadline) {
                CancelIoEx(h->handle, &operation);
                WaitForSingleObject(operation.hEvent, INFINITE);
                if (!e->text[0]) {
                    errno = ETIMEDOUT;
                    bc_fail(e, "Tempo limite HID excedido.");
                }
                CloseHandle(operation.hEvent);
                return -1;
            }
        }
    }
    CloseHandle(operation.hEvent);
    if (!ok)
        return hid_fail(e, output ? "escrever" : "ler");
    return (int)actual;
}
#else
static int hid_wait(hid_io *h, int output, unsigned timeout, bc_error *e) {
    double deadline = bc_comm_now() + (double)timeout / 1000;
    for (;;) {
        if (bc_comm_check(h->session, e) < 0)
            return -1;
        double remaining = (deadline - bc_comm_now()) * 1000;
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return bc_fail(e, "Tempo limite HID excedido.");
        }
        struct pollfd p = {h->fd, (short)(output ? POLLOUT : POLLIN), 0};
        int rc = poll(&p, 1, remaining > 25 ? 25 : (int)remaining + 1);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            return hid_fail(e, "aguardar");
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ENODEV;
            return hid_fail(e, "dispositivo desconectado");
        }
        if (rc)
            return 0;
    }
}
#endif
static int ch347_write(hid_io *h, const uint8_t *data, size_t n, bc_error *e) {
    size_t at = 0;
    while (at < n) {
        unsigned timeout = bc_comm_remaining(h->session, e);
        if (!timeout)
            return -1;
        size_t size = n - at;
        if (size > 510)
            size = 510;
        uint8_t packet[513];
        if (bc_comm_ch347_hid_encode(data + at, size, packet, e) < 0)
            return -1;
#ifdef _WIN32
        if (h->output_size != sizeof packet)
            return bc_fail(e, "CH347T: tamanho de relatório HID inesperado.");
        int rc = hid_transfer(h, packet, sizeof packet, timeout, 1, e);
#else
        if (hid_wait(h, 1, timeout, e) < 0)
            return -1;
        int rc = (int)write(h->fd, packet, sizeof packet);
        if (rc < 0)
            return hid_fail(e, "escrever CH347 HID");
#endif
        if (rc != (int)sizeof packet) {
            errno = EIO;
            return bc_fail(e, "CH347T: escrita HID incompleta; não será repetida.");
        }
        at += size;
    }
    return (int)n;
}

static int ch347_read(hid_io *h, uint8_t *data, size_t n, bc_error *e) {
    while (h->pending_at == h->pending_size) {
        unsigned timeout = bc_comm_remaining(h->session, e);
        if (!timeout)
            return -1;
        uint8_t packet[513];
        int windows = 0;
#ifdef _WIN32
        windows = 1;
        if (h->input_size != sizeof packet)
            return bc_fail(e, "CH347T: tamanho de entrada HID inesperado.");
        int rc = hid_transfer(h, packet, sizeof packet, timeout, 0, e);
#else
        if (hid_wait(h, 0, timeout, e) < 0)
            return -1;
        int rc = (int)read(h->fd, packet, 512);
        if (rc < 0)
            return hid_fail(e, "ler CH347 HID");
#endif
        if (rc < 0)
            return -1;
        rc = bc_comm_ch347_hid_decode(packet, (size_t)rc, windows, h->pending, e);
        if (rc < 0)
            return -1;
        h->pending_size = (size_t)rc;
        h->pending_at = 0;
    }
    size_t count = h->pending_size - h->pending_at;
    if (count > n)
        count = n;
    memcpy(data, h->pending + h->pending_at, count);
    h->pending_at += count;
    return (int)count;
}

static int hid_write(void *context, const uint8_t *data, size_t n, unsigned timeout, bc_error *e) {
    hid_io *h = context;
    if (h->session->kind == BC_CH347)
        return ch347_write(h, data, n, e);
    uint8_t buffer[513] = {0};
    size_t offset = h->session->kind == BC_MCP2221 ? 1 : 0;
    if (n + offset > sizeof buffer) {
        errno = EINVAL;
        return bc_fail(e, "Relatório HID grande demais.");
    }
    memcpy(buffer + offset, data, n);
#ifdef _WIN32
    size_t length = h->output_size;
    if (length < n + offset || length > sizeof buffer) {
        errno = EIO;
        return bc_fail(e, "Tamanho de relatório HID inesperado.");
    }
    int rc = hid_transfer(h, buffer, length, timeout, 1, e);
    return rc == (int)length ? (int)n : rc < 0 ? -1 : 0;
#else
    if (hid_wait(h, 1, timeout, e) < 0)
        return -1;
    ssize_t rc = write(h->fd, buffer, n + offset);
    if (rc < 0)
        return hid_fail(e, "escrever");
    return rc == (ssize_t)(n + offset) ? (int)n : 0;
#endif
}

int bc_comm_hid_normalize(bc_comm_kind kind, uint8_t *buffer, size_t actual, size_t requested,
                          bc_error *e) {
    if (requested != 64 || actual > 64 || actual < 1) {
        errno = EIO;
        return bc_fail(e, "Tamanho de resposta HID inválido.");
    }
    size_t minimum = kind == BC_MCP2221 ? 64 : buffer[0] == 0x16 ? 7 : buffer[0] == 0x13 ? 3 : 64;
    if (kind == BC_CP2112 && buffer[0] == 0x13) {
        if (actual < 3 || buffer[2] > 61) {
            errno = EIO;
            return bc_fail(e, "Cabeçalho CP2112 inválido.");
        }
        minimum += buffer[2];
    }
    if (actual < minimum) {
        errno = EIO;
        return bc_fail(e, "Relatório HID incompleto: %zu de pelo menos %zu bytes.", actual,
                       minimum);
    }
    memset(buffer + actual, 0, 64 - actual);
    return 64;
}

static int hid_read(void *context, uint8_t *data, size_t n, unsigned timeout, bc_error *e) {
    hid_io *h = context;
    if (h->session->kind == BC_CH347)
        return ch347_read(h, data, n, e);
    uint8_t buffer[513] = {0};
#ifdef _WIN32
    if (h->input_size > sizeof buffer) {
        errno = EIO;
        return bc_fail(e, "Tamanho de resposta HID inválido.");
    }
    int rc = hid_transfer(h, buffer, h->input_size, timeout, 0, e);
    if (rc < 0)
        return -1;
    size_t offset = h->session->kind == BC_MCP2221 ? 1 : 0;
    if ((size_t)rc < offset || buffer[0] != (offset ? 0 : buffer[0])) {
        errno = EIO;
        return bc_fail(e, "ID HID inválido.");
    }
    rc -= (int)offset;
    memmove(buffer, buffer + offset, (size_t)rc);
#else
    if (hid_wait(h, 0, timeout, e) < 0)
        return -1;
    int rc = (int)read(h->fd, buffer, sizeof buffer);
    if (rc < 0)
        return hid_fail(e, "ler");
#endif
    if (rc < 0)
        return -1;
    rc = bc_comm_hid_normalize(h->session->kind, buffer, (size_t)rc, n, e);
    if (rc < 0)
        return -1;
    memcpy(data, buffer, n);
    return rc;
}

static int hid_feature(void *context, uint8_t *data, size_t n, int get, unsigned timeout,
                       bc_error *e) {
    hid_io *h = context;
    (void)timeout;
#ifdef _WIN32
    uint8_t buffer[513] = {0};
    if (h->feature_size < n || h->feature_size > sizeof buffer) {
        errno = EIO;
        return bc_fail(e, "Tamanho de feature HID inválido.");
    }
    memcpy(buffer, data, n);
    BOOLEAN ok = get ? HidD_GetFeature(h->handle, buffer, h->feature_size)
                     : HidD_SetFeature(h->handle, buffer, h->feature_size);
    if (!ok)
        return hid_fail(e, "feature");
    if (get)
        memcpy(data, buffer, n);
    return (int)n;
#else
    int rc = ioctl(h->fd, get ? HIDIOCGFEATURE((unsigned)n) : HIDIOCSFEATURE((unsigned)n), data);
    if (rc < 0)
        return hid_fail(e, "feature");
    return rc;
#endif
}

int bc_comm_hid_open(bc_comm *s, bc_error *e) {
    json_object *list = json_object_new_array(), *selected = NULL;
    unsigned found = 0;
    if (bc_comm_hid_list(list, s->cancel, s->cancel_context, e) < 0) {
        json_object_put(list);
        return -1;
    }
    for (size_t i = 0; i < json_object_array_length(list); i++) {
        json_object *item = json_object_array_get_idx(list, i);
        if (bc_comm_select(item, s->config) &&
            (s->kind != BC_CH347 ||
             bc_num(item, "interface") == (!strcmp(bc_str(s->config, "bus"), "uart") ? 0 : 1))) {
            selected = item;
            found++;
        }
    }
    if (found != 1) {
        json_object_put(list);
        errno = found ? EEXIST : ENODEV;
        return bc_fail(e, found
                              ? "Mais de um HID corresponde à seleção. Escolha o dispositivo exato."
                              : "O adaptador HID selecionado está ausente.");
    }
    hid_io *h = calloc(1, sizeof *h);
    if (!h) {
        json_object_put(list);
        return bc_fail(e, "Memória insuficiente.");
    }
    h->session = s;
    h->lock = -1;
#ifndef _WIN32
    h->fd = -1;
#else
    h->handle = INVALID_HANDLE_VALUE;
#endif
    h->lock = bc_comm_lock(bc_str(selected, "device"), e);
    if (h->lock < 0)
        goto fail;
#ifndef _WIN32
    h->fd = open(bc_str(selected, "device"), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (h->fd < 0) {
        hid_fail(e, "abrir");
        goto fail;
    }
    struct hidraw_devinfo attributes = {0};
    if (ioctl(h->fd, HIDIOCGRAWINFO, &attributes) < 0 ||
        !adapter((unsigned short)attributes.vendor, (unsigned short)attributes.product) ||
        strcmp(adapter((unsigned short)attributes.vendor, (unsigned short)attributes.product),
               bc_str(s->config, "adapter"))) {
        bc_fail(e, "HID mudou desde a enumeração; selecione novamente.");
        goto fail;
    }
    if (s->kind == BC_CH347) {
        char physical[512] = {0};
        unsigned iface = 255;
        if (ioctl(h->fd, HIDIOCGRAWPHYS(sizeof physical), physical) < 0) {
            hid_fail(e, "identificar interface HID");
            goto fail;
        }
        physical[sizeof physical - 1] = 0;
        char *input = strrchr(physical, '/');
        if (!input || sscanf(input, "/input%u", &iface) != 1 ||
            iface != (unsigned)bc_num(selected, "interface")) {
            bc_fail(e, "CH347T: interface HID mudou desde a enumeração.");
            goto fail;
        }
    }
#else
    wchar_t path[1024];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bc_str(selected, "device"), -1, path,
                             1024)) {
        bc_fail(e, "Caminho HID inválido.");
        goto fail;
    }
    h->handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, NULL);
    if (h->handle == INVALID_HANDLE_VALUE) {
        hid_fail(e, "abrir");
        goto fail;
    }
    HIDD_ATTRIBUTES attributes = {0};
    attributes.Size = sizeof attributes;
    wchar_t serial_w[256] = {0};
    char serial[1024] = {0};
    HidD_GetSerialNumberString(h->handle, serial_w, sizeof serial_w);
    WideCharToMultiByte(CP_UTF8, 0, serial_w, -1, serial, sizeof serial, NULL, NULL);
    if (!HidD_GetAttributes(h->handle, &attributes) ||
        !adapter(attributes.VendorID, attributes.ProductID) ||
        strcmp(adapter(attributes.VendorID, attributes.ProductID), bc_str(s->config, "adapter")) ||
        (*bc_str(s->config, "serial") && strcmp(serial, bc_str(s->config, "serial")))) {
        bc_fail(e, "HID mudou desde a enumeração; selecione novamente.");
        goto fail;
    }
    PHIDP_PREPARSED_DATA parsed = NULL;
    HIDP_CAPS caps = {0};
    if (!HidD_GetPreparsedData(h->handle, &parsed)) {
        hid_fail(e, "descritor");
        goto fail;
    }
    NTSTATUS rc = HidP_GetCaps(parsed, &caps);
    HidD_FreePreparsedData(parsed);
    if (rc != HIDP_STATUS_SUCCESS) {
        bc_fail(e, "Descritor HID inválido.");
        goto fail;
    }
    h->input_size = caps.InputReportByteLength;
    h->output_size = caps.OutputReportByteLength;
    h->feature_size = caps.FeatureReportByteLength;
    if (s->kind == BC_CH347 && (h->input_size != 513 || h->output_size != 513)) {
        bc_fail(e, "CH347T: relatórios HID devem ter 512 bytes e ID zero.");
        goto fail;
    }
#endif
#ifndef _WIN32
    if (*bc_str(s->config, "serial")) {
        char serial[1024] = {0};
        int length = ioctl(h->fd, HIDIOCGRAWUNIQ(sizeof serial), serial);
        serial[sizeof serial - 1] = 0;
        if (length < 0 || strcmp(serial, bc_str(s->config, "serial"))) {
            bc_fail(e, "A série do HID mudou desde a enumeração; selecione novamente.");
            goto fail;
        }
    }
#endif
    json_object_object_add(s->config, "device", json_object_get(bc_get(selected, "device")));
    json_object_object_add(s->config, "serial", json_object_get(bc_get(selected, "serial")));
    s->io = (bc_comm_io){h, hid_write, hid_read, hid_feature, NULL, hid_close};
    json_object_put(list);
    return 0;
fail:
    json_object_put(list);
    hid_close(h);
    return -1;
}
