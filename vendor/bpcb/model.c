#define _POSIX_C_SOURCE 200809L
#include "bancada.h"
#include "communication.h"
#include "workbench.h"
#include <ctype.h>
#include <errno.h>
#ifndef _WIN32
#include <gnu/libc-version.h>
#endif
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/utsname.h>
#endif

static int numeric(json_object *v) {
    return v &&
           (json_object_is_type(v, json_type_int) || json_object_is_type(v, json_type_double)) &&
           isfinite(json_object_get_double(v));
}

static int ident(const char *s) {
    if (!s || !*s || strlen(s) > 100)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.')
            return 0;
    return 1;
}

int bc_component(const char *text, double *out) {
    char b[192];
    if (!text || strlen(text) >= sizeof b)
        return -1;
    strcpy(b, text);
    for (char *p = b; *p; p++)
        if (*p == ',')
            *p = '.';
    char *end;
    errno = 0;
    double v = strtod(b, &end);
    if (end == b || errno || !isfinite(v) || v < 0)
        return -1;
    while (isspace((unsigned char)*end))
        end++;
    switch (*end) {
        case 'k':
        case 'K':
            v *= 1e3;
            break;
        case 'M':
            v *= 1e6;
            break;
        case 'G':
            v *= 1e9;
            break;
        case 'm':
            v *= 1e-3;
            break;
        case 'u':
            v *= 1e-6;
            break;
        case 'n':
            v *= 1e-9;
            break;
        case 'p':
            v *= 1e-12;
            break;
        default:
            if (!strncmp(end, "µ", strlen("µ")))
                v *= 1e-6;
            break;
    }
    *out = v;
    return isfinite(v) ? 0 : -1;
}

typedef struct {
    const char *p;
    json_object *vars;
    unsigned depth;
    int bad;
    bc_error *e;
} parser;

static void ws(parser *p) {
    while (isspace((unsigned char)*p->p))
        p->p++;
}

static double sum(parser *p);

static double atom(parser *p) {
    ws(p);
    if (++p->depth > 48) {
        p->bad = 1;
        return 0;
    }
    double v = 0;
    if (*p->p == '+' || *p->p == '-') {
        int neg = *p->p++ == '-';
        v = atom(p);
        if (neg)
            v = -v;
    } else if (*p->p == '(') {
        p->p++;
        v = sum(p);
        ws(p);
        if (*p->p != ')')
            p->bad = 1;
        else
            p->p++;
    } else if (isdigit((unsigned char)*p->p) || *p->p == '.') {
        char *end;
        errno = 0;
        v = strtod(p->p, &end);
        if (end == p->p || errno)
            p->bad = 1;
        p->p = end;
    } else if (isalpha((unsigned char)*p->p) || *p->p == '_') {
        char name[100];
        size_t n = 0;
        while (isalnum((unsigned char)*p->p) || *p->p == '_') {
            if (n + 1 < sizeof name)
                name[n++] = *p->p;
            else
                p->bad = 1;
            p->p++;
        }
        name[n] = 0;
        ws(p);
        if (*p->p == '(') {
            p->p++;
            double args[8];
            int count = 0;
            do {
                if (count == 8) {
                    p->bad = 1;
                    break;
                }
                args[count++] = sum(p);
                ws(p);
                if (*p->p != ',')
                    break;
                p->p++;
            } while (!p->bad);
            if (*p->p != ')')
                p->bad = 1;
            else
                p->p++;
            if (!strcmp(name, "abs") && count == 1)
                v = fabs(args[0]);
            else if (!strcmp(name, "sqrt") && count == 1)
                v = sqrt(args[0]);
            else if (!strcmp(name, "log") && count == 1)
                v = log(args[0]);
            else if (!strcmp(name, "log1p") && count == 1)
                v = log1p(args[0]);
            else if ((!strcmp(name, "min") || !strcmp(name, "max")) && count >= 1) {
                v = args[0];
                for (int i = 1; i < count; i++)
                    v = !strcmp(name, "min") ? fmin(v, args[i]) : fmax(v, args[i]);
            } else
                p->bad = 1;
        } else {
            json_object *o = bc_get(p->vars, name);
            if (!numeric(o)) {
                p->bad = 1;
                bc_fail(p->e, "Valor ausente ou invalido: %s", name);
            } else
                v = json_object_get_double(o);
        }
    } else
        p->bad = 1;
    p->depth--;
    if (!isfinite(v))
        p->bad = 1;
    return v;
}

