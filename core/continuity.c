#define _XOPEN_SOURCE 700
#include "studio.h"
#include <archive.h>
#include <archive_entry.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

/* Backups contain data only. Every entry is verified before restoration. */
static int data_path(const char *s) {
    return !strcmp(s, "project.bpcb") || !strcmp(s, "backup.json") ||
           (bc_safe_path(s) && !strncmp(s, "assets/", 7));
}

static int copy_new(const char *source, const char *destination, bc_error *e) {
    gchar *parent = g_path_get_dirname(destination);
    int rc = bc_mkdirs(parent, e);
    g_free(parent);
    if (rc < 0)
        return -1;
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0)
        return bc_fail(e, "Abrir arquivo gerado: %s", strerror(errno));
    int out = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (out < 0) {
        close(in);
        return bc_fail(e, "Escolha um arquivo novo; destino indisponível ou já existente: %s",
                       strerror(errno));
    }
    char buffer[32768];
    int ok = 1;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
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
            ssize_t k = write(out, buffer + used, (size_t)(n - used));
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
    if (!ok) {
        unlink(destination);
        return bc_fail(e, "Não foi possível concluir o novo arquivo.");
    }
    return 0;
}

static int add_file(struct archive *a, const char *root, const char *relative, bc_error *e) {
    char path[4096];
    if (bc_path(path, sizeof path, root, relative, e) < 0)
        return -1;
    gchar *data = NULL;
    gsize size = 0;
    GError *error = NULL;
    if (!g_file_get_contents(path, &data, &size, &error)) {
        bc_fail(e, "Ler arquivo: %s", error ? error->message : path);
        g_clear_error(&error);
        return -1;
    }
    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, relative);
    archive_entry_set_size(entry, (la_int64_t)size);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    int ok = archive_write_header(a, entry) == ARCHIVE_OK &&
             archive_write_data(a, data, size) == (la_ssize_t)size;
    archive_entry_free(entry);
    g_free(data);
    return ok ? 0 : bc_fail(e, "Escrever backup: %s", archive_error_string(a));
}

static json_object *unpack(const char *file, const char *root, bc_error *e) {
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_filter_all(a);
    json_object *seen = json_object_new_object(), *meta = NULL, *project = NULL;
    struct archive_entry *entry = NULL;
    size_t total = 0;
    int status, ok = 0;
    if (archive_read_open_filename(a, file, 32768) != ARCHIVE_OK) {
        bc_fail(e, "Abrir backup: %s", archive_error_string(a));
        goto done;
    }
    while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        la_int64_t n = archive_entry_size(entry);
        if (!name || !data_path(name) || archive_entry_filetype(entry) != AE_IFREG ||
            archive_entry_symlink(entry) || archive_entry_hardlink(entry) || bc_get(seen, name) ||
            json_object_object_length(seen) > 101 || n < 0 || n > 64 * 1024 * 1024 ||
            (total += (size_t)n) > 128 * 1024 * 1024) {
            bc_fail(e, "Backup contém caminho, tipo, tamanho ou duplicação inválida.");
            goto done;
        }
        char path[4096];
        if (bc_path(path, sizeof path, root, name, e) < 0)
            goto done;
        gchar *parent = g_path_get_dirname(path);
        int made = bc_mkdirs(parent, e);
        g_free(parent);
        if (made < 0)
            goto done;
        FILE *out = fopen(path, "wb");
        if (!out) {
            bc_fail(e, "Criar arquivo temporário: %s", strerror(errno));
            goto done;
        }
        char buf[32768];
        la_ssize_t got;
        size_t used = 0;
        int written = 1;
        while ((got = archive_read_data(a, buf, sizeof buf)) > 0) {
            used += (size_t)got;
            if (used > (size_t)n || fwrite(buf, 1, (size_t)got, out) != (size_t)got) {
                written = 0;
                break;
            }
        }
        if (fclose(out))
            written = 0;
        if (got < 0 || !written || used != (size_t)n) {
            bc_fail(e, "Conteúdo truncado ou inválido no backup.");
            goto done;
        }
        char hash[65];
        if (bc_sha_file(path, hash, e) < 0)
            goto done;
        json_object_object_add(seen, name, json_object_new_string(hash));
    }
    if (status != ARCHIVE_EOF) {
        bc_fail(e, "Backup incompleto.");
        goto done;
    }
    char path[4096];
    if (bc_path(path, sizeof path, root, "backup.json", e) < 0 ||
        (meta = bc_read_json(path, e)) == NULL)
        goto done;
    json_object *files = bc_get(meta, "files");
    if (strcmp(bc_str(meta, "format"), "bpcb.studio.backup") || bc_num(meta, "schema") != 1 ||
        !json_object_is_type(files, json_type_object) ||
        json_object_object_length(files) + 1 != json_object_object_length(seen)) {
        bc_fail(e, "Manifesto de backup inválido.");
        goto done;
    }
    json_object_object_foreach(files, name, hash) {
        if (!strcmp(name, "backup.json") || !json_object_is_type(hash, json_type_string) ||
            strcmp(bc_str(seen, name), json_object_get_string(hash))) {
            bc_fail(e, "Integridade inválida: %s", name);
            goto done;
        }
    }
    if (bc_path(path, sizeof path, root, "project.bpcb", e) < 0 ||
        (project = bs_load(path, e)) == NULL)
        goto done;
    json_object *docs = bc_get(project, "documents"), *referenced = json_object_new_object();
    for (size_t i = 0; i < json_object_array_length(docs); i++) {
        const char *name = bc_str(json_object_array_get_idx(docs, i), "file");
        if (!bc_safe_path(name) || strncmp(name, "assets/", 7) || !bc_get(files, name)) {
            json_object_put(referenced);
            bc_fail(e, "Documento ausente no backup.");
            goto done;
        }
        json_object_object_add(referenced, name, json_object_new_boolean(1));
    }
    if (json_object_object_length(referenced) + 1 != json_object_object_length(files)) {
        json_object_put(referenced);
        bc_fail(e, "Backup contém arquivos sem vínculo com o projeto.");
        goto done;
    }
    json_object_put(referenced);
    json_object_object_add(meta, "project", project);
    project = NULL;
    ok = 1;
