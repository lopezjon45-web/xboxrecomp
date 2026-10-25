/* Private Windows layout allocator. Include after the Win32 declarations.
 *
 * A free RAM view alone says nothing about base + guest MMIO addresses. In
 * particular, a RAM base near 3 GB can put the physical window over the host
 * executable at 0x140000000. Select room for the whole guest layout, then map
 * ALL required windows before loading the XBE or starting polling threads.
 *
 * VirtualFree cannot release a slice of a Win32 reservation. Release the
 * address-space probe in full, and treat placement as a transaction: ordinary
 * fixed-address mappings never replace somebody else's memory. If a competing
 * allocation takes any slice, unwind our views and select another full span.
 * No placeholder API/newer Windows SDK is required.
 */
#ifndef XBOX_WINDOWS_GUEST_LAYOUT_H
#define XBOX_WINDOWS_GUEST_LAYOUT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    void *base, *mirrors[XBOX_NUM_MIRRORS];
    void *contig, *tiled, *nv2a, *mcpx, *flash;
} XboxWindowsLayout;

static void xbox_windows_layout_release(XboxWindowsLayout *p)
{
    int i;
    for (i = 0; i < XBOX_NUM_MIRRORS; ++i)
        if (p->mirrors[i]) UnmapViewOfFile(p->mirrors[i]);
    if (p->tiled) UnmapViewOfFile(p->tiled);
    if (p->contig) UnmapViewOfFile(p->contig);
    if (p->base) UnmapViewOfFile(p->base);
    if (p->nv2a) VirtualFree(p->nv2a, 0, MEM_RELEASE);
    if (p->mcpx) VirtualFree(p->mcpx, 0, MEM_RELEASE);
    if (p->flash) VirtualFree(p->flash, 0, MEM_RELEASE);
    memset(p, 0, sizeof *p);
}

static int xbox_windows_overlap(uint64_t lo, uint64_t hi,
                                uint64_t base, uint64_t size)
{
    return lo < base + size && base < hi;
}

#if XBOXRECOMP_UWP
/* App partition: MapViewOfFileEx is not in its headers, the placeholder calls
 * are (api-ms-win-core-memory-l1-1-6, Windows 10 1803 and later). One
 * placeholder the size of the span is reserved once and is ours from then on,
 * so nothing can take a slice of it and there is no retry. The plan tiles the
 * span with the regions and the holes between them in address order; each
 * entry but the last is split off the remaining placeholder (a split the size
 * of the whole placeholder is an error), a region is then replaced in place
 * and a hole is left as it is: reserved, inaccessible, faulting on a guest
 * touch as it does on desktop. Nothing here is executable, so the FromApp
 * restriction on executable pages never applies. */

typedef enum {
    XBOX_BACKING_HOLE,
    XBOX_BACKING_RAM,
    XBOX_BACKING_CONTIG,
    XBOX_BACKING_PRIVATE
} XboxBacking;

typedef struct {
    uint64_t va;            /* guest VA, a 64 KB multiple */
    uint64_t size;          /* a 64 KB multiple */
    XboxBacking backing;
    void **out;             /* where the host address lands; NULL for a hole */
} XboxRegion;

#define XBOX_LAYOUT_GRANULARITY UINT64_C(0x10000)
#define XBOX_LAYOUT_MAX_REGIONS (2 * (1 + XBOX_NUM_MIRRORS + 5) + 1)

static uint64_t xbox_windows_layout_span(size_t bytes)
{
    uint64_t span = (uint64_t)bytes * (uint64_t)(1 + XBOX_NUM_MIRRORS);
    return span < UINT64_C(0x100000000) ? UINT64_C(0x100000000) : span;
}

/* Pure. The regions of [0, span) in address order, holes included, so that
 * they tile it. The same constants and mirror rule as the desktop body: a
 * mirror that overlaps a hardware window is skipped. Returns the count, or 0
 * when a region is misaligned, empty, outside the span or overlaps another. */
