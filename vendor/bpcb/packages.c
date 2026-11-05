#define _POSIX_C_SOURCE 200809L
#include "bancada.h"
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#ifndef _WIN32
#include <spawn.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>
#ifndef _WIN32
extern char **environ;
#endif
typedef struct item {
    char *name;
    unsigned char *data;
    size_t size;
    struct item *next;
} item;

typedef struct {
    item *files;
    json_object *m;
    char digest[65];
} bundle;

static void digest(const void *data, size_t size, char out[65]) {
    unsigned char hash[32];
    unsigned len = 0;
    if (EVP_Digest(data, size, hash, &len, EVP_sha256(), NULL) != 1 || len != 32) {
        out[0] = 0;
        return;
    }
    for (unsigned i = 0; i < 32; i++)
        snprintf(out + 2 * i, 3, "%02x", hash[i]);
}

static void dispose(bundle *b) {
    while (b->files) {
        item *n = b->files->next;
        free(b->files->data);
        free(b->files->name);
        free(b->files);
        b->files = n;
    }
    if (b->m)
        json_object_put(b->m);
    memset(b, 0, sizeof *b);
}

static item *find_item(bundle *b, const char *name) {
    for (item *p = b->files; p; p = p->next)
        if (!strcmp(p->name, name))
            return p;
    return NULL;
}

static int check_bundle(bundle *b, bc_error *e) {
    item *meta = find_item(b, "manifest.json");
    if (!meta || meta->size > 2 * 1024 * 1024 || memchr(meta->data, 0, meta->size) ||
        !bc_utf8(meta->data, meta->size))
        return bc_fail(e, "Manifesto ausente ou invalido.");
    b->m = bc_parse_json((const char *)meta->data, meta->size, e);
    if (!b->m || bc_validate(b->m, e) < 0)
        return -1;
    digest(meta->data, meta->size, b->digest);
    json_object *hashes = bc_get(b->m, "files");
    if (!json_object_is_type(hashes, json_type_object))
        return bc_fail(e, "Indice de integridade ausente.");
    size_t actual = 0;
    for (item *p = b->files; p; p = p->next)
        if (strcmp(p->name, "manifest.json")) {
            actual++;
            char hash[65];
            digest(p->data, p->size, hash);
            if (!*hash || strcmp(hash, bc_str(hashes, p->name)))
                return bc_fail(e, "Arquivo corrompido: %s", p->name);
        }
    if (actual != (size_t)json_object_object_length(hashes))
        return bc_fail(e, "O conteudo nao corresponde ao indice de arquivos.");
    json_object *tests = bc_get(b->m, "tests");
    for (size_t i = 0; i < json_object_array_length(tests); i++) {
        json_object *t = json_object_array_get_idx(tests, i);
        if (!json_object_get_boolean(bc_get(t, "enabled")))
            continue;
        item *p = find_item(b, bc_str(t, "entry"));
#ifdef _WIN32
        int native = p && p->size >= 64 && p->data[0] == 'M' && p->data[1] == 'Z';
        if (native) {
            unsigned off = (unsigned)p->data[60] | ((unsigned)p->data[61] << 8) |
                           ((unsigned)p->data[62] << 16) | ((unsigned)p->data[63] << 24);
            native = off <= p->size - 6 && !memcmp(p->data + off, "PE\0\0\x64\x86", 6);
        }
        if (!native)
#else
        if (!p || p->size < 20 || memcmp(p->data, "\177ELF", 4))
#endif
            return bc_fail(e, "Rotina deve ser um executavel nativo: %s", bc_str(t, "entry"));
    }
    json_object *docs = bc_get(b->m, "documents");
    if (docs)
        for (size_t i = 0; i < json_object_array_length(docs); i++)
            if (!find_item(b, bc_str(json_object_array_get_idx(docs, i), "file")))
                return bc_fail(e, "Documento ausente do pacote.");
    return 0;
}