done:
    archive_read_free(a);
    json_object_put(seen);
    json_object_put(project);
    if (!ok) {
        json_object_put(meta);
        meta = NULL;
    }
    return meta;
}

int bs_backup(json_object *p, const char *project_path, const char *output, bc_error *e) {
    if (bs_shape(p, e) < 0)
        return -1;
    char root[4096], path[4096], source[4096];
    if (bs_temp_dir(root, "backup", e) < 0)
        return -1;
    int rc = -1;
    json_object *meta = json_object_new_object(), *files = json_object_new_object();
    json_object_object_add(meta, "format", json_object_new_string("bpcb.studio.backup"));
    json_object_object_add(meta, "schema", json_object_new_int(1));
    json_object_object_add(meta, "studio_version", json_object_new_string(BS_VERSION));
    json_object_object_add(meta, "files", files);
    gchar *directory = g_path_get_dirname(project_path);
    char *realroot = realpath(directory, NULL);
    g_free(directory);
    if (!realroot) {
        bc_fail(e, "Salve o projeto antes de criar o backup.");
        goto done;
    }
    if (bc_path(path, sizeof path, root, "project.bpcb", e) < 0 || bs_save(path, p, e) < 0)
        goto done;
    char hash[65];
    if (bc_sha_file(path, hash, e) < 0)
        goto done;
    json_object_object_add(files, "project.bpcb", json_object_new_string(hash));
    json_object *docs = bc_get(p, "documents");
    for (size_t i = 0; i < json_object_array_length(docs); i++) {
        const char *rel = bc_str(json_object_array_get_idx(docs, i), "file");
        if (bc_get(files, rel))
            continue;
        if (!bc_safe_path(rel) || strncmp(rel, "assets/", 7) ||
            bc_path(source, sizeof source, realroot, rel, e) < 0 ||
            bc_path(path, sizeof path, root, rel, e) < 0) {
            bc_fail(e, "Caminho de documento inválido.");
            goto done;
        }
        char *real = realpath(source, NULL);
        size_t len = strlen(realroot);
        int inside =
            real && !strncmp(real, realroot, len) && (real[len] == '/' || real[len] == '\\');
        free(real);
        if (!inside) {
            bc_fail(e, "Documento aponta para fora da pasta do projeto.");
            goto done;
        }
        if (bs_copy(source, path, e) < 0 || bc_sha_file(path, hash, e) < 0)
            goto done;
        json_object_object_add(files, rel, json_object_new_string(hash));
    }
    if (bc_path(path, sizeof path, root, "backup.json", e) < 0 ||
        bc_write_json(path, meta, e) < 0 || bc_path(path, sizeof path, root, "backup.zip", e) < 0)
        goto done;
    struct archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    int ok = archive_write_open_filename(a, path) == ARCHIVE_OK;
    json_object_object_foreach(files, name, value) {
        (void)value;
        if (ok && add_file(a, root, name, e) < 0)
            ok = 0;
    }
    if (ok && add_file(a, root, "backup.json", e) < 0)
        ok = 0;
    if (archive_write_close(a) != ARCHIVE_OK)
        ok = 0;
    archive_write_free(a);
    if (!ok) {
        if (!*e->text)
            bc_fail(e, "Não foi possível escrever o backup.");
        goto done;
    }
    json_object *verified = bs_backup_preview(path, p, e);
    if (!verified)
        goto done;
    json_object_put(verified);
    rc = copy_new(path, output, e);
done:
    free(realroot);
    json_object_put(meta);
    bs_remove_temp(root);
    return rc;
}

