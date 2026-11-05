#define _POSIX_C_SOURCE 200809L
#include "../lab.h"
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    json_object *request, *result, *trace;
    bc_lab_io *io;
    unsigned address;
    double deadline;
} device;

static void str(json_object *o, const char *k, const char *v) {
    json_object_object_add(o, k, json_object_new_string(v));
}

static void val(json_object *o, const char *k, double v) {
    json_object_object_add(o, k, json_object_new_double(v));
}

static const struct {
    const char *id, *name, *metrics, *actions;
    int address;
} devices[] = {
    {"bme280", "BME280", "temperature,pressure,humidity", "read,measure", 0x76},
    {"bmp280", "BMP280", "temperature,pressure", "read,measure", 0x76},
    {"ina219", "INA219", "bus_voltage,shunt_voltage,current,power", "read,measure", 0x40},
    {"mpu6050", "MPU-6050", "accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z,temperature",
     "read,measure", 0x68},
    {"ssd1306", "OLED SSD1306", "acknowledged", "pattern,off", 0x3c}};

static const struct {
    const char *id, *label, *unit;
} metrics[] = {{"temperature", "Temperatura (°C)", "°C"},
               {"pressure", "Pressão (Pa)", "Pa"},
               {"humidity", "Umidade relativa (%RH)", "%RH"},
               {"bus_voltage", "Tensão do barramento (V)", "V"},
               {"shunt_voltage", "Tensão no shunt (V)", "V"},
               {"current", "Corrente (A)", "A"},
               {"power", "Potência (W)", "W"},
               {"accel_x", "Aceleração X (m/s²)", "m/s²"},
               {"accel_y", "Aceleração Y (m/s²)", "m/s²"},
               {"accel_z", "Aceleração Z (m/s²)", "m/s²"},
               {"gyro_x", "Velocidade angular X (°/s)", "°/s"},
               {"gyro_y", "Velocidade angular Y (°/s)", "°/s"},
               {"gyro_z", "Velocidade angular Z (°/s)", "°/s"},
               {"acknowledged", "Resposta de comunicação (ACK)", "ACK"}};

const char *bc_lab_metric_label(const char *id) {
    for (size_t i = 0; i < sizeof metrics / sizeof *metrics; i++)
        if (!strcmp(id, metrics[i].id))
            return metrics[i].label;
    return id;
}

const char *bc_lab_metric_unit(const char *id) {
    for (size_t i = 0; i < sizeof metrics / sizeof *metrics; i++)
        if (!strcmp(id, metrics[i].id))
            return metrics[i].unit;
    return "";
}

static int token(const char *list, const char *value) {
    char in[160], find[80];
    if (!value || !strlen(value) || strlen(value) > 60)
        return 0;
    snprintf(in, sizeof in, ",%s,", list);
    snprintf(find, sizeof find, ",%s,", value);
    return strstr(in, find) != NULL;
}

json_object *bc_peripheral_catalog(void) {
    json_object *a = json_object_new_array();
    for (size_t i = 0; i < sizeof devices / sizeof *devices; i++) {
        json_object *r = json_object_new_object();
        str(r, "id", devices[i].id);
        str(r, "label", devices[i].name);
        str(r, "metrics", devices[i].metrics);
        str(r, "actions", devices[i].actions);
        str(r, "bus", "i2c");
        json_object_object_add(r, "address", json_object_new_int(devices[i].address));
        json_object_array_add(a, r);
    }
    return a;
}

