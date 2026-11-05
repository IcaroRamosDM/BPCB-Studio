#include "studio.h"
#include <FL/Fl.H>
#include <FL/platform.H>
#ifndef _WIN32
#include <X11/Xutil.h>
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <cairo.h>
#include <glib.h>
#include <chrono>
#include <stdexcept>
#include <csignal>
#ifndef _WIN32
extern char **environ;
#endif
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <fontconfig/fontconfig.h>
#include <algorithm>
#include <charconv>
#include <functional>
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <clocale>

#include "frame_editor.h"
#include "platform.inc"
#include "widgets.inc"
#include "picker.inc"
#include "form.inc"
#include "chart.inc"
#include "simulation.inc"
#include "document.inc"

struct Studio : Fl_Double_Window {
    json_object *project = nullptr;
    std::string path, worker, page = "overview", test_id, tab = "step";
    bool dirty = false, note_dirty = false;
    Fl_Group *header = nullptr, *sidebar = nullptr, *body = nullptr, *footer = nullptr;
    Fl_Box *status = nullptr;
    Grid *grid = nullptr;
    Fl_Text_Buffer *notes = nullptr;
    Fl_Text_Editor *note_editor = nullptr;
    Fl_Choice *test_choice = nullptr;
    bs_task *task = nullptr;
    std::function<void(json_object *)> task_callback;
    std::string operation;
    int row = -1;
#include "authoring.inc"
    unsigned draws = 0;

    void draw() override {
        draws++;
        Fl_Double_Window::draw();
    }

    Studio() : Fl_Double_Window(1280, 820, "BPCB Studio") {
        colors();
        color(pal.bg);
        size_range(1020, 700);
        char executable[4096];
        if (bs_worker_path(executable, nullptr) == 0)
            worker = executable;
        callback(
            [](Fl_Widget *, void *p) {
                ((Studio *)p)->close_requested();
            },
            this);
        render();
    }

    ~Studio() {
        if (task) {
            unwatch_work(task, bs_task_fd(task));
            bs_task_free(task);
        }
        release_notes();
        json_object_put(project);
    }

    void say(const std::string &s) {
        if (status)
            status->copy_label(s.c_str());
    }

    bool busy() {
        if (task) {
            say("Aguarde a operação em andamento.");
            return true;
        }
        return false;
    }

    void release_notes() {
        if (note_editor)
            note_editor->buffer(nullptr);
        delete notes;
        notes = nullptr;
        note_editor = nullptr;
    }

    void apply_notes() {
        if (!notes || !note_dirty || !project)
            return;
        char *txt = notes->text();
        json_object *b = bc_clone(bc_get(project, "board"));
        json_object_object_add(b, "notes", json_object_new_string(txt));
        free(txt);
        checkpoint();
        bc_error e = {{0}};
        if (bs_put(project, "board", "", -1, b, &e) < 0)
            message("Revisar notas", e.text);
        else {
            dirty = true;
            note_dirty = false;
        }
        json_object_put(b);
    }

    bool discard() {
        apply_notes();
        return !dirty || question("Descartar alterações não salvas?",
                                  "Há alterações neste projeto. Clique em Cancelar para salvá-las "
                                  "antes, ou em Continuar para descartá-las.");
    }

    void close_requested() {
        if (busy())
            return;
        if (discard())
            hide();
    }

    std::string start_folder() {
        return path.empty() ? home_folder() : parent_path(path);
    }

    void go(const std::string &p) {
        if (busy())
            return;
        apply_notes();
        page = p;
        row = -1;
        render();
    }

    void later(std::function<void()> f) {
        auto *fn = new std::function<void()>(std::move(f));
        Fl::add_timeout(
            0,
            [](void *p) {
                std::unique_ptr<std::function<void()>> cb((std::function<void()> *)p);
                (*cb)();
            },
            fn);
    }

    void launch(const std::string &op, const std::string &arg,
                std::function<void(json_object *)> done, const std::string &custom_path = "") {
        if (busy())
            return;
        operation = op;
        task_callback = std::move(done);
        task =
            bs_task_start(op.c_str(), project, (custom_path.empty() ? path : custom_path).c_str(),
                          arg.c_str(), worker.c_str());
        if (!task) {
            message("Operação não iniciada", "Não foi possível criar a tarefa.");
            return;
        }
        header->deactivate();
        sidebar->deactivate();
        body->deactivate();
        footer->deactivate();
        say("Processando…");
        watch_work(
            task, bs_task_fd(task),
            [](int, void *ctx) {
                ((Studio *)ctx)->task_done();
            },
            this);
    }

