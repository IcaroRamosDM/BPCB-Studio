class DebugBreakpoints : public Fl_Check_Browser {
    void *current_item = nullptr;

    void item_draw(void *item, int x, int y, int w, int h) const override {
        Fl_Check_Browser::item_draw(item, x, y, w, h);
        if (item == current_item) {
            fl_color(pal.accent);
            fl_rect(x, y, w, h);
        }
    }

public:
    using Fl_Check_Browser::Fl_Check_Browser;

    void highlight(int line) {
        void *item = item_first();
        for (int i = 1; item && i < line; i++)
            item = item_next(item);
        current_item = item;
        if (item)
            display(item);
        redraw();
    }
};

struct Simulation : Fl_Double_Window {
    bc_job *job = nullptr;
    bool debugging = false, debug_running = false;
    json_object *debug_pending = nullptr;
    DebugBreakpoints *breakpoints = nullptr;
    Fl_Hold_Browser *debug_path = nullptr;
    Fl_Box *debug_state = nullptr;
    Action *debug_continue = nullptr, *debug_step = nullptr, *debug_pause = nullptr;
    Fl_Tabs *debug_tabs = nullptr;
    Fl_Group *debug_results_page = nullptr, *debug_page = nullptr;
    TextPanel *debug_variables = nullptr;
    std::vector<std::string> step_ids;
    unsigned path_count = 0;

    void debug_release(bool run) {
        if (!debug_pending || !job)
            return;
        debug_running = run;
        bc_job_reply(job, bc_str(debug_pending, "id"), 1);
        json_object_put(debug_pending);
        debug_pending = nullptr;
        debug_state->copy_label("Executando a etapa…");
        debug_continue->deactivate();
        debug_step->deactivate();
    }

    void debug_event(json_object *e) {
        int index = (int)bc_num(e, "step_index");
        bool valid = index >= 0 && (size_t)index < step_ids.size() &&
                     step_ids[(size_t)index] == bc_str(e, "step_id");
        if (!valid) {
            bc_job_cancel(job);
            debug_state->copy_label("Etapa de depuração inválida.");
            return;
        }
        std::string line = std::to_string((unsigned)bc_num(e, "visit")) + " · " +
                           bc_str(e, "step_id") + " · " + bc_str(e, "message");
        debug_path->add(line.c_str());
        if (++path_count > 512) {
            debug_path->remove(1);
            path_count = 512;
        }
        debug_path->bottomline(debug_path->size());
        debug_path->select(debug_path->size());
        breakpoints->highlight(index + 1);
        variables = std::string("Antes da etapa: ") + bc_str(e, "step_id") + "\n" +
                    json_object_to_json_string_ext(bc_get(e, "variables"), JSON_C_TO_STRING_PRETTY);
        if (json_object_get_boolean(bc_get(e, "variables_truncated")))
            variables += "\nExibição limitada a 256 variáveis / 20 KiB.";
        debug_variables->buffer.text(variables.c_str());
        json_object_put(debug_pending);
        debug_pending = bc_clone(e);
        if (debug_running && !breakpoints->checked(index + 1)) {
            debug_release(true);
            return;
        }
        debug_running = false;
        debug_tabs->value(debug_page);
        debug_state->copy_label((std::string("Pausado antes de ") + bc_str(e, "step_id")).c_str());
        debug_continue->activate();
        debug_step->activate();
        state->copy_label(
            "Simulação pausada. Avançar executa uma etapa; Continuar respeita os pontos marcados.");
    }

    std::string root, log;
    TextPanel *trace;
    Grid *results;
    Action *finish, *graph_button = nullptr, *variables_button = nullptr, *debug_inspect = nullptr;
    Fl_Box *trace_caption = nullptr, *path_caption = nullptr;
    Fl_Box *state;
    std::vector<std::vector<std::string>> rows;
    Fl_Double_Window *prompt = nullptr;
    Fl_Input *value_input = nullptr;
    Fl_Box *validation = nullptr;
    json_object *pending = nullptr;
    Fl_Choice *reading_unit = nullptr, *chart_signal = nullptr;
    Fl_Double_Window *chart_window = nullptr;
    TracePlot *chart = nullptr;
    std::string variables = "Nenhuma etapa executada.";