int bc_peripheral_validate(const char *bus, json_object *op, bc_error *e) {
    if (strcmp(bus, "i2c") ||
        bc_lab_object(op,
                      "|kind||device||action||metric||address||shunt_ohms||current_lsb||range||"
                      "gyro_range||height||pattern||label|",
                      e) < 0)
        return bc_fail(e, "Componente IoT exige I²C e campos conhecidos.");
    if (strcmp(bc_str(op, "kind"), "peripheral"))
        return bc_fail(e, "Tipo de componente inválido.");
    size_t index = 0;
    while (index < sizeof devices / sizeof *devices &&
           strcmp(devices[index].id, bc_str(op, "device")))
        index++;
    if (index == sizeof devices / sizeof *devices ||
        !token(devices[index].actions, bc_str(op, "action")) ||
        !token(devices[index].metrics, bc_str(op, "metric")))
        return bc_fail(e, "Modelo, ação ou grandeza do componente inválidos.");
    int n;
    double v;
    if (bc_lab_int(op, "address", devices[index].address, 8, 119, &n, e) < 0)
        return -1;
    if (index == 2 && (bc_lab_number(op, "shunt_ohms", 0.1, 0.000001, 100, &v, e) < 0 ||
                       bc_lab_number(op, "current_lsb", 0.0001, 1e-9, 1, &v, e) < 0))
        return -1;
    if (index == 3 &&
        (bc_lab_int(op, "range", 2, 2, 16, &n, e) < 0 || (n != 2 && n != 4 && n != 8 && n != 16) ||
         bc_lab_int(op, "gyro_range", 250, 250, 2000, &n, e) < 0 ||
         (n != 250 && n != 500 && n != 1000 && n != 2000)))
        return bc_fail(e, "Escala MPU-6050 inválida.");
    if (index == 4 && (bc_lab_int(op, "height", 64, 32, 64, &n, e) < 0 || (n != 32 && n != 64)))
        return bc_fail(e, "Altura SSD1306 inválida.");
    if (index == 4 && bc_get(op, "pattern") &&
        !token("checkerboard,bars,black,white", bc_str(op, "pattern")))
        return bc_fail(e, "Padrão OLED inválido.");
    return 0;
}

static int active(device *d, bc_error *e) {
    double now = d->io->clock(d->io->context);
    if (!isfinite(now) || !isfinite(d->deadline) ||
        (d->io->cancelled && d->io->cancelled(d->io->context)) || now > d->deadline)
        return bc_fail(e, "Componente cancelado ou tempo de 5 s esgotado.");
    return 0;
}

static int pause_device(device *d, unsigned ms, bc_error *e) {
    if (active(d, e) < 0)
        return -1;
    return d->io->wait_ms ? d->io->wait_ms(d->io->context, ms, e)
                          : bc_fail(e, "Transporte não oferece espera limitada.");
}

static int exchange(device *d, const uint8_t *tx, size_t nt, uint8_t *rx, size_t nr, bc_error *e) {
    if (active(d, e) < 0 || json_object_array_length(d->trace) >= BC_LAB_TRACE)
        return bc_fail(e, "Limite de operações do componente atingido.");
    json_object *op = json_object_new_object();
    char *h = bc_comm_format_bytes(tx, nt, 1);
    str(op, "write", h);
    free(h);
    json_object_object_add(op, "read", json_object_new_int64((int64_t)nr));
    json_object_object_add(op, "address", json_object_new_int((int)d->address));
    json_object *r = d->io->transaction(d->io->context, op, e);
    if (!r) {
        json_object_put(op);
        return -1;
    }
    if (active(d, e) < 0) {
        json_object_put(op);
        json_object_put(r);
        return -1;
    }
    uint8_t data[32];
    size_t size = 0;
    int rc = bc_comm_hex(bc_str(r, "received"), data, sizeof data, &size, e);
    if (rc < 0 || size != nr) {
        json_object_put(op);
        json_object_put(r);
        return bc_fail(e, "Componente devolveu comprimento diferente do esperado.");
    }
    if (nr)
        memcpy(rx, data, nr);
    json_object_object_add(r, "operation", op);
    json_object_array_add(d->trace, r);
    return 0;
}

static int readreg(device *d, uint8_t reg, uint8_t *out, size_t n, bc_error *e) {
    return exchange(d, &reg, 1, out, n, e);
}