    void task_done() {
        if (!task || !bs_task_done(task))
            return;
        unwatch_work(task, bs_task_fd(task));
        bc_error e = {{0}};
        json_object *r = bs_task_result(task, &e);
        bs_task_free(task);
        task = nullptr;
        header->activate();
        sidebar->activate();
        body->activate();
        footer->activate();
        auto done = std::move(task_callback);
        task_callback = {};
        if (!r) {
            say("Operação não concluída.");
            message("Confira estas informações", e.text);
        } else {
            say("Operação concluída.");
            if (done)
                done(r);
        }
        json_object_put(r);
    }

    void adopt(json_object *p, const std::string &file, bool changed) {
        undo_states.clear();
        redo_states.clear();
        release_notes();
        json_object_put(project);
        project = json_object_get(p);
        path = file;
        dirty = changed;
        note_dirty = false;
        page = "overview";
        row = -1;
        test_id = "";
        tab = "step";
        render();
    }

    void new_project(bool example = false) {
        if (busy() || !discard())
            return;
        json_object *p = example ? bs_example() : bs_new();
        if (!example) {
            json_object *b = Form::edit("board", p, bc_get(p, "board"), "Qual placa será testada?");
            if (!b) {
                json_object_put(p);
                return;
            }
            bc_error e = {{0}};
            bs_put(p, "board", "", -1, b, &e);
            json_object_put(b);
        }
        adopt(p, "", true);
        json_object_put(p);
        save();
    }

    void open() {
        if (busy() || !discard())
            return;
        std::string f = FilePicker::pick("Abrir projeto BPCB Studio", start_folder(), "project");
        if (f.empty())
            return;
        launch(
            "load", "",
            [this, f](json_object *r) {
                adopt(r, f, false);
            },
            f);
    }

    void save() {
        if (!project || busy())
            return;
        apply_notes();
        std::string f = path;
        if (f.empty())
            f = FilePicker::pick("Salvar projeto BPCB Studio", start_folder(), "project", true);
        if (f.empty())
            return;
        launch(
            "save", "",
            [this, f](json_object *) {
                path = f;
                dirty = false;
                render();
                say("Projeto salvo em " + f);
            },
            f);
    }

    void import_package() {
        if (busy() || !discard())
            return;
        std::string package =
            FilePicker::pick("Importar bancada editável", start_folder(), "package");
        if (package.empty())
            return;
        std::string destination =
            FilePicker::pick("Salvar projeto importado", start_folder(), "project", true);
        if (destination.empty())
            return;
        launch(
            "import", package,
            [this, destination](json_object *r) {
                adopt(r, destination, false);
            },
            destination);
    }

    void validate() {
        if (!project || busy())
            return;
        apply_notes();
        launch("validate", "", [this](json_object *r) {
            if (!json_object_array_length(r)) {
                message("Projeto válido",
                        "As referências, os limites e as etapas estão consistentes.\n\nA validação "
                        "do projeto não substitui a validação elétrica da PCB. Use Simular para "
                        "revisar as instruções e as respostas esperadas.");
                return;
            }
            validation_list(r);
        });
    }

    bool ready() {
        if (!project)
            return false;
        apply_notes();
        if (path.empty()) {
            message("Salve o projeto", "Clique em Salvar e escolha um nome para o projeto .bpcb "
                                       "antes de exportar ou simular.");
            return false;
        }
        bc_error e = {{0}};
        if (bs_check(project, &e) < 0) {
            validate();
            return false;
        }
        return true;
    }

    void export_package() {
        if (busy() || !ready())
            return;
        std::string f = FilePicker::pick("Exportar para o BPCB", start_folder(), "package", true);
        if (f.empty())
            return;
        launch("export", f, [f](json_object *) {
            message("Bancada exportada",
                    f + "\n\nO pacote inclui as etapas, o executor em C, os valores dos "
                        "componentes e os documentos.\n\nNo BPCB, use Instalar bancada e selecione "
                        "este arquivo. Plataforma de execução: " BS_PLATFORM_LABEL
                        ". Para outro sistema, abra o .bpcb nesse sistema e exporte novamente.");
        });
    }