    Simulation(const std::string &root_, const std::string &board, const std::string &test,
               bool debug = false, json_object *steps = nullptr)
        : Fl_Double_Window(1040, 760, "BPCB Studio · Simulação"), debugging(debug), root(root_) {
        color(pal.bg);
        size_range(880, 620);
        begin();
        ui_label(24, 20, 980, 40, "Simulação do teste", 25);
        ui_label(24, 66, 980, 46,
                 "Nenhum módulo USB será acessado. Leituras de sinais usam os valores de exemplo; "
                 "as pausas aguardam sua resposta.",
                 14, pal.muted);
        state = ui_label(24, 126, 980, 42, "Preparando o executor…", 15);
        if (debugging) {
            debug_tabs = new Fl_Tabs(24, 174, 992, 510);
            debug_results_page = new Fl_Group(24, 204, 992, 480, "Resultados e log");
        }
        results = new Grid(24, debugging ? 214 : 180, 992, debugging ? 176 : 210);
        results->data({"RESULTADO", "VALOR", "AVALIAÇÃO"}, {}, {.40, .20, .40});
        trace_caption = ui_label(24, 412, 992, 28, "Etapas executadas", 15);
        trace = new TextPanel(24, 444, 992, 230);
        if (debugging) {
            debug_results_page->end();
            debug_tabs->end();
            begin();
        }
        graph_button = new Action(24, 700, 160, 38, "Gráficos", [this] {
            chart_window->show();
        });
        variables_button = new Action(198, 700, 170, 38, "Variáveis", [this] {
            message("Estado da sequência", variables);
        });
        finish = new Action(838, 700, 178, 38, "Interromper", [this] {
            close_or_stop();
        });
        if (debugging) {
            debug_tabs->begin();
            debug_page = new Fl_Group(24, 204, 992, 480, "Depurador");
            debug_state = ui_label(40, 218, 944, 34, "Aguardando a primeira etapa…", 15);
            ui_label(40, 260, 440, 24, "Pontos de parada · próxima etapa destacada", 13, pal.muted);
            breakpoints = new DebugBreakpoints(40, 292, 440, 226);
            breakpoints->color(pal.card);
            breakpoints->textcolor(pal.ink);
            breakpoints->textsize(14);
            for (size_t i = 0; i < json_object_array_length(steps); i++) {
                auto *s = json_object_array_get_idx(steps, i);
                step_ids.emplace_back(bc_str(s, "id"));
                breakpoints->add((step_ids.back() + " · " + bc_str(s, "title")).c_str());
            }
            ui_label(504, 260, 480, 24, "Variáveis antes da próxima etapa", 13, pal.muted);
            debug_variables = new TextPanel(504, 292, 480, 226);
            debug_continue = new Action(40, 536, 180, 36, "Continuar", [this] {
                debug_release(true);
            });
            debug_step = new Action(236, 536, 180, 36, "Avançar etapa", [this] {
                debug_release(false);
            });
            debug_pause = new Action(432, 536, 180, 36, "Pausar", [this] {
                debug_running = false;
                debug_state->copy_label("Pausa solicitada para a próxima etapa.");
            });
            debug_inspect = new Action(628, 536, 180, 36, "Inspecionar", [this] {
                message("Variáveis e estado", variables);
            });
            debug_continue->deactivate();
            debug_step->deactivate();
            path_caption = ui_label(40, 584, 944, 24, "Caminho percorrido (últimas 512 etapas)", 13,
                                    pal.muted);
            debug_path = new Fl_Hold_Browser(40, 614, 944, 54);
            debug_path->textsize(13);
            debug_path->color(pal.card);
            debug_path->textcolor(pal.ink);
            debug_page->end();
            debug_tabs->end();
            size_range(1040, 660);
        }
        end();
        resizable(debugging ? nullptr : trace);
        set_modal();
        callback(
            [](Fl_Widget *, void *ctx) {
                ((Simulation *)ctx)->close_or_stop();
            },
            this);
        chart_window = new Fl_Double_Window(1000, 650, "BPCB Studio · Gráficos da simulação");
        chart_window->color(pal.bg);
        chart_window->begin();
        chart_signal = new Fl_Choice(24, 20, 420, 36);
        choice_style(chart_signal);
        chart_signal->callback(
            [](Fl_Widget *, void *p) {
                auto *s = (Simulation *)p;
                if (s->chart_signal->text())
                    s->chart->select(s->chart_signal->text());
            },
            this);
        new Action(460, 20, 90, 36, "Ajustar", [this] {
            chart->fit();
        });
        new Action(564, 20, 110, 36, "CSV", [this] {
            export_graph(false);
        });
        new Action(688, 20, 110, 36, "PNG", [this] {
            export_graph(true);
        });
        new Action(812, 20, 160, 36, "Voltar", [this] {
            chart_window->hide();
        });
        chart = new TracePlot();
        chart->resize(24, 76, 952, 550);
        chart_window->end();
        chart_window->resizable(chart);
        chart_window->callback([](Fl_Widget *w, void *) {
            w->hide();
        });
        auto *options = json_object_new_object();
        if (debugging)
            json_object_object_add(options, "studio_debug", json_object_new_boolean(1));
        job = bc_job_start("run", root.c_str(), board.c_str(), test.c_str(), "", options);
        json_object_put(options);
        if (job)
            watch_work(
                job, bc_job_notification_fd(job),
                [](int, void *ctx) {
                    ((Simulation *)ctx)->events();
                },
                this);
        else {
            state->copy_label("Não foi possível iniciar a simulação.");
            finish->copy_label("Fechar");
        }
        if (debugging) {
            int sx = 0, sy = 0, sw = 0, sh = 0;
            Fl::screen_work_area(sx, sy, sw, sh, 0);
            (void)sx;
            (void)sy;
            (void)sw;
            if (sh > 0 && sh < 810)
                size(w(), std::max(660, std::min(760, sh - 40)));
        }
    }