static int write8(device *d, uint8_t reg, uint8_t v, bc_error *e) {
    uint8_t b[] = {reg, v};
    return exchange(d, b, 2, NULL, 0, e);
}

static int write16(device *d, uint8_t reg, unsigned v, bc_error *e) {
    uint8_t b[] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
    return exchange(d, b, 3, NULL, 0, e);
}

static unsigned u16le(const uint8_t *b) {
    return (unsigned)b[0] | ((unsigned)b[1] << 8);
}

static unsigned u16be(const uint8_t *b) {
    return ((unsigned)b[0] << 8) | b[1];
}

static int sign16(unsigned n) {
    return n < 32768 ? (int)n : (int)n - 65536;
}

static int sign12(unsigned n) {
    return n < 2048 ? (int)n : (int)n - 4096;
}

static int reading(device *d, const char *name, double value, const char *unit, bc_error *e) {
    if (!isfinite(value))
        return bc_fail(e, "Conversão não finita em %s.", name);
    json_object *r = json_object_new_object();
    val(r, "value", value);
    str(r, "unit", unit);
    json_object_object_add(bc_get(d->result, "readings"), name, r);
    return 0;
}

static int bosch(device *d, int humidity, bc_error *e) {
    uint8_t id, status, c[26], h[7] = {0}, raw[8] = {0};
    if (readreg(d, 0xd0, &id, 1, e) < 0)
        return -1;
    if (id != (humidity ? 0x60 : 0x58))
        return bc_fail(e, "Chip ID 0x%02X não corresponde ao %s.", id,
                       humidity ? "BME280" : "BMP280");
    /* Wait for the NVM copy before reading factory calibration; no retry of writes. */
    for (unsigned i = 0;; i++) {
        if (readreg(d, 0xf3, &status, 1, e) < 0)
            return -1;
        if (!(status & 1))
            break;
        if (i >= 99)
            return bc_fail(e, "Calibração Bosch indisponível.");
        if (pause_device(d, 5, e) < 0)
            return -1;
    }
    if (readreg(d, 0x88, c, humidity ? 26 : 24, e) < 0 ||
        (humidity && readreg(d, 0xe1, h, 7, e) < 0))
        return -1;
    double t1 = u16le(c), t2 = sign16(u16le(c + 2)), t3 = sign16(u16le(c + 4)), p1 = u16le(c + 6),
           p2 = sign16(u16le(c + 8)), p3 = sign16(u16le(c + 10)), p4 = sign16(u16le(c + 12)),
           p5 = sign16(u16le(c + 14)), p6 = sign16(u16le(c + 16)), p7 = sign16(u16le(c + 18)),
           p8 = sign16(u16le(c + 20)), p9 = sign16(u16le(c + 22));
    if (t1 == 0 || t1 == 65535 || p1 == 0 || p1 == 65535)
        return bc_fail(e, "Coeficientes Bosch inválidos.");
    if (!strcmp(bc_str(d->request, "action"), "measure")) {
        if (write8(d, 0xf4, 0, e) < 0)
            return -1; /* Sleep permits changing oversampling. */
        for (unsigned i = 0;; i++) {
            if (readreg(d, 0xf3, &status, 1, e) < 0)
                return -1;
            if (!(status & 8))
                break;
            if (i >= 99)
                return bc_fail(e, "Sensor não entrou em repouso.");
            if (pause_device(d, 5, e) < 0)
                return -1;
        }
        if ((humidity && write8(d, 0xf2, 1, e) < 0) || write8(d, 0xf4, 0x25, e) < 0 ||
            pause_device(d, 12, e) < 0)
            return -1;
    }
    for (unsigned i = 0;; i++) {
        if (readreg(d, 0xf3, &status, 1, e) < 0)
            return -1;
        if (!(status & 9))
            break;
        if (i >= 99)
            return bc_fail(e, "Conversão Bosch não concluiu.");
        if (pause_device(d, 5, e) < 0)
            return -1;
    }
    if (readreg(d, 0xf7, raw, humidity ? 8 : 6, e) < 0)
        return -1;
    unsigned ap = ((unsigned)raw[0] << 12) | ((unsigned)raw[1] << 4) | (raw[2] >> 4),
             at = ((unsigned)raw[3] << 12) | ((unsigned)raw[4] << 4) | (raw[5] >> 4);
    if (ap == 0x80000 || at == 0x80000)
        return bc_fail(e, "Canal Bosch não foi medido.");
    double a = ((double)at / 16384.0 - t1 / 1024.0) * t2, b = ((double)at / 131072.0 - t1 / 8192.0);
    b = b * b * t3;
    double fine = a + b, temp = fine / 5120.0;
    a = fine / 2.0 - 64000.0;
    b = a * a * p6 / 32768.0;
    b += a * p5 * 2.0;
    b = b / 4.0 + p4 * 65536.0;
    a = (p3 * a * a / 524288.0 + p2 * a) / 524288.0;
    a = (1 + a / 32768.0) * p1;
    if (fabs(a) < 1e-12)
        return bc_fail(e, "Coeficiente de pressão zero.");
    double pressure = (1048576.0 - ap - b / 4096.0) * 6250.0 / a;
    pressure += (p9 * pressure * pressure / 2147483648.0 + pressure * p8 / 32768.0 + p7) / 16.0;
    if (temp < -40 || temp > 85 || pressure < 30000 || pressure > 110000)
        return bc_fail(e, "Leitura Bosch fora da faixa física especificada.");
    if (reading(d, "temperature", temp, "°C", e) < 0 ||
        reading(d, "pressure", pressure, "Pa", e) < 0)
        return -1;
    if (humidity) {
        double h1 = c[25], h2 = sign16(u16le(h)), h3 = h[2],
               h4 = sign12(((unsigned)h[3] << 4) | (h[4] & 15)),
               h5 = sign12(((unsigned)h[5] << 4) | (h[4] >> 4)),
               h6 = h[6] < 128 ? (double)h[6] : (double)h[6] - 256;
        double x = fine - 76800.0;
        x = ((double)u16be(raw + 6) - (h4 * 64 + h5 / 16384.0 * x)) *
            (h2 / 65536.0 * (1 + h6 / 67108864.0 * x * (1 + h3 / 67108864.0 * x)));
        x = x * (1 - h1 * x / 524288.0);
        if (reading(d, "humidity", fmin(100, fmax(0, x)), "%RH", e) < 0)
            return -1;
    }
    return 0;
}