static int read_bundle(const char *path, bundle *b, bc_error *e) {
    memset(b, 0, sizeof *b);
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > 128 * 1024 * 1024)
        return bc_fail(e, "Pacote ausente ou grande demais.");
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    if (archive_read_open_filename(a, path, 32768) != ARCHIVE_OK) {
        bc_fail(e, "Abrir pacote: %s", archive_error_string(a));
        archive_read_free(a);
        return -1;
    }
    struct archive_entry *entry;
    size_t total = 0, count = 0;
    int rc = -1, r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        la_int64_t n = archive_entry_size(entry);
        if (!bc_safe_path(name) || archive_entry_filetype(entry) != AE_IFREG ||
            archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry) || n < 0 || n > 64 * 1024 * 1024 || ++count > 4096) {
            bc_fail(e, "Entrada de pacote invalida.");
            goto done;
        }
        for (item *p = b->files; p; p = p->next)
            if (!strcasecmp(p->name, name)) {
                bc_fail(e, "Arquivo repetido: %s", name);
                goto done;
            }
        total += (size_t)n;
        if (total > 256 * 1024 * 1024) {
            bc_fail(e, "Pacote expandido grande demais.");
            goto done;
        }
        item *p = calloc(1, sizeof *p);
        if (!p)
            goto memory;
        p->name = strdup(name);
        p->size = (size_t)n;
        p->data = calloc(p->size + 1, 1);
        p->next = b->files;
        b->files = p;
        if (!p->name || !p->data)
            goto memory;
        size_t used = 0;
        while (used < p->size) {
            la_ssize_t k = archive_read_data(a, p->data + used, p->size - used);
            if (k <= 0) {
                bc_fail(e, "Dados truncados: %s", name);
                goto done;
            }
            used += (size_t)k;
        }
        unsigned char extra;
        if (archive_read_data(a, &extra, 1) != 0) {
            bc_fail(e, "Tamanho/CRC invalido no pacote.");
            goto done;
        }
    }
    if (r != ARCHIVE_EOF) {
        bc_fail(e, "Erro no ZIP: %s", archive_error_string(a));
        goto done;
    }
    rc = check_bundle(b, e);
    goto done;
memory:
    bc_fail(e, "Memoria insuficiente.");
done:
    archive_read_free(a);
    if (rc)
        dispose(b);
    return rc;
}

json_object *bc_inspect(const char *path, bc_error *e) {
    bundle b;
    if (read_bundle(path, &b, e) < 0)
        return NULL;
    json_object *m = bc_clone(b.m);
    json_object_object_add(m, "package_digest", json_object_new_string(b.digest));
    dispose(&b);
    return m;
}

static int parent_dir(const char *path, bc_error *e) {
    char b[4096];
    if (strlen(path) >= sizeof b)
        return bc_fail(e, "Caminho longo demais.");
    strcpy(b, path);
    char *s = strrchr(b, '/');
    if (!s)
        return 0;
    *s = 0;
    return bc_mkdirs(b, e);
}

