/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Marouene Boubakri
 *
 * Authors:
 *   Marouene Boubakri <marouene.boubakri@nxp.com>
 */

#include <libfdt.h>
#include <sbi/riscv_atomic.h>
#include <sbi/sbi_byteorder.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_reqfwd.h>
#include <sbi/sbi_string.h>
#include <sbi_utils/fdt/fdt_domain.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/mailbox/rpmi_msgprot.h>
#include <sbi_utils/mpxy/fdt_mpxy.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi.h>

/** Implementation ID and version reported for this service group */
#define MPXY_MM_IMPL_ID			0
#define MPXY_MM_IMPL_VERSION		RPMI_VERSION(1, 0)

/** Service group version implemented here */
#define MPXY_MM_SRVGRP_VERSION		RPMI_VERSION(1, 0)

/** Management Mode version reported when the DT does not give one */
#define MPXY_MM_VERSION_DEFAULT		RPMI_VERSION(1, 0)

/** How long MM_COMMUNICATE waits for the target domain, in microseconds */
#define MPXY_MM_COMPLETION_TIMEOUT_US	1000000

/** MM_COMMUNICATE reaches the target domain, MM_GET_ATTRIBUTES does not */
#define MPXY_MM_SEND_TIMEOUT_US		10

/** Longest service message of this service group */
#define MPXY_MM_MSG_MAX_LEN		sizeof(struct rpmi_mm_get_attributes_rsp)

/** The forwarded MM_COMMUNICATE request message */
struct mpxy_mm_forwarded_request {
	struct rpmi_message_header header;
	struct rpmi_mm_communicate_req data;
} __packed;

/**
 * An MPXY channel serving the RPMI MANAGEMENT_MODE service group where
 * Management Mode runs in another OpenSBI domain rather than behind an
 * RPMI mailbox.
 */
struct mpxy_mm_domain {
	struct mpxy_rpmi_channel_attrs msgprot_attrs;
	struct sbi_mpxy_channel channel;
	/** Domain running Management Mode */
	const struct sbi_domain *target;
	/** Forwarded message queue of the target domain */
	struct sbi_reqfwd_queue *queue;
	/** Management Mode attributes answered from the device tree */
	struct rpmi_mm_attributes mma;
	/** Token of the next forwarded RPMI request message */
	atomic_t token;
};

static int mpxy_mm_read_attributes(struct sbi_mpxy_channel *channel,
				   u32 *outmem, u32 base_attr_id,
				   u32 attr_count)
{
	struct mpxy_mm_domain *mm =
		container_of(channel, struct mpxy_mm_domain, channel);

	return mpxy_rpmi_read_attrs(&mm->msgprot_attrs, outmem,
				    base_attr_id, attr_count);
}

/** The MANAGEMENT_MODE service group defines no notification event */
static int mpxy_mm_enable_notification(struct mpxy_mm_domain *mm,
				       void *tx, u32 tx_len,
				       void *rx, u32 rx_max_len,
				       unsigned long *ack_len)
{
	struct rpmi_enable_notification_resp *rsp = rx;

	if (tx_len < sizeof(struct rpmi_enable_notification_req) ||
	    rx_max_len < sizeof(*rsp))
		return SBI_EINVAL;

	rsp->status = cpu_to_le32(RPMI_ERR_NOTSUPP);
	rsp->current_state = cpu_to_le32(RPMI_EVENT_NOTIF_DISABLE_STATE);
	*ack_len = sizeof(*rsp);

	return SBI_OK;
}

static int mpxy_mm_get_attributes(struct mpxy_mm_domain *mm,
				  void *rx, u32 rx_max_len,
				  unsigned long *ack_len)
{
	struct rpmi_mm_get_attributes_rsp *rsp = rx;

	if (rx_max_len < sizeof(*rsp))
		return SBI_EINVAL;

	rsp->status = cpu_to_le32(RPMI_SUCCESS);
	rsp->mma.mm_version = cpu_to_le32(mm->mma.mm_version);
	rsp->mma.shmem_addr_lo = cpu_to_le32(mm->mma.shmem_addr_lo);
	rsp->mma.shmem_addr_hi = cpu_to_le32(mm->mma.shmem_addr_hi);
	rsp->mma.shmem_size = cpu_to_le32(mm->mma.shmem_size);
	*ack_len = sizeof(*rsp);

	return SBI_OK;
}

/** Both data areas must lie inside the MM shared memory */
static bool mpxy_mm_range_ok(struct mpxy_mm_domain *mm, u32 off, u32 size)
{
	if (off > mm->mma.shmem_size)
		return false;

	/* Written this way so that off + size cannot wrap */
	return size <= (mm->mma.shmem_size - off);
}

