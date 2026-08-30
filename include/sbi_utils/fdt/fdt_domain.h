// SPDX-License-Identifier: BSD-2-Clause
/*
 * fdt_domain.c - Flat Device Tree Domain helper routines
 *
 * Copyright (c) 2020 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#ifndef __FDT_DOMAIN_H__
#define __FDT_DOMAIN_H__

#include <sbi/sbi_types.h>

#ifdef CONFIG_FDT_DOMAIN

struct sbi_domain;

/**
 * Iterate over each domains in device tree
 *
 * @param fdt device tree blob
 * @param opaque private pointer for each iteration
 * @param fn callback function for each iteration
 *
 * @return 0 on success and negative error code on failure
 */
int fdt_iterate_each_domain(void *fdt, void *opaque,
			    int (*fn)(void *fdt, int domain_offset,
				      void *opaque));

/**
 * Iterate over each memregion of a domain in device tree
 *
 * @param fdt device tree blob
 * @param domain_offset domain DT node offset
 * @param opaque private pointer for each iteration
 * @param fn callback function for each iteration
 *
 * @return 0 on success and negative error code on failure
 */
int fdt_iterate_each_memregion(void *fdt, int domain_offset, void *opaque,
			       int (*fn)(void *fdt, int domain_offset,
					 int region_offset, u32 region_access,
					 void *opaque));

/**
 * Fix up the domain configuration in the device tree
 *
 * This routine:
 * 1. Disables MMIO devices not accessible to the coldboot HART domain
 * 2. Removes "opensbi-domain" DT property from CPU DT nodes
 * 3. Removes domain configuration DT node under /chosen DT node
 *
 * It is recommended that platform support call this function in
 * their final_init() platform operation.
 *
 * @param fdt device tree blob
 */
void fdt_domain_fixup(void *fdt);

/**
 * Populate domains from device tree
 *
 * It is recommended that platform support call this function in
 * their domains_init() platform operation.
 *
 * @param fdt device tree blob
 *
 * @return 0 on success and negative error code on failure
 */
int fdt_domains_populate(const void *fdt);

/**
 * Find the domain registered for a domain instance DT node
 *
 * This only works after the domains have been populated, so callers
 * must not use it before the platform domains_init() operation has run.
 *
 * @param fdt device tree blob
 * @param domain_offset domain instance DT node offset
 *
 * @return pointer to the domain or NULL if not found
 */
struct sbi_domain *fdt_domain_get(const void *fdt, int domain_offset);

#else

static inline void fdt_domain_fixup(void *fdt) { }
static inline int fdt_domains_populate(const void *fdt) { return 0; }
static inline struct sbi_domain *fdt_domain_get(const void *fdt,
						int domain_offset)
{
	return NULL;
}

#endif

#endif /* __FDT_DOMAIN_H__ */
