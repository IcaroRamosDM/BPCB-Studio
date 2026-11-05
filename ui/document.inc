
struct DocumentViewer : Fl_Double_Window {
    std::string source, root;
    int page = 0, count = 1;
    bs_task *task = nullptr;
    Fl_PNG_Image *image = nullptr;
    Fl_Box *canvas, *status;
    Fl_Scroll *scroll;
    Action *previous, *next;

    DocumentViewer(const std::string &file, const std::string &title)
        : Fl_Double_Window(1060, 780, title.c_str()), source(file) {
        copy_label(title.c_str());
        color(pal.bg);
        size_range(800, 560);
        begin();
        previous = new Action(20, 16, 114, 36, "← Anterior", [this] {
            load(page - 1);
        });
        next = new Action(146, 16, 114, 36, "Próxima →", [this] {
            load(page + 1);
        });
        status = ui_label(280, 24, 560, 30, title, 14, pal.muted);
        new Action(922, 16, 118, 36, "Fechar", [this] {
            hide();
        });
        scroll = new Fl_Scroll(20, 70, 1020, 690);
        scroll->type(Fl_Scroll::BOTH);
        canvas = new Fl_Box(20, 70, 1000, 670);
        canvas->box(FL_FLAT_BOX);
        canvas->color(FL_WHITE);
        scroll->end();
        end();
        resizable(scroll);
        set_modal();
        load(0);
    }

    ~DocumentViewer() {
        if (task)
            retire_task(task, true);
        canvas->image(nullptr);
        delete image;
        bs_remove_temp(root.c_str());
    }

    void load(int n) {
        if (task || n < 0 || n >= count)
            return;
        previous->deactivate();
        next->deactivate();
        status->copy_label("Preparando a página…");
        task = bs_task_start("render", nullptr, source.c_str(), std::to_string(n).c_str(), "");
        if (task)
            watch_work(
                task, bs_task_fd(task),
                [](int, void *ctx) {
                    ((DocumentViewer *)ctx)->loaded();
                },
                this);
    }

    void loaded() {
        if (!task || !bs_task_done(task))
            return;
        unwatch_work(task, bs_task_fd(task));
        bc_error e = {{0}};
        json_object *r = bs_task_result(task, &e);
        bs_task_free(task);
        task = nullptr;
        if (!r) {
            status->copy_label(e.text);
            return;
        }
        canvas->image(nullptr);
        delete image;
        image = new Fl_PNG_Image(bc_str(r, "png"));
        bs_remove_temp(root.c_str());
        root = bc_str(r, "root");
        page = (int)bc_num(r, "page");
        count = (int)bc_num(r, "pages");
        scroll->scroll_to(0, 0);
        canvas->resize(scroll->x(), scroll->y(), image->w(), image->h());
        canvas->image(image);
        canvas->redraw();
        std::string text = "Página " + std::to_string(page + 1) + " de " + std::to_string(count);
        status->copy_label(text.c_str());
        if (page > 0)
            previous->activate();
        if (page + 1 < count)
            next->activate();
        json_object_put(r);
    }
};