    void simulate(bool debug = false) {
        if (busy() || !ready())
            return;
        json_object *tests = bc_get(project, "tests");
        if (test_id.empty() && json_object_array_length(tests))
            test_id = bc_str(json_object_array_get_idx(tests, 0), "id");
        std::string selected = test_id;
        if (!bs_find(tests, selected.c_str())) {
            message("Selecione um teste",
                    "Abra Testes e etapas e selecione o teste que deseja simular.");
            return;
        }
        launch("simulate", "", [this, selected, debug](json_object *r) {
            Simulation s(bc_str(r, "root"), bc_str(bc_get(project, "board"), "id"), selected, debug,
                         bc_get(bs_find(bc_get(project, "tests"), selected.c_str()), "steps"));
            s.show();
            while (s.shown())
                Fl::wait();
            say("Simulação encerrada. Nenhuma operação física foi executada.");
        });
    }

    std::string current_kind() {
        if (page == "components")
            return "component";
        if (page == "devices")
            return "device";
        if (page == "signals")
            return "signal";
        if (page == "documents")
            return "document";
        return tab;
    }

    json_object *current_rows() {
        return bs_rows(project, current_kind().c_str(), test_id.c_str());
    }

    json_object *selected_item() {
        auto *a = current_rows();
        return a && row >= 0 && (size_t)row < json_object_array_length(a)
                   ? json_object_array_get_idx(a, (size_t)row)
                   : nullptr;
    }

    json_object *draft(const std::string &kind) {
        json_object *v = bs_defaults(kind.c_str());
        const char *key = kind == "component" ? "ref" : "id";
        if (kind != "document" && kind != "board") {
            json_object *a = bs_rows(project, kind.c_str(), test_id.c_str());
            unsigned i = 1;
            std::string id;
            do {
                id = (kind == "component"   ? "R"
                      : kind == "test"      ? "teste"
                      : kind == "step"      ? "etapa"
                      : kind == "signal"    ? "sinal"
                      : kind == "device"    ? "dispositivo"
                      : kind == "parameter" ? "parametro"
                                            : "previsao") +
                     std::to_string(i++);
            } while (bs_find(a, id.c_str()));
            json_object_object_add(v, key, json_object_new_string(id.c_str()));
        }
        return v;
    }

    void edit_item(bool add) {
        if (busy())
            return;
        std::string kind = current_kind();
        json_object *old = add ? nullptr : selected_item();
        if (!add && !old)
            return;
        if (kind == "document") {
            if (add) {
                add_document();
                return;
            }
            json_object *d = bc_clone(old);
            json_object_object_add(
                d, "source",
                json_object_new_string((parent_path(path) + "/" + bc_str(old, "file")).c_str()));
            json_object *v = Form::edit("document", project, d, "Descrição do documento");
            json_object_put(d);
            if (v) {
                checkpoint();
                json_object_object_add(old, "title", json_object_new_string(bc_str(v, "title")));
                json_object_object_add(old, "role", json_object_new_string(bc_str(v, "role")));
                json_object_put(v);
                dirty = true;
                render();
            }
            return;
        }
        json_object *d = add ? draft(kind) : json_object_get(old);
        json_object *test = bs_find(bc_get(project, "tests"), test_id.c_str());
        if (test)
            json_object_object_add(project, "editing_test", json_object_get(test));
        json_object *v = Form::edit(kind, project, d,
                                    std::string(add ? "Adicionar · " : "Editar · ") +
                                        bs_kind_label(kind.c_str()));
        json_object_object_del(project, "editing_test");
        json_object_put(d);
        if (!v)
            return;
        checkpoint();
        bc_error e = {{0}};
        if (bs_put(project, kind.c_str(), test_id.c_str(), add ? -1 : row, v, &e) < 0)
            message("Revisar item", e.text);
        else {
            dirty = true;
            row = -1;
            render();
        }
        json_object_put(v);
    }

    void remove_item() {
        if (busy() || !selected_item())
            return;
        if (!question("Remover item?", "O item será retirado do projeto. As referências a ele "
                                       "serão verificadas quando você clicar em Validar."))
            return;
        checkpoint();
        bc_error e = {{0}};
        if (bs_delete(project, current_kind().c_str(), test_id.c_str(), row, &e) < 0)
            message("Não foi possível remover", e.text);
        else {
            dirty = true;
            row = -1;
            render();
        }
    }