static int mpxy_mm_communicate(struct mpxy_mm_domain *mm,
			       void *tx, u32 tx_len,
			       void *rx, u32 rx_max_len,
			       unsigned long *ack_len)
{
	const struct rpmi_mm_communicate_req *req = tx;
	struct rpmi_mm_communicate_rsp *rsp = rx;
	u32 ipoff, ipsize, opoff, opsize;
	unsigned long rsp_len = 0;
	struct sbi_reqfwd_message fmsg;
	s32 status;
	int ret;

	/*
	 * The forwarded message queue never touches domain memory, so the
	 * forwarded request and its response are bounced through firmware
	 * memory. They live on this stack frame, which outlives the wait,
	 * so several HARTs of the owner domain can be in flight at once.
	 */
	struct mpxy_mm_forwarded_request fwd;
	struct rpmi_mm_communicate_rsp fwd_rsp;

	if (tx_len < sizeof(*req) || rx_max_len < sizeof(*rsp))
		return SBI_EINVAL;

	if (!mm->queue) {
		mm->queue = sbi_reqfwd_find_queue(mm->target);
		if (!mm->queue)
			return SBI_ENODEV;
	}

	ipoff = le32_to_cpu(req->mm_comm_ipdata_off);
	ipsize = le32_to_cpu(req->mm_comm_ipdata_size);
	opoff = le32_to_cpu(req->mm_comm_opdata_off);
	opsize = le32_to_cpu(req->mm_comm_opdata_size);

	if (!mpxy_mm_range_ok(mm, ipoff, ipsize) ||
	    !mpxy_mm_range_ok(mm, opoff, opsize)) {
		status = RPMI_ERR_INVALID_ADDR;
		goto out;
	}

	/*
	 * REQUEST_FORWARD forwards a complete RPMI message, so build the
	 * message header the RPMI client would have sent to a platform
	 * microcontroller and put the service request data behind it.
	 */
	fwd.header.servicegroup_id = cpu_to_le16(RPMI_SRVGRP_MANAGEMENT_MODE);
	fwd.header.service_id = RPMI_MM_SRV_COMMUNICATE;
	fwd.header.flags = RPMI_MSG_NORMAL_REQUEST;
	fwd.header.datalen = cpu_to_le16(sizeof(fwd.data));
	fwd.header.token = cpu_to_le16(atomic_add_return(&mm->token, 1));
	sbi_memcpy(&fwd.data, req, sizeof(fwd.data));

	ret = sbi_reqfwd_send(mm->queue, &fmsg, &fwd, sizeof(fwd),
			      &fwd_rsp, sizeof(fwd_rsp), &rsp_len,
			      mm->channel.attrs.msg_completion_timeout);
	if (ret == SBI_ETIMEDOUT) {
		status = RPMI_ERR_TIMEOUT;
		goto out;
	}
	if (ret)
		return ret;

	/*
	 * The target domain answers with the response data of the RPMI
	 * message it was given, which is what this service must return.
	 */
	if (rsp_len != sizeof(fwd_rsp)) {
		status = RPMI_ERR_IO;
		goto out;
	}

	/*
	 * Both the forwarded response and the response of this service are
	 * little-endian RPMI message data, so the words are passed through
	 * without a conversion.
	 */
	rsp->status = fwd_rsp.status;
	rsp->mm_comm_retdata_size = fwd_rsp.mm_comm_retdata_size;
	*ack_len = sizeof(*rsp);

	return SBI_OK;

out:
	rsp->status = cpu_to_le32(status);
	rsp->mm_comm_retdata_size = cpu_to_le32(0);
	*ack_len = sizeof(*rsp);

	return SBI_OK;
}

static int mpxy_mm_send_message(struct sbi_mpxy_channel *channel,
				u32 message_id, void *tx, u32 tx_len,
				void *rx, u32 rx_max_len,
				unsigned long *ack_len)
{
	struct mpxy_mm_domain *mm =
		container_of(channel, struct mpxy_mm_domain, channel);

	if (!rx || !ack_len)
		return SBI_EINVAL;

	switch (message_id) {
	case RPMI_MM_SRV_ENABLE_NOTIFICATION:
		return mpxy_mm_enable_notification(mm, tx, tx_len, rx,
						   rx_max_len, ack_len);
	case RPMI_MM_SRV_GET_ATTRIBUTES:
		return mpxy_mm_get_attributes(mm, rx, rx_max_len, ack_len);
	case RPMI_MM_SRV_COMMUNICATE:
		return mpxy_mm_communicate(mm, tx, tx_len, rx, rx_max_len,
					   ack_len);
	default:
		break;
	}

	return SBI_ENOTSUPP;
}

/**
 * Read the MM shared memory from the domain memory region DT node it is
 * described by. It has to be a domain memory region anyway for both
 * domains to reach it.
 */