json_object *bs_backup_preview(const char *backup, json_object *current, bc_error *e) {
    char root[4096], before[65], after[65];
    if (bc_sha_file(backup, before, e) < 0 || bs_temp_dir(root, "verify", e) < 0)
        return NULL;
    json_object *m = unpack(backup, root, e);
    bs_remove_temp(root);
    if (!m)
        return NULL;
    if (bc_sha_file(backup, after, e) < 0 || strcmp(before, after)) {
        json_object_put(m);
        bc_fail(e, "O backup mudou durante a verificação.");
        return NULL;
    }
    json_object_object_add(m, "digest", json_object_new_string(after));
    json_object_object_add(
        m, "same_project",
        json_object_new_boolean(current && json_object_equal(current, bc_get(m, "project"))));
    return m;
}

json_object *bs_restore(const char *backup, const char *destination, const char *digest,
                        bc_error *e) {
    char hash[65], root[4096];
    if (!digest || strlen(digest) != 64 || bc_sha_file(backup, hash, e) < 0 ||
        strcmp(hash, digest)) {
        bc_fail(e, "O backup mudou; visualize novamente antes de restaurar.");
        return NULL;
    }
    struct stat st;
    if (lstat(destination, &st) == 0 || errno != ENOENT) {
        bc_fail(e, "Escolha uma pasta nova para a restauração.");
        return NULL;
    }
    if (bs_temp_dir(root, "restore", e) < 0)
        return NULL;
    json_object *m = unpack(backup, root, e), *result = NULL;
    if (!m)
        goto done;
    if (bc_sha_file(backup, hash, e) < 0 || strcmp(hash, digest)) {
        bc_fail(e, "O backup mudou; restauração cancelada.");
        goto done;
    }
    gchar *staging = g_strconcat(destination, ".preparando-XXXXXX", NULL);
    if (!g_mkdtemp(staging)) {
        bc_fail(e, "Criar pasta de restauração: %s", strerror(errno));
        g_free(staging);
        goto done;
    }
    int ok = 1;
    json_object *files = bc_get(m, "files");
    json_object_object_foreach(files, name, value) {
        (void)value;
        char src[4096], dst[4096];
        if (bc_path(src, sizeof src, root, name, e) < 0 ||
            bc_path(dst, sizeof dst, staging, name, e) < 0 || bs_copy(src, dst, e) < 0) {
            ok = 0;
            break;
        }
    }
    if (ok && lstat(destination, &st) == 0) {
        bc_fail(e, "A pasta de destino apareceu durante a restauração.");
        ok = 0;
    }
    if (ok && g_rename(staging, destination) < 0) {
        bc_fail(e, "Finalizar restauração: %s", strerror(errno));
        ok = 0;
    }
    if (ok) {
        result = bc_clone(bc_get(m, "project"));
    } else {
        char previous[1024];
        snprintf(previous, sizeof previous, "%.1000s", e->text);
        bc_fail(e, "%s Cópia parcial preservada em %s.", previous, staging);
    }
    g_free(staging);
done:
    json_object_put(m);
    bs_remove_temp(root);
    return result;
}