static void remove_stage(const char *path) {
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *en;
    while ((en = readdir(d))) {
        if (!strcmp(en->d_name, ".") || !strcmp(en->d_name, ".."))
            continue;
        char p[4096];
        if (bc_path(p, sizeof p, path, en->d_name, NULL) < 0)
            continue;
        struct stat st;
        if (lstat(p, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode))
            remove_stage(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static json_object *index_read(const char *root, bc_error *e) {
    char path[4096];
    if (bc_path(path, sizeof path, root, "active.json", e) < 0)
        return NULL;
    if (access(path, F_OK) < 0 && errno == ENOENT)
        return json_object_new_object();
    json_object *o = bc_read_json(path, e);
    if (o && !json_object_is_type(o, json_type_object)) {
        json_object_put(o);
        bc_fail(e, "Indice de bancadas invalido.");
        return NULL;
    }
    return o;
}

static int setup(const char *root, bc_error *e) {
    if (bc_mkdirs(root, e) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        char p[4096];
        if (bc_path(p, sizeof p, root, i ? "state" : "packages", e) < 0 || bc_mkdirs(p, e) < 0)
            return -1;
    }
    return 0;
}

int bc_install(const char *root, const char *package, const char *expected, bc_error *e) {
    bundle b;
    if (read_bundle(package, &b, e) < 0)
        return -1;
    if (expected && *expected && strcmp(expected, b.digest)) {
        dispose(&b);
        return bc_fail(e, "O pacote mudou apos a conferencia.");
    }
    char lock_path[4096], packages[4096], stage[4096], target[4096], key[256], index_path[4096];
    int rc = -1, lock = -1;
    json_object *index = NULL;
    stage[0] = 0;
    if (setup(root, e) < 0 || bc_path(lock_path, sizeof lock_path, root, "storage.lock", e) < 0)
        goto done;
    lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX) < 0) {
        bc_fail(e, "Nao foi possivel reservar a instalacao.");
        goto done;
    }
    if (bc_path(packages, sizeof packages, root, "packages", e) < 0)
        goto done;
    snprintf(key, sizeof key, "%s-%s-%.24s", bc_str(b.m, "id"), bc_str(b.m, "version"), b.digest);
    if (bc_path(target, sizeof target, packages, key, e) < 0 ||
        bc_path(stage, sizeof stage, packages, ".install-XXXXXX", e) < 0)
        goto done;
    if (!mkdtemp(stage)) {
        stage[0] = 0;
        bc_fail(e, "Criar instalacao temporaria: %s", strerror(errno));
        goto done;
    }
    for (item *p = b.files; p; p = p->next) {
        char file[4096];
        if (bc_path(file, sizeof file, stage, p->name, e) < 0 || parent_dir(file, e) < 0)
            goto done;
        int fd = open(file, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            bc_fail(e, "Criar arquivo: %s", strerror(errno));
            goto done;
        }
        size_t used = 0;
        int ok = 1;
        while (used < p->size) {
            ssize_t n = write(fd, p->data + used, p->size - used);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                ok = 0;
                break;
            }
            used += (size_t)n;
        }
#ifndef _WIN32
        if (fchmod(fd, !strncmp(p->name, "code/", 5) ? 0700 : 0600) < 0)
            ok = 0;
#endif
        if (close(fd) < 0)
            ok = 0;
        if (!ok) {
            bc_fail(e, "Falha ao escrever pacote.");
            goto done;
        }
    }
    if (rename(stage, target) < 0) {
        if (errno != EEXIST && errno != ENOTEMPTY) {
            bc_fail(e, "Ativar pasta do pacote: %s", strerror(errno));
            goto done;
        }
        if (bc_verify(b.m, target, e) < 0)
            goto done;
    } else
        stage[0] = 0;
    index = index_read(root, e);
    if (!index)
        goto done;
    const char *previous = bc_str(index, bc_str(b.m, "id"));
    if (*previous && strcmp(previous, key) && bc_safe_path(previous) && !strchr(previous, '/')) {
        char relative[512], old_state[4096], new_state[4096];
        snprintf(relative, sizeof relative, "state/%s.json", previous);
        if (bc_path(old_state, sizeof old_state, root, relative, e) < 0)
            goto done;
        if (access(old_state, F_OK) == 0) {
            json_object *values = bc_read_json(old_state, e);
            json_object *checked = values ? bc_apply(b.m, values, e) : NULL;
            if (!checked) {
                if (values)
                    json_object_put(values);
                goto done;
            }
            json_object_put(checked);
            snprintf(relative, sizeof relative, "state/%s.json", key);
            int saved = bc_path(new_state, sizeof new_state, root, relative, e);
            if (!saved)
                saved = bc_write_json(new_state, values, e);
            json_object_put(values);
            if (saved < 0)
                goto done;
        }
    }
    json_object_object_add(index, bc_str(b.m, "id"), json_object_new_string(key));
    if (bc_path(index_path, sizeof index_path, root, "active.json", e) < 0)
        goto done;
    rc = bc_write_json(index_path, index, e);
done:
    if (*stage)
        remove_stage(stage);
    if (index)
        json_object_put(index);
    if (lock >= 0)
        close(lock);
    dispose(&b);
    return rc;
}