    void move_item(int dir) {
        if (busy() || row < 0)
            return;
        checkpoint();
        if (bs_move(current_rows(), row, dir) == 0) {
            row += dir;
            dirty = true;
            refresh_rows();
            grid->select_row(row);
        }
    }

    void edit_board() {
        json_object *v = Form::edit("board", project, bc_get(project, "board"),
                                    "Identificação e preparação da placa");
        if (!v)
            return;
        checkpoint();
        bc_error e = {{0}};
        if (bs_put(project, "board", "", -1, v, &e) < 0)
            message("Revisar placa", e.text);
        else {
            dirty = true;
            render();
        }
        json_object_put(v);
    }

    void edit_test(bool add) {
        json_object *old = bs_find(bc_get(project, "tests"), test_id.c_str());
        if (!old && !add)
            return;
        json_object *d = add ? draft("test") : json_object_get(old);
        if (old)
            json_object_object_add(project, "editing_test", json_object_get(old));
        json_object *v = Form::edit("test", project, d,
                                    add ? "Que teste deseja criar?" : "Configuração do teste");
        json_object_object_del(project, "editing_test");
        json_object_put(d);
        if (!v)
            return;
        int idx = -1;
        json_object *a = bc_get(project, "tests");
        for (size_t i = 0; i < json_object_array_length(a); i++)
            if (json_object_array_get_idx(a, i) == old)
                idx = (int)i;
        checkpoint();
        bc_error e = {{0}};
        if (bs_put(project, "test", "", add ? -1 : idx, v, &e) < 0)
            message("Revisar teste", e.text);
        else {
            test_id = bc_str(v, "id");
            dirty = true;
            row = -1;
            render();
        }
        json_object_put(v);
    }

    void remove_test() {
        auto *a = bc_get(project, "tests");
        int idx = -1;
        for (size_t i = 0; i < json_object_array_length(a); i++)
            if (!strcmp(bc_str(json_object_array_get_idx(a, i), "id"), test_id.c_str()))
                idx = (int)i;
        if (idx < 0 ||
            !question("Remover teste?", "O teste e suas etapas serão removidos do projeto."))
            return;
        checkpoint();
        bc_error e = {{0}};
        if (bs_delete(project, "test", "", idx, &e) == 0) {
            test_id = "";
            dirty = true;
            render();
        }
    }

    void add_document() {
        if (path.empty()) {
            message("Salve o projeto", "Salve primeiro. Os documentos serão copiados para assets/ "
                                       "na mesma pasta do projeto.");
            return;
        }
        std::string source =
            FilePicker::pick("Adicionar esquemático ou PCB Print", start_folder(), "document");
        if (source.empty())
            return;
        json_object *d = bs_defaults("document");
        json_object_object_add(d, "source", json_object_new_string(source.c_str()));
        json_object_object_add(d, "title", json_object_new_string(basename_path(source).c_str()));
        json_object *v = Form::edit("document", project, d, "Descreva o documento");
        json_object_put(d);
        if (!v)
            return;
        json_object_object_add(project, "pending_document", json_object_get(v));
        std::string file = bc_str(v, "source");
        json_object_put(v);
        launch("document", file, [this](json_object *r) {
            checkpoint();
            json_object_put(project);
            project = json_object_get(r);
            dirty = true;
            render();
        });
        json_object_object_del(project, "pending_document");
    }

    void view_document() {
        json_object *d = selected_item();
        if (!d || current_kind() != "document")
            return;
        std::string file = parent_path(path) + "/" + bc_str(d, "file");
        DocumentViewer viewer(file, bc_str(d, "title"));
        viewer.show();
        while (viewer.shown())
            Fl::wait();
    }

