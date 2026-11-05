#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include "internal.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#else
#include <devguid.h>
#include <setupapi.h>
#endif
typedef struct {
    bc_comm *session;
    int lock;
#ifndef _WIN32
    int fd;
    struct termios previous;
    int restore, exclusive;
#else
    HANDLE handle;
    DCB previous;
    COMMTIMEOUTS timeouts;
    int restore;
#endif
} serial_io;

const char *bc_comm_serial_adapter(unsigned vid, unsigned pid) {
    if (vid == 0x1a86 && (pid == 0x7523 || pid == 0x5523 || pid == 0x55d4))
        return "ch340";
    if (vid == 0x1a86 && (pid == 0x55d9 || pid == 0x55db || pid == 0x55dd))
        return "ch347";
    if (vid == 0x10c4 && pid == 0xea60)
        return "cp2102";
    if (vid == 0x0403 && pid == 0x6001)
        return "ft232rl";
    if (vid == 0x0403 && pid == 0x6014)
        return "ft232h";
    if (vid == 0x04d8 && pid == 0x00dd)
        return "mcp2221a";
    return "serial";
}

static void ch347_serial_item(json_object *list, unsigned vid, unsigned pid) {
    int mode = bc_comm_ch347_mode(vid, pid);
    if (mode < 0 || mode == 2)
        return;
    json_object *item = json_object_array_get_idx(list, json_object_array_length(list) - 1);
    json_object_object_add(item, "mode", json_object_new_int(mode));
    json_object_object_add(item, "bus", json_object_new_string("uart"));
}
#ifndef _WIN32
static void read_text(const char *base, const char *file, char *out, size_t cap) {
    char path[4096];
    snprintf(path, sizeof path, "%.3800s/%s", base, file);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(out, (int)cap, f))
        out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}
#endif
int bc_comm_serial_list(json_object *list, bc_error *e) {
#ifndef _WIN32
    DIR *d = opendir("/sys/class/tty");
    if (!d)
        return 0;
    struct dirent *item;
    while ((item = readdir(d))) {
        if (strncmp(item->d_name, "ttyUSB", 6) && strncmp(item->d_name, "ttyACM", 6) &&
            strncmp(item->d_name, "ttyS", 4))
            continue;
        char path[4096], real[4096], port[300];
        snprintf(path, sizeof path, "/sys/class/tty/%s/device", item->d_name);
        if (!realpath(path, real))
            continue;
        unsigned vid = 0, pid = 0;
        char serial[256] = "", product[256] = "";
        for (unsigned up = 0; up < 10; up++) {
            char a[32] = "", b[32] = "";
            read_text(real, "idVendor", a, sizeof a);
            read_text(real, "idProduct", b, sizeof b);
            if (*a && *b) {
                vid = (unsigned)strtoul(a, NULL, 16);
                pid = (unsigned)strtoul(b, NULL, 16);
                read_text(real, "serial", serial, sizeof serial);
                read_text(real, "product", product, sizeof product);
                break;
            }
            char *slash = strrchr(real, '/');
            if (!slash || slash == real)
                break;
            *slash = 0;
        }
        snprintf(port, sizeof port, "/dev/%.255s", item->d_name);
        if (access(port, F_OK))
            continue;
        char label[1024];
        snprintf(label, sizeof label, "%s · %.250s%s%.250s", port,
                 *product ? product : bc_comm_serial_adapter(vid, pid), *serial ? " · série " : "",
                 serial);
        bc_comm_device(list, bc_comm_serial_adapter(vid, pid), port, serial, label,
                       access(port, R_OK | W_OK) ? "Sem permissão serial"
                                                 : "Porta serial detectada");
        ch347_serial_item(list, vid, pid);
    }
    closedir(d);
    (void)e;
#else
    HDEVINFO devices = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE)
        return bc_fail(e, "Não foi possível enumerar as portas COM.");
    SP_DEVINFO_DATA item = {0};
    item.cbSize = sizeof item;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &item); i++) {
        HKEY key = SetupDiOpenDevRegKey(devices, &item, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        char port[128] = {0};
        DWORD type = 0, size = sizeof port;
        if (RegQueryValueExA(key, "PortName", NULL, &type, (BYTE *)port, &size) != ERROR_SUCCESS) {
            RegCloseKey(key);
            continue;
        }
        RegCloseKey(key);
        if (strncmp(port, "COM", 3))
            continue;
        char friendly[512] = {0}, instance[1024] = {0};
        SetupDiGetDeviceRegistryPropertyA(devices, &item, SPDRP_FRIENDLYNAME, NULL,
                                          (BYTE *)friendly, sizeof friendly, NULL);
        SetupDiGetDeviceInstanceIdA(devices, &item, instance, sizeof instance, NULL);
        unsigned vid = 0, pid = 0;
        char *v = strstr(instance, "VID_"), *p = strstr(instance, "PID_");
        if (v && p) {
            sscanf(v + 4, "%x", &vid);
            sscanf(p + 4, "%x", &pid);
        }
        const char *serial = strrchr(instance, '\\');
        serial = serial ? serial + 1 : "";
        bc_comm_device(list, bc_comm_serial_adapter(vid, pid), port, serial,
                       *friendly ? friendly : port, "Porta COM detectada");
        ch347_serial_item(list, vid, pid);
    }
    SetupDiDestroyDeviceInfoList(devices);
#endif
    return 0;
}

