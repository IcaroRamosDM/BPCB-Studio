#define BC_WINDOWS_COMPAT_IMPLEMENTATION
#include "windows_compat.h"
#ifdef _WIN32
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <wchar.h>

static wchar_t *wide(const char *s) {
    return (wchar_t *)g_utf8_to_utf16(s, -1, NULL, NULL, NULL);
}

int bc_win_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    wchar_t *p = wide(path);
    if (!p) {
        errno = EINVAL;
        return -1;
    }
    DWORD attr = GetFileAttributesW(p);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        g_free(p);
        errno = ELOOP;
        return -1;
    }
    int fd = _wopen(p, flags | _O_BINARY | _O_NOINHERIT, mode);
    g_free(p);
    return fd;
}

int bc_win_stat(const char *path, struct stat *st) {
    GStatBuf value;
    int rc = g_stat(path, &value);
    if (rc)
        return rc;
    if (value.st_size > INT_MAX) {
        errno = EFBIG;
        return -1;
    }
    memset(st, 0, sizeof *st);
    st->st_mode = (mode_t)value.st_mode;
    st->st_size = (off_t)value.st_size;
    st->st_mtime = (time_t)value.st_mtime;
    return 0;
}

int bc_win_lstat(const char *path, struct stat *st) {
    int rc = bc_win_stat(path, st);
    wchar_t *p = wide(path);
    if (!p)
        return -1;
    DWORD a = GetFileAttributesW(p);
    g_free(p);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        memset(st, 0, sizeof *st);
        st->st_mode = 0120000;
        return 0;
    }
    return rc;
}

int bc_win_rename(const char *a, const char *b) {
    wchar_t *x = wide(a), *y = wide(b);
    int ok = x && y && MoveFileExW(x, y, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    g_free(x);
    g_free(y);
    if (!ok)
        errno = EACCES;
    return ok ? 0 : -1;
}

int bc_win_lock(int fd, int flags) {
    OVERLAPPED overlap = {0};
    DWORD mode = LOCKFILE_EXCLUSIVE_LOCK;
    if (flags & LOCK_NB)
        mode |= LOCKFILE_FAIL_IMMEDIATELY;
    if (!LockFileEx((HANDLE)_get_osfhandle(fd), mode, 0, 1, 0, &overlap)) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int bc_win_read(int fd, void *data, size_t size) {
    if (size > INT_MAX)
        size = INT_MAX;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (GetFileType(h) == FILE_TYPE_PIPE) {
        DWORD bytes = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &bytes, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE)
                return 0;
            errno = EIO;
            return -1;
        }
        if (!bytes) {
            errno = EAGAIN;
            return -1;
        }
        if (size > bytes)
            size = bytes;
    }
    return _read(fd, data, (unsigned int)size);
}

int bc_win_write(int fd, const void *data, size_t size) {
    return _write(fd, data, (unsigned int)(size > INT_MAX ? INT_MAX : size));
}

int bc_win_poll(struct bc_pollfd *p, unsigned long count, int timeout) {
    ULONGLONG until = GetTickCount64() + (unsigned)(timeout < 0 ? INT_MAX : timeout);
    do {
        int ready = 0;
        for (unsigned long i = 0; i < count; i++) {
            p[i].revents = 0;
            HANDLE h = (HANDLE)_get_osfhandle(p[i].fd);
            DWORD bytes = 0;
            if (p[i].events & POLLOUT)
                p[i].revents |= POLLOUT;
            if (p[i].events & POLLIN) {
                if (GetFileType(h) != FILE_TYPE_PIPE ||
                    PeekNamedPipe(h, NULL, 0, NULL, &bytes, NULL)) {
                    if (bytes || GetFileType(h) != FILE_TYPE_PIPE)
                        p[i].revents |= POLLIN;
                } else
                    p[i].revents |= POLLIN | POLLHUP;
            }
            if (p[i].revents)
                ready++;
        }
        if (ready)
            return ready;
        if (GetTickCount64() >= until)
            break;
        Sleep(5);
    } while (1);
    return 0;
}

int bc_win_mkstemp(char *name) {
    return g_mkstemp_full(name, _O_RDWR | _O_BINARY | _O_NOINHERIT, 0600);
}

struct tm *bc_win_gmtime(const time_t *t, struct tm *out) {
    return gmtime_s(out, t) ? NULL : out;
}

struct tm *bc_win_localtime(const time_t *t, struct tm *out) {
    return localtime_s(out, t) ? NULL : out;
}

char *bc_win_realpath(const char *path, char *resolved) {
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        errno = ENOENT;
        return NULL;
    }
    gchar *p = g_canonicalize_filename(path, NULL);
    for (char *c = p; *c; c++)
        if (*c == '\\')
            *c = '/';
    char *result = resolved ? strcpy(resolved, p) : strdup(p);
    g_free(p);
    return result;
}

const char *bc_bench_lock_path(void) {
    static char path[4096];
    if (!*path)
        snprintf(path, sizeof path, "%s/bpcb-bench.lock", g_get_tmp_dir());
    return path;
}

