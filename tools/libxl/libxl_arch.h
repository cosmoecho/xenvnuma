/*
 * Copyright (C) 2012      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#ifndef LIBXL_ARCH_H
#define LIBXL_ARCH_H

/* arch specific internal domain creation function */
int libxl__arch_domain_create(libxl__gc *gc, libxl_domain_config *d_config,
               uint32_t domid);

int libxl__vnuma_align_mem(libxl__gc *gc,
                            uint32_t domid,
                            struct libxl_domain_build_info *b_info,
                            vmemrange_t *memblks); 

unsigned long e820_memory_hole_size(unsigned long start,
                                    unsigned long end,
                                    struct e820entry e820[],
                                    int nr);

unsigned int libxl__vnodemap_is_usable(libxl__gc *gc,
                                libxl_domain_build_info *info);

#endif