    void resize(int x, int y, int width, int height) override {
        Fl_Double_Window::resize(x, y, width, height);
        if (!debugging || !debug_tabs)
            return;
        debug_tabs->resize(24, 174, width - 48, height - 250);
        debug_results_page->resize(24, 204, width - 48, height - 280);
        debug_page->resize(24, 204, width - 48, height - 280);
        results->resize(24, 214, width - 48, std::max(80, height - 584));
        trace_caption->resize(24, height - 348, width - 48, 28);
        trace->resize(24, height - 316, width - 48, 230);
        breakpoints->resize(40, 292, 440, std::max(120, height - 534));
        debug_variables->resize(504, 292, width - 544, std::max(120, height - 534));
        debug_continue->resize(40, height - 224, 180, 36);
        debug_step->resize(236, height - 224, 180, 36);
        debug_pause->resize(432, height - 224, 180, 36);
        debug_inspect->resize(628, height - 224, 180, 36);
        path_caption->resize(40, height - 176, width - 96, 24);
        debug_path->resize(40, height - 146, width - 96, 54);
        graph_button->resize(24, height - 60, 160, 38);
        variables_button->resize(198, height - 60, 170, 38);
        finish->resize(width - 202, height - 60, 178, 38);
    }

    ~Simulation() {
        json_object_put(debug_pending);
        if (job) {
            unwatch_work(job, bc_job_notification_fd(job));
            bc_job_free(job);
        }
        if (chart_window) {
            chart_window->hide();
            delete chart_window;
        }
        if (prompt) {
            prompt->hide();
            delete prompt;
        }
        json_object_put(pending);
        bs_remove_temp(root.c_str());
    }