static size_t xbox_windows_layout_plan(XboxWindowsLayout *p, uint64_t span,
                                       size_t bytes, size_t tiled_bytes,
                                       XboxRegion r[XBOX_LAYOUT_MAX_REGIONS])
{
    XboxRegion m[1 + XBOX_NUM_MIRRORS + 5];
    size_t n = 0, i, k;
    uint64_t at = 0;
#define XBOX_REGION(va_, size_, backing_, out_) \
    (m[n].va = (va_), m[n].size = (size_), m[n].backing = (backing_), m[n].out = (out_), n++)
    XBOX_REGION(0, bytes, XBOX_BACKING_RAM, &p->base);
    for (i = 0; i < XBOX_NUM_MIRRORS; ++i) {
        uint64_t lo = (uint64_t)(i + 1) * bytes, hi = lo + bytes;
        if (xbox_windows_overlap(lo, hi, XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_TILED_BASE, tiled_bytes) ||
            xbox_windows_overlap(lo, hi, XBOX_NV2A_BASE, XBOX_NV2A_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_MCPX_BASE, XBOX_MCPX_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_FLASH_BASE, XBOX_FLASH_SIZE))
            continue;
        XBOX_REGION(lo, bytes, XBOX_BACKING_RAM, &p->mirrors[i]);
    }
    XBOX_REGION(XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE, XBOX_BACKING_CONTIG, &p->contig);
    XBOX_REGION(XBOX_TILED_BASE, tiled_bytes, XBOX_BACKING_CONTIG, &p->tiled);
    XBOX_REGION(XBOX_NV2A_BASE, XBOX_NV2A_SIZE, XBOX_BACKING_PRIVATE, &p->nv2a);
    XBOX_REGION(XBOX_MCPX_BASE, XBOX_MCPX_SIZE, XBOX_BACKING_PRIVATE, &p->mcpx);
    XBOX_REGION(XBOX_FLASH_BASE, XBOX_FLASH_SIZE, XBOX_BACKING_PRIVATE, &p->flash);
#undef XBOX_REGION
    for (i = 1; i < n; ++i) {
        XboxRegion t = m[i];
        for (k = i; k > 0 && m[k - 1].va > t.va; --k) m[k] = m[k - 1];
        m[k] = t;
    }
    k = 0;
    for (i = 0; i < n; ++i) {
        if (m[i].va < at || !m[i].size ||
            m[i].va % XBOX_LAYOUT_GRANULARITY || m[i].size % XBOX_LAYOUT_GRANULARITY ||
            m[i].va + m[i].size > span)
            return 0;
        if (m[i].va > at) {
            r[k].va = at; r[k].size = m[i].va - at;
            r[k].backing = XBOX_BACKING_HOLE; r[k].out = NULL;
            k++;
        }
        r[k++] = m[i];
        at = m[i].va + m[i].size;
    }
    if (at < span) {
        r[k].va = at; r[k].size = span - at;
        r[k].backing = XBOX_BACKING_HOLE; r[k].out = NULL;
        k++;
    }
    return k;
}

/* No retry. Failure means the console refused the span or a placeholder call;
 * the caller logs GetLastError() and stops, as it does today. Nothing is
 * unwound: the process is about to exit. p->base is the placeholder's start,
 * so every guest VA still lands at base + VA. */