static int ina(device *d, bc_error *e) {
    double shunt, lsb;
    if (bc_lab_number(d->request, "shunt_ohms", 0.1, 0.000001, 100, &shunt, e) < 0 ||
        bc_lab_number(d->request, "current_lsb", 0.0001, 1e-9, 1, &lsb, e) < 0)
        return -1;
    if (!strcmp(bc_str(d->request, "action"), "measure")) {
        double c = floor(0.04096 / (lsb * shunt));
        if (c < 2 || c > 65534)
            return bc_fail(e, "Shunt/LSB não permitem calibração INA219 representável.");
        unsigned cal = (unsigned)c & 0xfffeu;
        if (write16(d, 0, 0x399b, e) < 0 || write16(d, 5, cal, e) < 0 || pause_device(d, 3, e) < 0)
            return -1;
    }
    uint8_t b[2];
    if (readreg(d, 0, b, 2, e) < 0)
        return -1;
    unsigned config = u16be(b);
    if (config & 0x8000)
        return bc_fail(e, "INA219 em reset.");
    unsigned bus;
    for (unsigned i = 0;; i++) {
        if (readreg(d, 2, b, 2, e) < 0)
            return -1;
        bus = u16be(b);
        if (bus & 1)
            return bc_fail(e, "INA219 sinaliza overflow de corrente/potência.");
        if (bus & 2)
            break;
        if (i >= 99)
            return bc_fail(e, "INA219 sem conversão pronta.");
        if (pause_device(d, 5, e) < 0)
            return -1;
    }
    if (reading(d, "bus_voltage", (bus >> 3) * 0.004, "V", e) < 0 || readreg(d, 1, b, 2, e) < 0)
        return -1;
    double sv = sign16(u16be(b)) * 0.00001;
    if (reading(d, "shunt_voltage", sv, "V", e) < 0 || readreg(d, 5, b, 2, e) < 0)
        return -1;
    unsigned calibration = u16be(b);
    const char *metric = bc_str(d->request, "metric");
    if (!calibration) {
        if (!strcmp(metric, "current") || !strcmp(metric, "power"))
            return bc_fail(e, "INA219 sem calibração: corrente/potência indisponíveis.");
        return 0;
    }
    lsb = 0.04096 / (calibration * shunt);
    if (readreg(d, 4, b, 2, e) < 0 || reading(d, "current", sign16(u16be(b)) * lsb, "A", e) < 0 ||
        readreg(d, 3, b, 2, e) < 0 || reading(d, "power", u16be(b) * 20.0 * lsb, "W", e) < 0)
        return -1;
    return 0;
}

