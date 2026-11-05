#define _POSIX_C_SOURCE 200809L
#include "bancada.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
#include <time.h>
#include <unistd.h>

static int number(json_object *v) {
    return v &&
           (json_object_is_type(v, json_type_double) || json_object_is_type(v, json_type_int)) &&
           isfinite(json_object_get_double(v));
}

static int name_ok(const char *s) {
    if (!s || !*s || strlen(s) > 64)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
            return 0;
    return 1;
}

static int paths(const char *root, const char *board, const char *unit, char dir[4096],
                 char file[4096], bc_error *e) {
    if (!name_ok(board) || (unit && !name_ok(unit)))
        return bc_fail(e, "Identificação: use até 64 letras, números, hífen ou sublinhado.");
    char base[4096], leaf[80];
    if (bc_path(base, sizeof base, root, "calibration", e) || bc_path(dir, 4096, base, board, e))
        return -1;
    if (unit) {
        snprintf(leaf, sizeof leaf, "%s.json", unit);
        return bc_path(file, 4096, dir, leaf, e);
    }
    return 0;
}

int bc_cal_validate(json_object *cal, bc_error *e) {
    if (!cal)
        return 0;
    if (!json_object_is_type(cal, json_type_object) || bc_num(cal, "schema") != 1 ||
        strcmp(bc_str(cal, "kind"), "adc-linear-v1") || !name_ok(bc_str(cal, "board_id")) ||
        !name_ok(bc_str(cal, "physical_id")) || !number(bc_get(cal, "vref_mv")) ||
        bc_num(cal, "vref_mv") < 2200 || bc_num(cal, "vref_mv") > 5500 ||
        floor(bc_num(cal, "vref_mv")) != bc_num(cal, "vref_mv"))
        return bc_fail(e, "Perfil de calibração inválido.");
    json_object *channels = bc_get(cal, "channels"), *bindings = bc_get(cal, "components");
    if (!json_object_is_type(channels, json_type_array) ||
        json_object_array_length(channels) != 4 ||
        !json_object_is_type(bindings, json_type_object) ||
        json_object_object_length(bindings) > 64)
        return bc_fail(e, "Canais ou contexto da calibração inválidos.");
    for (size_t i = 0; i < 4; i++) {
        json_object *ch = json_object_array_get_idx(channels, i);
        if (!number(bc_get(ch, "gain")) || !number(bc_get(ch, "offset_v")) ||
            bc_num(ch, "gain") < 0.5 || bc_num(ch, "gain") > 2 ||
            fabs(bc_num(ch, "offset_v")) > 0.5)
            return bc_fail(
                e, "Correção do ADC%zu fora da faixa: ganho 0,5 a 2; desvio até 0,5 V no pino.", i);
    }
    json_object_object_foreach(bindings, key, value) {
        (void)key;
        if (!number(value) || json_object_get_double(value) <= 0)
            return bc_fail(e, "Contexto de componentes inválido.");
    }
    return 0;
}

int bc_cal_check(json_object *cal, json_object *components, double vref, bc_error *e) {
    if (!cal)
        return 0;
    if (bc_cal_validate(cal, e) < 0)
        return -1;
    if (vref != bc_num(cal, "vref_mv"))
        return bc_fail(e,
                       "Calibração usa AVDD %.0f mV; o teste informa %.0f mV. Corrija a referência "
                       "ou use sem calibração.",
                       bc_num(cal, "vref_mv"), vref);
    json_object *bindings = bc_get(cal, "components");
    json_object_object_foreach(bindings, key, value) {
        double actual = 0, reference = json_object_get_double(value);
        if (bc_component(bc_str(components, key), &actual) ||
            fabs(actual - reference) > fmax(1e-9, fabs(reference) * 1e-12))
            return bc_fail(e,
                           "Calibração não corresponde ao valor atual de %s. Recalibre ou "
                           "selecione Sem calibração.",
                           key);
    }
    return 0;
}

double bc_cal_voltage(json_object *cal, unsigned channel, double nominal) {
    if (!cal || channel >= 4)
        return nominal;
    json_object *ch = json_object_array_get_idx(bc_get(cal, "channels"), channel);
    return nominal * bc_num(ch, "gain") + bc_num(ch, "offset_v");
}

