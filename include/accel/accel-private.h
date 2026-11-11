/*
 * Pages of vCPU-private address spaces for hardware accelerators
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#pragma once

#include "exec/hwaddr.h"

/// How an accelerator with a single guest physical address space maps and unmaps pages in it.
typedef struct
{
    void (*map)(void* host, hwaddr ipa, uint64_t size, bool writable);
    void (*unmap)(hwaddr ipa, uint64_t size);
} AccelPrivateOps;

void accel_private_init(const AccelPrivateOps* ops);
/// Maps the page at `ipa` of a vCPU's own address space `as`, if it's RAM there and system memory leaves it empty;
/// returns whether the access is to be retried.
bool accel_private_map_page(AddressSpace* as, hwaddr ipa, bool is_write);
/// System memory mapped `section`, which overrides private pages.
void accel_private_system_region_add(MemoryRegionSection* section);