static double product(parser *p) {
    double v = atom(p);
    ws(p);
    while (!p->bad && (*p->p == '*' || *p->p == '/')) {
        char op = *p->p++;
        double r = atom(p);
        if (op == '*')
            v *= r;
        else if (r == 0)
            p->bad = 1;
        else
            v /= r;
        ws(p);
    }
    return v;
}

static double sum(parser *p) {
    double v = product(p);
    ws(p);
    while (!p->bad && (*p->p == '+' || *p->p == '-')) {
        char op = *p->p++;
        double r = product(p);
        v = op == '+' ? v + r : v - r;
        ws(p);
    }
    return v;
}

int bc_eval(const char *expression, json_object *vars, double *out, bc_error *e) {
    if (!expression || strlen(expression) > 2048)
        return bc_fail(e, "Expressao grande demais.");
    parser p = {expression, vars, 0, 0, e};
    double v = sum(&p);
    ws(&p);
    if (p.bad || *p.p || !isfinite(v))
        return bc_fail(e, "Expressao invalida ou valor indisponivel: %s", expression);
    *out = v;
    return 0;
}

static char *render(const char *text, json_object *vars, bc_error *e) {
    char *out = malloc(65536);
    size_t used = 0;
    if (!out)
        return NULL;
    while (*text) {
        if (used > 65000)
            goto error;
        if (text[0] == '{' && text[1] == '{') {
            out[used++] = '{';
            text += 2;
            continue;
        }
        if (*text != '{') {
            out[used++] = *text++;
            continue;
        }
        const char *end = strchr(++text, '}');
        if (!end || end - text > 120)
            goto error;
        char field[128];
        memcpy(field, text, (size_t)(end - text));
        field[end - text] = 0;
        char *colon = strchr(field, ':');
        const char *fmt = "%.3f";
        char format[8];
        if (colon) {
            *colon++ = 0;
            if (strlen(colon) != 3 || colon[0] != '.' || colon[1] < '0' || colon[1] > '6' ||
                !strchr("fge", colon[2]))
                goto error;
            snprintf(format, sizeof format, "%%%s", colon);
            fmt = format;
        }
        json_object *value = bc_get(vars, field);
        if (!numeric(value) && (!json_object_is_type(value, json_type_string) || colon))
            goto error;
        int n = numeric(value)
                    ? snprintf(out + used, 65536 - used, fmt, json_object_get_double(value))
                    : snprintf(out + used, 65536 - used, "%s", json_object_get_string(value));
        if (n < 0 || (size_t)n >= 65536 - used)
            goto error;
        used += (size_t)n;
        text = end + 1;
    }
    out[used] = 0;
    return out;
error:
    free(out);
    bc_fail(e, "Modelo de texto invalido ou valor ausente.");
    return NULL;
}

json_object *bc_test(json_object *m, const char *id) {
    json_object *a = bc_get(m, "tests");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *t = json_object_array_get_idx(a, i);
        if (!strcmp(bc_str(t, "id"), id))
            return t;
    }
    return NULL;
}

