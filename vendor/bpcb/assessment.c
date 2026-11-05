#include "bancada.h"
#include "communication.h"
#include <string.h>
#include <math.h>
#ifdef _WIN32
#undef isfinite
#define isfinite __builtin_isfinite
#endif
#include <stdio.h>
#include <float.h>

/* Comparacoes numericas ficam no nucleo, com criterio visivel e reproduzivel. */
void bc_assess(json_object *measurement) {
    const char *verdict = "none";
    char criterion[192] = "Sem referência definida";
    json_object *reference = bc_get(measurement, "expected");
    double expected = json_object_get_double(reference), value = bc_num(measurement, "value");
    double percent = bc_num(measurement, "tolerance_pct"),
           absolute = bc_num(measurement, "tolerance_abs");
    if (reference &&
        (json_object_is_type(reference, json_type_int) ||
         json_object_is_type(reference, json_type_double)) &&
        isfinite(expected) && isfinite(percent) && isfinite(absolute) && percent >= 0 &&
        percent <= 100 && absolute >= 0) {
        double tolerance = fmax(fabs(expected) * percent / 100.0, absolute);
        verdict = fabs(value - expected) <= tolerance ? "pass" : "fail";
        snprintf(criterion, sizeof criterion, "%.6g ± %.3g %s", expected, tolerance,
                 bc_str(measurement, "unit"));
    }
    json_object *low = bc_get(measurement, "expected_min"),
                *high = bc_get(measurement, "expected_max");
    if (low && high &&
        (json_object_is_type(low, json_type_double) || json_object_is_type(low, json_type_int)) &&
        (json_object_is_type(high, json_type_double) || json_object_is_type(high, json_type_int)) &&
        isfinite(json_object_get_double(low)) && isfinite(json_object_get_double(high)) &&
        json_object_get_double(low) <= json_object_get_double(high)) {
        double minimum = json_object_get_double(low), maximum = json_object_get_double(high);
        verdict = value >= minimum && value <= maximum ? "pass" : "fail";
        snprintf(criterion, sizeof criterion, "%.6g a %.6g %s", minimum, maximum,
                 bc_str(measurement, "unit"));
    }
    if (json_object_is_type(bc_get(measurement, "byte_comparison"), json_type_boolean) &&
        json_object_get_boolean(bc_get(measurement, "byte_comparison"))) {
        uint8_t expected_bytes[BC_COMM_MAX], received[BC_COMM_MAX], mask[BC_COMM_MAX];
        size_t n = 0, r = 0, m = 0;
        bc_error error = {{0}};
        int valid = bc_comm_hex(bc_str(measurement, "expected_bytes"), expected_bytes,
                                sizeof expected_bytes, &n, &error) == 0 &&
                    n > 0 &&
                    bc_comm_hex(bc_str(measurement, "received_bytes"), received, sizeof received,
                                &r, &error) == 0 &&
                    r == n;
        memset(mask, 255, sizeof mask);
        if (bc_get(measurement, "mask_bytes"))
            valid = valid &&
                    bc_comm_hex(bc_str(measurement, "mask_bytes"), mask, sizeof mask, &m, &error) ==
                        0 &&
                    m == n;
        json_object *bits = bc_get(measurement, "comparison_bits");
        if (bits) {
            int64_t count = json_object_get_int64(bits);
            valid = valid && json_object_is_type(bits, json_type_int) && count > 0 &&
                    count <= (int64_t)n * 8 && count > (int64_t)(n - 1) * 8;
            if (valid && count % 8)
                mask[n - 1] &= (uint8_t)((1u << (unsigned)(count % 8)) - 1);
        }
        if (valid) {
            int matched = 1;
            for (size_t i = 0; i < n; i++)
                if ((expected_bytes[i] & mask[i]) != (received[i] & mask[i]))
                    matched = 0;
            verdict = matched ? "pass" : "fail";
            snprintf(criterion, sizeof criterion, "Comparação de %u bytes com máscara%s",
                     (unsigned)n, bits ? " e comprimento em bits" : "");
        } else {
            verdict = "none";
            snprintf(criterion, sizeof criterion,
                     "Comparação de bytes inválida ou incompleta; inconclusivo");
        }
    }
    if (json_object_is_type(bc_get(measurement, "communication_valid"), json_type_boolean) &&
        json_object_get_boolean(bc_get(measurement, "communication_valid")) && !reference &&
        (!low || !high ||
         (json_object_get_double(low) == -DBL_MAX && json_object_get_double(high) == DBL_MAX))) {
        verdict = isfinite(value) ? "pass" : "fail";
        snprintf(
            criterion, sizeof criterion, "%s",
            isfinite(value)
                ? "Resposta válida do protocolo/componente; limites elétricos no critério do ensaio"
                : "Valor de comunicação não finito");
    }
    if (bc_get(measurement, "communication_valid") &&
        (!json_object_is_type(bc_get(measurement, "communication_valid"), json_type_boolean) ||
         !json_object_get_boolean(bc_get(measurement, "communication_valid")))) {
        verdict = "fail";
        snprintf(criterion, sizeof criterion,
                 "Protocolo/componente retornou resposta inválida ou incompleta");
    }
    json_object_object_add(measurement, "verdict", json_object_new_string(verdict));
    json_object_object_add(measurement, "criterion", json_object_new_string(criterion));
}