    void append(const std::string &s) {
        std::string line = s + "\n";
        trace->buffer.append(line.c_str());
        if (trace->buffer.length() > 1000000) {
            int cut = trace->buffer.line_end(250000) + 1;
            trace->buffer.remove(0, cut);
        }
        trace->insert_position(trace->buffer.length());
        trace->show_insert_position();
    }

    void close_prompt() {
        if (prompt) {
            prompt->hide();
            Fl::delete_widget(prompt);
            prompt = nullptr;
        }
        json_object_put(pending);
        pending = nullptr;
        value_input = nullptr;
        validation = nullptr;
        reading_unit = nullptr;
    }

    void close_or_stop() {
        if (job && !bc_job_finished(job)) {
            close_prompt();
            bc_job_cancel(job);
            state->copy_label("Interrompendo a sequência…");
            finish->deactivate();
        } else
            hide();
    }

    void ask(json_object *e) {
        close_prompt();
        pending = json_object_get(e);
        prompt = new Fl_Double_Window(730, 540, "BPCB Studio · Etapa aguardando confirmação");
        prompt->color(pal.bg);
        prompt->begin();
        bool number = json_object_get_boolean(bc_get(e, "number"));
        ui_label(24, 22, 682, 38, number ? "Informe a medição" : "Confirme para continuar", 23);
        new TextPanel(24, 80, 682, 282, bc_str(e, "message"));
        if (number) {
            char range[180];
            snprintf(range, sizeof range, "Faixa de aprovação: %.8g a %.8g (na unidade base)",
                     bc_num(e, "min"), bc_num(e, "max"));
            ui_label(24, 380, 682, 22, range, 13, pal.muted);
            value_input = new Fl_Input(24, 410, 532, 38);
            field_style(value_input);
            value_input->when(FL_WHEN_ENTER_KEY_ALWAYS);
            value_input->callback(
                [](Fl_Widget *, void *v) {
                    ((Simulation *)v)->answer();
                },
                this);
            reading_unit = new Fl_Choice(570, 410, 136, 38);
            choice_style(reading_unit);
            std::string base = bc_str(e, "unit");
            if (base == "V") {
                reading_unit->add("V");
                reading_unit->add("mV");
            } else if (base == "ms" || base == "s" || base == "us") {
                reading_unit->add("ms");
                reading_unit->add("us");
                reading_unit->add("s");
            } else
                reading_unit->add(base.c_str());
            reading_unit->value(0);
            const char *wanted =
                *bc_str(e, "display_unit") ? bc_str(e, "display_unit") : bc_str(e, "unit");
            for (int i = 0; i < reading_unit->size() - 1; i++)
                if (reading_unit->text(i) && !strcmp(reading_unit->text(i), wanted))
                    reading_unit->value(i);
            if (bc_get(e, "sample")) {
                char sample[100];
                snprintf(sample, sizeof sample, "%.12g %s", bc_num(e, "sample"), bc_str(e, "unit"));
                value_input->value(sample);
            }
        }
        validation = ui_label(24, 458, 682, 22, "", 12, pal.error);
        new Action(398, 492, 132, 34, "Interromper", [this] {
            close_or_stop();
        });
        new Action(
            546, 492, 160, 34, "Continuar",
            [this] {
                answer();
            },
            true);
        prompt->end();
        prompt->set_modal();
        prompt->callback(
            [](Fl_Widget *, void *v) {
                ((Simulation *)v)->close_or_stop();
            },
            this);
        prompt->show();
        if (value_input)
            value_input->take_focus();
    }

    void answer() {
        if (!pending || !job)
            return;
        if (value_input) {
            double v;
            bc_error e = {{0}};
            if (bc_parse_reading(value_input->value(), bc_str(pending, "unit"),
                                 reading_unit && reading_unit->text() ? reading_unit->text()
                                                                      : bc_str(pending, "unit"),
                                 &v, &e) < 0) {
                validation->copy_label(e.text);
                return;
            }
            bc_job_reply_value(job, bc_str(pending, "id"), 1, v);
        } else
            bc_job_reply(job, bc_str(pending, "id"), 1);
        close_prompt();
    }