int bc_win_spawn(const char *exe, int *input, int *output, bc_win_child *child, char *error,
                 size_t cap) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd = NULL, wr = NULL, out = NULL, in = NULL, err_read = NULL, err_write = NULL;
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOEXW si = {0};
    SIZE_T bytes = 0;
    wchar_t *program = wide(exe), *command = NULL;
    int rc = -1, attributes_ready = 0;
    child->diagnostic_fd = -1;
    child->cancel = CreateEventW(&sa, TRUE, FALSE, NULL);
    if (!program || !child->cancel ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                         &child->parent, SYNCHRONIZE, TRUE, 0) ||
        !CreatePipe(&rd, &in, &sa, 65536) || !CreatePipe(&out, &wr, &sa, 65536) ||
        !CreatePipe(&err_read, &err_write, &sa, 65536))
        goto done;
    SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = rd;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = err_write;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    si.lpAttributeList = malloc(bytes);
    if (!si.lpAttributeList || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes))
        goto done;
    attributes_ready = 1;
    HANDLE handles[] = {rd, wr, err_write, child->cancel, child->parent};
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles, sizeof handles, NULL, NULL))
        goto done;
    command = calloc(wcslen(program) + 160, sizeof *command);
    if (!command)
        goto done;
    swprintf(command, wcslen(program) + 160, L"\"%ls\" --bpcb-stop %llu --bpcb-parent %llu",
             program, (unsigned long long)(uintptr_t)child->cancel,
             (unsigned long long)(uintptr_t)child->parent);
    if (!CreateProcessW(program, command, NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL,
                        &si.StartupInfo, &pi))
        goto done;
    child->process = pi.hProcess;
    CloseHandle(pi.hThread);
    *input = _open_osfhandle((intptr_t)in, _O_BINARY | _O_WRONLY | _O_NOINHERIT);
    if (*input >= 0)
        in = NULL;
    *output = _open_osfhandle((intptr_t)out, _O_BINARY | _O_RDONLY | _O_NOINHERIT);
    if (*output >= 0)
        out = NULL;
    child->diagnostic_fd =
        _open_osfhandle((intptr_t)err_read, _O_BINARY | _O_RDONLY | _O_NOINHERIT);
    if (child->diagnostic_fd >= 0)
        err_read = NULL;
    if (*input < 0 || *output < 0 || child->diagnostic_fd < 0)
        goto done;
    rc = 0;
done:
    if (rc)
        snprintf(error, cap, "Não foi possível iniciar o executor Windows (erro %lu).",
                 GetLastError());
    if (si.lpAttributeList) {
        if (attributes_ready)
            DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
    }
    g_free(program);
    free(command);
    if (rd)
        CloseHandle(rd);
    if (wr)
        CloseHandle(wr);
    if (in)
        CloseHandle(in);
    if (out)
        CloseHandle(out);
    if (err_read)
        CloseHandle(err_read);
    if (err_write)
        CloseHandle(err_write);
    return rc;
}

void bc_win_stop(bc_win_child *child, int force) {
    if (child->cancel)
        SetEvent(child->cancel);
    if (force && child->process)
        TerminateProcess(child->process, 1);
}

int bc_win_exited(bc_win_child *child, int *status) {
    if (!child->process || WaitForSingleObject(child->process, 0) != WAIT_OBJECT_0)
        return 0;
    DWORD code = 1;
    GetExitCodeProcess(child->process, &code);
    *status = (int)(code ? 256 : 0);
    return 1;
}

void bc_win_close_child(bc_win_child *child) {
    if (child->process) {
        if (WaitForSingleObject(child->process, 0) != WAIT_OBJECT_0) {
            bc_win_stop(child, 0);
            if (WaitForSingleObject(child->process, 8000) != WAIT_OBJECT_0)
                bc_win_stop(child, 1);
        }
        CloseHandle(child->process);
    }
    if (child->cancel)
        CloseHandle(child->cancel);
    if (child->parent)
        CloseHandle(child->parent);
    if (child->diagnostic_fd >= 0)
        _close(child->diagnostic_fd);
}

struct watcher {
    HANDLE handles[2];
    void (*stop)(int);
};

static DWORD WINAPI monitor_parent(void *p) {
    struct watcher *w = p;
    WaitForMultipleObjects(2, w->handles, FALSE, INFINITE);
    w->stop(0);
    CloseHandle(w->handles[0]);
    CloseHandle(w->handles[1]);
    free(w);
    return 0;
}

int bc_win_watch_parent(void (*stopped)(int)) {
    int n = 0;
    wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &n);
    struct watcher *w = calloc(1, sizeof *w);
    if (!args || !w) {
        if (args)
            LocalFree(args);
        free(w);
        return -1;
    }
    w->stop = stopped;
    for (int i = 1; i + 1 < n; i++) {
        if (!wcscmp(args[i], L"--bpcb-stop"))
            w->handles[0] = (HANDLE)(uintptr_t)_wcstoui64(args[i + 1], NULL, 10);
        if (!wcscmp(args[i], L"--bpcb-parent"))
            w->handles[1] = (HANDLE)(uintptr_t)_wcstoui64(args[i + 1], NULL, 10);
    }
    LocalFree(args);
    if (!w->handles[0] || !w->handles[1]) {
        free(w);
        return -1;
    }
    HANDLE thread = CreateThread(NULL, 0, monitor_parent, w, 0, NULL);
    if (!thread) {
        free(w);
        return -1;
    }
    CloseHandle(thread);
    return 0;
}
#endif
