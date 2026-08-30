/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 Ventana Micro Systems Inc.
 *
 * Authors:
 *   Anup Patel <apatel@ventanamicro.com>
 */

#include <sbi/sbi_byteorder.h>
#include <sbi/sbi_error.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi.h>

int mpxy_rpmi_read_attrs(const struct mpxy_rpmi_channel_attrs *attrs,
			 u32 *outmem, u32 base_attr_id, u32 attr_count)
{
	const u32 *attr_array = (const u32 *)attrs;
	u32 end_id = base_attr_id + attr_count - 1;
	u32 idx;

	if (end_id >= MPXY_MSGPROT_RPMI_ATTR_MAX_ID)
		return SBI_EBAD_RANGE;

	attr_array += attr_id2index(base_attr_id);
	for (idx = 0; idx < attr_count; idx++)
		outmem[idx] = cpu_to_le32(attr_array[idx]);

	return SBI_OK;
}