int bc_comm_serial_probe(json_object *c, bc_error *e) {
    const char *port = bc_str(c, "device");
#ifndef _WIN32
    struct stat st;
    if (strncmp(port, "/dev/", 5) || stat(port, &st) < 0 || !S_ISCHR(st.st_mode))
        return bc_fail(e,
                       "Selecione uma porta serial existente em /dev (dispositivo de caracteres).");
    if (access(port, R_OK | W_OK))
        return bc_fail(
            e, "Sem permissão de leitura/escrita na porta serial. Confira o grupo dialout.");
#else
    if (strncmp(port, "COM", 3) || !isdigit((unsigned char)port[3]))
        return bc_fail(e, "Use uma porta COM válida, como COM3.");
    for (const char *p = port + 3; *p; p++)
        if (!isdigit((unsigned char)*p))
            return bc_fail(e, "Porta COM inválida.");
    char target[1024];
    if (!QueryDosDeviceA(port, target, sizeof target))
        return bc_fail(e, "A porta COM selecionada está ausente.");
#endif
    if (strcmp(bc_str(c, "adapter"), "serial") || *bc_str(c, "serial")) {
        json_object *list = json_object_new_array();
        if (bc_comm_serial_list(list, e) < 0) {
            json_object_put(list);
            return -1;
        }
        int found = 0;
        for (size_t i = 0; i < json_object_array_length(list); i++) {
            json_object *item = json_object_array_get_idx(list, i);
            if (!strcmp(bc_str(item, "device"), port) &&
                (!strcmp(bc_str(c, "adapter"), "serial") ||
                 !strcmp(bc_str(item, "adapter"), bc_str(c, "adapter"))) &&
                (!*bc_str(c, "serial") || !strcmp(bc_str(item, "serial"), bc_str(c, "serial")))) {
                if (!strcmp(bc_str(c, "adapter"), "ch347")) {
                    int actual = (int)bc_num(item, "mode");
                    if (bc_num(c, "ch347_mode") != -1 && bc_num(c, "ch347_mode") != actual)
                        continue;
                    json_object_object_add(c, "ch347_mode", json_object_new_int(actual));
                }
                found++;
            }
        }
        json_object_put(list);
        if (found != 1)
            return bc_fail(e, "A porta não corresponde ao modelo/série selecionado. Atualize a "
                              "lista; nenhum substituto foi escolhido.");
    }
    return 0;
}

