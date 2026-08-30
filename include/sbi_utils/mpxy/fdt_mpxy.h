/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 Ventana Micro Systems Inc.
 *
 * Authors:
 *   Anup Patel <apatel@ventanamicro.com>
 */

#ifndef __FDT_MPXY_H__
#define __FDT_MPXY_H__

#include <sbi/sbi_types.h>
#include <sbi_utils/fdt/fdt_driver.h>

struct sbi_domain;

#ifdef CONFIG_FDT_MPXY

int fdt_mpxy_init(const void *fdt);

/**
 * Get the owner domain of an MPXY channel DT node
 *
 * The optional "opensbi-domain-instance" DT property of an MPXY channel
 * DT node points to the "opensbi,domain,instance" DT node of the domain
 * which owns the channel. A channel DT node without this property is
 * owned by the root domain.
 *
 * @param fdt device tree blob
 * @param nodeoff MPXY channel DT node offset
 *
 * @return pointer to the owner domain or NULL if the DT property is
 * present but does not point to a known domain
 */
struct sbi_domain *fdt_mpxy_get_owner_domain(const void *fdt, int nodeoff);

#else

static inline int fdt_mpxy_init(const void *fdt) { return 0; }

#endif

#endif
