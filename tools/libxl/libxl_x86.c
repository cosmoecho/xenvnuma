#include "libxl_internal.h"
#include "libxl_arch.h"

static const char *e820_names(int type)
{
    switch (type) {
        case E820_RAM: return "RAM";
        case E820_RESERVED: return "Reserved";
        case E820_ACPI: return "ACPI";
        case E820_NVS: return "ACPI NVS";
        case E820_UNUSABLE: return "Unusable";
        default: break;
    }
    return "Unknown";
}

static int e820_sanitize(libxl_ctx *ctx, struct e820entry src[],
                         uint32_t *nr_entries,
                         unsigned long map_limitkb,
                         unsigned long balloon_kb)
{
    uint64_t delta_kb = 0, start = 0, start_kb = 0, last = 0, ram_end;
    uint32_t i, idx = 0, nr;
    struct e820entry e820[E820MAX];

    if (!src || !map_limitkb || !nr_entries)
        return ERROR_INVAL;

    nr = *nr_entries;
    if (!nr)
        return ERROR_INVAL;

    if (nr > E820MAX)
        return ERROR_NOMEM;

    /* Weed out anything under 1MB */
    for (i = 0; i < nr; i++) {
        if (src[i].addr > 0x100000)
            continue;

        src[i].type = 0;
        src[i].size = 0;
        src[i].addr = -1ULL;
    }

    /* Find the lowest and highest entry in E820, skipping over
     * undesired entries. */
    start = -1ULL;
    last = 0;
    for (i = 0; i < nr; i++) {
        if ((src[i].type == E820_RAM) ||
            (src[i].type == E820_UNUSABLE) ||
            (src[i].type == 0))
            continue;

        start = src[i].addr < start ? src[i].addr : start;
        last = src[i].addr + src[i].size > last ?
               src[i].addr + src[i].size > last : last;
    }
    if (start > 1024)
        start_kb = start >> 10;

    /* Add the memory RAM region for the guest */
    e820[idx].addr = 0;
    e820[idx].size = (uint64_t)map_limitkb << 10;
    e820[idx].type = E820_RAM;

    /* .. and trim if neccessary */
    if (start_kb && map_limitkb > start_kb) {
        delta_kb = map_limitkb - start_kb;
        if (delta_kb)
            e820[idx].size -= (uint64_t)(delta_kb << 10);
    }
    /* Note: We don't touch balloon_kb here. Will add it at the end. */
    ram_end = e820[idx].addr + e820[idx].size;
    idx ++;

    LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "Memory: %"PRIu64"kB End of RAM: " \
               "0x%"PRIx64" (PFN) Delta: %"PRIu64"kB, PCI start: %"PRIu64"kB " \
               "(0x%"PRIx64" PFN), Balloon %"PRIu64"kB\n", (uint64_t)map_limitkb,
               ram_end >> 12, delta_kb, start_kb ,start >> 12,
               (uint64_t)balloon_kb);


    /* This whole code below is to guard against if the Intel IGD is passed into
     * the guest. If we don't pass in IGD, this whole code can be ignored.
     *
     * The reason for this code is that Intel boxes fill their E820 with
     * E820_RAM amongst E820_RESERVED and we can't just ditch those E820_RAM.
     * That is b/c any "gaps" in the E820 is considered PCI I/O space by
     * Linux and it would be utilized by the Intel IGD as I/O space while
     * in reality it was an RAM region.
     *
     * What this means is that we have to walk the E820 and for any region
     * that is RAM and below 4GB and above ram_end, needs to change its type
     * to E820_UNUSED. We also need to move some of the E820_RAM regions if
     * the overlap with ram_end. */
    for (i = 0; i < nr; i++) {
        uint64_t end = src[i].addr + src[i].size;

        /* We don't care about E820_UNUSABLE, but we need to
         * change the type to zero b/c the loop after this
         * sticks E820_UNUSABLE on the guest's E820 but ignores
         * the ones with type zero. */
        if ((src[i].type == E820_UNUSABLE) ||
            /* Any region that is within the "RAM region" can
             * be safely ditched. */
            (end < ram_end)) {
                src[i].type = 0;
                continue;
        }

        /* Look only at RAM regions. */
        if (src[i].type != E820_RAM)
            continue;

        /* We only care about RAM regions below 4GB. */
        if (src[i].addr >= (1ULL<<32))
            continue;

        /* E820_RAM overlaps with our RAM region. Move it */
        if (src[i].addr < ram_end) {
            uint64_t delta;

            src[i].type = E820_UNUSABLE;
            delta = ram_end - src[i].addr;
            /* The end < ram_end should weed this out */
            if (src[i].size - delta < 0)
                src[i].type = 0;
            else {
                src[i].size -= delta;
                src[i].addr = ram_end;
            }
            if (src[i].addr + src[i].size != end) {
                /* We messed up somewhere */
                src[i].type = 0;
                LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Computed E820 wrongly. Continuing on.");
            }
        }
        /* Lastly, convert the RAM to UNSUABLE. Look in the Linux kernel
           at git commit 2f14ddc3a7146ea4cd5a3d1ecd993f85f2e4f948
            "xen/setup: Inhibit resource API from using System RAM E820
           gaps as PCI mem gaps" for full explanation. */
        if (end > ram_end)
            src[i].type = E820_UNUSABLE;
    }

    /* Check if there is a region between ram_end and start. */
    if (start > ram_end) {
        int add_unusable = 1;
        for (i = 0; i < nr && add_unusable; i++) {
            if (src[i].type != E820_UNUSABLE)
                continue;
            if (ram_end != src[i].addr)
                continue;
            if (start != src[i].addr + src[i].size) {
                /* there is one, adjust it */
                src[i].size = start - src[i].addr;
            }
            add_unusable = 0;
        }
        /* .. and if not present, add it in. This is to guard against
           the Linux guest assuming that the gap between the end of
           RAM region and the start of the E820_[ACPI,NVS,RESERVED]
           is PCI I/O space. Which it certainly is _not_. */
        if (add_unusable) {
            e820[idx].type = E820_UNUSABLE;
            e820[idx].addr = ram_end;
            e820[idx].size = start - ram_end;
            idx++;
        }
    }
    /* Almost done: copy them over, ignoring the undesireable ones */
    for (i = 0; i < nr; i++) {
        if ((src[i].type == E820_RAM) ||
            (src[i].type == 0))
            continue;

        e820[idx].type = src[i].type;
        e820[idx].addr = src[i].addr;
        e820[idx].size = src[i].size;
        idx++;
    }
    /* At this point we have the mapped RAM + E820 entries from src. */
    if (balloon_kb || delta_kb) {
        /* and if we truncated the RAM region, then add it to the end. */
        e820[idx].type = E820_RAM;
        e820[idx].addr = (uint64_t)(1ULL << 32) > last ?
                         (uint64_t)(1ULL << 32) : last;
        /* also add the balloon memory to the end. */
        e820[idx].size = (uint64_t)(delta_kb << 10) +
                         (uint64_t)(balloon_kb << 10);
        idx++;

    }
    nr = idx;

    for (i = 0; i < nr; i++) {
      LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, ":\t[%"PRIx64" -> %"PRIx64"] %s",
                 e820[i].addr >> 12, (e820[i].addr + e820[i].size) >> 12,
                 e820_names(e820[i].type));
    }

    /* Done: copy the sanitized version. */
    *nr_entries = nr;
    memcpy(src, e820, nr * sizeof(struct e820entry));
    return 0;
}