json_object *bc_preview(json_object *m, json_object *t, bc_error *e) {
    json_object *vars = json_object_new_object(), *result = NULL;
    json_object *components = bc_get(m, "components");
    json_object_object_foreach(components, k, value) {
        double v;
        if (!bc_component(json_object_get_string(value), &v))
            json_object_object_add(vars, k, json_object_new_double(v));
    }
    json_object *a = bc_get(t, "parameters");
    for (size_t i = 0; i < json_object_array_length(a); i++) {
        json_object *p = json_object_array_get_idx(a, i), *value = bc_get(p, "default");
        if (!value && json_object_get_boolean(bc_get(p, "optional")))
            continue;
        double v = json_object_get_double(value);
        if (!numeric(value) || v < bc_num(p, "min") || v > bc_num(p, "max") ||
            (json_object_get_boolean(bc_get(p, "integer")) && floor(v) != v)) {
            bc_fail(e, "Valor fora da faixa: %s", bc_str(p, "label"));
            goto done;
        }
        json_object_object_add(vars, bc_str(p, "id"), json_object_new_double(v));
        json_object *choices = bc_get(p, "choices");
        if (choices) {
            if (!json_object_is_type(choices, json_type_array) ||
                json_object_array_length(choices) < 1 || json_object_array_length(choices) > 32) {
                bc_fail(e, "Opções inválidas.");
                goto done;
            }
            int found = 0;
            for (size_t k = 0; k < json_object_array_length(choices); k++) {
                json_object *choice = json_object_array_get_idx(choices, k);
                if (!numeric(bc_get(choice, "value")) || !*bc_str(choice, "label")) {
                    bc_fail(e, "Opção inválida.");
                    goto done;
                }
                if (bc_num(choice, "value") == v) {
                    char name[128];
                    snprintf(name, sizeof name, "%s_label", bc_str(p, "id"));
                    json_object_object_add(vars, name,
                                           json_object_new_string(bc_str(choice, "label")));
                    found = 1;
                }
            }
            if (!found) {
                bc_fail(e, "Selecione uma opção válida.");
                goto done;
            }
        }
    }
    json_object *derived = bc_get(t, "derived");
    int count = derived ? json_object_object_length(derived) : 0, remaining = count;
    for (int round = 0; round < count && remaining; round++) {
        int progress = 0;
        json_object_object_foreach(derived, k, expression) {
            if (bc_get(vars, k))
                continue;
            double v;
            bc_error ignored = {{0}};
            if (!bc_eval(json_object_get_string(expression), vars, &v, &ignored)) {
                json_object_object_add(vars, k, json_object_new_double(v));
                progress++;
                remaining--;
            }
        }
        if (!progress) {
            bc_fail(e, "Calculo incompleto: confira os componentes e as formulas do pacote.");
            goto done;
        }
    }
    json_object *constraints = bc_get(t, "constraints");
    if (constraints) {
        if (!json_object_is_type(constraints, json_type_array) ||
            json_object_array_length(constraints) > 32) {
            bc_fail(e, "Restrições de parâmetros inválidas.");
            goto done;
        }
        for (size_t i = 0; i < json_object_array_length(constraints); i++) {
            json_object *check = json_object_array_get_idx(constraints, i),
                        *minimum = bc_get(check, "min"), *maximum = bc_get(check, "max");
            double value = 0;
            if (!json_object_is_type(bc_get(check, "expression"), json_type_string) ||
                (!minimum && !maximum) || (minimum && !numeric(minimum)) ||
                (maximum && !numeric(maximum)) ||
                (minimum && maximum &&
                 json_object_get_double(minimum) > json_object_get_double(maximum)) ||
                bc_eval(bc_str(check, "expression"), vars, &value, e) < 0) {
                bc_fail(e, "Restrição de parâmetros inválida.");
                goto done;
            }
            if ((minimum && value < json_object_get_double(minimum)) ||
                (maximum && value > json_object_get_double(maximum))) {
                char *message = render(*bc_str(check, "message") ? bc_str(check, "message")
                                                                 : "Parâmetros incompatíveis.",
                                       vars, e);
                if (message) {
                    bc_fail(e, "%s", message);
                    free(message);
                }
                goto done;
            }
        }
    }
    char *instructions = render(bc_str(t, "instructions"), vars, e);
    if (!instructions)
        goto done;
    char *expected = render(bc_str(t, "expected"), vars, e);
    if (!expected) {
        free(instructions);
        goto done;
    }
    json_object *optional = bc_get(t, "reference_prediction");
    if (optional && bc_get(vars, bc_str(optional, "parameter"))) {
        double prediction;
        if (bc_eval(bc_str(optional, "expression"), vars, &prediction, e) < 0) {
            free(instructions);
            free(expected);
            goto done;
        }
        json_object_object_add(vars, "reference_prediction", json_object_new_double(prediction));
        char *more = render(bc_str(optional, "text"), vars, e);
        if (!more) {
            free(instructions);
            free(expected);
            goto done;
        }
        size_t size = strlen(expected) + strlen(more) + 3;
        char *joined = malloc(size);
        if (!joined) {
            free(more);
            free(instructions);
            free(expected);
            goto done;
        }
        snprintf(joined, size, "%s\n%s", expected, more);
        free(expected);
        free(more);
        expected = joined;
    }
    if (optional && !bc_get(vars, bc_str(optional, "parameter")) &&
        *bc_str(optional, "empty_text")) {
        char *more = render(bc_str(optional, "empty_text"), vars, e);
        if (!more) {
            free(instructions);
            free(expected);
            goto done;
        }
        size_t size = strlen(expected) + strlen(more) + 3;
        char *joined = malloc(size);
        if (!joined) {
            free(instructions);
            free(expected);
            free(more);
            goto done;
        }
        snprintf(joined, size, "%s\n%s", expected, more);
        free(expected);
        free(more);
        expected = joined;
    }
    result = json_object_new_object();
    json_object_object_add(result, "instructions", json_object_new_string(instructions));
    json_object_object_add(result, "expected", json_object_new_string(expected));
    json_object_object_add(result, "values", json_object_get(vars));
    free(instructions);
    free(expected);
done:
    json_object_put(vars);
    return result;
}