static int mpu(device *d, bc_error *e) {
    uint8_t id;
    if (readreg(d, 0x75, &id, 1, e) < 0)
        return -1;
    if (id != 0x68)
        return bc_fail(e, "WHO_AM_I 0x%02X não corresponde ao MPU-6050.", id);
    if (!strcmp(bc_str(d->request, "action"), "measure")) {
        int accel, gyro;
        if (bc_lab_int(d->request, "range", 2, 2, 16, &accel, e) < 0 ||
            bc_lab_int(d->request, "gyro_range", 250, 250, 2000, &gyro, e) < 0)
            return -1;
        unsigned ar = accel == 2   ? 0u
                      : accel == 4 ? 1u
                      : accel == 8 ? 2u
                                   : 3u,
                 gr = gyro == 250    ? 0u
                      : gyro == 500  ? 1u
                      : gyro == 1000 ? 2u
                                     : 3u;
        if (write8(d, 0x6b, 1, e) < 0 || write8(d, 0x1b, (uint8_t)(gr << 3), e) < 0 ||
            write8(d, 0x1c, (uint8_t)(ar << 3), e) < 0 || pause_device(d, 40, e) < 0)
            return -1;
    }
    uint8_t power, range[2], b[14];
    if (readreg(d, 0x6b, &power, 1, e) < 0 || readreg(d, 0x1b, range, 2, e) < 0)
        return -1;
    if (power & 0x60)
        return bc_fail(e, "MPU-6050 em sleep/cycle; use medição com inicialização explícita.");
    if (readreg(d, 0x3b, b, 14, e) < 0)
        return -1;
    double acc[] = {16384, 8192, 4096, 2048}, gyro[] = {131, 65.5, 32.8, 16.4};
    const char *axes[] = {"accel_x", "accel_y", "accel_z", "gyro_x", "gyro_y", "gyro_z"};
    for (unsigned i = 0; i < 6; i++) {
        unsigned offset = i < 3 ? i * 2 : (i + 1) * 2;
        double n = sign16(u16be(b + offset));
        double v = i < 3 ? n / acc[(range[1] >> 3) & 3] * 9.80665 : n / gyro[(range[0] >> 3) & 3];
        if (reading(d, axes[i], v, i < 3 ? "m/s²" : "°/s", e) < 0)
            return -1;
    }
    return reading(d, "temperature", sign16(u16be(b + 6)) / 340.0 + 36.53, "°C", e);
}

static int oled_cmd(device *d, const uint8_t *cmd, size_t n, bc_error *e) {
    uint8_t b[32] = {0};
    if (n > 31)
        return -1;
    memcpy(b + 1, cmd, n);
    return exchange(d, b, n + 1, NULL, 0, e);
}

