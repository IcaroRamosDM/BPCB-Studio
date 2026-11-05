#define _POSIX_C_SOURCE 200809L
#include "bancada.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *bc_str(json_object *o, const char *key) {
    json_object *v = bc_get(o, key);
    return v && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : "";
}

json_object *bc_get(json_object *o, const char *key) {
    json_object *v = NULL;
    if (o && key)
        json_object_object_get_ex(o, key, &v);
    return v;
}

double bc_num(json_object *o, const char *key) {
    return json_object_get_double(bc_get(o, key));
}

int bc_fail(bc_error *e, const char *fmt, ...) {
    if (e) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->text, sizeof e->text, fmt, ap);
        va_end(ap);
    }
    return -1;
}

json_object *bc_clone(json_object *o) {
    json_object *copy = NULL;
    if (o)
        json_object_deep_copy(o, &copy, NULL);
    return copy;
}

int bc_path(char *out, size_t cap, const char *base, const char *tail, bc_error *e) {
    int n = snprintf(out, cap, "%s/%s", base, tail);
    return n < 0 || (size_t)n >= cap ? bc_fail(e, "Caminho muito longo.") : 0;
}

int bc_mkdirs(const char *path, bc_error *e) {
#ifdef _WIN32
    if (g_mkdir_with_parents(path, 0700) < 0)
        return bc_fail(e, "Criar pasta: %s", strerror(errno));
    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode))
        return bc_fail(e, "Pasta inválida ou link.");
    return 0;
#else
    char b[4096];
    size_t len = strlen(path);
    if (!len || len >= sizeof b)
        return bc_fail(e, "Caminho invalido.");
    memcpy(b, path, len + 1);
    for (size_t i = 1; i <= len; i++)
        if (b[i] == '/' || b[i] == 0) {
            char save = b[i];
            b[i] = 0;
            if (mkdir(b, 0700) < 0 && errno != EEXIST)
                return bc_fail(e, "Criar pasta: %s", strerror(errno));
            struct stat st;
            if (lstat(b, &st) < 0 || !S_ISDIR(st.st_mode))
                return bc_fail(e, "Pasta invalida ou link: %s", b);
            b[i] = save;
        }
    return 0;
#endif
}

int bc_safe_path(const char *name) {
    if (!name || !*name || strlen(name) > 800 || name[0] == '/')
        return 0;
    const char *segment = name;
    for (const unsigned char *p = (const unsigned char *)name;; p++) {
        if (*p == '/' || !*p) {
            size_t n = (size_t)((const char *)p - segment);
            if (!n || (n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.'))
                return 0;
#ifdef _WIN32
            if (segment[n - 1] == '.' || segment[n - 1] == ' ')
                return 0;
            char base[16];
            size_t k = 0;
            while (k < n && segment[k] != '.' && k < sizeof base - 1) {
                base[k] = (char)toupper((unsigned char)segment[k]);
                k++;
            }
            base[k] = 0;
            if (!strcmp(base, "CON") || !strcmp(base, "PRN") || !strcmp(base, "AUX") ||
                !strcmp(base, "NUL") ||
                ((!strncmp(base, "COM", 3) || !strncmp(base, "LPT", 3)) && strlen(base) == 4 &&
                 base[3] >= '1' && base[3] <= '9'))
                return 0;
#endif
            segment = (const char *)p + 1;
        } else if (*p < 32 || *p == 127 || *p == '\\' || *p == ':')
            return 0;
        if (!*p)
            break;
    }
    return 1;
}

json_object *bc_read_json(const char *path, bc_error *e) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) {
        bc_fail(e, "Abrir %s: %s", path, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 2 ||
        st.st_size > 8 * 1024 * 1024) {
        close(fd);
        bc_fail(e, "Arquivo JSON invalido ou grande demais.");
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    char *data = malloc(n + 1);
    if (!data) {
        close(fd);
        bc_fail(e, "Memoria insuficiente.");
        return NULL;
    }
    size_t used = 0;
    while (used < n) {
        ssize_t r = read(fd, data + used, n - used);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        used += (size_t)r;
    }
    close(fd);
    data[used] = 0;
    json_object *o = used == n ? bc_parse_json(data, used, e) : NULL;
    free(data);
    return o;
}

int bc_write_json(const char *path, json_object *o, bc_error *e) {
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", path) >= (int)sizeof tmp)
        return bc_fail(e, "Caminho muito longo.");
    int fd = mkstemp(tmp);
    if (fd < 0)
        return bc_fail(e, "Salvar: %s", strerror(errno));
    const char *data = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PRETTY);
    size_t n = strlen(data), used = 0;
    int rc = 0;
    while (used < n) {
        ssize_t k = write(fd, data + used, n - used);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            rc = -1;
            break;
        }
        used += (size_t)k;
    }
    if (fsync(fd) < 0)
        rc = -1;
    if (close(fd) < 0)
        rc = -1;
    if (!rc && rename(tmp, path) < 0)
        rc = -1;
    if (rc) {
        unlink(tmp);
        return bc_fail(e, "Falha ao salvar: %s", strerror(errno));
    }
    return 0;
}

