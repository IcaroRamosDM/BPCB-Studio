/* Development-only process teardown: Cairo intentionally retains font caches.
 * The CLI owns no live Cairo objects/threads at process exit. Releasing those
 * caches keeps leak checks focused on unreleased application allocations. */
#include <cairo.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>

__attribute__((destructor)) static void sanitizer_release_cairo_cache(void) {
    pango_cairo_font_map_set_default(NULL);
    cairo_debug_reset_static_data();
    FcFini();
}
