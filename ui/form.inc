static void form_menu_add(Fl_Choice *menu, const char *text) {
    int index = menu->add("", 0, nullptr);
    menu->replace(index, text);
}

struct EditorField {
    json_object *definition = nullptr;
    std::string key, type;
    Fl_Box *caption = nullptr, *help = nullptr;
    Fl_Input *input = nullptr;
    Fl_Group *mask = nullptr;
    std::vector<Fl_Check_Button *> bits;
    Fl_Group *aux = nullptr;
    Fl_Hold_Browser *rows_view = nullptr;
    Fl_Box *summary = nullptr;
    json_object *rows = nullptr;
    std::vector<Fl_Widget *> controls;
    Fl_Choice *choice = nullptr;
    Fl_Text_Editor *editor = nullptr;
    Fl_Text_Buffer *buffer = nullptr;
    std::vector<std::string> values;
    int top = 0, height = 0;
};

struct Form : Fl_Double_Window {
    std::string kind;
    json_object *project, *catalog, *initial, *result = nullptr;
    Fl_Scroll *scroll;
    Fl_Group *content;
    Fl_Box *error;
    std::vector<EditorField> fields;
    std::string prior_lab_device;
    Action *cancel, *apply;

    Form(const std::string &kind_, json_object *p, json_object *values, const std::string &title)
        : Fl_Double_Window(880, 760, title.c_str()), kind(kind_), project(p) {
        copy_label(title.c_str());
        color(pal.bg);
        size_range(780, 600);
        catalog = bs_catalog(kind.c_str());
        initial = values ? bc_clone(values) : bs_defaults(kind.c_str());
        prior_lab_device = bc_str(initial, "lab_device");
        begin();
        ui_label(24, 20, 832, 36, title, 24);
        ui_label(
            24, 60, 832, 38,
            "Preencha os dados abaixo. As explicações acompanham cada campo; salve para aplicar.",
            13, pal.muted);
        scroll = new Fl_Scroll(20, 108, 840, 548);
        scroll->type(Fl_Scroll::VERTICAL);
        scroll->box(FL_FLAT_BOX);
        scroll->color(pal.bg);
        content = new Fl_Group(20, 108, 812, 548);
        content->resizable(nullptr);
        fields.reserve(json_object_array_length(catalog));
        for (size_t i = 0; i < json_object_array_length(catalog); i++) {
            json_object *def = json_object_array_get_idx(catalog, i);
            EditorField f;
            f.definition = def;
            f.key = bc_str(def, "key");
            f.type = bc_str(def, "type");
            f.caption = ui_label(0, 0, 740, 22, bc_str(def, "label"), 14);
            f.help = ui_label(0, 0, 740, 44, bc_str(def, "help"), 12, pal.muted);
            if (f.type == "expression" || f.key == "reference") {
                std::string hint = bc_str(def, "help");
                hint += "\nNomes disponíveis: ";
                json_object *arrays[] = {bc_get(p, "components"),
                                         bc_get(bc_get(p, "editing_test"), "parameters"),
                                         bc_get(bc_get(p, "editing_test"), "formulas"),
                                         bc_get(bc_get(p, "editing_test"), "steps")};
                bool first = true;
                for (unsigned ai = 0; ai < 4; ai++)
                    for (size_t j = 0; arrays[ai] && j < json_object_array_length(arrays[ai]);
                         j++) {
                        json_object *o = json_object_array_get_idx(arrays[ai], j);
                        const char *name = bc_str(o, ai == 0 ? "ref" : ai == 3 ? "variable" : "id");
                        if (ai == 3) {
                            std::string t = bc_str(o, "type");
                            if (t != "read" && t != "wait" && t != "manual" && t != "calculate" &&
                                t != "communication")
                                continue;
                        }
                        if (*name) {
                            if (!first)
                                hint += ", ";
                            hint += name;
                            first = false;
                        }
                    }
                if (first)
                    hint += "cadastre componentes, parâmetros ou medições nas etapas.";
                f.help->copy_label(hint.c_str());
            }
            std::string value;
            json_object *v = bc_get(initial, f.key.c_str());
            if (v) {
                if (json_object_is_type(v, json_type_string))
                    value = json_object_get_string(v);
                else if (json_object_is_type(v, json_type_double)) {
                    char number[128];
                    auto formatted =
                        std::to_chars(number, number + sizeof number, json_object_get_double(v));
                    value = formatted.ec == std::errc{}
                                ? std::string(number, formatted.ptr)
                                : json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN);
                } else
                    value = json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN);
            }
            if (f.type == "channels" || f.type == "wiring" || f.type == "frame" ||
                f.type == "replay") {
                f.rows = v ? bc_clone(v) : nullptr;
                if (f.type == "frame" && !f.rows)
                    f.rows = bc_frame_example();
                if ((f.type == "wiring" || f.type == "channels") &&
                    !json_object_is_type(f.rows, json_type_array)) {
                    json_object_put(f.rows);
                    f.rows = json_object_new_array();
                }
                if ((f.type == "channels" || f.type == "wiring") && !f.rows)
                    f.rows = json_object_new_array();
                f.aux = new Fl_Group(0, 0, 740,
                                     (f.type == "channels" || f.type == "wiring") ? 180 : 80);
                f.aux->resizable(nullptr);
                size_t index = fields.size();
                if (f.type == "channels" || f.type == "wiring") {
                    f.rows_view = new Fl_Hold_Browser(0, 0, 740, 122);
                    f.rows_view->color(pal.card);
                    f.rows_view->textcolor(pal.ink);
                    f.rows_view->textsize(14);
                    f.controls.push_back(new Action(0, 0, 130, 34, "Adicionar", [this, index] {
                        edit_channels(index, 0);
                    }));
                    f.controls.push_back(new Action(0, 0, 130, 34, "Editar", [this, index] {
                        edit_channels(index, 1);
                    }));
                    f.controls.push_back(new Action(0, 0, 130, 34, "Remover", [this, index] {
                        edit_channels(index, 2);
                    }));
                } else if (f.type == "frame") {
                    f.summary = ui_label(0, 0, 740, 30, "Formato binário", 13, pal.muted);
                    f.controls.push_back(
                        new Action(0, 0, 280, 34, "Editar formato e testar prévia…", [this, index] {
                            auto *r = WorkbenchFrameEditor::edit(fields[index].rows);
                            if (r) {
                                json_object_put(fields[index].rows);
                                fields[index].rows = r;
                                arrange();
                            }
                        }));
                } else {
                    f.summary =
                        ui_label(0, 0, 740, 30, "Sem captura — simulação usa o exemplo declarado.",
                                 13, pal.muted);
                    f.controls.push_back(
                        new Action(0, 0, 190, 34, "Importar captura…", [this, index] {
                            load_capture(index);
                        }));
                    f.controls.push_back(new Action(0, 0, 130, 34, "Remover", [this, index] {
                        json_object_put(fields[index].rows);
                        fields[index].rows = nullptr;
                        arrange();
                    }));
                }
                f.aux->end();
            } else if (f.type == "pinmask") {
                f.mask = new Fl_Group(0, 0, 740, 70);
                f.mask->resizable(nullptr);
                unsigned mask = (unsigned)bc_num(initial, f.key.c_str());
                for (unsigned bit = 0; bit < 8; bit++) {
                    auto *b = new Fl_Check_Button(0, 0, 140, 30);
                    std::string label = "P" + std::to_string(bit);
                    b->copy_label(label.c_str());
                    b->labelcolor(pal.ink);
                    b->value((int)((mask >> bit) & 1u));
                    f.bits.push_back(b);
                }
                f.mask->end();
            } else if (f.type == "choice" || f.type == "boolean" || f.type == "device" ||
                       f.type == "signal" || f.type == "target" || f.type == "channel") {
                f.choice = new Fl_Choice(0, 0, 740, 36);
                choice_style(f.choice);
                if (f.type == "pinmask") {
                    f.mask = new Fl_Group(0, 0, 740, 70);
                    f.mask->resizable(nullptr);
                    unsigned mask = (unsigned)bc_num(initial, f.key.c_str());
                    for (unsigned bit = 0; bit < 8; bit++) {
                        auto *b = new Fl_Check_Button(0, 0, 140, 30);
                        std::string label = "P" + std::to_string(bit);
                        b->copy_label(label.c_str());
                        b->labelcolor(pal.ink);
                        b->value((int)((mask >> bit) & 1u));
                        f.bits.push_back(b);
                    }
                    f.mask->end();
                } else if (f.type == "choice") {
                    json_object *options = bc_get(def, "options");
                    for (size_t j = 0; j < json_object_array_length(options); j++) {
                        json_object *o = json_object_array_get_idx(options, j);
                        form_menu_add(f.choice, bc_str(o, "label"));
                        f.values.emplace_back(bc_str(o, "id"));
                    }
                } else if (f.type == "boolean") {
                    f.choice->add("Não");
                    f.choice->add("Sim");
                    f.values = {"false", "true"};
                    if (value == "1")
                        value = "true";
                    if (value == "0")
                        value = "false";
                } else {
                    if (f.type == "channel") {
                        auto *test = bc_get(p, "editing_test"),
                             *rows = bc_get(test, "comm_channels");
                        if (!strcmp(bc_str(test, "comm_mode"), "multi"))
                            for (size_t j = 0; j < json_object_array_length(rows); j++) {
                                auto *row = json_object_array_get_idx(rows, j);
                                form_menu_add(f.choice, (std::string(bc_str(row, "id")) + " · " +
                                                         bc_str(row, "comm_bus"))
                                                            .c_str());
                                f.values.emplace_back(bc_str(row, "id"));
                            }
                        else {
                            form_menu_add(f.choice, "main · canal único");
                            f.values.emplace_back("main");
                        }
                    } else {
                        f.choice->add("Selecione…");
                        f.values.emplace_back("");
                    }
                    json_object *a = f.type == "device"   ? bc_get(p, "devices")
                                     : f.type == "signal" ? bc_get(p, "signals")
                                                          : nullptr;
                    if (f.type == "target")
                        a = bc_get(bc_get(p, "editing_test"), "steps");
                    for (size_t j = 0; a && j < json_object_array_length(a); j++) {
                        json_object *o = json_object_array_get_idx(a, j);
                        std::string id = bc_str(o, "id"),
                                    name = bc_str(o, f.type == "device"   ? "name"
                                                     : f.type == "signal" ? "label"
                                                                          : "title");
                        form_menu_add(f.choice, (id + " · " + name).c_str());
                        f.values.push_back(id);
                    }
                }
                auto it = std::find(f.values.begin(), f.values.end(), value);
                f.choice->value(it == f.values.end() ? 0
                                                     : (int)std::distance(f.values.begin(), it));
                f.choice->callback(
                    [](Fl_Widget *, void *ctx) {
                        ((Form *)ctx)->arrange();
                    },
                    this);
            } else if (f.type == "multiline") {
                f.buffer = new Fl_Text_Buffer();
                f.buffer->text(value.c_str());
                f.editor = new Fl_Text_Editor(0, 0, 740, 100);
                f.editor->buffer(f.buffer);
                f.editor->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
                f.editor->box(FL_BORDER_BOX);
                f.editor->color(pal.card);
                f.editor->textcolor(pal.ink);
                f.editor->textfont(FL_HELVETICA);
                f.editor->textsize(14);
            } else {
                f.input = new Fl_Input(0, 0, 740, 36);
                field_style(f.input);
                f.input->value(value.c_str());
                f.input->when(FL_WHEN_ENTER_KEY_ALWAYS);
                f.input->callback(
                    [](Fl_Widget *, void *ctx) {
                        ((Form *)ctx)->accept();
                    },
                    this);
            }
            fields.push_back(std::move(f));
        }
        content->end();
        scroll->end();
        error = ui_label(24, 670, 832, 30, "", 13, pal.error);
        cancel = new Action(562, 710, 136, 36, "Cancelar", [this] {
            hide();
        });
        apply = new Action(
            714, 710, 142, 36, "Salvar",
            [this] {
                accept();
            },
            true);
        end();
        resizable(scroll);
        set_modal();
        arrange();
    }

    ~Form() {
        for (auto &f : fields) {
            json_object_put(f.rows);
            if (f.buffer) {
                f.editor->buffer(nullptr);
                delete f.buffer;
            }
        }
        json_object_put(catalog);
        json_object_put(initial);
        json_object_put(result);
    }

    json_object *raw() {
        json_object *r = bc_clone(initial);
        for (auto &f : fields) {
            if (f.aux) {
                if (f.type == "frame" && strcmp(bc_str(r, "lab_protocol"), "frame") &&
                    !bc_get(initial, f.key.c_str()))
                    json_object_object_del(r, f.key.c_str());
                else
                    json_object_object_add(r, f.key.c_str(), bc_clone(f.rows));
            } else if (f.mask) {
                unsigned mask = 0;
                for (unsigned i = 0; i < 8; i++)
                    if (f.bits[i]->value())
                        mask |= 1u << i;
                json_object_object_add(r, f.key.c_str(), json_object_new_int64(mask));
            } else if (f.choice) {
                int i = f.choice->value();
                if (i < 0 || i >= (int)f.values.size())
                    i = 0;
                if (f.values.empty()) {
                    json_object_object_add(r, f.key.c_str(), json_object_new_string(""));
                    continue;
                }
                if (f.type == "boolean")
                    json_object_object_add(r, f.key.c_str(), json_object_new_boolean(i == 1));
                else
                    json_object_object_add(r, f.key.c_str(),
                                           json_object_new_string(f.values[(size_t)i].c_str()));
            } else if (f.editor) {
                char *s = f.buffer->text();
                json_object_object_add(r, f.key.c_str(), json_object_new_string(s));
                free(s);
            } else
                json_object_object_add(r, f.key.c_str(), json_object_new_string(f.input->value()));
        }
        return r;
    }

    void edit_channels(size_t index, int action) {
        auto &field = fields[index];
        int selected = field.rows_view->value() - 1;
        size_t count = json_object_array_length(field.rows);
        if (action && (selected < 0 || static_cast<size_t>(selected) >= count)) {
            error->copy_label("Selecione um canal.");
            return;
        }
        if (action == 2) {
            json_object_array_del_idx(field.rows, static_cast<size_t>(selected), 1);
            arrange();
            return;
        }
        if (!action && count >= (field.type == "wiring" ? 128u : 8u)) {
            error->copy_label("Use até oito canais.");
            return;
        }
        auto *draft =
            action ? bc_clone(json_object_array_get_idx(field.rows, static_cast<size_t>(selected)))
                   : bs_defaults(field.type == "wiring" ? "connection" : "channel");
        if (!action) {
            std::string id = "canal" + std::to_string(count + 1);
            json_object_object_add(draft, "id", json_object_new_string(id.c_str()));
        }
        auto *edited = Form::edit(field.type == "wiring" ? "connection" : "channel", project, draft,
                                  action ? "Editar canal" : "Adicionar canal");
        json_object_put(draft);
        if (!edited)
            return;
        for (size_t i = 0; i < count; i++)
            if ((!action || i != static_cast<size_t>(selected)) &&
                !strcmp(bc_str(json_object_array_get_idx(field.rows, i), "id"),
                        bc_str(edited, "id"))) {
                json_object_put(edited);
                error->copy_label("O nome do canal já existe.");
                return;
            }
        if (action)
            json_object_array_put_idx(field.rows, static_cast<size_t>(selected), edited);
        else
            json_object_array_add(field.rows, edited);
        arrange();
    }

    void load_capture(size_t index) {
        Fl_Native_File_Chooser chooser;
        chooser.title("Importar captura ou reprodução JSON");
        chooser.filter("JSON\t*.json");
        chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
        if (chooser.show() != 0 || !chooser.filename())
            return;
        std::string path = chooser.filename();
        bc_error e = {{0}};
        auto *input = bc_read_json(path.c_str(), &e);
        auto *capture = input ? (bc_get(input, "exchanges") ? bc_clone(input)
                                                            : bc_replay_from_capture(input, &e))
                              : nullptr;
        json_object_put(input);
        if (!capture || bc_replay_validate(capture, &e) < 0) {
            json_object_put(capture);
            error->copy_label(e.text);
            return;
        }
        json_object_put(fields[index].rows);
        fields[index].rows = capture;
        arrange();
    }

    void synchronize_lab() {
        if (kind != "step")
            return;
        auto *r = raw();
        if (strcmp(bc_str(r, "lab_mode"), "peripheral")) {
            json_object_put(r);
            return;
        }
        const char *device = bc_str(r, "lab_device");
        auto *catalog = bc_peripheral_catalog();
        json_object *selected = nullptr;
        for (size_t i = 0; i < json_object_array_length(catalog); i++) {
            auto *row = json_object_array_get_idx(catalog, i);
            if (!strcmp(bc_str(row, "id"), device))
                selected = row;
        }
        if (selected)
            for (auto &f : fields) {
                if (f.key == "lab_metric" || f.key == "lab_action") {
                    std::string wanted = bc_str(r, f.key.c_str()),
                                csv =
                                    bc_str(selected, f.key == "lab_metric" ? "metrics" : "actions");
                    f.choice->clear();
                    f.values.clear();
                    size_t start = 0;
                    while (start <= csv.size()) {
                        size_t end = csv.find(',', start);
                        std::string value =
                            csv.substr(start, end == std::string::npos ? end : end - start);
                        if (value.empty())
                            break;
                        f.values.push_back(value);
                        const char *label = f.key == "lab_metric"
                                                ? bc_lab_metric_label(value.c_str())
                                            : value == "read"    ? "Ler medição existente"
                                            : value == "measure" ? "Inicializar e medir"
                                            : value == "pattern" ? "Exibir padrão OLED"
                                                                 : "Desligar OLED";
                        form_menu_add(f.choice, label);
                        if (end == std::string::npos)
                            break;
                        start = end + 1;
                    }
                    auto it = std::find(f.values.begin(), f.values.end(), wanted);
                    f.choice->value(it == f.values.end()
                                        ? 0
                                        : static_cast<int>(std::distance(f.values.begin(), it)));
                }
                if (f.key == "lab_address" && prior_lab_device != device) {
                    int previous = prior_lab_device == "bme280" || prior_lab_device == "bmp280"
                                       ? 0x76
                                   : prior_lab_device == "ina219"  ? 0x40
                                   : prior_lab_device == "mpu6050" ? 0x68
                                                                   : 0x3c;
                    char *end = nullptr;
                    long current = std::strtol(f.input->value(), &end, 0);
                    if (end && !*end && current == previous)
                        f.input->value(
                            std::to_string(static_cast<int>(bc_num(selected, "address"))).c_str());
                }
            }
        prior_lab_device = device;
        json_object_put(catalog);
        json_object_put(r);
    }

    void arrange() {
        synchronize_lab();
        json_object *r = raw();
        scroll->scroll_to(0, 0);
        int y = 116;
        int width = w() - 84;
        for (auto &f : fields) {
            bool visible = bs_field_visible(f.definition, r) != 0;
            if (kind == "board" && f.key == "notes")
                visible = false;
            if (kind == "device") {
                std::string driver = bc_str(r, "driver");
                if (f.key == "output_mask" || f.key == "initial_output")
                    visible = driver == "tca9534a";
                if (f.key == "vref")
                    visible = driver == "ads7924" || driver == "mcp4728";
                if (f.key == "path")
                    visible = !strcmp(bc_str(r, "transport"), "linux_i2c");
            }
            if (kind == "signal") {
                bool reg = !strcmp(
                    bc_str(bs_find(bc_get(project, "devices"), bc_str(r, "device")), "driver"),
                    "register");
                if (f.key == "register" || f.key == "width" || f.key == "endian" ||
                    f.key == "bits" || f.key == "shift" || f.key == "gain" || f.key == "offset")
                    visible = reg;
            }
            if (kind == "step" && !strcmp(bc_str(r, "type"), "communication")) {
                std::string mode = bc_str(r, "lab_mode"), device = bc_str(r, "lab_device"),
                            protocol = bc_str(r, "lab_protocol");
                if (f.key == "comm_bus" &&
                    !strcmp(bc_str(bc_get(project, "editing_test"), "comm_mode"), "multi"))
                    visible = false;
                if (f.key == "lab_shunt" || f.key == "lab_current_lsb")
                    visible = mode == "peripheral" && device == "ina219";
                if (f.key == "lab_accel_range" || f.key == "lab_gyro_range")
                    visible = mode == "peripheral" && device == "mpu6050";
                if (f.key == "lab_oled_height" || f.key == "lab_pattern")
                    visible = mode == "peripheral" && device == "ssd1306";
                if (f.key == "comm_write" || f.key == "comm_encoding" || f.key == "comm_ending")
                    visible =
                        mode != "peripheral" && (mode != "protocol" || protocol != "modbus_rtu");
                if (f.key == "lab_success")
                    visible = mode == "protocol" && protocol == "at";
                if (f.key == "lab_length")
                    visible = mode == "protocol" && protocol == "length";
                if (f.key == "lab_slave" || f.key == "lab_function" || f.key == "lab_start" ||
                    f.key == "lab_quantity" || f.key == "lab_values")
                    visible = mode == "protocol" && protocol == "modbus_rtu";
                if (f.key == "lab_sample" || f.key == "lab_sim_success")
                    visible = mode != "raw" && protocol != "frame" && !bc_get(r, "lab_capture");
                if (f.key == "lab_fault" || f.key == "lab_fault_at" || f.key == "lab_fragment" ||
                    f.key == "lab_delay_ms" || f.key == "lab_fault_offset" ||
                    f.key == "lab_fault_xor")
                    visible = bc_get(r, "lab_capture") != nullptr;
            }
            Fl_Widget *widget = f.aux      ? (Fl_Widget *)f.aux
                                : f.mask   ? (Fl_Widget *)f.mask
                                : f.choice ? (Fl_Widget *)f.choice
                                : f.editor ? (Fl_Widget *)f.editor
                                           : (Fl_Widget *)f.input;
            if (!visible) {
                f.caption->hide();
                f.help->hide();
                widget->hide();
                continue;
            }
            f.caption->show();
            widget->show();
            f.help->show();
            f.caption->resize(36, y, width, 22);
            y += 26;
            int inputh = f.aux      ? ((f.type == "channels" || f.type == "wiring") ? 180 : 80)
                         : f.mask   ? 70
                         : f.editor ? 112
                                    : 38;
            widget->resize(36, y, width, inputh);
            if (f.aux) {
                if (f.rows_view) {
                    f.rows_view->resize(36, y, width, 126);
                    int selected = f.rows_view->value();
                    f.rows_view->clear();
                    for (size_t i = 0; i < json_object_array_length(f.rows); i++) {
                        auto *row = json_object_array_get_idx(f.rows, i);
                        f.rows_view->add(
                            (std::string(bc_str(row, "id")) + " · " +
                             (f.type == "wiring"
                                  ? std::string(bc_str(row, "connector")) + "/" +
                                        bc_str(row, "pin") + " → " + bc_str(row, "channel") + "/" +
                                        bc_str(row, "adapter_pin")
                                  : std::string(bc_str(row, "comm_bus")) + " · " +
                                        bc_str(row, "comm_adapters")))
                                .c_str());
                    }
                    f.rows_view->value(selected);
                }
                if (f.summary) {
                    f.summary->resize(36, y, width, 30);
                    std::string message =
                        f.rows && json_object_is_type(bc_get(f.rows, "exchanges"), json_type_array)
                            ? std::to_string(
                                  json_object_array_length(bc_get(f.rows, "exchanges"))) +
                                  " transações · " + bc_str(f.rows, "bus") + " · somente simulação"
                            : "Sem captura — simulação usa o exemplo declarado.";
                    if (f.type == "frame")
                        message = std::string(bc_str(f.rows, "name")) + " · " +
                                  std::to_string((int)bc_num(f.rows, "length")) + " bytes · " +
                                  std::to_string(
                                      json_object_is_type(bc_get(f.rows, "fields"), json_type_array)
                                          ? json_object_array_length(bc_get(f.rows, "fields"))
                                          : 0) +
                                  " campos";
                    f.summary->copy_label(message.c_str());
                }
                for (size_t i = 0; i < f.controls.size(); i++)
                    f.controls[i]->resize(36 + static_cast<int>(i) * 200,
                                          y + (f.rows_view ? 140 : 38), 190, 34);
            }
            if (f.mask)
                for (unsigned i = 0; i < 8; i++)
                    f.bits[i]->resize(36 + (int)(i % 4) * (width / 4), y + (int)(i / 4) * 34,
                                      width / 4, 30);
            y += inputh + 8;
            int hh = std::max(
                22, (int)wrap(f.help->label() ? f.help->label() : "", width, 12).size() * 17);
            f.help->resize(36, y, width, hh);
            y += hh + 22;
        }
        content->resize(20, 108, w() - 60, std::max(200, y - 108));
        json_object_put(r);
        scroll->redraw();
    }

    void resize(int x, int y, int w, int h) override {
        if (w == this->w() && h == this->h()) {
            Fl_Double_Window::resize(x, y, w, h);
            return;
        }
        Fl_Double_Window::resize(x, y, w, h);
        arrange();
    }

    void accept() {
        bc_error e = {{0}};
        json_object *r = raw(), *n = nullptr;
        int rc = bs_form(kind.c_str(), r, &n, &e);
        json_object_put(r);
        if (rc < 0) {
            error->copy_label(e.text);
            return;
        }
        json_object_put(result);
        result = n;
        hide();
    }

    static json_object *edit(const std::string &kind, json_object *p, json_object *values,
                             const std::string &title) {
        Form f(kind, p, values, title);
        f.show();
        while (f.shown())
            Fl::wait();
        return f.result ? json_object_get(f.result) : nullptr;
    }
};
