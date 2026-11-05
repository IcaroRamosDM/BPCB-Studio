#pragma once
#include "workbench.h"
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>
#include <cerrno>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

/* Independent editor: owns its draft; Cancel never mutates its caller. */
struct WorkbenchFrameEditor : Fl_Double_Window {
    json_object *draft = nullptr, *result = nullptr;
    Fl_Input *name, *length, *prefix, *offset, *start, *count, *preview_bytes;
    Fl_Choice *checksum, *endian;
    Fl_Hold_Browser *rows;
    Fl_Text_Buffer buffer;
    Fl_Text_Display *preview;
    std::vector<Fl_Widget *> widgets;

    static Fl_Input *input(int x, int y, int w, const char *label, const char *value) {
        auto *r = new Fl_Input(x, y, w, 32);
        r->copy_label(label);
        r->align(FL_ALIGN_TOP_LEFT);
        r->value(value);
        r->textsize(14);
        return r;
    }

    static Fl_Choice *choice(int x, int y, int w, const char *label, const char *values) {
        auto *r = new Fl_Choice(x, y, w, 32);
        r->copy_label(label);
        r->align(FL_ALIGN_TOP_LEFT);
        r->add(values);
        r->value(0);
        return r;
    }

    template <class Fn> void button(int x, int y, int w, const char *l, Fn fn) {
        auto *b = new Fl_Button(x, y, w, 34, l);
        auto *cb = new std::function<void()>(fn);
        b->user_data(cb);
        b->callback([](Fl_Widget *w, void *) {
            (*static_cast<std::function<void()> *>(w->user_data()))();
        });
        widgets.push_back(b);
    }

    void error(const char *s) {
        buffer.text(s);
    }

    static bool number(const char *s, double &v) {
        if (!s || !*s)
            return false;
        char *end = nullptr;
        errno = 0;
        v = strtod(s, &end);
        return end != s && !*end && !errno && std::isfinite(v);
    }

    WorkbenchFrameEditor(json_object *initial)
        : Fl_Double_Window(900, 820, "Formato de quadro UART"),
          draft(initial && bc_frame_validate(initial, nullptr) == 0 ? bc_clone(initial)
                                                                    : bc_frame_example()) {
        size_range(900, 700);
        begin();
        auto *scroll = new Fl_Scroll(8, 8, 884, 750);
        scroll->type(Fl_Scroll::VERTICAL);
        auto *body = new Fl_Group(8, 8, 854, 1120);
        body->resizable(nullptr);
        name = input(28, 40, 520, "Nome do formato", bc_str(draft, "name"));
        length = input(570, 40, 120, "Bytes totais",
                       std::to_string((int)bc_num(draft, "length")).c_str());
        prefix = input(28, 105, 800, "Prefixo (hexadecimal; opcional)", bc_str(draft, "prefix"));
        rows = new Fl_Hold_Browser(28, 160, 800, 180);
        rows->textsize(14);
        rows->tooltip("Identificador · posição em bytes · tipo · unidade");
        button(28, 352, 150, "Adicionar campo", [this] {
            edit_field(false);
        });
        button(192, 352, 150, "Editar campo", [this] {
            edit_field(true);
        });
        button(356, 352, 150, "Remover campo", [this] {
            int i = rows->value() - 1;
            if (i < 0)
                return;
            json_object_array_del_idx(bc_get(draft, "fields"), (size_t)i, 1);
            refresh();
        });
        checksum = choice(28, 430, 260, "Checksum", "Nenhum|Soma 8 bits|XOR 8 bits|CRC-16 Modbus");
        endian = choice(310, 430, 200, "Ordem do CRC", "Little endian|Big endian");
        auto *c = bc_get(draft, "checksum");
        const char *types[] = {"none", "sum8", "xor8", "crc16_modbus"};
        for (int i = 0; i < 4; i++)
            if (!strcmp(types[i], bc_str(c, "type")))
                checksum->value(i);
        endian->value(!strcmp(bc_str(c, "endian"), "big"));
        offset = input(28, 498, 240, "Posição do checksum",
                       std::to_string(c ? (int)bc_num(c, "offset") : 7).c_str());
        start = input(290, 498, 240, "Início da faixa coberta",
                      std::to_string((int)bc_num(c, "start")).c_str());
        count = input(552, 498, 276, "Bytes cobertos",
                      std::to_string(c ? (int)bc_num(c, "length") : 7).c_str());
        preview_bytes = input(28, 565, 800, "Quadro para prévia (hex; sem transmitir)",
                              "AA 55 C4 09 E4 0C 01 BD");
        button(28, 613, 180, "Decodificar prévia", [this] {
            decode();
        });
        button(224, 613, 180, "Importar formato…", [this] {
            load();
        });
        button(420, 613, 180, "Exportar formato…", [this] {
            save_file();
        });
        preview = new Fl_Text_Display(28, 670, 800, 400);
        preview->buffer(&buffer);
        preview->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        preview->textsize(14);
        error("Posições começam em zero. Campos, prefixo e checksum não podem se sobrepor. A "
              "prévia não acessa portas.");
        body->end();
        scroll->end();
        button(560, 776, 140, "Cancelar", [this] {
            hide();
        });
        button(714, 776, 160, "Aplicar formato", [this] {
            auto *r = collect();
            if (!r)
                return;
            json_object_put(result);
            result = r;
            hide();
        });
        end();
        resizable(scroll);
        set_modal();
        refresh();
        int sx = 0, sy = 0, sw = 0, sh = 0;
        Fl::screen_work_area(sx, sy, sw, sh, 0);
        (void)sx;
        (void)sy;
        (void)sw;
        if (sh > 0 && sh < 860)
            size(w(), std::max(700, std::min(820, sh - 40)));
    }

