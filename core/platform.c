#include "studio.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int bs_temp_dir(char out[4096], const char *purpose, bc_error *e) {
    if (!bs_identifier(purpose))
        return bc_fail(e, "Nome temporário inválido.");
    if (snprintf(out, 4096, "%s/bpcb-studio-%s-XXXXXX", g_get_tmp_dir(), purpose) >= 4096)
        return bc_fail(e, "Caminho temporário longo demais.");
    if (!g_mkdtemp(out))
        return bc_fail(e, "Não foi possível criar a pasta temporária.");
    return 0;
}

int bs_executable_path(char out[4096], bc_error *e) {
#ifdef _WIN32
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(NULL, path, 32768);
    if (!n || n >= 32768)
        return bc_fail(e, "Localizar o executável Windows.");
    char *utf8 = g_utf16_to_utf8((gunichar2 *)path, -1, NULL, NULL, NULL);
    if (!utf8)
        return bc_fail(e, "Caminho do executável inválido.");
    int rc = snprintf(out, 4096, "%s", utf8);
    g_free(utf8);
    if (rc < 0 || rc >= 4096)
        return bc_fail(e, "Caminho do executável longo demais.");
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
#else
    ssize_t n = readlink("/proc/self/exe", out, 4095);
    if (n < 0 || n >= 4095)
        return bc_fail(e, "Localizar o executável Linux.");
    out[n] = 0;
#endif
    return 0;
}

int bs_worker_path(char out[4096], bc_error *e) {
    char executable[4096];
    if (bs_executable_path(executable, e) < 0)
        return -1;
    char *dir = g_path_get_dirname(executable);
    int rc = bc_path(out, 4096, dir, BS_WORKER_NAME, e);
    g_free(dir);
    return rc;
}

void bs_platform_init(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    char executable[4096];
    if (bs_executable_path(executable, NULL) == 0) {
        char *dir = g_path_get_dirname(executable);
        char *fonts = g_build_filename(dir, "..", "fonts", "fonts.conf", NULL);
        if (g_file_test(fonts, G_FILE_TEST_IS_REGULAR))
            g_setenv("FONTCONFIG_FILE", fonts, TRUE);
        const char *names[] = {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf"};
        for (unsigned i = 0; i < 2; i++) {
            char *file = g_build_filename(dir, "..", "fonts", names[i], NULL);
            gunichar2 *wide = g_utf8_to_utf16(file, -1, NULL, NULL, NULL);
            if (wide)
                AddFontResourceExW((wchar_t *)wide, FR_PRIVATE, NULL);
            g_free(wide);
            g_free(file);
        }
        g_free(fonts);
        g_free(dir);
    }
#endif
}