static int profile_paths(const char *root, const char *id, char folder[4096], char state[4096],
                         bc_error *e) {
    json_object *index = index_read(root, e);
    if (!index)
        return -1;
    const char *key = bc_str(index, id);
    char relative[512];
    if (!*key || strchr(key, '/') || !bc_safe_path(key)) {
        json_object_put(index);
        return bc_fail(e, "Bancada nao instalada.");
    }
    snprintf(relative, sizeof relative, "packages/%s", key);
    int rc = bc_path(folder, 4096, root, relative, e);
    snprintf(relative, sizeof relative, "state/%s.json", key);
    if (!rc)
        rc = bc_path(state, 4096, root, relative, e);
    json_object_put(index);
    return rc;
}

json_object *bc_profile(const char *root, const char *id, char folder[4096], bc_error *e) {
    char state[4096], manifest[4096];
    if (profile_paths(root, id, folder, state, e) < 0 ||
        bc_path(manifest, sizeof manifest, folder, "manifest.json", e) < 0)
        return NULL;
    json_object *m = bc_read_json(manifest, e);
    if (!m || bc_validate(m, e) < 0 || strcmp(bc_str(m, "id"), id)) {
        if (m)
            json_object_put(m);
        return NULL;
    }
    if (access(state, F_OK) == 0) {
        json_object *v = bc_read_json(state, e), *copy = NULL;
        if (v) {
            copy = bc_apply(m, v, e);
            json_object_put(v);
        }
        json_object_put(m);
        m = copy;
    }
    return m;
}

json_object *bc_list(const char *root, bc_error *e) {
    json_object *idx = index_read(root, e), *result = json_object_new_array();
    if (!idx) {
        json_object_put(result);
        return NULL;
    }
    json_object_object_foreach(idx, id, v) {
        (void)v;
        char folder[4096];
        bc_error local = {{0}};
        json_object *m = bc_profile(root, id, folder, &local);
        if (m)
            json_object_array_add(result, m);
        /* Pacotes Python anteriores permanecem no disco, sem serem executados. */
    }
    json_object_put(idx);
    return result;
}

int bc_save(const char *root, const char *id, json_object *values, bc_error *e) {
    char folder[4096], state[4096];
    json_object *m = bc_profile(root, id, folder, e);
    if (!m)
        return -1;
    json_object *copy = bc_apply(m, values, e);
    json_object_put(m);
    if (!copy)
        return -1;
    json_object_put(copy);
    if (profile_paths(root, id, folder, state, e) < 0)
        return -1;
    return bc_write_json(state, values, e);
}

int bc_boot(const char *root, const char *seed, bc_error *e) {
    /* Inicializar o armazenamento nunca instala uma PCB implicitamente. */
    (void)seed;
    return setup(root, e);
}

int bc_verify(json_object *m, const char *folder, bc_error *e) {
    json_object *files = bc_get(m, "files");
    if (!json_object_is_type(files, json_type_object))
        return bc_fail(e, "Indice de integridade ausente.");
    json_object_object_foreach(files, k, v) {
        char path[4096], hash[65];
        (void)v;
        if (!bc_safe_path(k) || bc_path(path, sizeof path, folder, k, e) < 0 ||
            bc_sha_file(path, hash, e) < 0)
            return -1;
        if (strcmp(hash, bc_str(files, k)))
            return bc_fail(e, "Arquivo instalado foi alterado: %s", k);
    }
    return 0;
}

