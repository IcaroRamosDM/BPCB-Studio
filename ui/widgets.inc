
struct Palette {
    Fl_Color bg, card, ink, muted, line, accent, soft, error;
};

static Palette pal;
static bool dark = false;

static void colors() {
    pal = dark ? Palette{fl_rgb_color(25, 29, 34),    fl_rgb_color(34, 40, 47),
                         fl_rgb_color(231, 236, 241), fl_rgb_color(158, 173, 185),
                         fl_rgb_color(64, 76, 88),    fl_rgb_color(108, 211, 185),
                         fl_rgb_color(40, 69, 65),    fl_rgb_color(248, 142, 139)}
               : Palette{fl_rgb_color(243, 245, 248), FL_WHITE,
                         fl_rgb_color(30, 45, 61),    fl_rgb_color(99, 116, 132),
                         fl_rgb_color(210, 219, 228), fl_rgb_color(23, 111, 99),
                         fl_rgb_color(222, 240, 233), fl_rgb_color(174, 53, 57)};
    Fl::background(dark ? 25 : 243, dark ? 29 : 245, dark ? 34 : 248);
    Fl::background2(dark ? 34 : 255, dark ? 40 : 255, dark ? 47 : 255);
    Fl::foreground(dark ? 231 : 30, dark ? 236 : 45, dark ? 241 : 61);
    Fl::set_color(FL_SELECTION_COLOR, pal.soft);
}

struct Action : Fl_Button {
    std::function<void()> action;
    bool primary = false;

    Action(int x, int y, int w, int h, const char *text, std::function<void()> cb = {},
           bool strong = false)
        : Fl_Button(x, y, w, h), action(std::move(cb)), primary(strong) {
        copy_label(text);
        box(FL_FLAT_BOX);
        down_box(FL_FLAT_BOX);
        labelsize(14);
        callback([](Fl_Widget *w, void *) {
            auto f = static_cast<Action *>(w)->action;
            if (f)
                f();
        });
    }

    void draw() override {
        fl_color(primary ? (active_r() ? pal.accent : pal.line) : pal.card);
        fl_rectf(x(), y(), w(), h());
        fl_color(pal.line);
        fl_rect(x(), y(), w(), h());
        fl_color(active_r() ? (primary ? (dark ? pal.bg : FL_WHITE) : pal.ink) : pal.muted);
        fl_font(FL_HELVETICA, 14);
        fl_draw(label() ? label() : "", x() + 10, y(), w() - 20, h(),
                FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        if (Fl::focus() == this) {
            fl_color(pal.accent);
            fl_rect(x() + 2, y() + 2, w() - 4, h() - 4);
        }
    }
};

static Fl_Box *ui_label(int x, int y, int w, int h, const std::string &text, int size = 14,
                        Fl_Color color = 0) {
    auto *b = new Fl_Box(x, y, w, h);
    b->copy_label(text.c_str());
    b->labelsize(size);
    b->labelcolor(color ? color : pal.ink);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    return b;
}

static void field_style(Fl_Input_ *i) {
    i->box(FL_BORDER_BOX);
    i->color(pal.card);
    i->textcolor(pal.ink);
    i->textsize(14);
    i->selection_color(pal.soft);
}

static void choice_style(Fl_Choice *i) {
    i->box(FL_BORDER_BOX);
    i->color(pal.card);
    i->textcolor(pal.ink);
    i->textsize(14);
    i->labelcolor(pal.ink);
}

static std::vector<std::string> wrap(const std::string &s, int width, int size = 14) {
    std::vector<std::string> lines;
    std::string line;
    fl_font(FL_HELVETICA, size);
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 128 ? 1 : ((c & 0xe0) == 0xc0 ? 2 : ((c & 0xf0) == 0xe0 ? 3 : 4));
        n = std::min(n, s.size() - i);
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
            i++;
            continue;
        }
        std::string token = s.substr(i, n);
        if (!line.empty() && fl_width((line + token).c_str()) > width) {
            auto space = line.find_last_of(" ");
            if (space != std::string::npos && space > line.size() / 3) {
                lines.push_back(line.substr(0, space));
                line = line.substr(space + 1);
            } else {
                lines.push_back(line);
                line.clear();
            }
        }
        line += token;
        i += n;
    }
    lines.push_back(line);
    return lines;
}

struct Grid : Fl_Table_Row {
    std::vector<std::string> headings;
    std::vector<std::vector<std::string>> items;
    std::vector<double> fractions;
    std::function<void(int, bool)> selected;

    Grid(int x, int y, int w, int h) : Fl_Table_Row(x, y, w, h) {
        box(FL_BORDER_FRAME);
        table_box(FL_NO_BOX);
        color(pal.card);
        selection_color(pal.soft);
        row_header(0);
        col_header(1);
        col_header_height(34);
        row_resize(0);
        col_resize(0);
        type(SELECT_SINGLE);
        when(FL_WHEN_RELEASE);
        callback([](Fl_Widget *w, void *) {
            auto *g = static_cast<Grid *>(w);
            if (g->callback_context() != CONTEXT_CELL)
                return;
            int r = g->callback_row();
            if (r >= 0 && r < (int)g->items.size() && g->selected)
                g->selected(r, Fl::event_clicks() > 0);
        });
        end();
    }

    void sizes() {
        int total = std::max(100, w() - 24), used = 0;
        for (int c = 0; c < (int)headings.size(); c++) {
            int cw =
                c == (int)headings.size() - 1 ? total - used : (int)(total * fractions[(size_t)c]);
            col_width(c, std::max(50, cw));
            used += cw;
        }
        for (int r = 0; r < (int)items.size(); r++) {
            int h = 38;
            for (int c = 0; c < (int)items[(size_t)r].size(); c++)
                h = std::max(
                    h, 14 + (int)wrap(items[(size_t)r][(size_t)c], std::max(30, col_width(c) - 24))
                                    .size() *
                                18);
            row_height(r, h);
        }
    }

