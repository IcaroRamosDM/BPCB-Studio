#define _POSIX_C_SOURCE 200809L
#include "bancada.h"
#include <poppler.h>
#include <cairo.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Apenas renderiza paginas: nao executa JavaScript, links nem anexos do PDF. */
json_object *bc_render_document(const char *root, const char *id, const char *name,
                                json_object *options, bc_error *e) {
    char folder[4096], file[4096], cache[4096], destination[4096], temporary[4096];
    json_object *m = bc_profile(root, id, folder, e), *result = NULL;
    if (!m)
        return NULL;
    int found = 0;
    json_object *docs = bc_get(m, "documents");
    for (size_t i = 0; i < json_object_array_length(docs); i++)
        if (!strcmp(name, bc_str(json_object_array_get_idx(docs, i), "file")))
            found = 1;
    if (!found || !bc_safe_path(name)) {
        bc_fail(e, "Documento nao pertence a bancada.");
        goto done;
    }
    if (bc_verify(m, folder, e) < 0 || bc_path(file, sizeof file, folder, name, e) < 0)
        goto done;
    double page_number = bc_num(options, "page"), scale = bc_num(options, "scale");
    if (!isfinite(page_number) || page_number < 0 || page_number > 10000 ||
        floor(page_number) != page_number || !isfinite(scale) || scale < 0 || scale > 4) {
        bc_fail(e, "Pagina ou zoom invalido.");
        goto done;
    }
    GError *error = NULL;
    char *uri = g_filename_to_uri(file, NULL, &error);
    PopplerDocument *doc = uri ? poppler_document_new_from_file(uri, NULL, &error) : NULL;
    g_free(uri);
    if (!doc) {
        bc_fail(e, "Nao foi possivel ler o PDF: %s", error ? error->message : "erro de leitura");
        g_clear_error(&error);
        goto done;
    }
    int pages = poppler_document_get_n_pages(doc);
    if (page_number >= pages) {
        bc_fail(e, "Pagina inexistente.");
        g_object_unref(doc);
        goto done;
    }
    PopplerPage *page = poppler_document_get_page(doc, (int)page_number);
    double width = 0, height = 0;
    if (!page) {
        bc_fail(e, "Pagina indisponivel.");
        g_object_unref(doc);
        goto done;
    }
    poppler_page_get_size(page, &width, &height);
    if (scale == 0) {
        double viewport_width = bc_num(options, "width"),
               viewport_height = bc_num(options, "height");
        if (viewport_width < 100 || viewport_width > 5000 || viewport_height < 100 ||
            viewport_height > 5000 || width <= 0 || height <= 0) {
            bc_fail(e, "Area de leitura invalida.");
            g_object_unref(page);
            g_object_unref(doc);
            goto done;
        }
        scale = fmin(4, fmin(viewport_width / width, viewport_height / height));
    }
    width = ceil(width * scale);
    height = ceil(height * scale);
    if (width < 1 || height < 1 || width > 12000 || height > 12000 || width * height > 24000000) {
        bc_fail(e, "Pagina grande demais neste zoom.");
        g_object_unref(page);
        g_object_unref(doc);
        goto done;
    }
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)width, (int)height);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);
    int good = cairo_status(cr) == CAIRO_STATUS_SUCCESS &&
               cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
    if (good && bc_path(cache, sizeof cache, root, "preview", e) == 0 && bc_mkdirs(cache, e) == 0 &&
        bc_path(destination, sizeof destination, cache, "document.png", e) == 0 &&
        bc_path(temporary, sizeof temporary, cache, "page-XXXXXX", e) == 0) {
        int fd = mkstemp(temporary);
        if (fd < 0)
            bc_fail(e, "Falha no cache do documento.");
        else {
            close(fd);
            if (cairo_surface_write_to_png(surface, temporary) != CAIRO_STATUS_SUCCESS ||
                rename(temporary, destination) < 0)
                bc_fail(e, "Falha ao gravar a pagina do documento.");
            else {
                result = json_object_new_object();
                json_object_object_add(result, "path", json_object_new_string(destination));
                json_object_object_add(result, "scale", json_object_new_double(scale));
                json_object_object_add(result, "pages", json_object_new_int(pages));
                json_object_object_add(result, "page", json_object_new_int((int)page_number));
            }
            unlink(temporary);
        }
    } else if (!good)
        bc_fail(e, "Falha ao renderizar a pagina.");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    g_object_unref(page);
    g_object_unref(doc);
done:
    json_object_put(m);
    return result;
}
