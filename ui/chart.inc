struct PlotPoint {
    double time, value;
};

struct PlotSeries {
    std::vector<PlotPoint> points;
    std::string unit;
    double target = NAN, limit = NAN;
    size_t dropped = 0;
};

class TracePlot : public Fl_Widget {
public:
    std::map<std::string, PlotSeries> series;
    std::string selected;
    double view_from = NAN, view_to = NAN, cursor_a = NAN, cursor_b = NAN;

    TracePlot() : Fl_Widget(0, 0, 100, 100) {
    }

    void add(const std::string &name, const std::string &unit, double time, double value,
             double target = NAN, double limit = NAN) {
        if (!std::isfinite(time) || !std::isfinite(value) || name.empty() || name.size() > 160)
            return;
        if (!series.count(name) && series.size() >= 32)
            return;
        auto &s = series[name];
        s.unit = unit;
        s.target = target;
        s.limit = limit;
        if (s.points.size() >= 3000) {
            s.points.erase(s.points.begin(), s.points.begin() + 300);
            s.dropped += 300;
        }
        s.points.push_back({time, value});
        if (selected.empty())
            selected = name;
        redraw();
    }

    void fit() {
        view_from = view_to = cursor_a = cursor_b = NAN;
        redraw();
    }

    void select(const std::string &name) {
        selected = name;
        fit();
    }

    void clear() {
        series.clear();
        selected.clear();
        fit();
    }

    std::pair<double, double> bounds(bool full = false) const {
        auto it = series.find(selected);
        if (it == series.end() || it->second.points.empty())
            return {0, 1};
        const auto &points = it->second.points;
        double first = points.front().time, last = points.back().time;
        if (last <= first)
            last = first + 1;
        return !full && std::isfinite(view_from) && std::isfinite(view_to)
                   ? std::make_pair(view_from, view_to)
                   : std::make_pair(first, last);
    }

    void zoom(double factor, double center = .5) {
        if (!series.count(selected))
            return;
        auto full = bounds(true), old = bounds();
        double span =
            std::clamp((old.second - old.first) * factor, std::min(1e-6, full.second - full.first),
                       full.second - full.first);
        center = std::clamp(center, 0.0, 1.0);
        double pivot = old.first + center * (old.second - old.first);
        view_from = std::clamp(pivot - center * span, full.first, full.second - span);
        view_to = view_from + span;
        redraw();
    }

    PlotPoint nearest(double time) const {
        const auto &points = series.at(selected).points;
        return *std::min_element(points.begin(), points.end(),
                                 [time](const auto &a, const auto &b) {
                                     return std::fabs(a.time - time) < std::fabs(b.time - time);
                                 });
    }

