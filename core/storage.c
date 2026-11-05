#define _XOPEN_SOURCE 700
#include "studio.h"
#include <cairo-pdf.h>
#include <poppler.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

static int parent(const char *path, char out[4096], bc_error *e) {
    if (!path || strlen(path) > 4000)
        return bc_fail(e, "Caminho inválido.");
    gchar *absolute = g_canonicalize_filename(path, NULL), *dir = g_path_get_dirname(absolute);
    g_free(absolute);
    int length = snprintf(out, 4096, "%s", dir);
    g_free(dir);
    if (length < 0 || length >= 4096)
        return bc_fail(e, "Caminho longo demais.");
    return bc_mkdirs(out, e);
}

int bs_save(const char *path, json_object *p, bc_error *e) {
    char dir[4096];
    if (bs_shape(p, e) < 0 || parent(path, dir, e) < 0)
        return -1;
    json_object *current = bc_clone(p);
    json_object_object_add(current, "schema", json_object_new_int(bs_project_schema(p)));
    int rc = bc_write_json(path, current, e);
    json_object_put(current);
    return rc;
}

json_object *bs_load(const char *path, bc_error *e) {
    json_object *p = bc_read_json(path, e);
    if (p && bs_shape(p, e) < 0) {
        json_object_put(p);
        return NULL;
    }
    return p;
}

int bs_copy(const char *source, const char *target, bc_error *e) {
    char dir[4096], temp[4096];
    if (parent(target, dir, e) < 0)
        return -1;
    if (snprintf(temp, sizeof temp, "%s.tmp.XXXXXX", target) >= (int)sizeof temp)
        return bc_fail(e, "Caminho longo.");
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (in < 0)
        return bc_fail(e, "Abrir documento: %s", strerror(errno));
    if (fstat(in, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > 64 * 1024 * 1024) {
        close(in);
        return bc_fail(e, "Arquivo deve ser regular e ter até 64 MB.");
    }
    int out = mkstemp(temp);
    if (out < 0) {
        close(in);
        return bc_fail(e, "Criar cópia: %s", strerror(errno));
    }
    char buf[32768];
    int ok = 1;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = 0;
            break;
        }
        if (!n)
            break;
        ssize_t used = 0;
        while (used < n) {
            ssize_t k = write(out, buf + used, (size_t)(n - used));
            if (k < 0 && errno == EINTR)
                continue;
            if (k <= 0) {
                ok = 0;
                break;
            }
            used += k;
        }
        if (!ok)
            break;
    }
    close(in);
    if (fsync(out) < 0)
        ok = 0;
    if (close(out) < 0)
        ok = 0;
    if (ok && rename(temp, target) < 0)
        ok = 0;
    if (!ok) {
        unlink(temp);
        return bc_fail(e, "Falha ao copiar o arquivo.");
    }
    return 0;
}

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *en;
    while ((en = readdir(d))) {
        if (!strcmp(en->d_name, ".") || !strcmp(en->d_name, ".."))
            continue;
        char p[4096];
        struct stat st;
        if (bc_path(p, sizeof p, path, en->d_name, NULL) < 0 || lstat(p, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode))
            remove_tree(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(path);
}

void bs_remove_temp(const char *path) {
    if (!path || !*path)
        return;
    gchar *absolute = g_canonicalize_filename(path, NULL), *dir = g_path_get_dirname(absolute);
    gchar *temp = g_canonicalize_filename(g_get_tmp_dir(), NULL),
          *base = g_path_get_basename(absolute);
    int valid = !strcmp(dir, temp) && g_str_has_prefix(base, "bpcb-studio-") && strlen(base) > 18;
    g_free(dir);
    g_free(temp);
    g_free(base);
    struct stat st;
    if (valid && lstat(absolute, &st) == 0 && S_ISDIR(st.st_mode)
#ifndef _WIN32
        && st.st_uid == getuid()
#endif
    )
        remove_tree(absolute);
    g_free(absolute);
}

int bs_asset_pdf(const char *source, const char *destination, bc_error *e) {
    const char *ext = strrchr(source, '.');
    if (ext && !strcasecmp(ext, ".pdf"))
        return bs_copy(source, destination, e);
    if (!ext || strcasecmp(ext, ".svg"))
        return bc_fail(e, "Escolha um PDF ou SVG.");
    gchar *data = NULL;
    gsize length = 0;
    GError *err = NULL;
    if (!g_file_get_contents(source, &data, &length, &err)) {
        bc_fail(e, "Ler SVG: %s", err ? err->message : "erro");
        g_clear_error(&err);
        return -1;
    }
    if (length > 64 * 1024 * 1024) {
        g_free(data);
        return bc_fail(e, "SVG maior que 64 MB.");
    }
    RsvgHandle *handle = rsvg_handle_new_from_data((const guint8 *)data, length, &err);
    g_free(data);
    if (!handle) {
        bc_fail(e, "SVG inválido: %s", err ? err->message : "erro");
        g_clear_error(&err);
        return -1;
    }
    double w = 0, h = 0;
    if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &w, &h)) {
        gboolean hasw, hash, hasview;
        RsvgLength rw, rh;
        RsvgRectangle view;
        rsvg_handle_get_intrinsic_dimensions(handle, &hasw, &rw, &hash, &rh, &hasview, &view);
        if (hasview) {
            w = view.width;
            h = view.height;
        } else {
            w = 1000;
            h = 700;
        }
    }
    if (w <= 0 || h <= 0 || w > 50000 || h > 50000) {
        g_object_unref(handle);
        return bc_fail(e, "Dimensões SVG inválidas.");
    }
    char folder[4096];
    if (parent(destination, folder, e) < 0) {
        g_object_unref(handle);
        return -1;
    }
    cairo_surface_t *surface = cairo_pdf_surface_create(destination, w, h);
    cairo_t *cr = cairo_create(surface);
    RsvgRectangle rect = {0, 0, w, h};
    int ok = rsvg_handle_render_document(handle, cr, &rect, &err);
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_finish(surface);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
        ok = 0;
    cairo_surface_destroy(surface);
    g_object_unref(handle);
    if (!ok) {
        bc_fail(e, "Converter SVG: %s", err ? err->message : "erro ao escrever PDF");
        g_clear_error(&err);
        unlink(destination);
        return -1;
    }
    return 0;
}

