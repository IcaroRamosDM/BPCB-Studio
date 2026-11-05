static std::string home_folder() {
    return g_get_home_dir();
}

// FLTK watches descriptors on Linux and sockets on Windows. On Windows,
// worker events call Fl::awake; checks drain only active work before waiting.
// There is no timer or idle redraw/polling loop.
#ifdef _WIN32
struct WorkWatch {
    const void *key;
    int fd;
    void (*ready)(int, void *);
    void *context;

    static void check(void *data) {
        auto *w = static_cast<WorkWatch *>(data);
        w->ready(w->fd, w->context);
    }
};

static std::vector<WorkWatch *> work_watches;

static void unwatch_work(const void *key, int) {
    for (auto i = work_watches.begin(); i != work_watches.end(); ++i) {
        if ((*i)->key != key)
            continue;
        WorkWatch *w = *i;
        work_watches.erase(i);
        Fl::remove_check(WorkWatch::check, w);
        delete w;
        return;
    }
}

static void watch_work(const void *key, int fd, void (*ready)(int, void *), void *context) {
    unwatch_work(key, fd);
    auto *w = new WorkWatch{key, fd, ready, context};
    work_watches.push_back(w);
    Fl::add_check(WorkWatch::check, w);
}
#else
static void watch_work(const void *, int fd, void (*ready)(int, void *), void *context) {
    Fl::add_fd(fd, FL_READ, ready, context);
}

static void unwatch_work(const void *, int fd) {
    Fl::remove_fd(fd);
}
#endif
