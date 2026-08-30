/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 Ventana Micro Systems Inc.
 *
 * Authors:
 *   Anup Patel <apatel@ventanamicro.com>
 */

#ifndef __FDT_MPXY_RPMI_H__
#define __FDT_MPXY_RPMI_H__

#include <sbi/sbi_mpxy.h>
#include <sbi/sbi_types.h>

/** Convert the mpxy attribute ID to attribute array index */
#define attr_id2index(attr_id)	((attr_id) - SBI_MPXY_ATTR_MSGPROTO_ATTR_START)

enum mpxy_msgprot_rpmi_attr_id {
	MPXY_MSGPROT_RPMI_ATTR_SERVICEGROUP_ID = SBI_MPXY_ATTR_MSGPROTO_ATTR_START,
	MPXY_MSGPROT_RPMI_ATTR_SERVICEGROUP_VERSION,
	MPXY_MSGPROT_RPMI_ATTR_IMPL_ID,
	MPXY_MSGPROT_RPMI_ATTR_IMPL_VERSION,
	MPXY_MSGPROT_RPMI_ATTR_MAX_ID
};

/**
 * MPXY message protocol attributes for RPMI
 * Order of attribute fields must follow the
 * attribute IDs in `enum mpxy_msgprot_rpmi_attr_id`
 */
struct mpxy_rpmi_channel_attrs {
	u32 servicegrp_id;
	u32 servicegrp_ver;
	u32 impl_id;
	u32 impl_ver;
};

/** Make sure all attributes are packed for direct memcpy */
#define assert_field_offset(field, attr_offset)				\
	_Static_assert((offsetof(struct mpxy_rpmi_channel_attrs, field) /\
			sizeof(u32)) == attr_id2index(attr_offset),	\
		       "field " #field " of struct "			\
		       "mpxy_rpmi_channel_attrs is not at " #attr_offset)

assert_field_offset(servicegrp_id, MPXY_MSGPROT_RPMI_ATTR_SERVICEGROUP_ID);
assert_field_offset(servicegrp_ver, MPXY_MSGPROT_RPMI_ATTR_SERVICEGROUP_VERSION);
assert_field_offset(impl_id, MPXY_MSGPROT_RPMI_ATTR_IMPL_ID);
assert_field_offset(impl_ver, MPXY_MSGPROT_RPMI_ATTR_IMPL_VERSION);

/**
 * Read RPMI message protocol attributes of an MPXY channel
 *
 * @param attrs RPMI message protocol attributes of the channel
 * @param outmem little-endian output memory
 * @param base_attr_id first attribute ID to read
 * @param attr_count number of attributes to read
 *
 * @return 0 on success and negative error code on failure
 */
int mpxy_rpmi_read_attrs(const struct mpxy_rpmi_channel_attrs *attrs,
			 u32 *outmem, u32 base_attr_id, u32 attr_count);

#endif