    int handle(int event) override {
        if (!series.count(selected) || series.at(selected).points.empty())
            return Fl_Widget::handle(event);
        double relative = std::clamp(
            static_cast<double>(Fl::event_x() - x() - 85) / std::max(1, w() - 109), 0.0, 1.0);
        if (event == FL_MOUSEWHEEL) {
            zoom(std::pow(1.4, Fl::event_dy()), relative);
            return 1;
        }
        if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
            auto range = bounds();
            double time = nearest(range.first + relative * (range.second - range.first)).time;
            if (Fl::event_state(FL_SHIFT))
                cursor_b = time;
            else
                cursor_a = time;
            redraw();
            return 1;
        }
        return Fl_Widget::handle(event);
    }

    json_object *snapshot() const {
        auto *out = json_object_new_object(), *all = json_object_new_array(),
             *view = json_object_new_object();
        json_object_object_add(out, "series", all);
        json_object_object_add(out, "selected", json_object_new_string(selected.c_str()));
        json_object_object_add(out, "view", view);
        if (std::isfinite(view_from) && std::isfinite(view_to)) {
            json_object_object_add(view, "from", json_object_new_double(view_from));
            json_object_object_add(view, "to", json_object_new_double(view_to));
        }
        if (std::isfinite(cursor_a))
            json_object_object_add(view, "cursor_a", json_object_new_double(cursor_a));
        if (std::isfinite(cursor_b))
            json_object_object_add(view, "cursor_b", json_object_new_double(cursor_b));
        for (const auto &[name, s] : series) {
            if (s.points.empty())
                continue;
            auto *item = json_object_new_object(), *points = json_object_new_array();
            json_object_object_add(item, "name", json_object_new_string(name.c_str()));
            json_object_object_add(item, "unit", json_object_new_string(s.unit.c_str()));
            json_object_object_add(item, "dropped",
                                   json_object_new_int64(static_cast<int64_t>(s.dropped)));
            json_object_object_add(item, "points", points);
            if (std::isfinite(s.target))
                json_object_object_add(item, "target", json_object_new_double(s.target));
            if (std::isfinite(s.limit))
                json_object_object_add(item, "limit", json_object_new_double(s.limit));
            for (const auto &point : s.points) {
                auto *p = json_object_new_array();
                json_object_array_add(p, json_object_new_double(point.time));
                json_object_array_add(p, json_object_new_double(point.value));
                json_object_array_add(points, p);
            }
            json_object_array_add(all, item);
        }
        return out;
    }

    void draw() override {
        fl_push_clip(x(), y(), w(), h());
        fl_color(pal.card);
        fl_rectf(x(), y(), w(), h());
        fl_font(FL_HELVETICA, 13);
        fl_color(pal.ink);
        if (!series.count(selected) || series[selected].points.empty()) {
            fl_draw("As leituras aparecerão durante o ensaio.", x() + 24, y() + 40);
            fl_pop_clip();
            return;
        }
        const auto &s = series[selected];
        const auto &points = s.points;
        auto range = bounds();
        double t0 = range.first, t1 = range.second;
        double low = points.front().value, high = low;
        for (const auto &v : points) {
            low = std::min(low, v.value);
            high = std::max(high, v.value);
        }
        for (double n : {s.target, s.limit})
            if (std::isfinite(n)) {
                low = std::min(low, n);
                high = std::max(high, n);
            }
        double margin = std::max((high - low) * 0.08, std::max(std::fabs(high) * 0.01, 0.001));
        low -= margin;
        high += margin;
        int left = x() + 85, top = y() + 42, right = x() + w() - 24, bottom = y() + h() - 98;
        auto px = [&](double v) {
            return left + static_cast<int>(std::clamp((v - t0) / (t1 - t0), -100.0, 100.0) *
                                           (right - left));
        };
        auto py = [&](double v) {
            return bottom - static_cast<int>((v - low) / (high - low) * (bottom - top));
        };
        fl_color(pal.ink);
        fl_draw((selected + " (" + s.unit + ")").c_str(), left, y() + 23);
        for (int i = 0; i <= 4; i++) {
            double v = low + (high - low) * i / 4;
            int yy = py(v);
            fl_color(pal.line);
            fl_line(left, yy, right, yy);
            char label[48];
            std::snprintf(label, sizeof label, "%.5g", v);
            fl_color(pal.muted);
            fl_draw(label, x() + 4, yy - 9, 76, 18, FL_ALIGN_RIGHT);
            double t = t0 + (t1 - t0) * i / 4;
            std::snprintf(label, sizeof label, "%.3g s", t);
            fl_draw(label, px(t) - 35, bottom + 12, 70, 20, FL_ALIGN_CENTER);
        }
        fl_push_clip(left, top, right - left + 1, bottom - top + 1);
        auto horizontal = [&](double value, const char *label, Fl_Color color) {
            if (!std::isfinite(value))
                return;
            fl_color(color);
            fl_line_style(FL_DASH, 1);
            fl_line(left, py(value), right, py(value));
            fl_line_style(0);
            fl_draw(label, right - 130, py(value) - 5);
        };
        horizontal(s.target, "Alvo", pal.muted);
        horizontal(s.limit, "Limite máximo", pal.error);
        fl_color(pal.accent);
        fl_line_style(FL_SOLID, 2);
        for (size_t i = 1; i < points.size(); i++)
            if (points[i].time >= t0 && points[i - 1].time <= t1)
                fl_line(px(points[i - 1].time), py(points[i - 1].value), px(points[i].time),
                        py(points[i].value));
        if (points.size() == 1)
            fl_pie(px(points[0].time) - 3, py(points[0].value) - 3, 6, 6, 0, 360);
        fl_line_style(0);
        fl_color(pal.muted);
        for (double c : {cursor_a, cursor_b})
            if (std::isfinite(c) && c >= t0 && c <= t1)
                fl_line(px(c), top, px(c), bottom);
        fl_pop_clip();
        char label[240];
        double times[2] = {cursor_a, cursor_b};
        PlotPoint chosen[2]{};
        for (int i = 0; i < 2; i++)
            if (std::isfinite(times[i])) {
                chosen[i] = nearest(times[i]);
                std::snprintf(label, sizeof label, "%c: %.6g s / %.6g %s", i ? 'B' : 'A',
                              chosen[i].time, chosen[i].value, s.unit.c_str());
                fl_draw(label, left + i * (right - left) / 2, bottom + 48);
            }
        if (std::isfinite(cursor_a) && std::isfinite(cursor_b)) {
            std::snprintf(label, sizeof label, "B - A: %.6g s / %.6g %s",
                          chosen[1].time - chosen[0].time, chosen[1].value - chosen[0].value,
                          s.unit.c_str());
            fl_draw(label, left, bottom + 70);
        } else
            fl_draw("Clique: cursor A · Shift+clique: B · Roda do mouse: zoom", left, bottom + 70);
        fl_pop_clip();
    }
};