static int contains_frame(json_object *o) {
    if (json_object_is_type(o, json_type_object)) {
        if (!strcmp(bc_str(o, "protocol"), "frame"))
            return 1;
        json_object_object_foreach(o, k, v) {
            (void)k;
            if (contains_frame(v))
                return 1;
        }
    } else if (json_object_is_type(o, json_type_array))
        for (size_t i = 0; i < json_object_array_length(o); i++)
            if (contains_frame(json_object_array_get_idx(o, i)))
                return 1;
    return 0;
}

static int communication_since_013(json_object *d) {
    if (!strcmp(bc_str(d, "bus"), "jtag"))
        return 1;
    json_object *adapters = bc_get(d, "adapters");
    for (size_t i = 0;
         json_object_is_type(adapters, json_type_array) && i < json_object_array_length(adapters);
         i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(adapters, i)), "ch347"))
            return 1;
    return 0;
}

static int communication_since_015(json_object *d) {
    if (bc_num(d, "api") == 2)
        return 1;
    json_object *ops = bc_get(d, "operations");
    for (size_t i = 0; ops && i < json_object_array_length(ops); i++)
        if (bc_get(json_object_array_get_idx(ops, i), "kind"))
            return 1;
    return 0;
}

int bc_validate(json_object *m, bc_error *e) {
    if (!m || !json_object_is_type(m, json_type_object) ||
        strcmp(bc_str(m, "format"), "axtor.bancada"))
        return bc_fail(e, "Arquivo nao e um pacote de bancada.");
    if (bc_num(m, "api") != BC_API || strcmp(bc_str(m, "runtime"), "native"))
        return bc_fail(e, "Este pacote usa o nucleo antigo. Exporte uma versao com rotinas nativas "
                          "em C (API 2).");
    if (bc_num(m, "schema") != 1 || !ident(bc_str(m, "id")) || !*bc_str(m, "pcb") ||
        !*bc_str(m, "revision"))
        return bc_fail(e, "Identificacao de bancada invalida.");
#ifndef _WIN32
    if (*bc_str(m, "min_glibc")) {
        unsigned want_major = 0, want_minor = 0, have_major = 0, have_minor = 0;
        char trailing = 0;
        if (sscanf(bc_str(m, "min_glibc"), "%u.%u%c", &want_major, &want_minor, &trailing) != 2 ||
            sscanf(gnu_get_libc_version(), "%u.%u", &have_major, &have_minor) != 2 ||
            have_major < want_major || (have_major == want_major && have_minor < want_minor))
            return bc_fail(e, "O pacote exige glibc %s ou posterior.", bc_str(m, "min_glibc"));
    }
#endif
    unsigned a = 0, b = 0, c = 0;
    char tail = 0;
    unsigned major = 0, minor = 0, patch = 0;
    sscanf(BC_VERSION, "%u.%u.%u", &major, &minor, &patch);
    if (sscanf(bc_str(m, "min_app"), "%u.%u.%u%c", &a, &b, &c, &tail) != 3 || a > major ||
        (a == major && (b > minor || (b == minor && c > patch))))
        return bc_fail(e, "Pacote exige uma versao mais nova do aplicativo.");
    if (sscanf(bc_str(m, "version"), "%u.%u.%u%c", &a, &b, &c, &tail) != 3)
        return bc_fail(e, "Versao do pacote invalida.");
#ifdef _WIN32
    const char *platform = "windows-x86_64";
    int found = 0;
#else
    struct utsname os;
    if (uname(&os) < 0)
        return bc_fail(e, "Nao foi possivel identificar a plataforma.");
    char platform[128];
    snprintf(platform, sizeof platform, "linux-%.80s", os.machine);
    int found = 0;
#endif
    json_object *platforms = bc_get(m, "platforms");
    if (!json_object_is_type(platforms, json_type_array))
        return bc_fail(e, "Plataformas nao informadas.");
    for (size_t i = 0; i < json_object_array_length(platforms); i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(platforms, i)), platform))
            found = 1;
    if (!found)
        return bc_fail(e, "Plataforma incompativel: %s", platform);
    json_object *components = bc_get(m, "components"), *tests = bc_get(m, "tests");
    if (!json_object_is_type(components, json_type_object) ||
        json_object_object_length(components) > 2000 ||
        !json_object_is_type(tests, json_type_array) || json_object_array_length(tests) > 128)
        return bc_fail(e, "Cadastro invalido ou grande demais.");
    json_object_object_foreach(components, k, v) {
        if (!ident(k) || !json_object_is_type(v, json_type_string) ||
            strlen(json_object_get_string(v)) > 180)
            return bc_fail(e, "Componente invalido.");
    }
    json_object *descriptions = bc_get(m, "component_descriptions");
    if (descriptions) {
        if (!json_object_is_type(descriptions, json_type_object))
            return bc_fail(e, "Descricoes invalidas.");
        json_object_object_foreach(descriptions, key, value) {
            if (!bc_get(components, key) || !json_object_is_type(value, json_type_string) ||
                strlen(json_object_get_string(value)) > 1000)
                return bc_fail(e, "Descricao de componente invalida.");
        }
    }
    json_object *plan = bc_get(m, "session_plan");
    if (plan) {
        json_object *order = bc_get(plan, "tests"), *seen = json_object_new_object();
        int valid = json_object_is_type(plan, json_type_object) &&
                    json_object_is_type(order, json_type_array) &&
                    json_object_array_length(order) <= 16 && *bc_str(plan, "title") &&
                    strlen(bc_str(plan, "title")) <= 240;
        for (size_t i = 0; valid && i < json_object_array_length(order); i++) {
            json_object *id = json_object_array_get_idx(order, i);
            const char *name = json_object_get_string(id);
            if (!json_object_is_type(id, json_type_string) || !name || bc_get(seen, name) ||
                !bc_test(m, name)) {
                valid = 0;
                break;
            }
            json_object_object_add(seen, name, json_object_new_boolean(1));
        }
        json_object_put(seen);
        if (!valid)
            return bc_fail(
                e, "Roteiro de sessão inválido; use até 16 testes existentes sem repetição.");
    }
    json_object *cal = bc_get(m, "calibration");
    if (cal) {
        json_object *channels = bc_get(cal, "channels");
        if (strcmp(bc_str(cal, "kind"), "adc-linear-v1") ||
            !json_object_is_type(channels, json_type_array) ||
            json_object_array_length(channels) != 4)
            return bc_fail(e, "Declaração de calibração inválida.");
        for (size_t i = 0; i < 4; i++) {
            json_object *ch = json_object_array_get_idx(channels, i),
                        *deps = bc_get(ch, "components");
            if (!*bc_str(ch, "label") || strlen(bc_str(ch, "label")) > 160 ||
                !*bc_str(ch, "factor") || !json_object_is_type(deps, json_type_array) ||
                json_object_array_length(deps) > 64)
                return bc_fail(e, "Canal de calibração inválido.");
            for (size_t k = 0; k < json_object_array_length(deps); k++)
                if (!json_object_is_type(json_object_array_get_idx(deps, k), json_type_string) ||
                    !bc_get(components, json_object_get_string(json_object_array_get_idx(deps, k))))
                    return bc_fail(e, "Componente de calibração ausente.");
        }
    }
    if (bc_comm_declaration(bc_get(m, "communication"), e) < 0)
        return -1;
    unsigned comm_major = 0, comm_minor = 0, comm_patch = 0;
    sscanf(bc_str(m, "min_app"), "%u.%u.%u", &comm_major, &comm_minor, &comm_patch);
    int comm_compatible = comm_major > 0 || comm_minor >= 12;
    int comm_013 = comm_major > 0 || comm_minor >= 13;
    if (contains_frame(bc_get(m, "communication")) && !(comm_major > 0 || comm_minor >= 16))
        return bc_fail(e, "Quadros UART exigem BPCB 0.16.0 ou superior.");
    if (communication_since_015(bc_get(m, "communication")) &&
        !(comm_major > 0 || comm_minor >= 15))
        return bc_fail(e, "Comunicação multicanal/IoT/protocolos exige BPCB 0.15.0 ou superior.");
    if (communication_since_013(bc_get(m, "communication")) && !comm_013)
        return bc_fail(e, "Pacote CH347/JTAG deve exigir min_app 0.13.0 ou superior.");
    if (bc_get(m, "communication") && !comm_compatible)
        return bc_fail(e, "Pacote com comunicação API 1 deve exigir min_app 0.12.0 ou superior.");
    json_object *ids = json_object_new_object();
    for (size_t i = 0; i < json_object_array_length(tests); i++) {
        json_object *t = json_object_array_get_idx(tests, i);
        const char *id = bc_str(t, "id");
        if (!ident(id) || bc_get(ids, id) || !*bc_str(t, "title") ||
            strlen(bc_str(t, "title")) > 240 || strlen(bc_str(t, "purpose")) > 2000 ||
            strlen(bc_str(t, "instructions")) > 32000 || strlen(bc_str(t, "expected")) > 32000) {
            json_object_put(ids);
            return bc_fail(e, "Teste invalido ou repetido.");
        }
        json_object_object_add(ids, id, json_object_new_boolean(1));
        if (bc_comm_declaration(bc_get(t, "communication"), e) < 0) {
            json_object_put(ids);
            return -1;
        }
        json_object *wiring = bc_get(t, "wiring"), *decl = bc_get(t, "communication");
        if (!decl)
            decl = bc_get(m, "communication");
        if (bc_wiring_validate(wiring, decl, e) < 0) {
            json_object_put(ids);
            return -1;
        }
        if (wiring && json_object_array_length(wiring) && !(comm_major > 0 || comm_minor >= 16)) {
            json_object_put(ids);
            return bc_fail(e, "Mapa de conexões exige BPCB 0.16.0 ou superior.");
        }
        if (contains_frame(bc_get(t, "communication")) && !(comm_major > 0 || comm_minor >= 16)) {
            json_object_put(ids);
            return bc_fail(e, "Quadros UART exigem BPCB 0.16.0 ou superior.");
        }
        if (communication_since_015(bc_get(t, "communication")) &&
            !(comm_major > 0 || comm_minor >= 15)) {
            json_object_put(ids);
            return bc_fail(e, "Teste avançado exige BPCB 0.15.0 ou superior.");
        }
        if (communication_since_013(bc_get(t, "communication")) && !comm_013) {
            json_object_put(ids);
            return bc_fail(e, "Teste CH347/JTAG deve exigir min_app 0.13.0 ou superior.");
        }
        if (bc_get(t, "communication") && !comm_compatible) {
            json_object_put(ids);
            return bc_fail(e, "Teste com comunicação API 1 exige min_app 0.12.0 ou superior.");
        }
        json_object *timeout = bc_get(t, "timeout");
        if (timeout && (!numeric(timeout) || json_object_get_double(timeout) <= 0 ||
                        json_object_get_double(timeout) > 3600)) {
            json_object_put(ids);
            return bc_fail(e, "Tempo limite da rotina invalido.");
        }

        if (strcmp(bc_str(t, "mode"), "hardware") && strcmp(bc_str(t, "mode"), "simulation")) {
            json_object_put(ids);
            return bc_fail(e, "Modo do teste invalido.");
        }
        if (json_object_get_boolean(bc_get(t, "enabled")) &&
            (!bc_safe_path(bc_str(t, "entry")) || strncmp(bc_str(t, "entry"), "code/", 5))) {
            json_object_put(ids);
            return bc_fail(e, "Rotina nativa nao informada.");
        }
        json_object *params = bc_get(t, "parameters"), *derived = bc_get(t, "derived");
        if (!json_object_is_type(params, json_type_array) ||
            json_object_array_length(params) > 32 ||
            !json_object_is_type(derived, json_type_object) ||
            json_object_object_length(derived) > 64) {
            json_object_put(ids);
            return bc_fail(e, "Parametros ou formulas invalidos.");
        }
        json_object *names = json_object_new_object();
        for (size_t j = 0; j < json_object_array_length(params); j++) {
            json_object *p = json_object_array_get_idx(params, j);
            const char *key = bc_str(p, "id");
            if (!ident(key) || bc_get(names, key) || bc_get(components, key) ||
                !numeric(bc_get(p, "min")) || !numeric(bc_get(p, "max")) ||
                !numeric(bc_get(p, "step")) || bc_num(p, "step") <= 0 ||
                bc_num(p, "min") > bc_num(p, "max")) {
                json_object_put(names);
                json_object_put(ids);
                return bc_fail(e, "Parametro invalido.");
            }
            json_object_object_add(names, key, json_object_new_boolean(1));
        }
        json_object_object_foreach(derived, key, v) {
            if (!ident(key) || bc_get(names, key) || bc_get(components, key) ||
                !json_object_is_type(v, json_type_string) ||
                strlen(json_object_get_string(v)) > 2048) {
                json_object_put(names);
                json_object_put(ids);
                return bc_fail(e, "Formula invalida.");
            }
        }
        json_object_put(names);
        json_object *preview = bc_preview(m, t, e);
        if (!preview) {
            json_object_put(ids);
            return -1;
        }
        json_object_put(preview);
    }
    json_object_put(ids);
    json_object *docs = bc_get(m, "documents");
    if (docs &&
        (!json_object_is_type(docs, json_type_array) || json_object_array_length(docs) > 128))
        return bc_fail(e, "Documentacao invalida.");
    for (size_t i = 0; i < json_object_array_length(docs); i++)
        if (!bc_safe_path(bc_str(json_object_array_get_idx(docs, i), "file")))
            return bc_fail(e, "Caminho de documento invalido.");
    return 0;
}