    void refresh_rows() {
        if (!grid)
            return;
        std::vector<std::vector<std::string>> lines;
        json_object *a = current_rows();
        for (size_t i = 0; a && i < json_object_array_length(a); i++) {
            json_object *v = json_object_array_get_idx(a, i);
            std::string k = current_kind();
            if (k == "component")
                lines.push_back({bc_str(v, "ref"), bc_str(v, "value"), bc_str(v, "description")});
            else if (k == "device") {
                char address[30];
                snprintf(address, sizeof address, "0x%02X", (unsigned)bc_num(v, "address"));
                lines.push_back({bc_str(v, "name"),
                                 std::string(bc_str(v, "driver")) + " · " + address,
                                 bc_str(v, "transport")});
            } else if (k == "signal")
                lines.push_back(
                    {bc_str(v, "label"),
                     std::string(bc_str(v, "location")) + " / " + bc_str(v, "return"),
                     std::string(bc_str(v, "direction")) + " · " + bc_str(v, "device")});
            else if (k == "step") {
                std::string type = bc_str(v, "type"), action = type;
                json_object *catalog = bs_catalog("step"),
                            *choices = bc_get(json_object_array_get_idx(catalog, 2), "options");
                // Look up the action by field key; the catalogue order is not part of the UI
                // contract.
                for (size_t j = 0; j < json_object_array_length(catalog); j++)
                    if (!strcmp(bc_str(json_object_array_get_idx(catalog, j), "key"), "type"))
                        choices = bc_get(json_object_array_get_idx(catalog, j), "options");
                for (size_t j = 0; choices && j < json_object_array_length(choices); j++) {
                    json_object *c = json_object_array_get_idx(choices, j);
                    if (!strcmp(bc_str(c, "id"), type.c_str()))
                        action = bc_str(c, "label");
                }
                json_object_put(catalog);
                std::string detail = bc_str(v, "expression");
                if (type == "confirm" || type == "manual" || type == "message")
                    detail = bc_str(v, "message");
                else if (type == "read" || type == "wait")
                    detail = std::string(bc_str(v, "signal")) + " → " + bc_str(v, "variable");
                else if (type == "write")
                    detail = std::string(bc_str(v, "signal")) + " = " + bc_str(v, "expression");
                else if (type == "initialize")
                    detail = bc_str(v, "device");
                else if (type == "delay")
                    detail = std::to_string(bc_num(v, "seconds")) + " s";
                lines.push_back(
                    {std::to_string(i + 1) + ". " + bc_str(v, "title"), action, detail});
            } else if (k == "parameter") {
                char value[150];
                snprintf(value, sizeof value, "%.8g %s (%.8g..%.8g)", bc_num(v, "default"),
                         bc_str(v, "unit"), bc_num(v, "min"), bc_num(v, "max"));
                lines.push_back({bc_str(v, "label"), value, bc_str(v, "help")});
            } else if (k == "formula")
                lines.push_back({bc_str(v, "id"), bc_str(v, "expression"),
                                 "Calculada antes do teste; use componentes e parâmetros."});
            else if (k == "document")
                lines.push_back(
                    {bc_str(v, "title"), bc_str(v, "role"), basename_path(bc_str(v, "file"))});
        }
        std::string k = current_kind();
        grid->data(k == "component" ? std::vector<std::string>{"COMPONENTE", "VALOR", "DESCRIÇÃO"}
                   : k == "step"    ? std::vector<std::string>{"ETAPA", "AÇÃO", "CONFIGURAÇÃO"}
                   : k == "parameter"
                       ? std::vector<std::string>{"PARÂMETRO", "VALOR INICIAL", "ORIENTAÇÃO"}
                       : std::vector<std::string>{"NOME", "CONFIGURAÇÃO", "DETALHE"},
                   lines,
                   k == "component" ? std::vector<double>{.15, .22, .63}
                                    : std::vector<double>{.28, .29, .43});
    }

    void list_page(const std::string &title, const std::string &intro, int top = 0) {
        int x = 240, y = 110 + top, width = w() - 264;
        ui_label(x, y, width, 40, title, 26);
        ui_label(x, y + 48, width, 54, intro, 14, pal.muted);
        new Action(
            x, y + 118, 140, 36, "Adicionar",
            [this] {
                edit_item(true);
            },
            true);
        new Action(x + 152, y + 118, 110, 36, "Editar", [this] {
            edit_item(false);
        });
        new Action(x + 274, y + 118, 110, 36, "Remover", [this] {
            remove_item();
        });
        if (current_kind() == "document")
            new Action(x + 400, y + 118, 148, 36, "Visualizar", [this] {
                view_document();
            });
        if (current_kind() == "step") {
            new Action(x + 400, y + 118, 80, 36, "↑", [this] {
                move_item(-1);
            });
            new Action(x + 492, y + 118, 80, 36, "↓", [this] {
                move_item(1);
            });
        }
        if (current_kind() != "document")
            new Action(x + width - 148, y + 118, 148, 36, "Duplicar item", [this] {
                duplicate(false);
            });
        grid = new Grid(x, y + 170, width, h() - (y + 170) - 84);
        grid->selected = [this](int i, bool twice) {
            row = i;
            if (twice)
                later([this] {
                    if (current_kind() == "document")
                        view_document();
                    else
                        edit_item(false);
                });
        };
        refresh_rows();
    }

