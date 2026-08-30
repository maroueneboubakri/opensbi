/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 Ventana Micro Systems Inc.
 *
 * Authors:
 *   Anup Patel <apatel@ventanamicro.com>
 */

#include <libfdt.h>
#include <sbi/sbi_domain.h>
#include <sbi_utils/fdt/fdt_domain.h>
#include <sbi_utils/mpxy/fdt_mpxy.h>

/* List of FDT MPXY drivers generated at compile time */
extern const struct fdt_driver *const fdt_mpxy_drivers[];

int fdt_mpxy_init(const void *fdt)
{
	return fdt_driver_init_all(fdt, fdt_mpxy_drivers);
}

struct sbi_domain *fdt_mpxy_get_owner_domain(const void *fdt, int nodeoff)
{
	const fdt32_t *val;
	int len, doffset;

	val = fdt_getprop(fdt, nodeoff, "opensbi-domain-instance", &len);
	if (!val || len < 4)
		return &root;

	doffset = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*val));
	if (doffset < 0)
		return NULL;

	return fdt_domain_get(fdt, doffset);
}
