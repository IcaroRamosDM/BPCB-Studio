#define _POSIX_C_SOURCE 200809L
#include "plot.h"
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif

static int finite_number(json_object *v) {
    return (json_object_is_type(v, json_type_double) || json_object_is_type(v, json_type_int)) &&
           isfinite(json_object_get_double(v)) && fabs(json_object_get_double(v)) <= 1e15;
}

int bc_plot_validate(json_object *plot, bc_error *e) {
    json_object *series = bc_get(plot, "series");
    if (!json_object_is_type(series, json_type_array))
        return bc_fail(e, "Gráfico sem sinais válidos.");
    size_t count = json_object_array_length(series);
    if (!count || count > 32)
        return bc_fail(e, "Gráfico sem sinais válidos.");
    for (size_t i = 0; i < count; i++) {
        json_object *s = json_object_array_get_idx(series, i), *points = bc_get(s, "points");
        if (!json_object_is_type(points, json_type_array))
            return bc_fail(e, "Sinal inválido.");
        size_t n = json_object_array_length(points);
        if (!*bc_str(s, "name") || strlen(bc_str(s, "name")) > 160 ||
            strlen(bc_str(s, "unit")) > 32 || !n || n > 3000)
            return bc_fail(e, "Sinal inválido ou acima de 3.000 pontos.");
        double previous = -1;
        for (size_t k = 0; k < n; k++) {
            json_object *p = json_object_array_get_idx(points, k);
            if (!json_object_is_type(p, json_type_array) || json_object_array_length(p) != 2)
                return bc_fail(e, "Amostra inválida no gráfico.");
            json_object *t = json_object_array_get_idx(p, 0), *v = json_object_array_get_idx(p, 1);
            if (!finite_number(t) || !finite_number(v) || json_object_get_double(t) < 0 ||
                json_object_get_double(t) < previous)
                return bc_fail(e, "Amostra inválida no gráfico.");
            previous = json_object_get_double(t);
        }
        for (unsigned k = 0; k < 2; k++) {
            json_object *value = bc_get(s, k ? "limit" : "target");
            if (value && !finite_number(value))
                return bc_fail(e, "Referência inválida no gráfico.");
        }
    }
    json_object *view = bc_get(plot, "view");
    const char *keys[] = {"from", "to", "cursor_a", "cursor_b"};
    for (unsigned k = 0; k < 4; k++) {
        json_object *v = bc_get(view, keys[k]);
        if (v && !finite_number(v))
            return bc_fail(e, "Janela do gráfico inválida.");
    }
    if (bc_get(view, "from") && bc_get(view, "to") && bc_num(view, "from") >= bc_num(view, "to"))
        return bc_fail(e, "Intervalo do gráfico inválido.");
    return 0;
}

static double at(json_object *points, size_t i, unsigned axis) {
    return json_object_get_double(
        json_object_array_get_idx(json_object_array_get_idx(points, i), axis));
}

static void text(cairo_t *cr, double x, double y, double size, const char *s) {
    cairo_set_font_size(cr, size);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, s);
}

static double nearest(json_object *points, double time, double *value) {
    size_t best = 0;
    for (size_t i = 1; i < json_object_array_length(points); i++)
        if (fabs(at(points, i, 0) - time) < fabs(at(points, best, 0) - time))
            best = i;
    *value = at(points, best, 1);
    return at(points, best, 0);
}