static int mpxy_mm_parse_region(const void *fdt, int nodeoff,
				struct rpmi_mm_attributes *mma)
{
	const fdt32_t *val;
	u64 base, size;
	int roff, len;

	val = fdt_getprop(fdt, nodeoff, "opensbi-mm-memregion", &len);
	if (!val || len < 4)
		return SBI_ENODEV;

	roff = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*val));
	if (roff < 0)
		return SBI_EINVAL;

	if (fdt_node_check_compatible(fdt, roff, "opensbi,domain,memregion"))
		return SBI_EINVAL;

	val = fdt_getprop(fdt, roff, "base", &len);
	if (!val || len < 8)
		return SBI_EINVAL;
	base = ((u64)fdt32_to_cpu(val[0]) << 32) | fdt32_to_cpu(val[1]);

	val = fdt_getprop(fdt, roff, "order", &len);
	if (!val || len < 4)
		return SBI_EINVAL;
	len = fdt32_to_cpu(*val);
	if (len < 3 || len >= 32)
		return SBI_EINVAL;
	size = BIT(len);

	mma->shmem_addr_lo = (u32)base;
	mma->shmem_addr_hi = (u32)(base >> 32);
	mma->shmem_size = (u32)size;

	return SBI_OK;
}

static int mpxy_mm_init(const void *fdt, int nodeoff,
			const struct fdt_match *match)
{
	struct mpxy_mm_domain *mm;
	const fdt32_t *val;
	int rc, len, doff;

	val = fdt_getprop(fdt, nodeoff, "riscv,sbi-mpxy-channel-id", &len);
	if (!val || len < 4)
		return SBI_ENODEV;

	mm = sbi_zalloc(sizeof(*mm));
	if (!mm)
		return SBI_ENOMEM;

	mm->channel.channel_id = fdt32_to_cpu(*val);

	/* The channel is owned by the domain calling MM_COMMUNICATE */
	mm->channel.owner_domain = fdt_mpxy_get_owner_domain(fdt, nodeoff);
	if (!mm->channel.owner_domain) {
		rc = SBI_EINVAL;
		goto fail_free;
	}

	/* Management Mode runs in the domain the requests are forwarded to */
	val = fdt_getprop(fdt, nodeoff, "opensbi-reqfwd-target", &len);
	if (!val || len < 4) {
		rc = SBI_ENODEV;
		goto fail_free;
	}
	doff = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*val));
	mm->target = fdt_domain_get(fdt, doff);
	if (!mm->target || mm->target == mm->channel.owner_domain) {
		rc = SBI_EINVAL;
		goto fail_free;
	}

	rc = mpxy_mm_parse_region(fdt, nodeoff, &mm->mma);
	if (rc)
		goto fail_free;

	mm->mma.mm_version = MPXY_MM_VERSION_DEFAULT;
	val = fdt_getprop(fdt, nodeoff, "riscv,rpmi-mm-version", &len);
	if (val && len >= 4)
		mm->mma.mm_version = fdt32_to_cpu(*val);

	mm->channel.attrs.msg_completion_timeout =
		MPXY_MM_COMPLETION_TIMEOUT_US;
	val = fdt_getprop(fdt, nodeoff,
			  "riscv,sbi-mpxy-completion-timeout-us", &len);
	if (val && len >= 4)
		mm->channel.attrs.msg_completion_timeout = fdt32_to_cpu(*val);

	mm->channel.read_attributes = mpxy_mm_read_attributes;
	mm->channel.send_message_with_response = mpxy_mm_send_message;

	mm->channel.attrs.msg_proto_id = SBI_MPXY_MSGPROTO_RPMI_ID;
	mm->channel.attrs.msg_proto_version = RPMI_VERSION(1, 0);
	mm->channel.attrs.msg_data_maxlen = MPXY_MM_MSG_MAX_LEN;
	mm->channel.attrs.msg_send_timeout = MPXY_MM_SEND_TIMEOUT_US;

	mm->msgprot_attrs.servicegrp_id = RPMI_SRVGRP_MANAGEMENT_MODE;
	mm->msgprot_attrs.servicegrp_ver = MPXY_MM_SRVGRP_VERSION;
	mm->msgprot_attrs.impl_id = MPXY_MM_IMPL_ID;
	mm->msgprot_attrs.impl_ver = MPXY_MM_IMPL_VERSION;

	rc = sbi_mpxy_register_channel(&mm->channel);
	if (rc)
		goto fail_free;

	return SBI_OK;

fail_free:
	sbi_free(mm);
	return rc;
}

static const struct fdt_match mpxy_mm_match[] = {
	{ .compatible = "riscv,rpmi-mpxy-mm-domain" },
	{},
};

const struct fdt_driver fdt_mpxy_rpmi_mm_domain = {
	.experimental = true,
	.match_table = mpxy_mm_match,
	.init = mpxy_mm_init,
};
