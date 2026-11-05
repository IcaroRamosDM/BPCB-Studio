#include "studio.h"
#include <string.h>

typedef struct {
    const char *kind;
    const char *json;
} catalog_extension;

#include "catalog/extra.h"

void bs_catalog_extend(const char *kind, json_object *fields) {
    bs_communication_catalog(kind, fields);
    for (size_t i = 0; i < sizeof extra / sizeof *extra; i++)
        if (!strcmp(kind, extra[i].kind))
            json_object_array_add(fields, json_tokener_parse(extra[i].json));
    if (!strcmp(kind, "step"))
        for (size_t i = 0; i < json_object_array_length(fields); i++) {
            json_object *f = json_object_array_get_idx(fields, i);
            const char *key = bc_str(f, "key");
            if (!strcmp(key, "min") || !strcmp(key, "max"))
                json_object_object_add(
                    f, "help",
                    json_object_new_string("Critério de aprovação. A leitura fora da faixa é "
                                           "registrada e interrompe a sequência."));
            if (!strcmp(key, "unit"))
                json_object_object_add(f, "when",
                                       json_object_new_string("manual,calculate,result"));
            if (!strcmp(key, "timeout"))
                json_object_object_add(f, "when",
                                       json_object_new_string("confirm,manual,read,wait"));
        }
    bs_lab_catalog(kind, fields);
}