static int oled(device *d, bc_error *e) {
    if (!strcmp(bc_str(d->request, "action"), "off")) {
        uint8_t c = 0xae;
        return oled_cmd(d, &c, 1, e);
    }
    int height;
    if (bc_lab_int(d->request, "height", 64, 32, 64, &height, e) < 0)
        return -1;
    uint8_t init[] = {0xae,
                      0xd5,
                      0x80,
                      0xa8,
                      (uint8_t)(height - 1),
                      0xd3,
                      0,
                      0x40,
                      0x8d,
                      0x14,
                      0x20,
                      0x02,
                      0xa1,
                      0xc8,
                      0xda,
                      (uint8_t)(height == 64 ? 0x12 : 0x02),
                      0x81,
                      0x7f,
                      0xd9,
                      0xf1,
                      0xdb,
                      0x40,
                      0xa4,
                      0xa6};
    if (oled_cmd(d, init, sizeof init, e) < 0)
        return -1;
    const char *pattern =
        bc_get(d->request, "pattern") ? bc_str(d->request, "pattern") : "checkerboard";
    for (unsigned page = 0; page < (unsigned)height / 8; page++) {
        uint8_t cmd[] = {(uint8_t)(0xb0 + page), 0x00, 0x10};
        if (oled_cmd(d, cmd, 3, e) < 0)
            return -1;
        for (unsigned col = 0; col < 128; col += 16) {
            uint8_t b[17];
            b[0] = 0x40;
            for (unsigned i = 0; i < 16; i++)
                b[i + 1] = !strcmp(pattern, "black")   ? 0
                           : !strcmp(pattern, "white") ? 255
                           : !strcmp(pattern, "bars")  ? ((col + i) / 8 % 2 ? 0 : 255)
                                                       : ((col + i) % 2 ? 0xaa : 0x55);
            if (exchange(d, b, sizeof b, NULL, 0, e) < 0)
                return -1;
        }
    }
    uint8_t c = 0xaf;
    if (oled_cmd(d, &c, 1, e) < 0)
        return -1;
    json_object_object_add(d->result, "requires_visual_confirmation", json_object_new_boolean(1));
    return 0;
}

json_object *bc_peripheral_execute(json_object *op, bc_lab_io *io, bc_error *e) {
    if (bc_peripheral_validate("i2c", op, e) < 0 || !io || !io->transaction || !io->clock)
        return NULL;
    size_t index = 0;
    while (strcmp(devices[index].id, bc_str(op, "device")))
        index++;
    int addr;
    if (bc_lab_int(op, "address", devices[index].address, 8, 119, &addr, e) < 0)
        return NULL;
    json_object *r = json_object_new_object(), *trace = json_object_new_array();
    json_object_object_add(r, "trace", trace);
    json_object_object_add(r, "readings", json_object_new_object());
    str(r, "device", bc_str(op, "device"));
    str(r, "metric", bc_str(op, "metric"));
    device d = {op, r, trace, io, (unsigned)addr, io->clock(io->context) + 5.0};
    int rc = index < 2    ? bosch(&d, index == 0, e)
             : index == 2 ? ina(&d, e)
             : index == 3 ? mpu(&d, e)
                          : oled(&d, e);
    if (!rc && index == 4)
        rc = reading(&d, "acknowledged", 1, "ACK", e);
    json_object *reading_value = bc_get(bc_get(r, "readings"), bc_str(op, "metric"));
    if (rc < 0 || !reading_value) {
        if (!e->text[0])
            bc_fail(e, "Grandeza indisponível.");
        json_object_put(r);
        return NULL;
    }
    val(r, "value", bc_num(reading_value, "value"));
    str(r, "unit", bc_str(reading_value, "unit"));
    json_object_object_add(r, "matched", json_object_new_boolean(1));
    json_object_object_add(r, "operation", bc_clone(op));
    return r;
}