static int collect(const char *base, const char *relative, bundle *b, bc_error *e, unsigned depth) {
    if (depth > 20)
        return bc_fail(e, "Pastas profundas demais.");
    char dir[4096];
    if (*relative) {
        if (bc_path(dir, sizeof dir, base, relative, e) < 0)
            return -1;
    } else
        snprintf(dir, sizeof dir, "%s", base);
    DIR *d = opendir(dir);
    if (!d)
        return bc_fail(e, "Abrir pasta: %s", strerror(errno));
    int rc = 0;
    struct dirent *en;
    while (!rc && (en = readdir(d))) {
        if (en->d_name[0] == '.' || !strcmp(en->d_name, "manifest.json"))
            continue;
        char rel[4096], path[4096];
        if (*relative)
            rc = bc_path(rel, sizeof rel, relative, en->d_name, e);
        else
            snprintf(rel, sizeof rel, "%s", en->d_name);
        if (rc || !bc_safe_path(rel) || bc_path(path, sizeof path, base, rel, e) < 0) {
            rc = -1;
            break;
        }
        struct stat st;
        if (lstat(path, &st) < 0) {
            rc = bc_fail(e, "Arquivo ausente.");
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            rc = collect(base, rel, b, e, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode) || st.st_size > 64 * 1024 * 1024) {
            rc = bc_fail(e, "Arquivo invalido: %s", rel);
            break;
        }
        size_t total = (size_t)st.st_size;
        unsigned entries = 1;
        for (item *existing = b->files; existing; existing = existing->next) {
            total += existing->size;
            entries++;
        }
        if (total > 256 * 1024 * 1024 || entries > 4096) {
            rc = bc_fail(e, "Conteudo do pacote grande demais.");
            break;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            rc = bc_fail(e, "Abrir arquivo: %s", rel);
            break;
        }
        item *p = calloc(1, sizeof *p);
        if (!p) {
            fclose(f);
            rc = bc_fail(e, "Memoria insuficiente.");
            break;
        }
        p->name = strdup(rel);
        p->size = (size_t)st.st_size;
        p->data = calloc(p->size + 1, 1);
        p->next = b->files;
        b->files = p;
        if (!p->data || !p->name || fread(p->data, 1, p->size, f) != p->size)
            rc = bc_fail(e, "Falha ao ler arquivo.");
        fclose(f);
    }
    closedir(d);
    return rc;
}

int bc_pack(const char *folder, const char *destination, json_object *override, bc_error *e) {
    bundle b = {0};
    char manifest[4096], temp[4096];
    int rc = -1;
    if (bc_path(manifest, sizeof manifest, folder, "manifest.json", e) < 0)
        return -1;
    b.m = override ? bc_clone(override) : bc_read_json(manifest, e);
    if (!b.m || bc_validate(b.m, e) < 0 || collect(folder, "", &b, e, 0) < 0)
        goto done;
    json_object *hashes = json_object_new_object();
    for (item *p = b.files; p; p = p->next) {
        char hash[65];
        digest(p->data, p->size, hash);
        json_object_object_add(hashes, p->name, json_object_new_string(hash));
    }
    json_object_object_add(b.m, "files", hashes);
    item *meta = calloc(1, sizeof *meta);
    if (!meta) {
        bc_fail(e, "Memoria insuficiente.");
        goto done;
    }
    meta->name = strdup("manifest.json");
    const char *js = json_object_to_json_string_ext(b.m, JSON_C_TO_STRING_PRETTY);
    meta->data = (unsigned char *)strdup(js);
    meta->size = strlen(js);
    meta->next = b.files;
    b.files = meta;
    json_object_put(b.m);
    b.m = NULL;
    if (check_bundle(&b, e) < 0)
        goto done;
    if (parent_dir(destination, e) < 0 ||
        snprintf(temp, sizeof temp, "%s.tmp.XXXXXX", destination) >= (int)sizeof temp)
        goto done;
    int fd = mkstemp(temp);
    if (fd < 0) {
        bc_fail(e, "Criar pacote: %s", strerror(errno));
        goto done;
    }
    struct archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    int ok = archive_write_open_fd(a, fd) == ARCHIVE_OK;
    for (item *p = b.files; ok && p; p = p->next) {
        struct archive_entry *en = archive_entry_new();
        archive_entry_set_pathname(en, p->name);
        archive_entry_set_filetype(en, AE_IFREG);
        archive_entry_set_perm(en, !strncmp(p->name, "code/", 5) ? 0755 : 0644);
        archive_entry_set_size(en, (la_int64_t)p->size);
        ok = archive_write_header(a, en) == ARCHIVE_OK &&
             archive_write_data(a, p->data, p->size) == (la_ssize_t)p->size;
        archive_entry_free(en);
    }
    if (archive_write_close(a) != ARCHIVE_OK)
        ok = 0;
    archive_write_free(a);
    if (fsync(fd) < 0)
        ok = 0;
    if (close(fd) < 0)
        ok = 0;
    if (ok && rename(temp, destination) == 0)
        rc = 0;
    else {
        unlink(temp);
        bc_fail(e, "Falha ao gravar pacote.");
    }
done:
    dispose(&b);
    return rc;
}