    ~WorkbenchFrameEditor() {
        preview->buffer(nullptr);
        for (auto *w : widgets)
            delete static_cast<std::function<void()> *>(w->user_data());
        json_object_put(draft);
        json_object_put(result);
    }

    void refresh() {
        int selected = rows->value();
        rows->clear();
        auto *a = bc_get(draft, "fields");
        for (size_t i = 0; i < json_object_array_length(a); i++) {
            auto *f = json_object_array_get_idx(a, i);
            std::string text = std::string(bc_str(f, "id")) + " · byte " +
                               std::to_string((int)bc_num(f, "offset")) + " · " +
                               bc_str(f, "type") + " · " + bc_str(f, "unit");
            rows->add(text.c_str());
        }
        rows->value(selected);
    }

    json_object *collect() {
        auto *r = bc_clone(draft);
        json_object_object_add(r, "name", json_object_new_string(name->value()));
        json_object_object_add(r, "prefix", json_object_new_string(prefix->value()));
        double n;
        if (!number(length->value(), n) || n < 1 || n > 4096 || floor(n) != n) {
            json_object_put(r);
            error("Comprimento deve ser inteiro entre 1 e 4096 bytes.");
            return nullptr;
        }
        json_object_object_add(r, "length", json_object_new_int((int)n));
        auto *c = json_object_new_object();
        const char *types[] = {"none", "sum8", "xor8", "crc16_modbus"};
        json_object_object_add(c, "type", json_object_new_string(types[checksum->value()]));
        if (checksum->value()) {
            Fl_Input *inputs[] = {offset, start, count};
            const char *keys[] = {"offset", "start", "length"};
            for (int i = 0; i < 3; i++) {
                if (!number(inputs[i]->value(), n) || n < 0 || n > 4096 || floor(n) != n) {
                    json_object_put(r);
                    json_object_put(c);
                    error("Posições e tamanho do checksum devem ser inteiros válidos.");
                    return nullptr;
                }
                json_object_object_add(c, keys[i], json_object_new_int((int)n));
            }
            if (checksum->value() == 3)
                json_object_object_add(c, "endian",
                                       json_object_new_string(endian->value() ? "big" : "little"));
        }
        json_object_object_add(r, "checksum", c);
        bc_error e = {{0}};
        if (bc_frame_validate(r, &e) < 0) {
            error(e.text);
            json_object_put(r);
            return nullptr;
        }
        return r;
    }