static int libxl__e820_alloc(libxl__gc *gc, uint32_t domid,
        libxl_domain_config *d_config)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    int rc;
    uint32_t nr;
    struct e820entry map[E820MAX];
    libxl_domain_build_info *b_info;

    if (d_config == NULL || d_config->c_info.type == LIBXL_DOMAIN_TYPE_HVM)
        return ERROR_INVAL;

    b_info = &d_config->b_info;
    if (!libxl_defbool_val(b_info->u.pv.e820_host))
        return ERROR_INVAL;

    rc = xc_get_machine_memory_map(ctx->xch, map, E820MAX);
    if (rc < 0) {
        errno = rc;
        return ERROR_FAIL;
    }
    nr = rc;
    rc = e820_sanitize(ctx, map, &nr, b_info->target_memkb,
                       (b_info->max_memkb - b_info->target_memkb) +
                       b_info->u.pv.slack_memkb);
    if (rc)
        return ERROR_FAIL;

    rc = xc_domain_set_memory_map(ctx->xch, domid, map, nr);

    if (rc < 0) {
        errno  = rc;
        return ERROR_FAIL;
    }
    return 0;
}

int libxl__arch_domain_create(libxl__gc *gc, libxl_domain_config *d_config,
        uint32_t domid)
{
    int ret = 0;
    int tsc_mode;
    uint32_t rtc_timeoffset;
    libxl_ctx *ctx = libxl__gc_owner(gc);

    if (d_config->b_info.type == LIBXL_DOMAIN_TYPE_PV)
        xc_domain_set_memmap_limit(ctx->xch, domid,
                                   (d_config->b_info.max_memkb +
                                    d_config->b_info.u.pv.slack_memkb));

    switch (d_config->b_info.tsc_mode) {
    case LIBXL_TSC_MODE_DEFAULT:
        tsc_mode = 0;
        break;
    case LIBXL_TSC_MODE_ALWAYS_EMULATE:
        tsc_mode = 1;
        break;
    case LIBXL_TSC_MODE_NATIVE:
        tsc_mode = 2;
        break;
    case LIBXL_TSC_MODE_NATIVE_PARAVIRT:
        tsc_mode = 3;
        break;
    default:
        abort();
    }
    xc_domain_set_tsc_info(ctx->xch, domid, tsc_mode, 0, 0, 0);
    if (libxl_defbool_val(d_config->b_info.disable_migrate))
        xc_domain_disable_migrate(ctx->xch, domid);
    rtc_timeoffset = d_config->b_info.rtc_timeoffset;
    if (libxl_defbool_val(d_config->b_info.localtime)) {
        time_t t;
        struct tm *tm;

        t = time(NULL);
        tm = localtime(&t);

        rtc_timeoffset += tm->tm_gmtoff;
    }

    if (rtc_timeoffset)
        xc_domain_set_time_offset(ctx->xch, domid, rtc_timeoffset);

    if (d_config->b_info.type == LIBXL_DOMAIN_TYPE_HVM ||
        libxl_defbool_val(d_config->c_info.pvh)) {

        unsigned long shadow;
        shadow = (d_config->b_info.shadow_memkb + 1023) / 1024;
        xc_shadow_control(ctx->xch, domid, XEN_DOMCTL_SHADOW_OP_SET_ALLOCATION, NULL, 0, &shadow, 0, NULL);
    }

    if (d_config->c_info.type == LIBXL_DOMAIN_TYPE_PV &&
            libxl_defbool_val(d_config->b_info.u.pv.e820_host)) {
        ret = libxl__e820_alloc(gc, domid, d_config);
        if (ret) {
            LIBXL__LOG_ERRNO(gc->owner, LIBXL__LOG_ERROR,
                    "Failed while collecting E820 with: %d (errno:%d)\n",
                    ret, errno);
        }
    }

    return ret;
}