int bc_export(const char *root, const char *id, const char *destination, json_object *values,
              bc_error *e) {
    char folder[4096];
    json_object *m = bc_profile(root, id, folder, e);
    if (!m)
        return -1;
    json_object *copy = bc_apply(m, values, e);
    json_object_put(m);
    if (!copy)
        return -1;
    int rc = bc_verify(copy, folder, e);
    if (!rc)
        rc = bc_pack(folder, destination, copy, e);
    json_object_put(copy);
    return rc;
}
#ifndef _WIN32
static void *document_reaper(void *arg) {
    pid_t pid = *(pid_t *)arg;
    free(arg);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    return NULL;
}
#endif
int bc_open_document(const char *root, const char *id, const char *name, bc_error *e) {
    char folder[4096], file[4096];
    json_object *m = bc_profile(root, id, folder, e);
    if (!m)
        return -1;
    int found = 0;
    json_object *docs = bc_get(m, "documents");
    if (docs)
        for (size_t i = 0; i < json_object_array_length(docs); i++)
            if (!strcmp(name, bc_str(json_object_array_get_idx(docs, i), "file")))
                found = 1;
    int rc = found ? bc_verify(m, folder, e) : bc_fail(e, "Documento nao pertence a bancada.");
    json_object_put(m);
    if (rc || bc_path(file, sizeof file, folder, name, e) < 0)
        return -1;
#ifdef _WIN32
    gunichar2 *wide = g_utf8_to_utf16(file, -1, NULL, NULL, NULL);
    intptr_t opened =
        (intptr_t)ShellExecuteW(NULL, L"open", (wchar_t *)wide, NULL, NULL, SW_SHOWNORMAL);
    g_free(wide);
    return opened > 32 ? 0 : bc_fail(e, "Não foi possível abrir o leitor de PDF.");
#else
    pid_t pid;
    char *args[] = {"xdg-open", file, NULL};
    rc = posix_spawnp(&pid, args[0], NULL, NULL, args, environ);
    if (rc)
        return bc_fail(e, "Nao foi possivel abrir o leitor de PDF: %s", strerror(rc));
    pid_t *task = malloc(sizeof *task);
    if (task) {
        *task = pid;
        pthread_t reaper;
        if (pthread_create(&reaper, NULL, document_reaper, task) == 0)
            pthread_detach(reaper);
        else
            free(task);
    }
#endif
    /* O leitor pertence a sessao do usuario e pode permanecer aberto. */
    return 0;
}
