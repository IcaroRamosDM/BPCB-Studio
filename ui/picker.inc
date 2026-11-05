
struct FilePicker : Fl_Double_Window {
    Fl_Input *address, *filename;
    Grid *list;
    Fl_Box *status;
    Action *choose, *up, *home, *go;
    bs_task *task = nullptr;
    std::string directory, filter, answer;
    bool save;
    json_object *listing = nullptr;

    FilePicker(const char *title, const std::string &start, const std::string &filter_, bool save_)
        : Fl_Double_Window(900, 640, title), filter(filter_), save(save_) {
        copy_label(title);
        color(pal.bg);
        size_range(760, 540);
        begin();
        ui_label(24, 20, 820, 32, title, 23);
        up = new Action(24, 70, 96, 36, "↑ Acima", [this] {
            load(parent_path(directory));
        });
        home = new Action(132, 70, 120, 36, "Pasta pessoal", [this] {
            load(home_folder());
        });
        address = new Fl_Input(268, 70, 536, 36);
        field_style(address);
        address->when(FL_WHEN_ENTER_KEY_ALWAYS);
        address->callback(
            [](Fl_Widget *, void *v) {
                auto *p = (FilePicker *)v;
                p->load(p->address->value());
            },
            this);
        go = new Action(816, 70, 60, 36, "Ir", [this] {
            load(address->value());
        });
        list = new Grid(24, 122, 852, 368);
        list->selected = [this](int i, bool twice) {
            if (!listing)
                return;
            json_object *a = bc_get(listing, "entries");
            if (i < 0 || (size_t)i >= json_object_array_length(a))
                return;
            json_object *entry = json_object_array_get_idx(a, (size_t)i);
            std::string name = bc_str(entry, "name");
            if (json_object_get_boolean(bc_get(entry, "directory"))) {
                if (twice)
                    load(directory + "/" + name);
                else
                    filename->value("");
            } else {
                filename->value(name.c_str());
                if (twice)
                    accept();
            }
        };
        status = ui_label(24, 502, 852, 32, "", 13, pal.muted);
        ui_label(24, 544, 82, 36, "Arquivo", 13, pal.muted);
        filename = new Fl_Input(108, 538, 768, 36);
        field_style(filename);
        filename->when(FL_WHEN_ENTER_KEY_ALWAYS);
        filename->callback(
            [](Fl_Widget *, void *v) {
                ((FilePicker *)v)->accept();
            },
            this);
        new Action(604, 594, 126, 34, "Cancelar", [this] {
            hide();
        });
        choose = new Action(
            744, 594, 132, 34, save ? "Salvar aqui" : "Selecionar",
            [this] {
                accept();
            },
            true);
        end();
        resizable(list);
        set_modal();
        load(start.empty() ? home_folder() : start);
    }

    ~FilePicker() {
        if (task)
            retire_task(task);
        json_object_put(listing);
    }

    void load(const std::string &dir) {
        if (task)
            return;
        status->copy_label("Lendo a pasta…");
        choose->deactivate();
        list->deactivate();
        address->deactivate();
        up->deactivate();
        home->deactivate();
        go->deactivate();
        task = bs_task_start("browse", nullptr, dir.c_str(), filter.c_str(), "");
        if (!task) {
            status->copy_label("Não foi possível ler a pasta.");
            return;
        }
        watch_work(
            task, bs_task_fd(task),
            [](int, void *ctx) {
                ((FilePicker *)ctx)->loaded();
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
        choose->activate();
        list->activate();
        address->activate();
        up->activate();
        home->activate();
        go->activate();
        if (!r) {
            status->copy_label(e.text);
            return;
        }
        json_object_put(listing);
        listing = r;
        directory = bc_str(r, "path");
        address->value(directory.c_str());
        std::vector<std::vector<std::string>> rows;
        json_object *a = bc_get(r, "entries");
        for (size_t i = 0; i < json_object_array_length(a); i++) {
            json_object *v = json_object_array_get_idx(a, i);
            rows.push_back({bc_str(v, "name"),
                            json_object_get_boolean(bc_get(v, "directory")) ? "Pasta" : "Arquivo"});
        }
        list->data({"NOME", "TIPO"}, rows, {.80, .20});
        status->copy_label(
            rows.empty() ? "Nenhum arquivo deste tipo. Use a barra de endereço ou abra outra pasta."
                         : "Duplo clique abre uma pasta ou seleciona um arquivo.");
    }

    void accept() {
        if (task)
            return;
        std::string name = filename->value();
        if (name.empty()) {
            status->copy_label("Selecione um arquivo ou digite o nome.");
            return;
        }
        std::string candidate = name[0] == '/' ? name : directory + "/" + name;
        std::string ext = filter == "project" ? ".bpcb" : filter == "package" ? ".bancada" : "";
        if (save && !ext.empty() &&
            (candidate.size() < ext.size() ||
             candidate.substr(candidate.size() - ext.size()) != ext))
            candidate += ext;
        if (save && access(candidate.c_str(), F_OK) == 0 &&
            !question("Substituir arquivo?", "Já existe um arquivo neste caminho:\n" + candidate +
                                                 "\n\nContinuar irá substituí-lo."))
            return;
        answer = candidate;
        hide();
    }

    static std::string pick(const char *title, const std::string &start, const char *filter,
                            bool save = false) {
#ifdef _WIN32
        Fl_Native_File_Chooser chooser;
        chooser.title(title);
        chooser.directory(start.c_str());
        chooser.type(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE
                          : Fl_Native_File_Chooser::BROWSE_FILE);
        chooser.filter(!strcmp(filter, "project")   ? "Projeto BPCB Studio\t*.bpcb"
                       : !strcmp(filter, "package") ? "Bancada\t*.bancada"
                                                    : "Documentos\t*.{pdf,svg}");
        if (save)
            chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM |
                            Fl_Native_File_Chooser::USE_FILTER_EXT);
        if (chooser.show() != 0 || !chooser.filename())
            return {};
        return chooser.filename();
#else
        FilePicker p(title, start, filter, save);
        p.show();
        while (p.shown())
            Fl::wait();
        return p.answer;
#endif
    }
};
