#define _POSIX_C_SOURCE 200809L
#include "bancada.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif

int bc_parse_reading(const char *text, const char *base, const char *display, double *out,
                     bc_error *e) {
    if (!text || strlen(text) > 100)
        return bc_fail(e, "Leitura muito longa.");
    char input[104];
    strcpy(input, text);
    for (char *p = input; *p; p++)
        if (*p == ',')
            *p = '.';
    errno = 0;
    char *end;
    double value = strtod(input, &end);
    if (end == input || errno || !isfinite(value))
        return bc_fail(e, "Digite uma leitura finita, por exemplo 10 mV ou 0,010 V.");
    while (isspace((unsigned char)*end))
        end++;
    char *tail = end + strlen(end);
    while (tail > end && isspace((unsigned char)tail[-1]))
        *--tail = 0;
    const char *unit = *end ? end : display;
    double factor = 1;
    if (!strcmp(base, "V")) {
        if (!strcmp(unit, "mV"))
            factor = 0.001;
        else if (strcmp(unit, "V"))
            return bc_fail(e, "Use V ou mV; confira a unidade selecionada.");
    } else if (!strcmp(base, "ms") || !strcmp(base, "s") || !strcmp(base, "us")) {
        if (!strcmp(unit, "s"))
            factor = 1000;
        else if (!strcmp(unit, "us") || !strcmp(unit, "µs") || !strcmp(unit, "μs"))
            factor = 0.001;
        else if (strcmp(unit, "ms"))
            return bc_fail(e, "Use ms, us ou s.");
        if (!strcmp(base, "s"))
            factor /= 1000;
        else if (!strcmp(base, "us"))
            factor *= 1000;
    } else if (strcmp(unit, base))
        return bc_fail(e, "Unidade incompatível com %s.", base);
    *out = value * factor;
    return isfinite(*out) ? 0 : bc_fail(e, "Leitura fora da faixa numérica.");
}

void bc_local_time(const char *iso, char *out, size_t capacity) {
    GDateTime *utc = g_date_time_new_from_iso8601(iso, NULL),
              *local = utc ? g_date_time_to_local(utc) : NULL;
    char *text = local ? g_date_time_format(local, "%d/%m/%Y %H:%M:%S %z") : NULL;
    snprintf(out, capacity, "%s", text ? text : iso);
    g_free(text);
    if (local)
        g_date_time_unref(local);
    if (utc)
        g_date_time_unref(utc);
}
