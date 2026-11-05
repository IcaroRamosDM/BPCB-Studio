static int detailed(void *context, json_object *step, double *value, json_object **acquired) {
    (void)step;
    mock *m = context;
    m->confirmations++;
    *value = m->external;
    *acquired = json_object_new_object();
    str(*acquired, "instrument", "OFFLINE MOCK");
    return 0;
}

static json_object *adc_project(void) {
    json_object *p = bs_example(), *d = bs_defaults("device");
    str(d, "id", "adc");
    str(d, "driver", "ads7924");
    num(d, "vref", 5);
    json_object_array_add(bc_get(p, "devices"), d);
    json_object *s = bs_defaults("signal");
    str(s, "id", "feedback");
    str(s, "label", "Retorno");
    str(s, "device", "adc");
    str(s, "direction", "input");
    str(s, "unit", "V");
    str(s, "location", "TP1");
    str(s, "return", "GND");
    num(s, "channel", 0);
    num(s, "min", 0);
    num(s, "max", 5);
    num(s, "safe", 0);
    num(s, "simulate", 2.5);
    num(s, "diagnostic", 0);
    num(s, "plot", 1);
    str(s, "simulation_values", "0; 1; 2.45; 2.4; 2.5; 2.5");
    json_object_array_add(bc_get(p, "signals"), s);
    json_object *t = bs_find(bc_get(p, "tests"), "divisor"), *steps = json_object_new_array();
    json_object_object_add(t, "steps", steps);
    s = step("stable", "wait");
    str(s, "signal", "feedback");
    str(s, "variable", "volts");
    num(s, "min", 2.4);
    num(s, "max", 2.6);
    num(s, "stable_count", 3);
    num(s, "interval_ms", 20);
    num(s, "timeout", 1);
    str(s, "plot_target", "2.5");
    str(s, "plot_limit", "2.6");
    json_object_array_add(steps, s);
    s = step("result", "result");
    str(s, "expression", "volts");
    str(s, "reference", "2.5");
    json_object_array_add(steps, s);
    return p;
}

static void test_evolution(void) {
    bc_error e = {{0}};
    double value;
    CHECK(bc_parse_reading("110 mV", "V", "V", &value, &e) == 0 && fabs(value - .110) < 1e-12);
    CHECK(bc_parse_reading("0,011 V", "V", "mV", &value, &e) == 0 && fabs(value - .011) < 1e-12);
    CHECK(bc_parse_reading("1000 us", "s", "s", &value, &e) == 0 && fabs(value - .001) < 1e-12);
    CHECK(bc_parse_reading("1 s", "ms", "ms", &value, &e) == 0 && value == 1000);
    CHECK(bc_parse_reading("1 ms", "us", "us", &value, &e) == 0 && value == 1000);
    CHECK(bc_parse_reading("nan", "V", "V", &value, &e) < 0);
    CHECK(bc_parse_reading("2 A", "V", "mV", &value, &e) < 0);
    json_object *p = bs_example(), *t = bs_find(bc_get(p, "tests"), "divisor"),
                *steps = bc_get(t, "steps"), *manual = json_object_array_get_idx(steps, 1);
    num(manual, "min", 0);
    num(manual, "max", .1);
    num(manual, "sample", .110);
    str(manual, "display_unit", "mV");
    num(manual, "plot", 1);
    num(manual, "record_reading", 1);
    CHECK(bs_check(p, &e) == 0);
    mock m = {.external = .110};
    bs_host h = host(&m, 0);
    h.confirm_detail = detailed;
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && strstr(e.text, "fora da faixa"));
    CHECK(m.results == 1 && m.acquisitions == 1 && m.samples == 1 &&
          fabs(m.measured - .110) < 1e-12 && m.states > 0);
    json_object_put(p);
    p = adc_project();
    CHECK(bs_check(p, &e) == 0);
    json_object *bad = bc_clone(p);
    str(bs_find(bc_get(bad, "tests"), "divisor"), "id", "adc");
    num(json_object_array_get_idx(bc_get(bad, "devices"), 0), "address", 128);
    json_object *bad_issues = bs_validate(bad);
    CHECK(!strcmp(bc_str(json_object_array_get_idx(bad_issues, 0), "page"), "devices"));
    json_object_put(bad_issues);
    json_object_put(bad);
    m = (mock){0};
    h = host(&m, 1);
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) == 0 && m.samples == 5 && m.reads == 0 &&
          fabs(m.measured - 2.5) < 1e-12);
    json_object *signal = bs_find(bc_get(p, "signals"), "feedback");
    str(signal, "simulation_values", "1;6");
    m = (mock){0};
    CHECK(bs_check(p, &e) == 0);
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && m.samples == 2 && m.measured == 6);
    str(signal, "simulation_values", "0;1");
    m = (mock){0};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && strstr(e.text, "Tempo esgotado") &&
          m.time >= 1);
    str(signal, "simulation_values", "2.5");
    t = bs_find(bc_get(p, "tests"), "divisor");
    str(t, "simulation_at", "stable");
    for (unsigned i = 0; i < 3; i++) {
        str(t, "simulation_fault", i == 0 ? "communication" : i == 1 ? "cancel" : "timeout");
        m = (mock){0};
        CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 &&
              strstr(e.text, "Cenário simulado") && m.samples == 0);
    }
    str(t, "simulation_fault", "none");
    str(signal, "calibration_channel", "0");
    num(t, "require_calibration", 1);
    json_object *manifest = bs_manifest(p, 0, &e);
    if (!manifest)
        fprintf(stderr, "%s\n", e.text);
    CHECK(manifest != NULL && bc_get(manifest, "calibration"));
    json_object *input = json_tokener_parse(
        "{\"physical_id\":\"mock\",\"vref_mv\":5000,\"points\":[[1,1.1,4,4.1],null,null,null]}");
    json_object *cal = bc_cal_fit(manifest, input, &e);
    CHECK(cal != NULL);
    json_object_put(input);
    json_object_put(manifest);
    m = (mock){0};
    h = host(&m, 0);
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && m.reads == 0 && m.confirmations == 0);
    h.calibration = cal;
    m = (mock){0};
    m.values[0] = 2.4;
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) == 0 && fabs(m.measured - 2.5) < 1e-12);
    num(cal, "vref_mv", 3300);
    m = (mock){0};
    CHECK(bs_execute(p, "divisor", NULL, NULL, &h, &e) < 0 && m.reads == 0);
    json_object_put(cal);
    json_object_put(p);
    p = bs_example();
    t = bs_find(bc_get(p, "tests"), "divisor");
    json_object *b = bc_get(p, "board");
    str(b, "session_order", "divisor");
    json_object *edit = bc_clone(json_object_array_get_idx(bc_get(p, "components"), 0));
    str(edit, "ref", "R99");
    CHECK(bs_put(p, "component", "", 0, edit, &e) == 0);
    json_object_put(edit);
    CHECK(strstr(bc_str(json_object_array_get_idx(bc_get(t, "steps"), 3), "expression"), "R99") !=
              NULL &&
          bs_check(p, &e) == 0);
    edit = bc_clone(t);
    str(edit, "id", "novo");
    CHECK(bs_put(p, "test", "", 0, edit, &e) == 0);
    json_object_put(edit);
    CHECK(!strcmp(bc_str(b, "session_order"), "novo"));
    CHECK(bs_duplicate(p, "test", "", 0, &e) == 1 &&
          json_object_array_length(bc_get(p, "tests")) == 2);
    json_object *order = bs_session_order(p, &e);
    CHECK(order && json_object_array_length(order) == 1);
    json_object_put(order);
    str(b, "session_order", "novo;novo");
    CHECK(bs_check(p, &e) < 0);
    str(b, "session_order", "novo");
    json_object *donor = bs_example(), *merged = bs_reuse_test(p, donor, "divisor", &e);
    CHECK(merged && json_object_array_length(bc_get(merged, "tests")) == 3);
    json_object_put(merged);
    str(json_object_array_get_idx(bc_get(donor, "components"), 0), "ref", "R99");
    str(json_object_array_get_idx(bc_get(donor, "components"), 0), "value", "9 kΩ");
    CHECK(bs_reuse_test(p, donor, "divisor", &e) == NULL);
    json_object_put(donor);
    t = bs_find(bc_get(p, "tests"), "novo");
    str(json_object_array_get_idx(bc_get(t, "steps"), 3), "expression", "missing_variable");
    json_object *issues = bs_validate(p);
    int located = 0;
    for (size_t i = 0; i < json_object_array_length(issues); i++) {
        json_object *v = json_object_array_get_idx(issues, i);
        if (!strcmp(bc_str(v, "test"), "novo") && bc_num(v, "index") == 3)
            located = 1;
    }
    CHECK(located);
    json_object_put(issues);
    json_object_put(p);
}

