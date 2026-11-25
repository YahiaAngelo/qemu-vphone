/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef QEMU_APPLE_GFX_DENSITY_H
#define QEMU_APPLE_GFX_DENSITY_H

#include <stdbool.h>
#include <stdint.h>

static inline bool apple_gfx_set_density(uint32_t ppi,
                                       uint16_t width, uint16_t height,
                                       double *width_mm, double *height_mm)
{
    if (!ppi) {
        return true;
    }
    if (ppi > UINT16_MAX || !width || !height) {
        return false;
    }

    /* Match reims-vgpu's density conversion, including its 25.46 divisor. */
    *width_mm = width / (ppi / 25.46);
    *height_mm = height / (ppi / 25.46);
    return true;
}

#endif
