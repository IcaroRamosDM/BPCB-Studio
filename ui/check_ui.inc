
static void tick(double seconds = .08) {
    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until)
        Fl::wait(.01);
}

static void require(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}

static void xdo(std::vector<std::string> args) {
    std::vector<char *> av;
    for (auto &s : args)
        av.push_back(s.data());
    av.push_back(nullptr);
    pid_t pid;
    require(posix_spawnp(&pid, "xdotool", nullptr, nullptr, av.data(), environ) == 0,
            "xdotool spawn");
    int state = 0;
    for (int n = 0; n < 400; n++) {
        if (waitpid(pid, &state, WNOHANG) == pid) {
            require(WIFEXITED(state) && !WEXITSTATUS(state), "xdotool failed");
            tick(.04);
            return;
        }
        tick(.01);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &state, 0);
    throw std::runtime_error("xdotool timeout");
}

static void click(Fl_Window *w, int x, int y) {
    auto id = std::to_string((unsigned long)fl_xid(w));
    xdo({"xdotool", "windowfocus", id, "mousemove", "--window", id, std::to_string(x),
         std::to_string(y), "click", "1"});
}

static std::vector<unsigned long> pixels(Fl_Window *w, int x, int y, int width, int height) {
    tick(.03);
    XSync(fl_display, False);
    XImage *im = XGetImage(fl_display, fl_xid(w), x, y, (unsigned)width, (unsigned)height,
                           AllPlanes, ZPixmap);
    require(im, "XGetImage");
    std::vector<unsigned long> out;
    for (int yy = 0; yy < height; yy++)
        for (int xx = 0; xx < width; xx++)
            out.push_back(XGetPixel(im, xx, yy));
    XDestroyImage(im);
    return out;
}

static void capture(Fl_Window *w, const std::string &path) {
    auto px = pixels(w, 0, 0, w->w(), w->h());
    std::vector<uint32_t> data;
    data.reserve(px.size());
    for (auto v : px)
        data.push_back((uint32_t)v);
    cairo_surface_t *sf = cairo_image_surface_create_for_data(
        (unsigned char *)data.data(), CAIRO_FORMAT_RGB24, w->w(), w->h(), w->w() * 4);
    require(cairo_surface_write_to_png(sf, path.c_str()) == CAIRO_STATUS_SUCCESS,
            "write screenshot");
    cairo_surface_destroy(sf);
}

static void grid_check(Studio &app) {
    auto *g = app.grid;
    require(g && g->items.size() >= 3, "3 rows required");
    Fl::focus(nullptr);
    tick();
    int x, y, w, h, hx, hy, hw, hh;
    g->head_rect(hx, hy, hw, hh);
    g->rect(2, x, y, w, h);
    auto head = pixels(&app, hx, hy, hw, hh), other = pixels(&app, x, y, w, h);
    for (int i : {0, 1, 0, 1}) {
        int ax, ay, aw, ah;
        g->rect(i, ax, ay, aw, ah);
        click(&app, ax + 30, ay + ah / 2);
        require(pixels(&app, hx, hy, hw, hh) == head, "Header disappeared after click");
        require(pixels(&app, x, y, w, h) == other, "Unselected row disappeared after click");
    }
}