static void test_continuity(void) {
    bc_error e = {{0}};
    char root[4096], project[4096], backup[4096], restored[4096], drawing[4096], manual[4096];
    CHECK(bs_temp_dir(root, "continuity", &e) == 0);
    CHECK(bc_path(project, sizeof project, root, "project.bpcb", &e) == 0);
    CHECK(bc_path(backup, sizeof backup, root, "copy.bpcbstudio", &e) == 0);
    CHECK(bc_path(restored, sizeof restored, root, "restored", &e) == 0);
    CHECK(bc_path(drawing, sizeof drawing, root, "drawing.svg", &e) == 0);
    CHECK(bc_path(manual, sizeof manual, root, "manual.pdf", &e) == 0);
    FILE *f = fopen(drawing, "w");
    CHECK(f != NULL);
    fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\"><text x=\"10\" "
          "y=\"20\">TEST</text></svg>",
          f);
    fclose(f);
    json_object *p = bs_example();
    CHECK(bs_document(p, project, drawing, "Esquema", "schematic", &e) == 0);
    CHECK(bs_save(project, p, &e) == 0);
    CHECK(bs_backup(p, project, backup, &e) == 0);
    json_object *preview = bs_backup_preview(backup, p, &e);
    CHECK(preview && json_object_get_boolean(bc_get(preview, "same_project")));
    CHECK(bs_backup(p, project, project, &e) < 0);
    json_object *preserved = bs_load(project, &e);
    CHECK(preserved && json_object_equal(preserved, p));
    json_object_put(preserved);
    CHECK(bs_restore(backup, restored, "invalid", &e) == NULL);
    json_object *copy = bs_restore(backup, restored, bc_str(preview, "digest"), &e);
    if (!copy)
        fprintf(stderr, "%s\n", e.text);
    CHECK(copy && json_object_equal(copy, p));
    CHECK(bs_restore(backup, restored, bc_str(preview, "digest"), &e) == NULL);
    CHECK(bs_manual(p, manual, &e) == 0);
    CHECK(bs_manual(p, manual, &e) < 0);
    json_object_put(copy);
    json_object_put(preview);
    json_object_put(p);
    bs_remove_temp(root);
}