json_object *bc_apply(json_object *m, json_object *values, bc_error *e) {
    json_object *copy = bc_clone(m);
    if (!copy) {
        bc_fail(e, "Memoria insuficiente.");
        return NULL;
    }
    if (values) {
        json_object *components = bc_get(values, "components"), *dest = bc_get(copy, "components");
        if (components && !json_object_is_type(components, json_type_object))
            goto invalid;
        if (components) {
            json_object_object_foreach(components, k, v) {
                if (!bc_get(dest, k) || !json_object_is_type(v, json_type_string) ||
                    strlen(json_object_get_string(v)) > 180)
                    goto invalid;
                json_object_object_add(dest, k, json_object_get(v));
            }
        }
        json_object *descriptions = bc_get(values, "component_descriptions");
        if (descriptions) {
            if (!json_object_is_type(descriptions, json_type_object))
                goto invalid;
            json_object *target = bc_get(copy, "component_descriptions");
            if (!target) {
                target = json_object_new_object();
                json_object_object_add(copy, "component_descriptions", target);
            }
            json_object_object_foreach(descriptions, key, value) {
                json_object_object_add(target, key, json_object_get(value));
            }
        }
        json_object *params = bc_get(values, "parameters");
        if (params && !json_object_is_type(params, json_type_object))
            goto invalid;
        if (params) {
            json_object_object_foreach(params, id, entries) {
                json_object *test = bc_test(copy, id);
                if (!test || !json_object_is_type(entries, json_type_object))
                    goto invalid;
                json_object *a = bc_get(test, "parameters");
                json_object_object_foreach(entries, key, value) {
                    int found = 0;
                    for (size_t i = 0; i < json_object_array_length(a); i++) {
                        json_object *p = json_object_array_get_idx(a, i);
                        if (!strcmp(bc_str(p, "id"), key)) {
                            json_object_object_add(p, "default", json_object_get(value));
                            found = 1;
                            break;
                        }
                    }
                    if (!found)
                        goto invalid;
                }
            }
        }
    }
    if (bc_validate(copy, e) < 0) {
        json_object_put(copy);
        return NULL;
    }
    return copy;
invalid:
    bc_fail(e, "Montagem ou parametros nao pertencem a esta bancada.");
    json_object_put(copy);
    return NULL;
}