static int ui_check(Studio &app, const char *output) {
    try {
        bc_error e = {{0}};
        require(bc_mkdirs(output, &e) == 0, "output directory");
        app.show();
        tick();
        capture(&app, std::string(output) + "/welcome.png");
        json_object *p = bs_example();
        auto *components = bc_get(p, "components");
        for (int i = 2; i <= 15; i++) {
            json_object *c = bs_defaults("component");
            std::string ref = "R" + std::to_string(i);
            json_object_object_add(c, "ref", json_object_new_string(ref.c_str()));
            json_object_object_add(
                c, "description",
                json_object_new_string(
                    i == 2 ? "Descrição longa para verificar a quebra de linha e a preservação do "
                             "cabeçalho. Componentes com "
                             "palavras_muito_longas_sem_espacos_tambem_devem_quebrar_corretamente."
                           : "Componente de exemplo para a lista."));
            json_object_array_add(components, c);
        }
        app.adopt(p, std::string(output) + "/example.bpcb", false);
        json_object_put(p);
        app.go("components");
        tick();
        grid_check(app);
        capture(&app, std::string(output) + "/components.png");
        Fl::focus(nullptr);
        tick(.2);
        unsigned before = app.draws;
        tick(.6);
        require(app.draws == before, "idle redraw");
        auto id = std::to_string((unsigned long)fl_xid(&app));
        xdo({"xdotool", "windowsize", id, "1020", "700"});
        tick();
        require(app.w() == 1020, "minimum resize");
        require(app.grid->w() > 600 && app.grid->h() > 200, "grid resize");
        grid_check(app);
        capture(&app, std::string(output) + "/minimum.png");
        xdo({"xdotool", "windowsize", id, "1280", "820"});
        tick();
        app.go("tests");
        tick();
        grid_check(app);
        capture(&app, std::string(output) + "/steps.png");
        for (const char *page : {"overview", "devices", "signals", "documents", "notes", "tests"}) {
            app.go(page);
            tick();
        }
        app.row = 1;
        size_t count = json_object_array_length(app.current_rows());
        app.duplicate(false);
        require(json_object_array_length(app.current_rows()) == count + 1, "duplicate step");
        app.history(false);
        require(json_object_array_length(app.current_rows()) == count, "undo duplicate");
        app.history(true);
        require(json_object_array_length(app.current_rows()) == count + 1, "redo duplicate");
        app.history(false);
        app.go("tools");
        tick();
        capture(&app, std::string(output) + "/tools.png");
        app.go("tests");
        auto *data = bs_defaults("step");
        Form form("step", app.project, data, "Teste do formulário");
        json_object_put(data);
        form.show();
        tick();
        EditorField *title = nullptr;
        for (auto &f : form.fields)
            if (f.key == "title")
                title = &f;
        require(title && title->input, "title field");
        click(&form, title->input->x() + 20, title->input->y() + 18);
        auto fid = std::to_string((unsigned long)fl_xid(&form));
        xdo({"xdotool", "key", "--window", fid, "ctrl+a", "type", "--window", fid,
             "Etapa editada"});
        require(!form.result, "editing should not commit early");
        capture(&form, std::string(output) + "/step-editor.png");
        click(&form, form.apply->x() + 30, form.apply->y() + 18);
        require(form.result && !strcmp(bc_str(form.result, "title"), "Etapa editada"), "form save");
        FilePicker picker("Abrir projeto", output, "project", false);
        picker.show();
        for (int n = 0; n < 300 && picker.task; n++)
            tick(.01);
        require(!picker.task && picker.listing, "async browser");
        capture(&picker, std::string(output) + "/picker.png");
        picker.hide();

        {
            Form device_form("device", app.project, nullptr, "Dispositivo I²C");
            device_form.show();
            tick();
            EditorField *mask = nullptr;
            for (auto &f : device_form.fields)
                if (f.key == "output_mask")
                    mask = &f;
            require(mask && mask->bits.size() == 8, "digital pin checkboxes");
            mask->bits[1]->value(1);
            device_form.accept();
            require(device_form.result && bc_num(device_form.result, "output_mask") == 2,
                    "pin mask normalized");
        }
        char simroot[] = "/tmp/bpcb-studio-ui-XXXXXX";
        require(mkdtemp(simroot) != nullptr, "simulation temp");
        std::string project_file = std::string(simroot) + "/demo.bpcb",
                    package = std::string(simroot) + "/demo.bancada";
        json_object *demo = bs_example();
        auto *demo_test = bs_find(bc_get(demo, "tests"), "divisor");
        for (size_t i = 0; i < json_object_array_length(bc_get(demo_test, "steps")); i++)
            json_object_object_add(json_object_array_get_idx(bc_get(demo_test, "steps"), i), "plot",
                                   json_object_new_boolean(1));
        require(bs_save(project_file.c_str(), demo, &e) == 0, "save simulation project");
        require(bs_export(demo, project_file.c_str(), app.worker.c_str(), package.c_str(), 1, &e) ==
                    0,
                e.text);
        require(bc_install(simroot, package.c_str(), nullptr, &e) == 0, e.text);
        json_object_put(demo);
        {
            Simulation sim(simroot, "exemplo_divisor", "divisor");
            sim.show();
            for (int n = 0; n < 1000 && sim.job; n++) {
                tick(.01);
                if (sim.prompt) {
                    if (sim.value_input)
                        sim.value_input->value(
                            !strcmp(bc_str(sim.pending, "id"), "entrada") ? "5" : "2.5");
                    sim.answer();
                }
            }
            require(!sim.job && sim.rows.size() == 1, "interactive simulation finish");
            require(sim.rows[0][1].find("1000") != std::string::npos, "simulation calculation");
            require(sim.chart->series.size() == 3 &&
                        sim.variables.find("u_mid") != std::string::npos,
                    "simulation plots and variables");
            sim.chart_window->show();
            tick();
            const double trajectory[] = {0, .1, .24, .245, .25};
            for (unsigned i = 0; i < 5; i++)
                sim.chart->add("Retorno de exemplo (simulado)", "V", i * .1, trajectory[i], .25,
                               .26);
            sim.refresh_graph();
            sim.chart->select("Retorno de exemplo (simulado)");
            for (int i = 0; i < sim.chart_signal->size() - 1; i++)
                if (!strcmp(sim.chart_signal->text(i), "Retorno de exemplo (simulado)"))
                    sim.chart_signal->value(i);
            sim.chart->zoom(.8);
            sim.chart->cursor_a = sim.chart->bounds(true).first;
            sim.chart->cursor_b = sim.chart->bounds(true).second;
            capture(sim.chart_window, std::string(output) + "/simulation-graph.png");
            sim.chart_window->hide();
            capture(&sim, std::string(output) + "/simulation.png");
            sim.hide();
        }
        std::string svg = std::string(output) + "/test-document.svg";
        FILE *source = fopen(svg.c_str(), "w");
        require(source, "SVG fixture");
        fputs(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"800\" height=\"400\"><rect x=\"20\" "
            "y=\"20\" width=\"760\" height=\"360\" fill=\"white\" stroke=\"black\"/><text x=\"80\" "
            "y=\"180\" font-size=\"32\">Esquematico - BPCB Studio</text></svg>",
            source);
        fclose(source);
        {
            DocumentViewer viewer(svg, "Documento de teste");
            viewer.show();
            for (int n = 0; n < 500 && viewer.task; n++)
                tick(.01);
            require(!viewer.task && viewer.image && viewer.image->w() > 100, "document preview");
            capture(&viewer, std::string(output) + "/document.png");
            viewer.hide();
        }
        dark = true;
        colors();
        app.render();
        tick();
        capture(&app, std::string(output) + "/dark.png");
        require(app.grid != nullptr, "dark page");
        grid_check(app);
        for (const char *bus : {"i2c", "spi", "uart", "jtag"}) {
            auto *values = bs_defaults("step");
            json_object_object_add(values, "type", json_object_new_string("communication"));
            json_object_object_add(values, "comm_bus", json_object_new_string(bus));
            Form communication("step", app.project, values,
                               std::string("Bloco de comunicação · ") + bus);
            json_object_put(values);
            communication.show();
            tick();
            EditorField *address = nullptr, *bits = nullptr, *write = nullptr;
            for (auto &field : communication.fields) {
                if (field.key == "comm_address")
                    address = &field;
                if (field.key == "comm_bits")
                    bits = &field;
                if (field.key == "comm_write")
                    write = &field;
            }
            for (auto &field : communication.fields)
                if (field.choice)
                    require(field.choice->size() == static_cast<int>(field.values.size()) + 1,
                            "flat menu labels");
            require(address && bits && write && write->editor, "communication fields");
            require(address->input->visible() == !strcmp(bus, "i2c"), "I2C field visibility");
            require(bits->input->visible() == !strcmp(bus, "jtag"), "JTAG field visibility");
            communication.scroll->scroll_to(0, 400);
            tick();
            capture(&communication, std::string(output) + "/communication-" + bus + ".png");
            communication.hide();
        }
        {
            auto *test = bs_defaults("test"), *channels = json_object_new_array(),
                 *channel = bs_defaults("channel");
            json_object_object_add(channel, "id", json_object_new_string("sensor"));
            json_object_array_add(channels, channel);
            json_object_object_add(test, "comm_channels", channels);
            json_object_object_add(test, "comm_mode", json_object_new_string("multi"));
            json_object_object_add(test, "comm_enabled", json_object_new_boolean(1));
            Form setup("test", app.project, test, "Canais da sequência");
            setup.show();
            tick();
            EditorField *rows = nullptr;
            for (auto &f : setup.fields)
                if (f.key == "comm_channels")
                    rows = &f;
            require(rows && rows->rows_view && rows->controls.size() == 3,
                    "structured channel editor");
            setup.scroll->scroll_to(0, rows->caption->y() - 130);
            tick();
            capture(&setup, std::string(output) + "/laboratorio-canais.png");
            setup.accept();
            require(setup.result &&
                        json_object_array_length(bc_get(setup.result, "comm_channels")) == 1,
                    "channel form save");
            Form one("channel", app.project, channel, "Editar canal");
            one.show();
            tick();
            capture(&one, std::string(output) + "/laboratorio-canal.png");
            one.accept();
            require(one.result && !strcmp(bc_str(one.result, "id"), "sensor"),
                    "channel fields accepted");
            json_object_object_add(app.project, "editing_test", bc_clone(test));
            auto *step = bs_defaults("step");
            json_object_object_add(step, "type", json_object_new_string("communication"));
            json_object_object_add(step, "comm_channel", json_object_new_string("sensor"));
            json_object_object_add(step, "lab_mode", json_object_new_string("peripheral"));
            Form component("step", app.project, step, "Componente IoT");
            component.show();
            tick();
            EditorField *capture_field = nullptr;
            for (auto &f : component.fields)
                if (f.key == "lab_capture")
                    capture_field = &f;
            require(capture_field && capture_field->aux, "capture import control");
            for (auto &f : component.fields)
                if (f.key == "lab_device")
                    component.scroll->scroll_to(0, f.caption->y() - 130);
            tick();
            capture(&component, std::string(output) + "/laboratorio-componente.png");
            component.accept();
            require(component.result && !strcmp(bc_str(component.result, "lab_mode"), "peripheral"),
                    "IoT form save");
            json_object_object_add(step, "lab_mode", json_object_new_string("protocol"));
            Form protocol("step", app.project, step, "Protocolo UART");
            protocol.show();
            tick();
            for (auto &f : protocol.fields)
                if (f.key == "lab_protocol")
                    protocol.scroll->scroll_to(0, f.caption->y() - 130);
            tick();
            capture(&protocol, std::string(output) + "/laboratorio-protocolo.png");
            protocol.hide();
            json_object_put(step);
            json_object_put(test);
            json_object_object_del(app.project, "editing_test");
        }

        {
            WorkbenchFrameEditor editor(nullptr);
            editor.show();
            tick();
            editor.decode();
            require(editor.buffer.length() > 200, "frame visual preview");
            capture(&editor, std::string(output) + "/uart-quadro-editor.png");
            auto *spec = editor.collect();
            require(spec && bc_frame_validate(spec, &e) == 0, "frame visual save");
            json_object_put(spec);
            editor.hide();
            auto *test = bs_defaults("test"), *pins = json_object_new_array();
            json_object_array_add(pins, bs_defaults("connection"));
            json_object_object_add(test, "wiring", pins);
            Form wiring("test", app.project, test, "Mapa de conexões");
            wiring.show();
            tick();
            EditorField *map = nullptr;
            for (auto &f : wiring.fields)
                if (f.key == "wiring")
                    map = &f;
            require(map && map->rows_view && map->rows_view->size() == 1, "wiring visual rows");
            wiring.scroll->scroll_to(0, map->caption->y() - 130);
            tick();
            capture(&wiring, std::string(output) + "/mapa-conexoes.png");
            wiring.accept();
            require(wiring.result, "wiring visual save");
            json_object_put(test);
            auto *step = bs_defaults("step");
            json_object_object_add(step, "type", json_object_new_string("communication"));
            json_object_object_add(step, "lab_mode", json_object_new_string("protocol"));
            json_object_object_add(step, "lab_protocol", json_object_new_string("frame"));
            Form frame("step", app.project, step, "Quadro UART");
            frame.show();
            tick();
            EditorField *format = nullptr;
            for (auto &f : frame.fields)
                if (f.key == "lab_frame")
                    format = &f;
            require(format && format->aux && format->aux->visible(), "frame field visible");
            frame.scroll->scroll_to(0, format->caption->y() - 130);
            tick();
            capture(&frame, std::string(output) + "/uart-quadro-formulario.png");
            frame.accept();
            require(frame.result, "frame form save");
            json_object_put(step);
        }
        {
            char debugroot[4096];
            require(bs_temp_dir(debugroot, "qa_debug", &e) == 0, e.text);
            auto *demo = bs_example();
            std::string pkg = std::string(debugroot) + "/debug.bancada";
            require(bs_export(demo, project_file.c_str(), app.worker.c_str(), pkg.c_str(), 1, &e) ==
                        0,
                    e.text);
            require(bc_install(debugroot, pkg.c_str(), nullptr, &e) == 0, e.text);
            auto *steps = bc_get(bs_find(bc_get(demo, "tests"), "divisor"), "steps");
            Simulation sim(debugroot, "exemplo_divisor", "divisor", true, steps);
            json_object_put(demo);
            sim.show();
            for (int n = 0; n < 500 && !sim.debug_pending; n++) {
                tick(.01);
            }
            require(sim.debug_pending && sim.breakpoints->nchecked() == 0,
                    "debug initial pause without accidental breakpoint");
            require(sim.w() <= 1040, "debug notebook width");
            capture(&sim, std::string(output) + "/depurador-pausa.png");
            sim.resize(sim.x(), sim.y(), 1040, 660);
            tick();
            require(sim.finish->y() + sim.finish->h() <= sim.h() &&
                        sim.debug_step->y() + sim.debug_step->h() < sim.debug_path->y(),
                    "debug minimum height");
            capture(&sim, std::string(output) + "/depurador-minimo.png");
            sim.resize(sim.x(), sim.y(), 1040, 760);
            int first = (int)bc_num(sim.debug_pending, "visit");
            sim.debug_release(false);
            for (int n = 0; n < 500 && (!sim.debug_pending ||
                                        (int)bc_num(sim.debug_pending, "visit") == first);
                 n++) {
                tick(.01);
                if (sim.prompt)
                    sim.answer();
            }
            require(sim.debug_pending && (int)bc_num(sim.debug_pending, "visit") > first,
                    "single step pauses at next step");
            sim.breakpoints->checked(3, 1);
            sim.debug_release(true);
            for (int n = 0; n < 500 && !sim.debug_pending; n++) {
                tick(.01);
                if (sim.prompt)
                    sim.answer();
            }
            require(sim.debug_pending && (int)bc_num(sim.debug_pending, "step_index") == 2,
                    "breakpoint hit");
            sim.breakpoints->check_none();
            sim.debug_release(true);
            for (int n = 0; n < 1000 && sim.job; n++) {
                tick(.01);
                if (sim.prompt)
                    sim.answer();
            }
            require(!sim.job && sim.rows.size() == 1, "debug execution completes");
            capture(&sim, std::string(output) + "/depurador-concluido.png");
            sim.hide();
        }
        app.hide();
        puts("PASS: GUI navigation, actual clicks, partial redraw, forms, minimum size and dark "
             "theme.");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "GUI FAIL: %s\n", e.what());
        app.hide();
        return 1;
    }
}
