#ifndef _XEN_PUBLIC_VNUMA_H
#define _XEN_PUBLIC_VNUMA_H

#include "xen.h"

/*
 * Following structures are used to represent vNUMA
 * topology to guest if requested.
 */

/*
 * Memory ranges can be used to define
 * vNUMA memory node boundaries by the
 * linked list. As of now, only one range
 * per domain is suported.
 */
struct vmemrange {
    uint64_t start, end;
    uint64_t _reserved;
};

typedef struct vmemrange vmemrange_t;
DEFINE_XEN_GUEST_HANDLE(vmemrange_t);

/*
 * vNUMA topology specifies vNUMA node
 * number, distance table, memory ranges and
 * vcpu mapping provided for guests.
 */

struct vnuma_topology_info {
    /* IN */
    domid_t domid;
    /* OUT */
    union {
        XEN_GUEST_HANDLE(uint) h;
        uint64_t    _pad;
    } nr_vnodes;
    union {
        XEN_GUEST_HANDLE(uint) h;
        uint64_t    _pad;
    } nr_vcpus;
    union {
        XEN_GUEST_HANDLE(uint) h;
        uint64_t    _pad;
    } vdistance;
    union {
        XEN_GUEST_HANDLE(uint) h;
        uint64_t    _pad;
    } vcpu_to_vnode;
    union {
        XEN_GUEST_HANDLE(vmemrange_t) h;
        uint64_t    _pad;
    } vmemrange;
};
typedef struct vnuma_topology_info vnuma_topology_info_t;
DEFINE_XEN_GUEST_HANDLE(vnuma_topology_info_t);

#endif
