#pragma once
#include "bancada.h"
#include <cairo.h>
void bc_plot_draw(cairo_t *cr, json_object *series, json_object *view, double x, double y,
                  double width, double height);
