#pragma once
#include "lab.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Pure, bounded data schemas. No scripts or hardware discovery are performed. */
json_object *bc_frame_example(void);
int bc_frame_validate(json_object *, bc_error *);
json_object *bc_frame_decode(json_object *, const uint8_t *, size_t, const char *, bc_error *);
int bc_frame_operation_validate(json_object *, bc_error *);
json_object *bc_frame_execute(json_object *, bc_lab_io *, bc_error *);
int bc_wiring_validate(json_object *, json_object *, bc_error *);
char *bc_wiring_text(json_object *, bc_error *);
/* Explicit local compatibility records, independent from optional test history. */
int bc_compatibility_validate(json_object *, bc_error *);
json_object *bc_firmware_bundle_inspect(const char *, bc_error *);
int bc_firmware_bundle_export(json_object *, const char *, bc_error *);
json_object *bc_firmware_bundle_import(const char *, const char *, const char *, bc_error *);
json_object *bc_compatibility_list(const char *, bc_error *);
json_object *bc_compatibility_record(json_object *, const char *, const char *, const char *,
                                     bc_error *);
int bc_compatibility_save(const char *, json_object *, int, bc_error *);
json_object *bc_compatibility_match(json_object *, json_object *, const char *, const char *,
                                    bc_error *);
#ifdef __cplusplus
}
#endif
