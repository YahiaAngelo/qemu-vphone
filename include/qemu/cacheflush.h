/*
 * Flush the host cpu caches.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_CACHEFLUSH_H
#define QEMU_CACHEFLUSH_H

/**
 * flush_idcache_range:
 * @rx: instruction address
 * @rw: data address
 * @len: length to flush
 *
 * Flush @len bytes of the data cache at @rw and the icache at @rx
 * to bring them in sync.  The two addresses may be different virtual
 * mappings of the same physical page(s).
 */

#if defined(__i386__) || defined(__x86_64__) || defined(__s390__)

static inline void flush_idcache_range(uintptr_t rx, uintptr_t rw, size_t len)
{
    /* icache is coherent and does not require flushing. */
}

#elif defined(EMSCRIPTEN)

static inline void flush_idcache_range(uintptr_t rx, uintptr_t rw, size_t len)
{
    /* Wasm doesn't have executable region of memory. */
}

#else

void flush_idcache_range(uintptr_t rx, uintptr_t rw, size_t len);

#endif

/*
 * Set when a guest may access its RAM with caching off while QEMU accesses it
 * through cacheable mappings (KVM on a host without stage-2 forced
 * write-back, such as Apple Silicon): QEMU then cleans and invalidates the
 * lines it touches to the Point of Coherency around each access.
 */
extern bool qemu_guest_ram_noncoherent;

#if defined(__aarch64__) && !defined(CONFIG_DARWIN) && !defined(CONFIG_WIN32)
void qemu_dcache_flush_poc(const void *p, size_t len);
#else
static inline void qemu_dcache_flush_poc(const void *p, size_t len) { }
#endif

/* Makes `len` bytes at `p` of guest RAM coherent with a non-cacheable guest. */
static inline void qemu_guest_ram_sync(const void *p, size_t len)
{
    if (qemu_guest_ram_noncoherent) {
        qemu_dcache_flush_poc(p, len);
    }
}

#endif /* QEMU_CACHEFLUSH_H */