json_object *bc_cal_fit(json_object *profile, json_object *input, bc_error *e) {
    json_object *meta = bc_get(profile, "calibration"), *spec = bc_get(meta, "channels"),
                *points = bc_get(input, "points");
    const char *unit = bc_str(input, "physical_id");
    double vref = bc_num(input, "vref_mv");
    if (strcmp(bc_str(meta, "kind"), "adc-linear-v1") || json_object_array_length(spec) != 4 ||
        !name_ok(unit) || !number(bc_get(input, "vref_mv")) || vref < 2200 || vref > 5500 ||
        floor(vref) != vref || !json_object_is_type(points, json_type_array) ||
        json_object_array_length(points) != 4) {
        bc_fail(e,
                "Informe a placa física, AVDD e dois pontos completos para cada canal a calibrar.");
        return NULL;
    }
    json_object *vars = json_object_new_object(), *cal = json_object_new_object(),
                *channels = json_object_new_array(), *bindings = json_object_new_object();
    json_object *components = bc_get(profile, "components");
    json_object_object_foreach(components, key, value) {
        double v = 0;
        if (!bc_component(json_object_get_string(value), &v))
            json_object_object_add(vars, key, json_object_new_double(v));
    }
    json_object_object_add(cal, "schema", json_object_new_int(1));
    json_object_object_add(cal, "kind", json_object_new_string("adc-linear-v1"));
    json_object_object_add(cal, "board_id", json_object_new_string(bc_str(profile, "id")));
    json_object_object_add(cal, "revision", json_object_new_string(bc_str(profile, "revision")));
    json_object_object_add(cal, "physical_id", json_object_new_string(unit));
    json_object_object_add(cal, "vref_mv", json_object_new_double(vref));
    json_object_object_add(cal, "channels", channels);
    json_object_object_add(cal, "components", bindings);
    unsigned fitted = 0;
    for (size_t i = 0; i < 4; i++) {
        json_object *row = json_object_array_get_idx(points, i),
                    *definition = json_object_array_get_idx(spec, i);
        double gain = 1, offset = 0, factor = 1;
        if (bc_eval(bc_str(definition, "factor"), vars, &factor, e) || factor <= 0)
            goto failed;
        if (row) {
            if (!json_object_is_type(row, json_type_array) || json_object_array_length(row) != 4)
                goto invalid;
            double v[4];
            for (size_t k = 0; k < 4; k++) {
                json_object *n = json_object_array_get_idx(row, k);
                if (!number(n))
                    goto invalid;
                v[k] = json_object_get_double(n);
                if (v[k] < 0 || v[k] > factor * vref / 1000.0)
                    goto invalid;
            }
            if (v[2] - v[0] < factor * vref / 1000.0 * 0.02 || v[3] <= v[1]) {
                bc_fail(
                    e, "ADC%zu: use dois pontos crescentes, separados por pelo menos 2%% da faixa.",
                    i);
                goto failed;
            }
            gain = (v[3] - v[1]) / (v[2] - v[0]);
            offset = (v[1] - gain * v[0]) / factor;
            fitted++;
        }
        json_object *ch = json_object_new_object();
        json_object_object_add(ch, "gain", json_object_new_double(gain));
        json_object_object_add(ch, "offset_v", json_object_new_double(offset));
        json_object_object_add(ch, "calibrated", json_object_new_boolean(row != NULL));
        if (row)
            json_object_object_add(ch, "points", bc_clone(row));
        json_object_array_add(channels, ch);
        if (row) {
            json_object *dependencies = bc_get(definition, "components");
            for (size_t k = 0; k < json_object_array_length(dependencies); k++) {
                const char *name =
                    json_object_get_string(json_object_array_get_idx(dependencies, k));
                json_object *v = bc_get(vars, name);
                if (!v) {
                    bc_fail(e, "Componente ausente na calibração: %s.", name);
                    goto failed;
                }
                json_object_object_add(bindings, name, json_object_get(v));
            }
        }
    }
    if (!fitted) {
        bc_fail(e, "Preencha os dois pontos de pelo menos um canal.");
        goto failed;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char stamp[40];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm);
    json_object_object_add(cal, "updated", json_object_new_string(stamp));
    if (bc_cal_validate(cal, e) < 0)
        goto failed;
    json_object_put(vars);
    return cal;
invalid:
    bc_fail(e, "Pontos inválidos: use as unidades indicadas e valores dentro da faixa de medição.");
failed:
    json_object_put(vars);
    json_object_put(cal);
    return NULL;
}

json_object *bc_cal_load(const char *root, const char *board, const char *unit, bc_error *e) {
    char dir[4096], file[4096];
    if (paths(root, board, unit, dir, file, e))
        return NULL;
    json_object *cal = bc_read_json(file, e);
    if (!cal)
        return NULL;
    if (bc_cal_validate(cal, e) || strcmp(bc_str(cal, "board_id"), board) ||
        strcmp(bc_str(cal, "physical_id"), unit)) {
        if (!e->text[0])
            bc_fail(e, "A calibração pertence a outra placa.");
        json_object_put(cal);
        return NULL;
    }
    return cal;
}

json_object *bc_cal_list(const char *root, const char *board, bc_error *e) {
    char dir[4096], file[4096];
    if (paths(root, board, NULL, dir, file, e))
        return NULL;
    json_object *list = json_object_new_array();
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT)
            return list;
        bc_fail(e, "Abrir calibrações: %s", strerror(errno));
        json_object_put(list);
        return NULL;
    }
    struct dirent *item;
    while ((item = readdir(d))) {
        size_t n = strlen(item->d_name);
        if (n < 6 || n > 69 || strcmp(item->d_name + n - 5, ".json"))
            continue;
        char id[65];
        memcpy(id, item->d_name, n - 5);
        id[n - 5] = 0;
        bc_error ignored = {{0}};
        json_object *cal = bc_cal_load(root, board, id, &ignored);
        if (cal)
            json_object_array_add(list, cal);
        if (json_object_array_length(list) >= 1000)
            break;
    }
    closedir(d);
    return list;
}

int bc_cal_save(const char *root, json_object *cal, bc_error *e) {
    char dir[4096], file[4096], lockfile[4096];
    if (bc_cal_validate(cal, e) || !cal)
        return -1;
    if (paths(root, bc_str(cal, "board_id"), bc_str(cal, "physical_id"), dir, file, e) ||
        bc_mkdirs(dir, e) || bc_path(lockfile, sizeof lockfile, dir, "calibration.lock", e))
        return -1;
    int fd = open(lockfile, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX) < 0) {
        if (fd >= 0)
            close(fd);
        return bc_fail(e, "Não foi possível reservar a calibração.");
    }
    int rc = bc_write_json(file, cal, e);
    close(fd);
    return rc;
}