int bc_sha_file(const char *path, char out[65], bc_error *e) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0)
        return bc_fail(e, "Arquivo ausente: %s", path);
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > 128 * 1024 * 1024) {
        close(fd);
        return bc_fail(e, "Arquivo invalido: %s", path);
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char buf[32768], hash[32];
    unsigned n = 0;
    int rc = 0;
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1)
        rc = -1;
    off_t verified = 0;
    while (!rc) {
        ssize_t k = read(fd, buf, sizeof buf);
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0) {
            rc = -1;
            break;
        }
        if (!k)
            break;
        if ((off_t)k > st.st_size - verified) {
            rc = -1;
            break;
        }
        verified += (off_t)k;
        if (EVP_DigestUpdate(ctx, buf, (size_t)k) != 1)
            rc = -1;
    }
    struct stat final;
    if (!rc && (verified != st.st_size || fstat(fd, &final) < 0 || final.st_size != st.st_size ||
                final.st_mtime != st.st_mtime || final.st_ctime != st.st_ctime))
        rc = -1;
    if (!rc && (EVP_DigestFinal_ex(ctx, hash, &n) != 1 || n != 32))
        rc = -1;
    EVP_MD_CTX_free(ctx);
    close(fd);
    if (rc)
        return bc_fail(e, "Falha ao verificar arquivo.");
    for (unsigned i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", hash[i]);
    return 0;
}

int bc_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i++];
        if (c < 0x80)
            continue;
        unsigned need;
        unsigned value, min;
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            value = c & 31u;
            min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            value = c & 15u;
            min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            value = c & 7u;
            min = 0x10000;
        } else
            return 0;
        if (i + need > n)
            return 0;
        while (need--) {
            unsigned char d = s[i++];
            if ((d & 0xc0u) != 0x80u)
                return 0;
            value = (value << 6) | (d & 63u);
        }
        if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return 0;
    }
    return 1;
}

json_object *bc_parse_json(const char *data, size_t size, bc_error *e) {
    if (size > 8 * 1024 * 1024 || memchr(data, 0, size) ||
        !bc_utf8((const unsigned char *)data, size)) {
        bc_fail(e, "JSON deve ser texto UTF-8 valido.");
        return NULL;
    }
    /* O modo estrito desta versao de json-c recusa alguns UTF-8 literais.
     * Escapes Unicode preservam os mesmos caracteres sem relaxar a gramatica. */
    char *ascii = malloc(size * 6 + 1);
    if (!ascii) {
        bc_fail(e, "Memoria insuficiente.");
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < size;) {
        unsigned c = (unsigned char)data[i++];
        if (c < 128) {
            ascii[used++] = (char)c;
            continue;
        }
        unsigned more;
        if (c < 0xe0) {
            more = 1;
            c &= 31;
        } else if (c < 0xf0) {
            more = 2;
            c &= 15;
        } else {
            more = 3;
            c &= 7;
        }
        while (more--)
            c = (c << 6) | ((unsigned char)data[i++] & 63u);
        if (c <= 0xffff)
            used += (size_t)sprintf(ascii + used, "\\u%04x", c);
        else {
            c -= 0x10000;
            used += (size_t)sprintf(ascii + used, "\\u%04x\\u%04x", 0xd800u + (c >> 10),
                                    0xdc00u + (c & 1023));
        }
    }
    ascii[used] = 0;
    struct json_tokener *tok = json_tokener_new_ex(64);
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    json_object *o = json_tokener_parse_ex(tok, ascii, (int)used);
    size_t end = json_tokener_get_parse_end(tok);
    while (end < used && isspace((unsigned char)ascii[end]))
        end++;
    if (json_tokener_get_error(tok) != json_tokener_success || end != used) {
        if (o)
            json_object_put(o);
        o = NULL;
        bc_fail(e, "JSON invalido: %s", json_tokener_error_desc(json_tokener_get_error(tok)));
    }
    json_tokener_free(tok);
    free(ascii);
    return o;
}
