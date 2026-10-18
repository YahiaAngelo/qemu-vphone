/*
 * Apple A13 GXF.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef HW_ARM_APPLE_A13_GXF_H
#define HW_ARM_APPLE_A13_GXF_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/a13.h"

void apple_a13_init_gxf(AppleA13State *cpu);

/*
 * The GXF and SPRR system registers for any ARMCPU with ARM_FEATURE_GXF:
 * apple_gxf_define_cpregs() before realize, apple_gxf_define_override_cpregs()
 * (the GL-banked versions of standard EL1 registers) after it.
 */
void apple_gxf_define_cpregs(ARMCPU *cpu);
void apple_gxf_define_override_cpregs(ARMCPU *cpu);
/* The GL1-banked registers at the M1 encodings (S3_6_C15_C10_n) as well */
void apple_gxf_define_m1_gl_cpregs(ARMCPU *cpu);
/* M1-style: of the overrides, only VBAR_EL1 (EL1 registers stay unbanked) */
void apple_gxf_define_m1_override_cpregs(ARMCPU *cpu);

void apple_a13_init_gxf_override(AppleA13State *cpu);

#endif
