/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <assert.h>
#include <math.h>
#include "../../hw/display/apple-gfx-density.h"

static long round_trip_density(uint16_t width, double width_mm)
{
    /* The display settings transport stores millimetres as a float. */
    return lround(width / (double)(float)width_mm * 25.46);
}

int main(void)
{
    double width_mm = 400.0, height_mm = 300.0;

    assert(apple_gfx_set_density(0, 0, 0, &width_mm, &height_mm));
    assert(width_mm == 400.0 && height_mm == 300.0);
    assert(round_trip_density(1488, width_mm) == 95);

    assert(apple_gfx_set_density(326, 1488, 2266, &width_mm, &height_mm));
    assert(fabs(width_mm - 116.21006134969325) < 1e-9);
    assert(fabs(height_mm - 176.97042944785278) < 1e-9);
    assert(round_trip_density(1488, width_mm) == 326);
    assert(round_trip_density(2266, height_mm) == 326);

    assert(apple_gfx_set_density(460, 1179, 2556, &width_mm, &height_mm));
    assert(fabs(width_mm - 65.25508695652175) < 1e-9);
    assert(fabs(height_mm - 141.4690434782609) < 1e-9);
    assert(round_trip_density(1179, width_mm) == 460);
    assert(round_trip_density(2556, height_mm) == 460);

    assert(!apple_gfx_set_density(326, 0, 0, &width_mm, &height_mm));
    assert(!apple_gfx_set_density(65536, 1488, 2266,
                                  &width_mm, &height_mm));
    assert(fabs(width_mm - 65.25508695652175) < 1e-9);
    assert(fabs(height_mm - 141.4690434782609) < 1e-9);
    return 0;
}