    void data(std::vector<std::string> heads, std::vector<std::vector<std::string>> rows_,
              std::vector<double> widths = {}) {
        headings = std::move(heads);
        items = std::move(rows_);
        fractions = std::move(widths);
        if (fractions.size() != headings.size())
            fractions.assign(headings.size(), 1.0 / (double)std::max((size_t)1, headings.size()));
        cols((int)headings.size());
        rows((int)items.size());
        select_all_rows(0);
        sizes();
        redraw();
    }

    void resize(int x, int y, int w, int h) override {
        Fl_Table_Row::resize(x, y, w, h);
        sizes();
    }

    void rect(int row, int &x, int &y, int &w, int &h) {
        find_cell(CONTEXT_CELL, row, 0, x, y, w, h);
        w = this->w() - 24;
    }

    void head_rect(int &x, int &y, int &w, int &h) {
        find_cell(CONTEXT_COL_HEADER, 0, 0, x, y, w, h);
        w = this->w() - 24;
    }

    void draw_cell(TableContext ctx, int r, int c, int x, int y, int w, int h) override {
        if (ctx == CONTEXT_STARTPAGE) {
            fl_font(FL_HELVETICA, 14);
            return;
        }
        if (ctx != CONTEXT_CELL && ctx != CONTEXT_COL_HEADER)
            return;
        fl_push_clip(x, y, w, h);
        bool head = ctx == CONTEXT_COL_HEADER;
        fl_color(head ? pal.bg : (row_selected(r) ? pal.soft : pal.card));
        fl_rectf(x, y, w, h);
        fl_color(pal.line);
        fl_line(x, y + h - 1, x + w, y + h - 1);
        fl_color(head ? pal.muted : pal.ink);
        fl_font(FL_HELVETICA, head ? 12 : 14);
        std::string text;
        if (c >= 0 && c < (int)headings.size()) {
            if (head)
                text = headings[(size_t)c];
            else if (r >= 0 && r < (int)items.size() && c < (int)items[(size_t)r].size())
                text = items[(size_t)r][(size_t)c];
        }
        auto lines = wrap(text, w - 24, head ? 12 : 14);
        int yy = y + 23;
        for (auto &l : lines) {
            fl_draw(l.c_str(), x + 12, yy);
            yy += 18;
        }
        fl_pop_clip();
    }
};

struct TextPanel : Fl_Text_Display {
    Fl_Text_Buffer buffer;

    TextPanel(int x, int y, int w, int h, const std::string &s = "") : Fl_Text_Display(x, y, w, h) {
        Fl_Text_Display::buffer(&buffer);
        buffer.text(s.c_str());
        box(FL_BORDER_BOX);
        color(pal.card);
        textcolor(pal.ink);
        textfont(FL_HELVETICA);
        textsize(14);
        wrap_mode(WRAP_AT_BOUNDS, 0);
    }

    ~TextPanel() {
        Fl_Text_Display::buffer(nullptr);
    }

    void text(const std::string &s) {
        buffer.text(s.c_str());
    }
};

static void message(const std::string &title, const std::string &text) {
    Fl_Double_Window win(700, 450, title.c_str());
    win.color(pal.bg);
    ui_label(24, 22, 652, 42, title, 23);
    auto *display = new TextPanel(24, 76, 652, 298, text);
    new Action(536, 394, 140, 36, "Fechar", [&] {
        win.hide();
    });
    win.end();
    win.resizable(display);
    win.size_range(580, 350);
    win.set_modal();
    win.show();
    while (win.shown())
        Fl::wait();
}

static bool question(const std::string &title, const std::string &text) {
    bool yes = false;
    Fl_Double_Window win(640, 340, title.c_str());
    win.color(pal.bg);
    ui_label(24, 22, 592, 44, title, 22);
    auto *body = new TextPanel(24, 78, 592, 194, text);
    new Action(330, 288, 130, 34, "Cancelar", [&] {
        win.hide();
    });
    new Action(
        476, 288, 140, 34, "Continuar",
        [&] {
            yes = true;
            win.hide();
        },
        true);
    win.end();
    win.resizable(body);
    win.size_range(600, 320);
    win.set_modal();
    win.show();
    while (win.shown())
        Fl::wait();
    return yes;
}

static std::string parent_path(std::string p) {
    char *absolute = g_canonicalize_filename(p.c_str(), nullptr),
         *dir = g_path_get_dirname(absolute);
    std::string result = dir;
    g_free(dir);
    g_free(absolute);
    return result;
}

static std::string basename_path(const std::string &p) {
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

static void retire_task(bs_task *task, bool remove_preview = false) {
    if (!task)
        return;
    unwatch_work(task, bs_task_fd(task));

    struct Retirement {
        bs_task *task;
        bool remove;
    };

    auto cleanup = [](int, void *ctx) {
        auto *r = static_cast<Retirement *>(ctx);
        if (!bs_task_done(r->task))
            return;
        unwatch_work(r->task, bs_task_fd(r->task));
        bc_error ignored = {{0}};
        json_object *result = bs_task_result(r->task, &ignored);
        if (r->remove)
            bs_remove_temp(bc_str(result, "root"));
        json_object_put(result);
        bs_task_free(r->task);
        delete r;
    };
    auto *r = new Retirement{task, remove_preview};
    if (bs_task_done(task))
        cleanup(0, r);
    else
        watch_work(task, bs_task_fd(task), cleanup, r);
}