static int xbox_windows_layout_create(XboxWindowsLayout *p, HANDLE ram,
                                      HANDLE physical, size_t bytes,
                                      size_t tiled_bytes)
{
    XboxRegion r[XBOX_LAYOUT_MAX_REGIONS];
    uint64_t span = xbox_windows_layout_span(bytes);
    size_t n, i;
    char *base;
    memset(p, 0, sizeof *p);
    n = xbox_windows_layout_plan(p, span, bytes, tiled_bytes, r);
    if (!n) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    base = VirtualAlloc2FromApp(NULL, NULL, (SIZE_T)span,
                                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (!base) return 0;
    for (i = 0; i < n; ++i) {
        void *at = base + r[i].va, *got = NULL;
        SIZE_T size = (SIZE_T)r[i].size;
        if (i + 1 < n && !VirtualFree(at, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            return 0;
        switch (r[i].backing) {
        case XBOX_BACKING_HOLE:
            continue;
        case XBOX_BACKING_RAM:
        case XBOX_BACKING_CONTIG:
            /* Contig and tiled both map offset 0 of the contig section, so the
             * tiled aperture aliases contig, not RAM. */
            got = MapViewOfFile3FromApp(r[i].backing == XBOX_BACKING_RAM ? ram : physical,
                                        GetCurrentProcess(), at, 0, size,
                                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
            break;
        case XBOX_BACKING_PRIVATE:
            got = VirtualAlloc2FromApp(NULL, at, size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                                       PAGE_READWRITE, NULL, 0);
            if (got && !VirtualAlloc(got, size, MEM_COMMIT, PAGE_READWRITE)) got = NULL;
            break;
        }
        if (!got) return 0;
        *r[i].out = got;
    }
    return 1;
}

#else /* !XBOXRECOMP_UWP */

static int xbox_windows_layout_try(XboxWindowsLayout *p, uintptr_t base,
                                   HANDLE ram, HANDLE physical, size_t bytes,
                                   size_t tiled_bytes)
{
    DWORD error;
    int i;
    memset(p, 0, sizeof *p);
    p->base = MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0, bytes, (void *)base);
    if (!p->base) goto failed;
    p->contig = MapViewOfFileEx(physical, FILE_MAP_ALL_ACCESS, 0, 0,
                               XBOX_CONTIG_SIZE, (void *)(base + XBOX_CONTIG_BASE));
    if (!p->contig) goto failed;
    p->tiled = MapViewOfFileEx(physical, FILE_MAP_ALL_ACCESS, 0, 0,
                              tiled_bytes, (void *)(base + XBOX_TILED_BASE));
    if (!p->tiled) goto failed;
    p->nv2a = VirtualAlloc((void *)(base + XBOX_NV2A_BASE), XBOX_NV2A_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p->nv2a) goto failed;
    p->mcpx = VirtualAlloc((void *)(base + XBOX_MCPX_BASE), XBOX_MCPX_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p->mcpx) goto failed;
    p->flash = VirtualAlloc((void *)(base + XBOX_FLASH_BASE), XBOX_FLASH_SIZE,
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p->flash) goto failed;

    for (i = 0; i < XBOX_NUM_MIRRORS; ++i) {
        uint64_t lo = (uint64_t)(i + 1) * bytes, hi = lo + bytes;
        /* Specific hardware windows take precedence over generic RAM wrap.
         * At 128 MB or larger some mirrors cross these guest addresses. */
        if (xbox_windows_overlap(lo, hi, XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_TILED_BASE, tiled_bytes) ||
            xbox_windows_overlap(lo, hi, XBOX_NV2A_BASE, XBOX_NV2A_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_MCPX_BASE, XBOX_MCPX_SIZE) ||
            xbox_windows_overlap(lo, hi, XBOX_FLASH_BASE, XBOX_FLASH_SIZE))
            continue;
        p->mirrors[i] = MapViewOfFileEx(ram, FILE_MAP_ALL_ACCESS, 0, 0,
                                       bytes, (void *)(base + (uintptr_t)lo));
        if (!p->mirrors[i]) goto failed;
    }
    return 1;

failed:
    error = GetLastError();
    xbox_windows_layout_release(p);
    SetLastError(error);
    return 0;
}

static int xbox_windows_layout_create(XboxWindowsLayout *p, HANDLE ram,
                                      HANDLE physical, size_t bytes,
                                      size_t tiled_bytes)
{
    size_t span = bytes * (size_t)(1 + XBOX_NUM_MIRRORS);
    unsigned attempt;
    /* Includes device apertures even when RAM + its mirrors need only 1.8 GB.
     * This reserves virtual address space, not 4 GB of physical memory. */
    if (span < UINT64_C(0x100000000)) span = (size_t)UINT64_C(0x100000000);
    memset(p, 0, sizeof *p);
    for (attempt = 0; attempt < 8; ++attempt) {
        void *probe = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) return 0;
        if (!VirtualFree(probe, 0, MEM_RELEASE)) return 0;
        if (xbox_windows_layout_try(p, (uintptr_t)probe, ram, physical,
                                    bytes, tiled_bytes))
            return 1;
    }
    return 0;
}

#endif /* XBOXRECOMP_UWP */

#endif
