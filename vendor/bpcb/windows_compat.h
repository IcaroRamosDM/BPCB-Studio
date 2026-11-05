#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <shellapi.h>
#include <windows.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#define LOCK_EX 2
#define LOCK_NB 4
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0170000) == 0120000)
#endif
struct bc_pollfd {
    int fd;
    short events, revents;
};

int bc_win_poll(struct bc_pollfd *p, unsigned long count, int timeout);
int bc_win_open(const char *path, int flags, ...);
int bc_win_stat(const char *path, struct stat *st);
int bc_win_lstat(const char *path, struct stat *st);
int bc_win_rename(const char *a, const char *b);
int bc_win_lock(int fd, int flags);
int bc_win_read(int fd, void *data, size_t size);
int bc_win_write(int fd, const void *data, size_t size);
int bc_win_mkstemp(char *name);
struct tm *bc_win_gmtime(const time_t *t, struct tm *out);
struct tm *bc_win_localtime(const time_t *t, struct tm *out);
char *bc_win_realpath(const char *path, char *resolved);

typedef struct {
    HANDLE process, cancel, parent;
    int diagnostic_fd;
} bc_win_child;

int bc_win_spawn(const char *exe, int *input, int *output, bc_win_child *child, char *error,
                 size_t cap);
void bc_win_stop(bc_win_child *child, int force);
int bc_win_exited(bc_win_child *child, int *status);
void bc_win_close_child(bc_win_child *child);
int bc_win_watch_parent(void (*stopped)(int));
const char *bc_bench_lock_path(void);
#ifndef BC_WINDOWS_COMPAT_IMPLEMENTATION
#define open bc_win_open
#define read bc_win_read
#define write bc_win_write
#define poll bc_win_poll
#define pollfd bc_pollfd
#define lstat bc_win_lstat
#define stat(path, st) bc_win_stat(path, st)
#define mkdir(path, mode) g_mkdir(path, mode)
#define unlink g_unlink
#define rmdir g_rmdir
#define chmod g_chmod
#define rename bc_win_rename
#define mkstemp bc_win_mkstemp
#define mkdtemp g_mkdtemp
#define realpath bc_win_realpath
#define flock bc_win_lock
#define fsync _commit
#define gmtime_r bc_win_gmtime
#define localtime_r bc_win_localtime
#define WIFEXITED(status) (1)
#define WEXITSTATUS(status) ((status) >> 8)
#endif
#else
#define bc_bench_lock_path() "/tmp/bpcb-bench.lock"
#endif