static int serial_fail(bc_error *e, const char *op) {
#ifdef _WIN32
    DWORD code = GetLastError();
    errno = code == ERROR_ACCESS_DENIED                                          ? EACCES
            : code == ERROR_DEVICE_NOT_CONNECTED || code == ERROR_FILE_NOT_FOUND ? ENODEV
                                                                                 : EIO;
    return bc_fail(e, "Serial: %s falhou (Windows %lu).", op, (unsigned long)code);
#else
    int saved = errno;
    return bc_fail(e, "Serial: %s: %s.", op, strerror(saved));
#endif
}

static void serial_close(void *context) {
    serial_io *s = context;
#ifndef _WIN32
    if (s->fd >= 0) {
        if (s->restore)
            tcsetattr(s->fd, TCSANOW, &s->previous);
        if (s->exclusive)
            ioctl(s->fd, TIOCNXCL);
        close(s->fd);
    }
#else
    if (s->handle != INVALID_HANDLE_VALUE) {
        if (s->restore) {
            SetCommState(s->handle, &s->previous);
            SetCommTimeouts(s->handle, &s->timeouts);
        }
        CloseHandle(s->handle);
    }
#endif
    bc_comm_unlock(s->lock);
    free(s);
}
#ifndef _WIN32
static speed_t speed(unsigned baud) {
    switch (baud) {
        case 300:
            return B300;
        case 600:
            return B600;
        case 1200:
            return B1200;
        case 2400:
            return B2400;
        case 4800:
            return B4800;
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        case 230400:
            return B230400;
        case 460800:
            return B460800;
        case 500000:
            return B500000;
        case 576000:
            return B576000;
        case 921600:
            return B921600;
        case 1000000:
            return B1000000;
        case 1500000:
            return B1500000;
        case 2000000:
            return B2000000;
        case 3000000:
            return B3000000;
        default:
            return 0;
    }
}