void bc_plot_draw(cairo_t *cr, json_object *s, json_object *view, double x, double y, double w,
                  double h) {
    json_object *points = bc_get(s, "points");
    size_t n = json_object_array_length(points);
    if (!n)
        return;
    double from = at(points, 0, 0), to = at(points, n - 1, 0);
    if (to <= from)
        to = from + 1;
    if (bc_get(view, "from") && bc_get(view, "to")) {
        from = bc_num(view, "from");
        to = bc_num(view, "to");
    }
    double low = at(points, 0, 1), high = low;
    for (size_t i = 0; i < n; i++) {
        double v = at(points, i, 1);
        low = fmin(low, v);
        high = fmax(high, v);
    }
    for (unsigned i = 0; i < 2; i++) {
        json_object *v = bc_get(s, i ? "limit" : "target");
        if (v) {
            low = fmin(low, json_object_get_double(v));
            high = fmax(high, json_object_get_double(v));
        }
    }
    double margin = fmax((high - low) * .08, fmax(fabs(high) * .01, .001));
    low -= margin;
    high += margin;
    double left = x + 65, right = x + w - 22, top = y + 38, bottom = y + h - 78;
#define PX(t) (left + ((t) - from) / (to - from) * (right - left))
#define PY(v) (bottom - ((v) - low) / (high - low) * (bottom - top))
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    char label[256];
    snprintf(label, sizeof label, "%.160s (%s)", bc_str(s, "name"), bc_str(s, "unit"));
    cairo_set_source_rgb(cr, .08, .32, .27);
    text(cr, left, y + 21, 12, label);
    for (unsigned i = 0; i <= 4; i++) {
        double value = low + (high - low) * i / 4, time = from + (to - from) * i / 4;
        cairo_set_source_rgb(cr, .84, .87, .9);
        cairo_set_line_width(cr, .6);
        cairo_move_to(cr, left, PY(value));
        cairo_line_to(cr, right, PY(value));
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, .35, .39, .43);
        snprintf(label, sizeof label, "%.5g", value);
        text(cr, x + 3, PY(value) + 3, 9, label);
        snprintf(label, sizeof label, "%.3g s", time);
        text(cr, PX(time) - 18, bottom + 18, 9, label);
    }
    cairo_save(cr);
    cairo_rectangle(cr, left, top, right - left, bottom - top);
    cairo_clip(cr);
    for (unsigned i = 0; i < 2; i++)
        if (bc_get(s, i ? "limit" : "target")) {
            double value = bc_num(s, i ? "limit" : "target"), dashes[] = {3, 3};
            cairo_set_source_rgb(cr, i ? .72 : .45, i ? .15 : .45, i ? .2 : .45);
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_move_to(cr, left, PY(value));
            cairo_line_to(cr, right, PY(value));
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
            text(cr, right - 100, PY(value) - 5, 10, i ? "Limite máximo" : "Alvo");
        }
    cairo_set_source_rgb(cr, .08, .44, .34);
    cairo_set_line_width(cr, 1.5);
    for (size_t i = 0; i < n; i++) {
        double xx = PX(at(points, i, 0)), yy = PY(at(points, i, 1));
        if (i)
            cairo_line_to(cr, xx, yy);
        else
            cairo_move_to(cr, xx, yy);
    }
    cairo_stroke(cr);
    if (n == 1) {
        cairo_arc(cr, PX(at(points, 0, 0)), PY(at(points, 0, 1)), 2, 0, 6.283185);
        cairo_fill(cr);
    }
    cairo_restore(cr);
    cairo_set_source_rgb(cr, .3, .35, .4);
    double times[2] = {0, 0}, values[2] = {0, 0};
    int have[2] = {0, 0};
    for (unsigned i = 0; i < 2; i++)
        if (bc_get(view, i ? "cursor_b" : "cursor_a")) {
            times[i] = nearest(points, bc_num(view, i ? "cursor_b" : "cursor_a"), &values[i]);
            have[i] = 1;
            if (times[i] >= from && times[i] <= to) {
                cairo_move_to(cr, PX(times[i]), top);
                cairo_line_to(cr, PX(times[i]), bottom);
                cairo_stroke(cr);
            }
            snprintf(label, sizeof label, "%c: %.6g s / %.6g %s", i ? 'B' : 'A', times[i],
                     values[i], bc_str(s, "unit"));
            text(cr, left + (i ? 230 : 0), bottom + 37, 9, label);
        }
    if (have[0] && have[1]) {
        snprintf(label, sizeof label, "B - A: %.6g s / %.6g %s", times[1] - times[0],
                 values[1] - values[0], bc_str(s, "unit"));
        text(cr, left, bottom + 53, 9, label);
    }
    if (bc_num(s, "dropped") > 0) {
        snprintf(label, sizeof label, "Amostras antigas removidas da memória: %.0f",
                 bc_num(s, "dropped"));
        text(cr, left, y + h - 7, 8, label);
    }
    cairo_restore(cr);
