    std::vector<std::string> undo_states, redo_states;

    void checkpoint() {
        if (!project)
            return;
        std::string state = json_object_to_json_string_ext(project, JSON_C_TO_STRING_PLAIN);
        if (undo_states.empty() || undo_states.back() != state)
            undo_states.push_back(std::move(state));
        size_t bytes = 0;
        for (const auto &item : undo_states)
            bytes += item.size();
        while (undo_states.size() > 50 || (bytes > 32 * 1024 * 1024 && undo_states.size() > 1)) {
            bytes -= undo_states.front().size();
            undo_states.erase(undo_states.begin());
        }
        redo_states.clear();
    }

    void history(bool redo) {
        if (busy() || !project)
            return;
        apply_notes();
        auto &from = redo ? redo_states : undo_states;
        auto &to = redo ? undo_states : redo_states;
        if (from.empty())
            return;
        bc_error e = {{0}};
        json_object *restored = bc_parse_json(from.back().c_str(), from.back().size(), &e);
        if (!restored) {
            message("Histórico de edição", e.text);
            return;
        }
        to.emplace_back(json_object_to_json_string_ext(project, JSON_C_TO_STRING_PLAIN));
        from.pop_back();
        release_notes();
        json_object_put(project);
        project = restored;
        dirty = true;
        note_dirty = false;
        row = -1;
        render();
    }

    void duplicate(bool test) {
        if (busy() || !project)
            return;
        std::string kind = test ? "test" : current_kind();
        int index = row;
        if (test) {
            index = -1;
            json_object *a = bc_get(project, "tests");
            for (size_t i = 0; i < json_object_array_length(a); i++)
                if (!strcmp(bc_str(json_object_array_get_idx(a, i), "id"), test_id.c_str()))
                    index = (int)i;
        }
        checkpoint();
        bc_error e = {{0}};
        int added = bs_duplicate(project, kind.c_str(), test_id.c_str(), index, &e);
        if (added < 0) {
            message("Duplicar", e.text);
            return;
        }
        if (test)
            test_id =
                bc_str(json_object_array_get_idx(bc_get(project, "tests"), (size_t)added), "id");
        else
            row = added;
        dirty = true;
        render();
    }

    void validation_list(json_object *issues) {
        Fl_Double_Window dialog(900, 540, "BPCB Studio · Pendências");
        dialog.color(pal.bg);
        dialog.begin();
        ui_label(24, 20, 852, 56,
                 "Selecione uma pendência e clique em Abrir item, ou dê dois cliques na linha.",
                 17);
        auto *list = new Grid(24, 92, 852, 350);
        std::vector<std::vector<std::string>> rows;
        for (size_t i = 0; i < json_object_array_length(issues); i++) {
            json_object *issue = json_object_array_get_idx(issues, i);
            rows.push_back({bc_str(issue, "where"), bc_str(issue, "message")});
        }
        list->data({"LOCAL", "O QUE REVISAR"}, rows, {.35, .65});
        int selected = -1;
        bool open = false;
        list->selected = [&](int i, bool twice) {
            selected = i;
            if (twice) {
                open = true;
                dialog.hide();
            }
        };
        new Action(564, 470, 148, 38, "Voltar", [&] {
            dialog.hide();
        });
        new Action(
            728, 470, 148, 38, "Abrir item",
            [&] {
                if (selected >= 0) {
                    open = true;
                    dialog.hide();
                }
            },
            true);
        dialog.end();
        dialog.set_modal();
        dialog.show();
        while (dialog.shown())
            Fl::wait();
        if (!open || selected < 0)
            return;
        json_object *issue = json_object_array_get_idx(issues, (size_t)selected);
        page = bc_str(issue, "page");
        test_id = bc_str(issue, "test");
        row = (int)bc_num(issue, "index");
        std::string kind = bc_str(issue, "kind");
        tab = kind == "parameter" || kind == "formula" ? kind : "step";
        render();
        if (grid && row >= 0)
            grid->select_row(row);
        if (kind == "board")
            edit_board();
        else if (kind == "test")
            edit_test(false);
        else if (row >= 0)
            edit_item(false);
    }

    void reuse() {
        if (busy() || !project)
            return;
        apply_notes();
        std::string file =
            FilePicker::pick("Reaproveitar teste de outro projeto", start_folder(), "project");
        if (file.empty())
            return;
        launch(
            "load", "",
            [this](json_object *source) {
                auto *tests = bc_get(source, "tests");
                if (!json_object_array_length(tests)) {
                    message("Projeto sem testes",
                            "Escolha um projeto que contenha pelo menos um teste.");
                    return;
                }
                Fl_Double_Window dialog(720, 320, "Reaproveitar sequência");
                dialog.color(pal.bg);
                dialog.begin();
                ui_label(24, 22, 672, 96,
                         "O teste será copiado com os componentes, dispositivos e sinais da "
                         "origem. Identificadores iguais precisam ter cadastros iguais. Documentos "
                         "permanecem no projeto de origem.",
                         15);
                auto *choice = new Fl_Choice(24, 140, 672, 40);
                choice_style(choice);
                for (size_t i = 0; i < json_object_array_length(tests); i++)
                    choice->add(bc_str(json_object_array_get_idx(tests, i), "title"));
                choice->value(0);
                bool accepted = false;
                new Action(378, 250, 150, 38, "Cancelar", [&] {
                    dialog.hide();
                });
                new Action(
                    546, 250, 150, 38, "Copiar teste",
                    [&] {
                        accepted = true;
                        dialog.hide();
                    },
                    true);
                dialog.end();
                dialog.set_modal();
                dialog.show();
                while (dialog.shown())
                    Fl::wait();
                if (!accepted)
                    return;
                bc_error e = {{0}};
                json_object *merged = bs_reuse_test(
                    project, source,
                    bc_str(json_object_array_get_idx(tests, (size_t)choice->value()), "id"), &e);
                if (!merged) {
                    message("Reaproveitamento não concluído", e.text);
                    return;
                }
                checkpoint();
                json_object_put(project);
                project = merged;
                dirty = true;
                page = "tests";
                auto *all = bc_get(project, "tests");
                test_id =
                    bc_str(json_object_array_get_idx(all, json_object_array_length(all) - 1), "id");
                render();
            },
            file);
    }

    std::string artifact_file(const char *title, const char *filter, const char *suggested,
                              bool save_file) {
        Fl_Native_File_Chooser chooser;
        chooser.title(title);
        chooser.filter(filter);
        chooser.type(save_file ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE
                               : Fl_Native_File_Chooser::BROWSE_FILE);
        chooser.directory(start_folder().c_str());
        if (suggested)
            chooser.preset_file(suggested);
        return chooser.show() == 0 && chooser.filename() ? chooser.filename() : "";
    }

    void backup() {
        if (busy() || !project)
            return;
        apply_notes();
        if (path.empty()) {
            message("Salve o projeto", "Salve o projeto antes de criar uma cópia com documentos.");
            return;
        }
        auto file = artifact_file("Backup do projeto e documentos", "Backup Studio\t*.bpcbstudio",
                                  "projeto.bpcbstudio", true);
        if (file.empty())
            return;
        launch("backup", file, [file](json_object *) {
            message("Backup verificado",
                    file + "\n\nO projeto e os documentos foram relidos e conferidos por SHA-256.");
        });
    }

    void restore() {
        if (busy())
            return;
        auto file = artifact_file("Conferir backup do Studio", "Backup Studio\t*.bpcbstudio",
                                  nullptr, false);
        if (file.empty())
            return;
        launch("backup-preview", file, [this, file](json_object *preview) {
            json_object *restored = bc_get(preview, "project"), *board = bc_get(restored, "board");
            std::string text = std::string(bc_str(board, "pcb")) + " · " +
                               bc_str(board, "revision") + " · versão " + bc_str(board, "version") +
                               "\n\nIntegridade dos arquivos confirmada.\nDiferenças em relação ao "
                               "projeto aberto:\n";
            const char *keys[] = {"components", "devices", "signals", "tests", "documents"},
                       *labels[] = {"Componentes", "Dispositivos", "Sinais", "Testes",
                                    "Documentos"};
            for (unsigned k = 0; k < 5; k++) {
                auto *before = bc_get(project, keys[k]), *after = bc_get(restored, keys[k]);
                text += std::string(labels[k]) + ": " +
                        std::to_string(before ? json_object_array_length(before) : 0) + " → " +
                        std::to_string(json_object_array_length(after)) +
                        (json_object_equal(before, after) ? " (iguais)" : " (diferentes)") + "\n";
            }
            text += "\nA cópia será restaurada em uma pasta nova. O projeto atual continuará no "
                    "seu local.";
            if (!question("Prévia da restauração", text) || !discard())
                return;
            auto chosen = artifact_file("Nome da nova pasta restaurada", "Pasta nova\t*",
                                        "projeto-restaurado", true);
            if (chosen.empty())
                return;
            if (!project)
                project = bs_new();
            json_object_object_add(project, "restore_digest",
                                   json_object_new_string(bc_str(preview, "digest")));
            launch(
                "restore", file,
                [this, chosen](json_object *p) {
                    adopt(p, chosen + "/project.bpcb", false);
                    say("Projeto restaurado em pasta nova.");
                },
                chosen);
            json_object_object_del(project, "restore_digest");
        });
    }

    void manual() {
        if (busy() || !project)
            return;
        apply_notes();
        auto file =
            artifact_file("Gerar manual da bancada", "PDF\t*.pdf", "manual-bancada.pdf", true);
        if (file.empty())
            return;
        launch("manual", file, [file](json_object *) {
            message("Manual gerado",
                    file + "\n\nInclui versão, preparação, conexões, componentes, parâmetros, "
                           "fórmulas, etapas e critérios cadastrados.");
        });
    }

    void tools_page() {
        ui_label(240, 110, w() - 264, 42, "Ferramentas do projeto", 27);
        ui_label(240, 166, w() - 264, 56,
                 "Reaproveite procedimentos, gere a documentação e preserve uma cópia completa do "
                 "projeto.",
                 15, pal.muted);
        new Action(240, 244, 270, 40, "Reaproveitar teste", [this] {
            reuse();
        });
        new Action(530, 244, 270, 40, "Manual em PDF", [this] {
            manual();
        });
        new Action(240, 308, 270, 40, "Criar backup verificado", [this] {
            backup();
        });
        new Action(530, 308, 270, 40, "Conferir e restaurar backup", [this] {
            restore();
        });
        new Action(240, 372, 270, 40, "Configurar roteiro da sessão", [this] {
            edit_board();
        });
        ui_label(240, 444, w() - 280, 116,
                 "↶ Desfazer: Ctrl+Z · ↷ Refazer: Ctrl+Y\nDuplicar mantém uma cópia editável no "
                 "projeto.\nRenomear identificadores atualiza referências em fórmulas e etapas.\nA "
                 "sessão exportada requer BPCB 0.11 ou mais recente.",
                 15, pal.muted);
    }