    void refresh_graph() {
        if ((size_t)(chart_signal->size() ? chart_signal->size() - 1 : 0) == chart->series.size())
            return;
        chart_signal->clear();
        int selected = 0, i = 0;
        for (const auto &[name, series] : chart->series) {
            (void)series;
            chart_signal->add(name.c_str());
            if (name == chart->selected)
                selected = i;
            i++;
        }
        chart_signal->value(selected);
    }

    void export_graph(bool png) {
        if (chart->series.empty()) {
            message("Gráfico vazio", "Marque Gráfico no sinal ou na etapa e execute a simulação.");
            return;
        }
        Fl_Native_File_Chooser chooser;
        chooser.title(png ? "Exportar gráfico PNG" : "Exportar amostras CSV");
        chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
        chooser.filter(png ? "PNG\t*.png" : "CSV\t*.csv");
        chooser.preset_file(png ? "Studio-grafico.png" : "Studio-amostras.csv");
        if (chooser.show() != 0 || !chooser.filename())
            return;
        json_object *data = chart->snapshot();
        bc_error e = {{0}};
        int rc = png ? bc_plot_png(data, chooser.filename(), &e)
                     : bc_plot_csv(data, chooser.filename(), &e);
        json_object_put(data);
        message(rc ? "Exportação não concluída" : "Gráfico exportado",
                rc ? e.text : chooser.filename());
    }

    void events() {
        if (!job)
            return;
        bc_job_acknowledge(job);
        json_object *e;
        while ((e = bc_job_next(job))) {
            std::string type = bc_str(e, "type");
            if (type == "sample") {
                auto *values = bc_get(e, "values");
                json_object_object_foreach(values, key, value) {
                    chart->add(key, bc_str(e, "unit"), bc_num(e, "elapsed"),
                               json_object_get_double(value),
                               bc_get(e, "target") ? bc_num(e, "target") : NAN,
                               bc_get(e, "limit") ? bc_num(e, "limit") : NAN);
                }
                refresh_graph();
            } else if (type == "status") {
                if (bc_get(e, "variables"))
                    variables = std::string("Etapa: ") + bc_str(e, "step_id") + "\n" +
                                json_object_to_json_string_ext(bc_get(e, "variables"),
                                                               JSON_C_TO_STRING_PRETTY);
                state->copy_label(bc_str(e, "message"));
                append(bc_str(e, "message"));
            } else if (type == "confirm") {
                if (debugging && json_object_get_boolean(bc_get(e, "debug")))
                    debug_event(e);
                else
                    ask(e);
            } else if (type == "measurement") {
                char val[160];
                snprintf(val, sizeof val, "%.8g %s", bc_num(e, "value"), bc_str(e, "unit"));
                std::string verdict = bc_str(e, "verdict");
                std::string mark = verdict == "pass"   ? "✓ Dentro da tolerância"
                                   : verdict == "fail" ? "× Fora da tolerância"
                                                       : "Sem comparação";
                rows.push_back({bc_str(e, "label"), val, mark + "\n" + bc_str(e, "criterion")});
                results->data({"RESULTADO", "VALOR", "AVALIAÇÃO"}, rows, {.40, .20, .40});
            } else if (type == "finished") {
                close_prompt();
                json_object_put(debug_pending);
                debug_pending = nullptr;
                if (debugging) {
                    debug_continue->deactivate();
                    debug_step->deactivate();
                    debug_pause->deactivate();
                    debug_state->copy_label("Depuração encerrada.");
                }
                state->copy_label(bc_str(e, "message"));
                append(bc_str(e, "message"));
                finish->copy_label("Fechar");
                finish->activate();
            }
            json_object_put(e);
        }
        if (bc_job_finished(job)) {
            unwatch_work(job, bc_job_notification_fd(job));
            bc_job_free(job);
            job = nullptr;
        }
    }
};