#undef PX
#undef PY
}

static void csv_string(FILE *f, const char *value) {
    fputc('"', f);
    const char *first = value + strspn(value, " \t\r\n");
    if (*first && strchr("=+-@", *first))
        fputc('\'', f);
    for (; *value; value++) {
        if (*value == '"')
            fputc('"', f);
        fputc(*value, f);
    }
    fputc('"', f);
}

int bc_plot_csv(json_object *plot, const char *destination, bc_error *e) {
    if (bc_plot_validate(plot, e))
        return -1;
    int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return bc_fail(e, "Escolha um CSV novo: %s.", strerror(errno));
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(destination);
        return bc_fail(e, "Abrir CSV.");
    }
    fputs("sinal,unidade,tempo_s,valor,alvo,limite,amostras_antigas_removidas\r\n", f);
    json_object *all = bc_get(plot, "series");
    for (size_t i = 0; i < json_object_array_length(all); i++) {
        json_object *s = json_object_array_get_idx(all, i), *points = bc_get(s, "points");
        for (size_t k = 0; k < json_object_array_length(points); k++) {
            csv_string(f, bc_str(s, "name"));
            fputc(',', f);
            csv_string(f, bc_str(s, "unit"));
            fprintf(f, ",%.12g,%.12g,", at(points, k, 0), at(points, k, 1));
            if (bc_get(s, "target"))
                fprintf(f, "%.12g", bc_num(s, "target"));
            fputc(',', f);
            if (bc_get(s, "limit"))
                fprintf(f, "%.12g", bc_num(s, "limit"));
            fprintf(f, ",%.0f\r\n", bc_num(s, "dropped"));
        }
    }
    int ok = !ferror(f);
    if (fflush(f) || fsync(fd))
        ok = 0;
    if (fclose(f))
        ok = 0;
    if (!ok) {
        unlink(destination);
        return bc_fail(e, "Falha ao concluir o CSV.");
    }
    return 0;
}

static cairo_status_t png_write(void *context, const unsigned char *data, unsigned length) {
    int fd = *(int *)context;
    size_t used = 0;
    while (used < length) {
        ssize_t n = write(fd, data + used, length - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return CAIRO_STATUS_WRITE_ERROR;
        used += (size_t)n;
    }
    return CAIRO_STATUS_SUCCESS;
}

int bc_plot_png(json_object *plot, const char *destination, bc_error *e) {
    if (bc_plot_validate(plot, e))
        return -1;
    json_object *series = bc_get(plot, "series"), *selected = NULL;
    for (size_t i = 0; i < json_object_array_length(series); i++) {
        json_object *s = json_object_array_get_idx(series, i);
        if (!strcmp(bc_str(s, "name"), bc_str(plot, "selected")))
            selected = s;
    }
    if (!selected)
        selected = json_object_array_get_idx(series, 0);
    int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return bc_fail(e, "Escolha um PNG novo: %s.", strerror(errno));
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1400, 850);
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, 2, 2);
    bc_plot_draw(cr, selected, bc_get(plot, "view"), 0, 0, 700, 425);
    cairo_status_t status = cairo_status(cr);
    cairo_destroy(cr);
    if (status == CAIRO_STATUS_SUCCESS)
        status = cairo_surface_write_to_png_stream(surface, png_write, &fd);
    cairo_surface_destroy(surface);
    int synced = fsync(fd), closed = close(fd);
    if (status != CAIRO_STATUS_SUCCESS || synced || closed) {
        unlink(destination);
        return bc_fail(e, "Falha ao concluir o PNG.");
    }
    return 0;
}