static void notes_footer(cairo_t *cr, unsigned page) {
    char label[120];
    snprintf(label, sizeof label, "BPCB Studio " BS_VERSION " | Página %u", page);
    cairo_save(cr);
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 8);
    cairo_set_source_rgb(cr, .35, .4, .45);
    cairo_move_to(cr, 48, 820);
    cairo_show_text(cr, label);
    cairo_restore(cr);
}

int bs_notes_pdf(const char *text, const char *destination, bc_error *e) {
    char dir[4096];
    if (parent(destination, dir, e) < 0)
        return -1;
    cairo_surface_t *sf = cairo_pdf_surface_create(destination, 595, 842);
    cairo_t *cr = cairo_create(sf);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("Sans 11");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_width(layout, 499 * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_text(layout, text, -1);
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    double y = 48;
    unsigned page = 1;
    do {
        PangoRectangle logical;
        pango_layout_iter_get_line_extents(iter, NULL, &logical);
        double h = (double)logical.height / PANGO_SCALE;
        if (y + h > 794) {
            notes_footer(cr, page++);
            cairo_show_page(cr);
            y = 48;
        }
        PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
        cairo_move_to(cr, 48,
                      y - (double)logical.y / PANGO_SCALE +
                          (double)pango_layout_iter_get_baseline(iter) / PANGO_SCALE);
        pango_cairo_show_layout_line(cr, line);
        y += h;
    } while (pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);
    g_object_unref(layout);
    notes_footer(cr, page);
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_finish(sf);
    int rc = cairo_surface_status(sf) == CAIRO_STATUS_SUCCESS
                 ? 0
                 : bc_fail(e, "Erro ao produzir notas em PDF.");
    cairo_surface_destroy(sf);
    return rc;
}

int bs_document(json_object *p, const char *project_path, const char *source, const char *title,
                const char *role, bc_error *e) {
    if (!project_path || !*project_path)
        return bc_fail(e, "Salve o projeto antes de adicionar documentos.");
    const char *ext = strrchr(source, '.');
    if (!ext || (strcasecmp(ext, ".pdf") && strcasecmp(ext, ".svg")))
        return bc_fail(e, "Documentos aceitos: PDF e SVG.");
    char hash[65], root[4096], rel[120], target[4096];
    if (bc_sha_file(source, hash, e) < 0 || parent(project_path, root, e) < 0)
        return -1;
    snprintf(rel, sizeof rel, "assets/%.32s%s", hash, !strcasecmp(ext, ".pdf") ? ".pdf" : ".svg");
    if (bc_path(target, sizeof target, root, rel, e) < 0 || bs_copy(source, target, e) < 0)
        return -1;
    json_object *d = json_object_new_object();
    json_object_object_add(d, "title", json_object_new_string(title));
    json_object_object_add(d, "role", json_object_new_string(role));
    json_object_object_add(d, "file", json_object_new_string(rel));
    json_object_array_add(bc_get(p, "documents"), d);
    return 0;
}

static int runtime_files(const char *worker, const char *stage, bc_error *e) {
    char source[4096], dest[4096], folder[4096];
    if (parent(worker, folder, e) < 0 ||
        bc_path(dest, sizeof dest, stage, "code/" BS_WORKER_NAME, e) < 0 ||
        bs_copy(worker, dest, e) < 0)
        return -1;
#ifdef _WIN32
    if (bc_path(source, sizeof source, folder, "bpcb_sequence.dependencies.json", e) < 0)
        return -1;
    json_object *deps = bc_read_json(source, e);
    if (!deps)
        return -1;
    json_object *files = bc_get(deps, "files");
    char hash[65];
    int rc = -1;
    if (!json_object_is_type(files, json_type_object) || json_object_object_length(files) > 160 ||
        bc_sha_file(worker, hash, e) < 0 || strcmp(hash, bc_str(deps, "executor_sha256"))) {
        bc_fail(e, "Executor Windows e manifesto de bibliotecas não correspondem. Recompile e "
                   "empacote o Studio.");
        goto done;
    }
    json_object_object_foreach(files, name, digest) {
        const char *ext = strrchr(name, '.');
        if (!bc_safe_path(name) || strchr(name, '/') || !ext || strcasecmp(ext, ".dll") ||
            !json_object_is_type(digest, json_type_string)) {
            bc_fail(e, "Nome de biblioteca Windows inválido.");
            goto done;
        }
        if (bc_path(source, sizeof source, folder, name, e) < 0 || bc_sha_file(source, hash, e) < 0)
            goto done;
        if (strcmp(hash, json_object_get_string(digest))) {
            bc_fail(e, "Biblioteca Windows alterada: %s", name);
            goto done;
        }
        char rel[900];
        snprintf(rel, sizeof rel, "code/%s", name);
        if (bc_path(dest, sizeof dest, stage, rel, e) < 0 || bs_copy(source, dest, e) < 0)
            goto done;
    }
    rc = 0;
done:
    json_object_put(deps);
    return rc;
#else
    (void)source;
    return 0;
#endif
}

int bs_export(json_object *project, const char *project_path, const char *worker,
              const char *output, int simulate, bc_error *e) {
#ifdef _WIN32
    if (!simulate) {
        json_object *devices = bc_get(project, "devices"), *tests = bc_get(project, "tests");
        int physical = 0;
        for (size_t i = 0; i < json_object_array_length(tests); i++)
            if (strcmp(bc_str(json_object_array_get_idx(tests, i), "mode"), "simulation"))
                physical = 1;
        json_object *signals = bc_get(project, "signals");
        for (size_t i = 0; i < json_object_array_length(signals); i++)
            if (bc_num(json_object_array_get_idx(signals, i), "diagnostic"))
                physical = 1;
        for (size_t i = 0; physical && i < json_object_array_length(devices); i++)
            if (!strcmp(bc_str(json_object_array_get_idx(devices, i), "transport"), "linux_i2c"))
                return bc_fail(e,
                               "Este projeto usa /dev/i2c-N. Para executar no Windows, escolha USB "
                               "CH341 nos dispositivos ou mantenha os testes apenas em simulação.");
    }
#endif
    json_object *m = bs_manifest(project, simulate, e);
    if (!m)
        return -1;
    char temp[4096];
    if (bs_temp_dir(temp, "export", e) < 0) {
        json_object_put(m);
        return -1;
    }
    int rc = -1;
    char dest[4096], source[4096], root[4096];
    if (parent(project_path, root, e) < 0 || runtime_files(worker, temp, e) < 0)
        goto done;
    json_object *docs = bc_get(project, "documents"), *compiled = bc_get(m, "documents");
    for (size_t i = 0; i < json_object_array_length(docs); i++) {
        json_object *d = json_object_array_get_idx(docs, i);
        if (bc_path(source, sizeof source, root, bc_str(d, "file"), e) < 0 ||
            bc_path(dest, sizeof dest, temp, bc_str(d, "file"), e) < 0 ||
            bs_copy(source, dest, e) < 0)
            goto done;
        char rel[100];
        snprintf(rel, sizeof rel, "docs/document_%u.pdf", (unsigned)(i + 1));
        if (bc_path(dest, sizeof dest, temp, rel, e) < 0 || bs_asset_pdf(source, dest, e) < 0)
            goto done;
        json_object *doc = json_object_new_object();
        json_object_object_add(doc, "title", json_object_new_string(bc_str(d, "title")));
        json_object_object_add(doc, "file", json_object_new_string(rel));
        json_object_array_add(compiled, doc);
    }
    const char *notes = bc_str(bc_get(project, "board"), "notes");
    if (*notes) {
        if (bc_path(dest, sizeof dest, temp, "docs/notas.pdf", e) < 0 ||
            bs_notes_pdf(notes, dest, e) < 0)
            goto done;
        json_object *d = json_object_new_object();
        json_object_object_add(d, "title", json_object_new_string("● Notas da bancada"));
        json_object_object_add(d, "file", json_object_new_string("docs/notas.pdf"));
        json_object_array_add(compiled, d);
    }
    if (!simulate) {
        if (bc_path(dest, sizeof dest, temp, "docs/manual.pdf", e) < 0 ||
            bs_manual(project, dest, e) < 0)
            goto done;
        json_object *d = json_object_new_object();
        json_object_object_add(d, "title",
                               json_object_new_string("Manual da bancada · procedimento e versão"));
        json_object_object_add(d, "file", json_object_new_string("docs/manual.pdf"));
        json_object_array_add(compiled, d);
    }
    if (bc_path(dest, sizeof dest, temp, "manifest.json", e) < 0 || bc_write_json(dest, m, e) < 0)
        goto done;
    {
        char outdir[4096];
        if (parent(output, outdir, e) < 0)
            goto done;
    }
    rc = bc_pack(temp, output, m, e);
done:
    json_object_put(m);
    bs_remove_temp(temp);
    return rc;
}

json_object *bs_import(const char *package, const char *project_path, bc_error *e) {
    json_object *m = bc_inspect(package, e);
    if (!m)
        return NULL;
    json_object *p = bc_clone(bc_get(m, "studio_source"));
    if (!p) {
        json_object_put(m);
        bc_fail(e, "Este pacote não contém um projeto editável do BPCB Studio. Rotinas C "
                   "existentes não são convertidas automaticamente em etapas.");
        return NULL;
    }
    char temp[4096], folder[4096], root[4096], src[4096], dst[4096];
    int ok = 0;
    if (bs_temp_dir(temp, "import", e) < 0) {
        json_object_put(m);
        json_object_put(p);
        return NULL;
    }
    if (bs_shape(p, e) < 0 || parent(project_path, root, e) < 0 ||
        bc_install(temp, package, bc_str(m, "package_digest"), e) < 0)
        goto end;
    json_object *profile = bc_profile(temp, bc_str(m, "id"), folder, e);
    if (!profile)
        goto end;
    json_object_put(profile);
    json_object *docs = bc_get(p, "documents");
    for (size_t i = 0; i < json_object_array_length(docs); i++) {
        const char *rel = bc_str(json_object_array_get_idx(docs, i), "file");
        if (!bc_safe_path(rel) || strncmp(rel, "assets/", 7)) {
            bc_fail(e, "Caminho de documento inválido.");
            goto end;
        }
        if (bc_path(src, sizeof src, folder, rel, e) < 0 ||
            bc_path(dst, sizeof dst, root, rel, e) < 0 || bs_copy(src, dst, e) < 0)
            goto end;
    }
    if (bs_save(project_path, p, e) < 0)
        goto end;
    ok = 1;
end:
    json_object_put(m);
    bs_remove_temp(temp);
    if (!ok) {
        json_object_put(p);
        p = NULL;
    }
    return p;
}

static int compare(const void *a, const void *b) {
    json_object *x = *(json_object *const *)a, *y = *(json_object *const *)b;
    int dx = json_object_get_boolean(bc_get(x, "directory")),
        dy = json_object_get_boolean(bc_get(y, "directory"));
    return dx != dy ? dy - dx : strcasecmp(bc_str(x, "name"), bc_str(y, "name"));
}

json_object *bs_browse(const char *path, const char *filter, bc_error *e) {
    char *real = realpath(path, NULL);
    if (!real) {
        bc_fail(e, "Abrir pasta: %s", strerror(errno));
        return NULL;
    }
    DIR *dir = opendir(real);
    if (!dir) {
        free(real);
        bc_fail(e, "Ler pasta: %s", strerror(errno));
        return NULL;
    }
    json_object *r = json_object_new_object(), *a = json_object_new_array();
    json_object_object_add(r, "path", json_object_new_string(real));
    json_object_object_add(r, "entries", a);
    free(real);
    struct dirent *en;
    size_t count = 0;
    while ((en = readdir(dir)) && count < 20000) {
        if (en->d_name[0] == '.')
            continue;
        struct stat st;
        char full[4096];
        if (bc_path(full, sizeof full, bc_str(r, "path"), en->d_name, e) < 0 || stat(full, &st) < 0)
            continue;
        int isdir = S_ISDIR(st.st_mode);
        const char *ext = strrchr(en->d_name, '.');
        int match = !strcmp(filter, "all") ||
                    (!strcmp(filter, "project") && ext && !strcasecmp(ext, ".bpcb")) ||
                    (!strcmp(filter, "package") && ext && !strcasecmp(ext, ".bancada")) ||
                    (!strcmp(filter, "document") && ext &&
                     (!strcasecmp(ext, ".pdf") || !strcasecmp(ext, ".svg")));
        if (!isdir && (!S_ISREG(st.st_mode) || !match))
            continue;
        json_object *v = json_object_new_object();
        json_object_object_add(v, "name", json_object_new_string(en->d_name));
        json_object_object_add(v, "directory", json_object_new_boolean(isdir));
        json_object_array_add(a, v);
        count++;
    }
    closedir(dir);
    json_object_array_sort(a, compare);
    return r;
}

json_object *bs_document_preview(const char *source, int page, bc_error *e) {
    char tmp[4096];
    if (bs_temp_dir(tmp, "preview", e) < 0)
        return NULL;
    char pdf[4096], png[4096];
    if (bc_path(pdf, sizeof pdf, tmp, "document.pdf", e) < 0 ||
        bc_path(png, sizeof png, tmp, "page.png", e) < 0) {
        bs_remove_temp(tmp);
        return NULL;
    }
    if (bs_asset_pdf(source, pdf, e) < 0) {
        bs_remove_temp(tmp);
        return NULL;
    }
    GError *err = NULL;
    gchar *uri = g_filename_to_uri(pdf, NULL, &err);
    PopplerDocument *doc = uri ? poppler_document_new_from_file(uri, NULL, &err) : NULL;
    g_free(uri);
    if (!doc) {
        bc_fail(e, "Abrir documento: %s", err ? err->message : "erro");
        g_clear_error(&err);
        bs_remove_temp(tmp);
        return NULL;
    }
    int count = poppler_document_get_n_pages(doc);
    if (page < 0)
        page = 0;
    if (page >= count)
        page = count - 1;
    PopplerPage *p = poppler_document_get_page(doc, page);
    if (!p) {
        g_object_unref(doc);
        bs_remove_temp(tmp);
        bc_fail(e, "Documento sem páginas.");
        return NULL;
    }
    double w, h;
    poppler_page_get_size(p, &w, &h);
    double scale = fmin(1.5, 1400 / fmax(w, h));
    int width = (int)ceil(w * scale), height = (int)ceil(h * scale);
    if (width < 1 || height < 1 || width > 16000 || height > 16000) {
        g_object_unref(p);
        g_object_unref(doc);
        bs_remove_temp(tmp);
        bc_fail(e, "Página inválida.");
        return NULL;
    }
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    cairo_t *cr = cairo_create(sf);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    poppler_page_render(p, cr);
    cairo_destroy(cr);
    cairo_status_t status = cairo_surface_write_to_png(sf, png);
    cairo_surface_destroy(sf);
    g_object_unref(p);
    g_object_unref(doc);
    if (status != CAIRO_STATUS_SUCCESS) {
        bs_remove_temp(tmp);
        bc_fail(e, "Falha ao desenhar página.");
        return NULL;
    }
    json_object *r = json_object_new_object();
    json_object_object_add(r, "root", json_object_new_string(tmp));
    json_object_object_add(r, "png", json_object_new_string(png));
    json_object_object_add(r, "pages", json_object_new_int(count));
    json_object_object_add(r, "page", json_object_new_int(page));
    return r;
}