int bs_manual(json_object *p, const char *output, bc_error *e) {
    if (bs_check(p, e) < 0)
        return -1;
    json_object *b = bc_get(p, "board");
    GString *s = g_string_new("");
    g_string_append_printf(
        s,
        "MANUAL DA BANCADA\n%s · %s\nVersão do procedimento: %s · BPCB Studio %s\nAutor: "
        "%s\n\nPREPARAÇÃO E CONEXÕES\n%s\n\nNOTAS\n%s\n\nROTEIRO DA SESSÃO\n%s\n%s\nCada teste "
        "requer início manual no BPCB.\n\nCOMPONENTES\n",
        bc_str(b, "pcb"), bc_str(b, "revision"), bc_str(b, "version"), BS_VERSION,
        bc_str(b, "author"), bc_str(b, "connections"), bc_str(b, "notes"),
        bc_str(b, "session_title"), bc_str(b, "session_order"));
    json_object *a = bc_get(p, "components");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *v = json_object_array_get_idx(a, i);
        g_string_append_printf(s, "%s = %s — %s\n", bc_str(v, "ref"), bc_str(v, "value"),
                               bc_str(v, "description"));
    }
    a = bc_get(p, "signals");
    g_string_append(s, "\nPONTOS E CANAIS\n");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *v = json_object_array_get_idx(a, i);
        g_string_append_printf(
            s, "%s: %s / referência %s · %s canal %.0f · faixa %.8g..%.8g %s · encerramento %.8g\n",
            bc_str(v, "label"), bc_str(v, "location"), bc_str(v, "return"), bc_str(v, "device"),
            bc_num(v, "channel"), bc_num(v, "min"), bc_num(v, "max"), bc_str(v, "unit"),
            bc_num(v, "safe"));
    }
    a = bc_get(p, "tests");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *t = json_object_array_get_idx(a, i);
        g_string_append_printf(
            s, "\nTESTE %zu — %s (%s)\nModo: %s · limite %.8g s\n%s\n\n%s\n\nESPERADO\n%s\n", i + 1,
            bc_str(t, "title"), bc_str(t, "id"), bc_str(t, "mode"), bc_num(t, "timeout"),
            bc_str(t, "purpose"), bc_str(t, "instructions"), bc_str(t, "expected"));
        const char *groups[] = {"parameters", "formulas", "steps"},
                   *kinds[] = {"parameter", "formula", "step"};
        for (unsigned k = 0; k < 3; k++) {
            json_object *items = bc_get(t, groups[k]), *cat = bs_catalog(kinds[k]);
            for (size_t j = 0; j < json_object_array_length(items); j++) {
                json_object *item = json_object_array_get_idx(items, j);
                g_string_append_printf(s, "\n%s %zu (%s)\n", bs_kind_label(kinds[k]), j + 1,
                                       bc_str(item, "id"));
                for (size_t q = 0; q < json_object_array_length(cat); q++) {
                    json_object *f = json_object_array_get_idx(cat, q),
                                *v = bc_get(item, bc_str(f, "key"));
                    if (!v || !bs_field_visible(f, item))
                        continue;
                    const char *text =
                        json_object_is_type(v, json_type_string)
                            ? json_object_get_string(v)
                            : json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN);
                    if (*text)
                        g_string_append_printf(s, "%s: %s\n", bc_str(f, "label"), text);
                }
            }
            json_object_put(cat);
        }
    }
    g_string_append(s, "\nEste manual descreve o procedimento cadastrado. Simulação não confirma "
                       "tensões, conexões ou comportamento físico. Leituras fora da faixa são "
                       "registradas e interrompem a sequência.\n");
    char temp[4096], pdf[4096];
    int rc = -1;
    if (bs_temp_dir(temp, "manual", e) == 0) {
        if (bc_path(pdf, sizeof pdf, temp, "manual.pdf", e) == 0 &&
            bs_notes_pdf(s->str, pdf, e) == 0)
            rc = copy_new(pdf, output, e);
        bs_remove_temp(temp);
    }
    g_string_free(s, TRUE);
    return rc;
}