    void render() {
        release_notes();
        clear();
        header = sidebar = body = footer = nullptr;
        grid = nullptr;
        status = nullptr;
        test_choice = nullptr;
        color(pal.bg);
        begin();
        header = new Fl_Group(0, 0, w(), 88);
        header->resizable(nullptr);
        ui_label(24, 22, 330, 32, "BPCB Studio", 25);
        ui_label(24, 59, 420, 20, "CRIAÇÃO DE TESTES DE PCB", 11, pal.muted);
        int right = w() - 24;
        new Action(right - 142, 24, 142, 36, dark ? "Tema escuro" : "Tema claro", [this] {
            later([this] {
                apply_notes();
                dark = !dark;
                colors();
                render();
            });
        });
        new Action(right - 254, 24, 100, 36, "Salvar", [this] {
            save();
        });
        new Action(right - 366, 24, 100, 36, "Abrir", [this] {
            open();
        });
        new Action(right - 478, 24, 100, 36, "Novo", [this] {
            new_project();
        });
        auto *undo_button = new Action(348, 26, 52, 34, "↶", [this] {
            history(false);
        });
        undo_button->shortcut(FL_CTRL + 'z');
        if (undo_states.empty())
            undo_button->deactivate();
        auto *redo_button = new Action(406, 26, 52, 34, "↷", [this] {
            history(true);
        });
        redo_button->shortcut(FL_CTRL + 'y');
        if (redo_states.empty())
            redo_button->deactivate();
        header->end();
        sidebar = new Fl_Group(16, 104, 196, h() - 184);
        sidebar->resizable(nullptr);
        const std::vector<std::pair<std::string, std::string>> nav = {
            {"overview", "Visão geral"},  {"components", "Componentes"},
            {"devices", "Dispositivos"},  {"signals", "Conexões e sinais"},
            {"tests", "Testes e etapas"}, {"documents", "Documentos"},
            {"notes", "Notas"},           {"tools", "Ferramentas"}};
        for (size_t i = 0; i < nav.size(); i++) {
            auto item = nav[i];
            std::string title = item.second;
            if (item.first == "notes" && project && *bc_str(bc_get(project, "board"), "notes"))
                title += " ●";
            auto *b = new Action(
                16, 110 + (int)i * 50, 196, 40, title.c_str(),
                [this, id = item.first] {
                    later([this, id] {
                        go(id);
                    });
                },
                page == item.first);
            if (!project && item.first != "overview")
                b->deactivate();
        }
        new Action(16, h() - 144, 196, 40, "Importar .bancada", [this] {
            import_package();
        });
        sidebar->end();
        body = new Fl_Group(224, 98, w() - 224, h() - 174);
        body->resizable(nullptr);
        if (!project) {
            ui_label(264, 142, w() - 316, 76, "Uma bancada começa\ncom um procedimento claro.", 30);
            ui_label(264, 242, w() - 320, 78,
                     "Cadastre a placa, descreva as conexões e monte os testes por etapas.\nO BPCB "
                     "Studio prepara o pacote; o BPCB executa os testes.",
                     16, pal.muted);
            new Action(
                264, 350, 238, 46, "Criar projeto",
                [this] {
                    new_project();
                },
                true);
            new Action(520, 350, 238, 46, "Abrir exemplo sem hardware", [this] {
                new_project(true);
            });
            ui_label(264, 440, w() - 316, 148,
                     "1   Identifique a placa e seus componentes.\n\n2   Defina medições, comandos "
                     "e pausas de confirmação.\n\n3   Valide, simule e exporte o arquivo .bancada.",
                     16);
        } else if (page == "overview") {
            auto *b = bc_get(project, "board");
            ui_label(240, 110, w() - 276, 42,
                     std::string(bc_str(b, "pcb")) + " · " + bc_str(b, "revision"), 27);
            ui_label(240, 160, w() - 276, 48, path.empty() ? "Projeto ainda não salvo" : path, 13,
                     pal.muted);
            new Action(240, 218, 240, 38, "Editar identificação da placa", [this] {
                edit_board();
            });
            auto *scroll = new Fl_Scroll(240, 278, w() - 264, h() - 366);
            scroll->type(Fl_Scroll::VERTICAL);
            int yy = 290;
            int ww = w() - 304;
            const std::vector<std::pair<std::string, std::string>> guide = {
                {"Componentes", "Quais valores montados influenciam os cálculos? Cadastre "
                                "resistores, capacitores e outros componentes substituíveis."},
                {"Dispositivos e sinais", "Qual módulo conversa com a placa? Quais canais, "
                                          "testpoints, retornos e limites serão usados?"},
                {"Testes e etapas",
                 "O que o operador deve conectar, informar ou confirmar? Adicione medições, "
                 "comandos, condições e resultados na ordem desejada."},
                {"Revisão antes de distribuir",
                 "Valide as referências. Simule com valores conhecidos. Exporte o pacote e "
                 "instale-o no BPCB."}};
            for (auto &g : guide) {
                ui_label(254, yy, ww, 30, g.first, 18);
                ui_label(254, yy + 36, ww, 64, g.second, 14, pal.muted);
                yy += 120;
            }
            ui_label(254, yy, ww, 28, "Preparação e conexões da placa", 18);
            int nh = std::max(70, (int)wrap(bc_str(b, "connections"), ww).size() * 20);
            ui_label(254, yy + 40, ww, nh, bc_str(b, "connections"), 14);
            scroll->end();
        } else if (page == "components")
            list_page("Componentes substituíveis",
                      "Valores e descrições seguem no pacote. R, C e indutores podem ser usados "
                      "nas fórmulas pelo identificador.");
        else if (page == "devices")
            list_page("Dispositivos da placa",
                      "Escolha o circuito e o transporte. O catálogo atual inclui TCA9534A, "
                      "MCP4728, ADS7924 e registros I²C simples.");
        else if (page == "signals")
            list_page("Conexões e sinais", "Defina onde medir ou conectar, a referência elétrica, "
                                           "os limites e o comando de encerramento de cada saída.");
        else if (page == "documents")
            list_page("Esquemáticos e PCB Prints",
                      "Adicione PDF ou SVG. O pacote preserva o original e inclui a versão "
                      "visualizável pelo BPCB.");
        else if (page == "tools")
            tools_page();
        else if (page == "notes") {
            ui_label(240, 110, w() - 264, 42, "Notas da bancada", 27);
            ui_label(240, 168, w() - 264, 58,
                     "Orientações adicionais para quem executar os testes. O indicador ● aparece "
                     "somente quando há notas.",
                     14, pal.muted);
            notes = new Fl_Text_Buffer();
            notes->text(bc_str(bc_get(project, "board"), "notes"));
            note_editor = new Fl_Text_Editor(240, 252, w() - 264, h() - 394);
            note_editor->buffer(notes);
            note_editor->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
            note_editor->box(FL_BORDER_BOX);
            note_editor->color(pal.card);
            note_editor->textcolor(pal.ink);
            note_editor->textfont(FL_HELVETICA);
            note_editor->textsize(15);
            note_dirty = false;
            notes->add_modify_callback(
                [](int, int a, int d, int, const char *, void *ctx) {
                    if (a || d)
                        ((Studio *)ctx)->note_dirty = true;
                },
                this);
            new Action(
                w() - 218, h() - 126, 194, 36, "Aplicar notas",
                [this] {
                    apply_notes();
                    render();
                    say("Notas aplicadas. Salve o projeto para persistir.");
                },
                true);
        } else if (page == "tests") {
            json_object *tests = bc_get(project, "tests");
            if (!bs_find(tests, test_id.c_str()))
                test_id = json_object_array_length(tests)
                              ? bc_str(json_object_array_get_idx(tests, 0), "id")
                              : "";
            ui_label(240, 106, w() - 264, 36, "Testes da bancada", 25);
            test_choice = new Fl_Choice(240, 154, w() - 624, 38);
            choice_style(test_choice);
            int current = 0;
            for (size_t i = 0; i < json_object_array_length(tests); i++) {
                json_object *t = json_object_array_get_idx(tests, i);
                test_choice->add(bc_str(t, "title"));
                if (!strcmp(bc_str(t, "id"), test_id.c_str()))
                    current = (int)i;
            }
            test_choice->value(current);
            test_choice->callback(
                [](Fl_Widget *, void *ctx) {
                    auto *s = (Studio *)ctx;
                    int i = s->test_choice->value();
                    auto *a = bc_get(s->project, "tests");
                    if (i >= 0 && (size_t)i < json_object_array_length(a)) {
                        s->test_id = bc_str(json_object_array_get_idx(a, (size_t)i), "id");
                        s->row = -1;
                        s->later([s] {
                            s->render();
                        });
                    }
                },
                this);
            new Action(
                w() - 366, 154, 100, 38, "Novo teste",
                [this] {
                    edit_test(true);
                },
                true);
            new Action(w() - 254, 154, 108, 38, "Configurar", [this] {
                edit_test(false);
            });
            new Action(w() - 134, 154, 110, 38, "Remover", [this] {
                remove_test();
            });
            if (!test_id.empty()) {
                new Action(
                    240, 212, 130, 34, "Etapas",
                    [this] {
                        tab = "step";
                        row = -1;
                        render();
                    },
                    tab == "step");
                new Action(
                    382, 212, 148, 34, "Parâmetros",
                    [this] {
                        tab = "parameter";
                        row = -1;
                        render();
                    },
                    tab == "parameter");
                new Action(
                    542, 212, 148, 34, "Previsões",
                    [this] {
                        tab = "formula";
                        row = -1;
                        render();
                    },
                    tab == "formula");
                new Action(706, 212, 164, 34, "Duplicar teste", [this] {
                    duplicate(true);
                });
                list_page(tab == "step"        ? "Sequência do teste"
                          : tab == "parameter" ? "O que o operador pode ajustar?"
                                               : "Cálculos antes da execução",
                          tab == "step"
                              ? "Cada linha é uma ação. Use pausas para instruções e medições; as "
                                "condições podem interromper ou desviar o fluxo."
                          : tab == "parameter" ? "Defina os campos do BPCB, com valor inicial, "
                                                 "unidade, limites e explicação."
                                               : "Use expressões matemáticas e nomes dos "
                                                 "componentes/parâmetros. Exemplo: tensao / R1.",
                          162);
            } else
                ui_label(240, 278, w() - 264, 80,
                         "Crie o primeiro teste. Depois adicione parâmetros, previsões e as etapas "
                         "do procedimento.",
                         17, pal.muted);
        }
        body->end();
        footer = new Fl_Group(0, h() - 64, w(), 64);
        footer->resizable(nullptr);
        status = ui_label(18, h() - 45, w() - 700, 38,
                          project ? (dirty ? "Alterações não salvas." : "Projeto salvo.")
                                  : "Pronto para criar uma bancada.",
                          13, pal.muted);
        auto *v = new Action(w() - 512, h() - 50, 140, 36, "Validar", [this] {
            validate();
        });
        auto *d = new Action(w() - 664, h() - 50, 140, 36, "Depurar", [this] {
            simulate(true);
        });
        auto *s = new Action(w() - 360, h() - 50, 154, 36, "Simular teste", [this] {
            simulate();
        });
        auto *e = new Action(
            w() - 194, h() - 50, 170, 36, "Exportar .bancada",
            [this] {
                export_package();
            },
            true);
        if (!project) {
            v->deactivate();
            s->deactivate();
            d->deactivate();
            e->deactivate();
        }
        footer->end();
        end();
        resizable(nullptr);
        redraw();
    }

    void resize(int x, int y, int w, int h) override {
        bool size = w != this->w() || h != this->h();
        if (size)
            apply_notes();
        Fl_Double_Window::resize(x, y, w, h);
        if (size && header && !task)
            render();
    }
};

#ifndef _WIN32
#include "check_ui.inc"
#endif

int main(int argc, char **argv) {
    bs_platform_init();
    bc_set_wakeup([] {
        Fl::awake();
    });
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    FcInit();
    Fl::scheme("gtk+");
    Fl::lock();
    Fl_Tooltip::disable();
    Fl::set_font(FL_HELVETICA, "DejaVu Sans");
    Fl::set_font(FL_HELVETICA_BOLD, "DejaVu Sans Bold");
    Studio app;
#ifndef _WIN32
    if (argc == 3 && !strcmp(argv[1], "--ui-check"))
        return ui_check(app, argv[2]);
#endif
    if (argc == 2) {
        bc_error e = {{0}};
        json_object *p = bs_load(argv[1], &e);
        if (p) {
            app.adopt(p, argv[1], false);
            json_object_put(p);
        } else
            message("Abrir projeto", e.text);
    }
    app.show();
    return Fl::run();
}