/*
 * Checks for the beginnig and end of RAM in e820 map for domain
 * and aligns start of first and end of last vNUMA memory block to
 * that map. vnode memory size are passed here Megabytes.
 * For PV guest e820 map has fixed hole sizes.
 */
int libxl__vnuma_align_mem(libxl__gc *gc,
                            uint32_t domid,
                            libxl_domain_build_info *b_info, /* IN: mem sizes */
                            vmemrange_t *memblks)        /* OUT: linux numa blocks in pfn */
{
    int i, j, rc;
    uint64_t next_start_pfn, end_max = 0, size, mem_hole;
    uint32_t nr;
    struct e820entry map[E820MAX];
    
    if (b_info->nr_vnodes == 0)
        return -EINVAL;
    libxl_ctx *ctx = libxl__gc_owner(gc);

    /* retreive e820 map for this host */
    rc = xc_get_machine_memory_map(ctx->xch, map, E820MAX);

    if (rc < 0) {
        errno = rc;
        return -EINVAL;
    }
    nr = rc;
    rc = e820_sanitize(ctx, map, &nr, b_info->target_memkb,
                       (b_info->max_memkb - b_info->target_memkb) +
                       b_info->u.pv.slack_memkb);
    if (rc)
    {   
        errno = rc;
        return -EINVAL;
    }

    /* max pfn for this host */
    for (j = nr - 1; j >= 0; j--)
        if (map[j].type == E820_RAM) {
            end_max = map[j].addr + map[j].size;
            break;
        }
    
    memset(memblks, 0, sizeof(*memblks) * b_info->nr_vnodes);
    next_start_pfn = 0;

    memblks[0].start = map[0].addr;

    for(i = 0; i < b_info->nr_vnodes; i++) {
        /* start can be not zero */
        memblks[i].start += next_start_pfn; 
        memblks[i].end = memblks[i].start + (b_info->vnuma_memszs[i] << 20);
        
        size = memblks[i].end - memblks[i].start;
        /*
         * For pv host with e820_host option turned on we need
         * to rake into account memory holes. For pv host with
         * e820_host disabled or unset, the map is contiguous 
         * RAM region.
         */ 
        if (libxl_defbool_val(b_info->u.pv.e820_host)) {
            while (mem_hole = e820_memory_hole_size(memblks[i].start,
                                                 memblks[i].end, map, nr),
                    memblks[i].end - memblks[i].start - mem_hole < size)
            {
                memblks[i].end += mem_hole;

                if (memblks[i].end > end_max) {
                    memblks[i].end = end_max;
                    break;
                }
            }
        }
        next_start_pfn = memblks[i].end;
    }
    
    if (memblks[i-1].end > end_max)
        memblks[i-1].end = end_max;

    return 0;
}

/* 
 * Used for PV guest with e802_host enabled and thus
 * having non-contiguous e820 memory map.
 */
unsigned long e820_memory_hole_size(unsigned long start,
                                    unsigned long end,
                                    struct e820entry e820[],
                                    int nr)
{
    int i;
    unsigned long absent, start_pfn, end_pfn;

    absent = end - start;
    for(i = 0; i < nr; i++) {
        /* if not E820_RAM region, skip it and dont substract from absent */
        if(e820[i].type == E820_RAM) {
            start_pfn = e820[i].addr;
            end_pfn =   e820[i].addr + e820[i].size;
            /* beginning pfn is in this region? */
            if (start >= start_pfn && start <= end_pfn) {
                if (end > end_pfn)
                    absent -= end_pfn - start;
                else
                    /* fit the region? then no absent pages */
                    absent -= end - start;
                continue;
            }
            /* found the end of range in this region? */
            if (end <= end_pfn && end >= start_pfn) {
                absent -= end - start_pfn;
                /* no need to look for more ranges */
                break;
            }
        }
    }
    return absent;
}