    void edit_field(bool edit) {
        auto *a = bc_get(draft, "fields");
        int index = rows->value() - 1;
        if (edit && (index < 0 || (size_t)index >= json_object_array_length(a)))
            return;
        if (!edit && json_object_array_length(a) >= 32) {
            error("Limite de 32 campos.");
            return;
        }
        auto *f = edit ? bc_clone(json_object_array_get_idx(a, (size_t)index))
                       : json_tokener_parse(
                             "{\"id\":\"campo\",\"label\":\"Campo\",\"offset\":0,\"type\":\"u8\","
                             "\"endian\":\"little\",\"scale\":1,\"bias\":0,\"unit\":\"\"}");
        Fl_Double_Window dialog(600, 650, "Campo do quadro");
        dialog.begin();
        const char *keys[] = {"id", "label", "offset", "unit", "scale", "bias", "min", "max"};
        const char *labels[] = {"Identificador",
                                "Nome apresentado",
                                "Posição em bytes",
                                "Unidade",
                                "Escala (valor = bruto × escala + ajuste)",
                                "Ajuste",
                                "Mínimo (opcional)",
                                "Máximo (opcional)"};
        Fl_Input *inputs[8];
        for (int i = 0; i < 8; i++) {
            auto *v = bc_get(f, keys[i]);
            inputs[i] = input(28, 32 + i * 61, 540, labels[i], v ? json_object_get_string(v) : "");
        }
        auto *t = choice(28, 530, 260, "Tipo numérico", "u8|s8|u16|s16|u32|s32|f32");
        auto *order = choice(310, 530, 258, "Ordem dos bytes", "Little endian|Big endian");
        const char *types[] = {"u8", "s8", "u16", "s16", "u32", "s32", "f32"};
        for (int i = 0; i < 7; i++)
            if (!strcmp(types[i], bc_str(f, "type")))
                t->value(i);
        order->value(!strcmp(bc_str(f, "endian"), "big"));
        Fl_Button cancel(280, 600, 130, 34, "Cancelar"), apply(426, 600, 142, 34, "Aplicar");
        bool accepted = false;
        cancel.callback(
            [](Fl_Widget *, void *p) {
                static_cast<Fl_Window *>(p)->hide();
            },
            &dialog);
        std::function<void()> fn = [&] {
            auto *r = bc_clone(f);
            for (int i = 0; i < 8; i++) {
                if (i == 0 || i == 1 || i == 3)
                    json_object_object_add(r, keys[i], json_object_new_string(inputs[i]->value()));
                else {
                    double v;
                    if (i >= 6 && !*inputs[i]->value()) {
                        json_object_object_del(r, keys[i]);
                        continue;
                    }
                    if (!number(inputs[i]->value(), v) ||
                        (i == 2 && (v < 0 || v > 4095 || floor(v) != v))) {
                        json_object_put(r);
                        fl_alert("Número inválido: %s", labels[i]);
                        return;
                    }
                    json_object_object_add(r, keys[i],
                                           i == 2 ? json_object_new_int((int)v)
                                                  : json_object_new_double(v));
                }
            }
            json_object_object_add(r, "type", json_object_new_string(types[t->value()]));
            json_object_object_add(r, "endian",
                                   json_object_new_string(order->value() ? "big" : "little"));
            json_object_put(f);
            f = r;
            accepted = true;
            dialog.hide();
        };
        apply.callback(
            [](Fl_Widget *, void *p) {
                (*static_cast<std::function<void()> *>(p))();
            },
            &fn);
        dialog.end();
        dialog.set_modal();
        dialog.show();
        while (dialog.shown())
            Fl::wait();
        if (accepted) {
            if (edit)
                json_object_array_put_idx(a, (size_t)index, f);
            else
                json_object_array_add(a, f);
            refresh();
        } else
            json_object_put(f);
    }

    void decode() {
        auto *r = collect();
        if (!r)
            return;
        bc_error e = {{0}};
        uint8_t bytes[4096];
        size_t n = 0;
        auto *decoded = bc_comm_hex(preview_bytes->value(), bytes, sizeof bytes, &n, &e) < 0
                            ? nullptr
                            : bc_frame_decode(r, bytes, n, "", &e);
        if (!decoded)
            error(e.text);
        else {
            std::string report = json_object_get_boolean(bc_get(decoded, "matched"))
                                     ? "Quadro válido"
                                     : "Quadro não aprovado";
            report += " · " + std::to_string(n) + " bytes\n" + bc_str(decoded, "message");
            report += "\n\n";
            auto *readings = bc_get(decoded, "readings");
            json_object_object_foreach(readings, id, reading) {
                std::string label = bc_str(reading, "label");
                if (label == id) {
                    if (label == "temperature")
                        label = "Temperatura";
                    else if (label == "voltage")
                        label = "Tensão";
                    else if (label == "status")
                        label = "Estado";
                }
                char value[100], raw[100];
                snprintf(value, sizeof value, "%.9g", bc_num(reading, "value"));
                snprintf(raw, sizeof raw, "%.9g", bc_num(reading, "raw"));
                report += label + " [" + id +
                          "]: " + (bc_get(reading, "value") ? value : "valor inválido") + " " +
                          bc_str(reading, "unit") + " — " +
                          (json_object_get_boolean(bc_get(reading, "matched"))
                               ? "dentro dos limites"
                               : "fora dos limites ou não finito") +
                          "\n    Valor bruto: " + raw + "\n\n";
            }
            error(report.c_str());
        }
        json_object_put(decoded);
        json_object_put(r);
    }

    void load() {
        Fl_Native_File_Chooser c;
        c.title("Importar formato de quadro");
        c.type(Fl_Native_File_Chooser::BROWSE_FILE);
        c.filter("JSON\t*.json");
        if (c.show() || !c.filename())
            return;
        bc_error e = {{0}};
        auto *r = bc_read_json(c.filename(), &e);
        if (!r || bc_frame_validate(r, &e) < 0) {
            error(e.text);
            json_object_put(r);
            return;
        }
        auto *edited = WorkbenchFrameEditor::edit(r);
        json_object_put(r);
        if (edited) {
            json_object_put(result);
            result = edited;
            hide();
        }
    }

    void save_file() {
        auto *r = collect();
        if (!r)
            return;
        Fl_Native_File_Chooser c;
        c.title("Exportar formato de quadro");
        c.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
        c.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
        c.filter("JSON\t*.json");
        c.preset_file("formato-uart.json");
        if (!c.show() && c.filename()) {
            bc_error e = {{0}};
            error(bc_write_json(c.filename(), r, &e) < 0 ? e.text : "Formato exportado.");
        }
        json_object_put(r);
    }

    static json_object *edit(json_object *initial) {
        WorkbenchFrameEditor d(initial);
        d.show();
        while (d.shown())
            Fl::wait();
        return d.result ? bc_clone(d.result) : nullptr;
    }
};
