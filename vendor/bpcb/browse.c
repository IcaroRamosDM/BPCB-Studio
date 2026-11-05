#define _XOPEN_SOURCE 700
#include "bancada.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int entry_compare(const void *a, const void *b) {
    json_object *left = *(json_object *const *)a, *right = *(json_object *const *)b;
    int ld = json_object_get_boolean(bc_get(left, "directory"));
    int rd = json_object_get_boolean(bc_get(right, "directory"));
    return ld != rd ? rd - ld : strcasecmp(bc_str(left, "name"), bc_str(right, "name"));
}

static void add_place(json_object *places, const char *label, const char *path) {
    struct stat st;
    if (!path || stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
        return;
    json_object *item = json_object_new_object();
    json_object_object_add(item, "label", json_object_new_string(label));
    json_object_object_add(item, "path", json_object_new_string(path));
    json_object_array_add(places, item);
}

static json_object *known_places(void) {
    json_object *places = json_object_new_array();
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = g_get_home_dir();
#endif
    add_place(places, "Pasta pessoal", home);
    if (home) {
        const char *folders[] = {"Downloads", "Documentos", "Documents", "Área de Trabalho",
                                 "Desktop"};
        const char *labels[] = {"Downloads", "Documentos", "Documents", "Área de Trabalho",
                                "Desktop"};
        char path[4096];
        for (unsigned i = 0; i < sizeof folders / sizeof folders[0]; i++)
            if (!bc_path(path, sizeof path, home, folders[i], NULL))
                add_place(places, labels[i], path);
    }
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++)
        if (drives & (1u << i)) {
            char drive[] = "A:/";
            drive[0] = (char)('A' + i);
            add_place(places, drive, drive);
        }
#else
    add_place(places, "Arquivos do Linux", "/");
    add_place(places, "Windows (C:)", "/mnt/c");
#endif
    return places;
}

/* Executada em uma tarefa C: nao ha previsualizacao nem acesso a USB.
 * O cancelamento e consultado durante a leitura; nenhum dado parcial e publicado. */
json_object *bc_browse(const char *path, int (*cancelled)(void *), void *context, bc_error *e) {
    if (cancelled && cancelled(context))
        return NULL;
    char *absolute = realpath(path, NULL);
    if (!absolute) {
        bc_fail(e, "Abrir pasta: %s", strerror(errno));
        return NULL;
    }
    DIR *dir = opendir(absolute);
    if (!dir) {
        bc_fail(e, "Abrir pasta: %s", strerror(errno));
        free(absolute);
        return NULL;
    }
    json_object *result = json_object_new_object(), *entries = json_object_new_array();
    json_object_object_add(result, "path", json_object_new_string(absolute));
    json_object_object_add(result, "entries", entries);
    json_object_object_add(result, "locations", known_places());
    free(absolute);
    int failed = 0, stopped = 0, truncated = 0;
    for (;;) {
        if (cancelled && cancelled(context)) {
            stopped = 1;
            break;
        }
        errno = 0;
        struct dirent *item = readdir(dir);
        if (!item) {
            if (errno) {
                bc_fail(e, "Ler pasta: %s", strerror(errno));
                failed = 1;
            }
            break;
        }
        if (item->d_name[0] == '.')
            continue;
        if (!bc_utf8((const unsigned char *)item->d_name, strlen(item->d_name)))
            continue;
        struct stat st;
#ifdef _WIN32
        char full[4096];
        if (bc_path(full, sizeof full, bc_str(result, "path"), item->d_name, e) ||
            stat(full, &st) < 0)
            continue;
#else
        if (fstatat(dirfd(dir), item->d_name, &st, 0) < 0)
            continue;
#endif
        int directory = S_ISDIR(st.st_mode);
        const char *extension = strrchr(item->d_name, '.');
        if (!directory && (!S_ISREG(st.st_mode) || !extension || strcmp(extension, ".bancada")))
            continue;
        if (json_object_array_length(entries) >= 20000) {
            truncated = 1;
            break;
        }
        json_object *entry = json_object_new_object();
        json_object_object_add(entry, "name", json_object_new_string(item->d_name));
        json_object_object_add(entry, "directory", json_object_new_boolean(directory));
        json_object_array_add(entries, entry);
    }
    closedir(dir);
    if (failed || stopped) {
        json_object_put(result);
        return NULL;
    }
    json_object_array_sort(entries, entry_compare);
    json_object_object_add(result, "truncated", json_object_new_boolean(truncated));
    return result;
}