static int serial_wait(serial_io *s, int writing, unsigned timeout, bc_error *e) {
    double deadline = bc_comm_now() + (double)timeout / 1000;
    for (;;) {
        if (bc_comm_check(s->session, e) < 0)
            return -1;
        double remain = (deadline - bc_comm_now()) * 1000;
        if (remain <= 0) {
            errno = ETIMEDOUT;
            return bc_fail(e, "Tempo limite serial excedido.");
        }
        struct pollfd p = {s->fd, (short)(writing ? POLLOUT : POLLIN), 0};
        int rc = poll(&p, 1, remain > 25 ? 25 : (int)remain + 1);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            return serial_fail(e, "aguardar");
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ENODEV;
            return bc_fail(e, "Porta serial desconectada.");
        }
        if (rc)
            return 0;
    }
}
#endif
static int serial_drain(serial_io *s, size_t bytes, bc_error *e) {
    for (;;) {
        if (bc_comm_check(s->session, e) < 0)
            return -1;
#ifndef _WIN32
        int queued = 0;
        if (ioctl(s->fd, TIOCOUTQ, &queued) < 0)
            return serial_fail(e, "conferir fila de transmissão");
#else
        DWORD errors = 0;
        COMSTAT state = {0};
        if (!ClearCommError(s->handle, &errors, &state))
            return serial_fail(e, "conferir fila de transmissão");
        DWORD queued = state.cbOutQue;
#endif
        if (!queued)
            break;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    // USB bridges may empty the OS queue into their UART FIFO before the last bit.
    // Keep framing/handle alive for a conservative full-block serialization time.
    unsigned frame = 1u + (unsigned)bc_num(s->session->config, "data_bits") +
                     (unsigned)bc_num(s->session->config, "stop_bits") +
                     (strcmp(bc_str(s->session->config, "parity"), "none") ? 1u : 0u);
    double end = bc_comm_now() + (double)bytes * frame / bc_num(s->session->config, "baud");
    while (bc_comm_now() < end) {
        if (bc_comm_check(s->session, e) < 0)
            return -1;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    return bc_comm_check(s->session, e);
}

static int serial_write(void *context, const uint8_t *data, size_t n, unsigned timeout,
                        bc_error *e) {
    serial_io *s = context;
    size_t at = 0;
    while (at < n) {
        if (bc_comm_check(s->session, e) < 0)
            return -1;
#ifndef _WIN32
        if (serial_wait(s, 1, timeout, e) < 0)
            return -1;
        ssize_t count = write(s->fd, data + at, n - at);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count <= 0)
            return serial_fail(e, "escrever");
        at += (size_t)count;
#else
        DWORD count = 0;
        if (!WriteFile(s->handle, data + at, (DWORD)(n - at), &count, NULL))
            return serial_fail(e, "escrever");
        if (!count) {
            errno = ETIMEDOUT;
            return bc_fail(e, "Tempo limite da escrita serial.");
        }
        at += count;
#endif
    }
    (void)timeout;
    if (serial_drain(s, n, e) < 0)
        return -1;
    return (int)n;
}

static int serial_read(void *context, uint8_t *data, size_t n, unsigned timeout, bc_error *e) {
    serial_io *s = context;
#ifndef _WIN32
    for (;;) {
        if (serial_wait(s, 0, timeout, e) < 0)
            return -1;
        ssize_t count = read(s->fd, data, n);
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (count < 0)
            return serial_fail(e, "ler");
        return (int)count;
    }
#else
    double deadline = bc_comm_now() + (double)timeout / 1000;
    for (;;) {
        if (bc_comm_check(s->session, e) < 0)
            return -1;
        DWORD count = 0;
        if (!ReadFile(s->handle, data, (DWORD)n, &count, NULL))
            return serial_fail(e, "ler");
        if (count)
            return (int)count;
        if (bc_comm_now() >= deadline) {
            errno = ETIMEDOUT;
            return bc_fail(e, "Tempo limite da leitura serial.");
        }
    }
#endif
}

int bc_comm_serial_open(bc_comm *s, bc_error *e) {
    if (bc_comm_serial_probe(s->config, e) < 0)
        return -1;
    if (!*bc_str(s->config, "serial")) {
        json_object *list = json_object_new_array();
        if (bc_comm_serial_list(list, e) < 0) {
            json_object_put(list);
            return -1;
        }
        for (size_t i = 0; i < json_object_array_length(list); i++) {
            json_object *item = json_object_array_get_idx(list, i);
            if (!strcmp(bc_str(item, "device"), bc_str(s->config, "device"))) {
                json_object_object_add(s->config, "serial",
                                       json_object_get(bc_get(item, "serial")));
                break;
            }
        }
        json_object_put(list);
    }
    serial_io *u = calloc(1, sizeof *u);
    if (!u)
        return bc_fail(e, "Memória insuficiente.");
    u->session = s;
    u->lock = -1;
#ifndef _WIN32
    u->fd = -1;
#else
    u->handle = INVALID_HANDLE_VALUE;
#endif
    u->lock = bc_comm_lock(bc_str(s->config, "device"), e);
    if (u->lock < 0)
        goto fail;
#ifndef _WIN32
    u->fd = open(bc_str(s->config, "device"), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (u->fd < 0) {
        serial_fail(e, "abrir");
        goto fail;
    }
    if (ioctl(u->fd, TIOCEXCL) < 0) {
        serial_fail(e, "obter acesso exclusivo");
        goto fail;
    }
    u->exclusive = 1;
    if (!isatty(u->fd) || tcgetattr(u->fd, &u->previous) < 0) {
        serial_fail(e, "identificar terminal");
        goto fail;
    }
    u->restore = 1;
    struct termios t = u->previous;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD | CRTSCTS);
    t.c_cflag |= bc_num(s->config, "data_bits") == 7 ? CS7 : CS8;
    if (bc_num(s->config, "stop_bits") == 2)
        t.c_cflag |= CSTOPB;
    if (strcmp(bc_str(s->config, "parity"), "none")) {
        t.c_cflag |= PARENB;
        if (!strcmp(bc_str(s->config, "parity"), "odd"))
            t.c_cflag |= PARODD;
        t.c_iflag |= INPCK;
    }
    if (!strcmp(bc_str(s->config, "flow_control"), "rtscts"))
        t.c_cflag |= CRTSCTS;
    speed_t baud = speed((unsigned)bc_num(s->config, "baud"));
    if (!baud) {
        bc_fail(e, "Baud rate não disponível neste sistema; use uma velocidade UART padrão.");
        goto fail;
    }
    cfsetispeed(&t, baud);
    cfsetospeed(&t, baud);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(u->fd, TCSANOW, &t) < 0) {
        serial_fail(e, "configurar");
        goto fail;
    }
    int modem = 0;
    if (ioctl(u->fd, TIOCMGET, &modem) == 0) {
        modem = json_object_get_boolean(bc_get(s->config, "dtr")) ? modem | TIOCM_DTR
                                                                  : modem & ~TIOCM_DTR;
        if (strcmp(bc_str(s->config, "flow_control"), "rtscts"))
            modem = json_object_get_boolean(bc_get(s->config, "rts")) ? modem | TIOCM_RTS
                                                                      : modem & ~TIOCM_RTS;
        if (ioctl(u->fd, TIOCMSET, &modem) < 0) {
            serial_fail(e, "configurar DTR/RTS");
            goto fail;
        }
    }
    if (tcflush(u->fd, TCIFLUSH) < 0) {
        serial_fail(e, "limpar entrada anterior");
        goto fail;
    }
#else
    char path[160];
    snprintf(path, sizeof path, "\\\\.\\%s", bc_str(s->config, "device"));
    u->handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (u->handle == INVALID_HANDLE_VALUE) {
        serial_fail(e, "abrir");
        goto fail;
    }
    u->previous.DCBlength = sizeof u->previous;
    if (!GetCommState(u->handle, &u->previous) || !GetCommTimeouts(u->handle, &u->timeouts)) {
        serial_fail(e, "ler configuração");
        goto fail;
    }
    u->restore = 1;
    DCB d = u->previous;
    d.BaudRate = (DWORD)bc_num(s->config, "baud");
    d.ByteSize = (BYTE)bc_num(s->config, "data_bits");
    d.StopBits = bc_num(s->config, "stop_bits") == 2 ? TWOSTOPBITS : ONESTOPBIT;
    d.Parity = !strcmp(bc_str(s->config, "parity"), "even")  ? EVENPARITY
               : !strcmp(bc_str(s->config, "parity"), "odd") ? ODDPARITY
                                                             : NOPARITY;
    d.fParity = d.Parity != NOPARITY;
    d.fBinary = TRUE;
    d.fOutX = d.fInX = FALSE;
    d.fOutxDsrFlow = FALSE;
    d.fDsrSensitivity = FALSE;
    d.fAbortOnError = FALSE;
    d.fDtrControl = json_object_get_boolean(bc_get(s->config, "dtr")) ? DTR_CONTROL_ENABLE
                                                                      : DTR_CONTROL_DISABLE;
    d.fOutxCtsFlow = !strcmp(bc_str(s->config, "flow_control"), "rtscts");
    d.fRtsControl = d.fOutxCtsFlow                                      ? RTS_CONTROL_HANDSHAKE
                    : json_object_get_boolean(bc_get(s->config, "rts")) ? RTS_CONTROL_ENABLE
                                                                        : RTS_CONTROL_DISABLE;
    COMMTIMEOUTS t = {0};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = 25;
    t.WriteTotalTimeoutConstant = (DWORD)bc_num(s->config, "timeout_ms");
    if (!SetCommState(u->handle, &d) || !SetCommTimeouts(u->handle, &t) ||
        !PurgeComm(u->handle, PURGE_RXCLEAR)) {
        serial_fail(e, "configurar");
        goto fail;
    }
#endif
    s->io = (bc_comm_io){u, serial_write, serial_read, NULL, NULL, serial_close};
    return 0;
fail:
    serial_close(u);
    return -1;
}
