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
#include <sbi/sbi_string.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/mailbox/rpmi_msgprot.h>
#include <sbi_utils/mpxy/fdt_mpxy.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi.h>
#include <sbi_utils/mpxy/reqfwd_queue.h>

/** Implementation ID and version reported for this service group */
#define MPXY_MM_IMPL_ID			0
#define MPXY_MM_IMPL_VERSION		RPMI_VERSION(1, 0)

/** Service group version implemented here */
#define MPXY_MM_SRVGRP_VERSION		RPMI_VERSION(1, 0)

/** Management Mode version reported when the DT does not give one */
#define MPXY_MM_VERSION_DEFAULT		RPMI_VERSION(1, 0)

/** How long MM_COMMUNICATE waits for the MM agent, in microseconds */
#define MPXY_MM_COMPLETION_TIMEOUT_US	1000000

/** MM_COMMUNICATE reaches the MM agent, MM_GET_ATTRIBUTES does not */
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
 * Management Mode runs in software served by an RPMI REQUEST_FORWARD
 * channel rather than behind an RPMI mailbox.
 */
struct mpxy_mm_forward {
	struct mpxy_rpmi_channel_attrs msgprot_attrs;
	struct sbi_mpxy_channel channel;
	/** Queue the MM requests are forwarded to */
	struct reqfwd_queue *queue;
	/** ID the queue is registered under */
	u32 queue_id;
	/** Management Mode attributes answered from the device tree */
	struct rpmi_mm_attributes mma;
	/** Token of the next forwarded RPMI request message */
	atomic_t token;
};

static int mpxy_mm_read_attributes(struct sbi_mpxy_channel *channel,
				   u32 *outmem, u32 base_attr_id,
				   u32 attr_count)
{
	struct mpxy_mm_forward *mm =
		container_of(channel, struct mpxy_mm_forward, channel);

	return mpxy_rpmi_read_attrs(&mm->msgprot_attrs, outmem,
				    base_attr_id, attr_count);
}

/** The MANAGEMENT_MODE service group defines no notification event */
static int mpxy_mm_enable_notification(struct mpxy_mm_forward *mm,
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

static int mpxy_mm_get_attributes(struct mpxy_mm_forward *mm,
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
static bool mpxy_mm_range_ok(struct mpxy_mm_forward *mm, u32 off, u32 size)
{
	if (off > mm->mma.shmem_size)
		return false;

	/* Written this way so that off + size cannot wrap */
	return size <= (mm->mma.shmem_size - off);
}

static int mpxy_mm_communicate(struct mpxy_mm_forward *mm,
			       void *tx, u32 tx_len,
			       void *rx, u32 rx_max_len,
			       unsigned long *ack_len)
{
	const struct rpmi_mm_communicate_req *req = tx;
	struct rpmi_mm_communicate_rsp *rsp = rx;
	u32 ipoff, ipsize, opoff, opsize;
	unsigned long rsp_len = 0;
	struct reqfwd_message fmsg;
	s32 status;
	int ret;

	/*
	 * The forwarded message queue never touches the memory of the RPMI
	 * client, so the forwarded request and its response are bounced
	 * through firmware memory. They live on this stack frame, which
	 * outlives the wait, so several HARTs can be in flight at once.
	 */
	struct mpxy_mm_forwarded_request fwd;
	struct rpmi_mm_communicate_rsp fwd_rsp;

	if (tx_len < sizeof(*req) || rx_max_len < sizeof(*rsp))
		return SBI_EINVAL;

	/*
	 * The REQUEST_FORWARD channel may well have been probed after this
	 * one, so its queue is looked up on first use rather than at probe
	 * time.
	 */
	if (!mm->queue) {
		mm->queue = reqfwd_queue_find(mm->queue_id);
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

	ret = reqfwd_queue_send(mm->queue, &fmsg, &fwd, sizeof(fwd),
				&fwd_rsp, sizeof(fwd_rsp), &rsp_len,
				mm->channel.attrs.msg_completion_timeout);
	if (ret == SBI_ETIMEDOUT) {
		status = RPMI_ERR_TIMEOUT;
		goto out;
	}
	if (ret)
		return ret;

	/*
	 * The MM agent answers with the response data of the RPMI message
	 * it was given, which is what this service must return.
	 */
	if (rsp_len != sizeof(fwd_rsp)) {
		status = RPMI_ERR_IO;
		goto out;
	}

	/*
	 * The RPMI client reads MM_COMM_RETURN_DATA_SIZE bytes at
	 * MM_COMM_OUTPUT_DATA_OFFSET, so an agent claiming more than the
	 * output area it was given would send the client reading past that
	 * area and possibly past the MM shared memory. Only the offsets of
	 * the request were checked on the way in, so check this on the way
	 * out.
	 */
	if (le32_to_cpu(fwd_rsp.mm_comm_retdata_size) > opsize) {
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
	struct mpxy_mm_forward *mm =
		container_of(channel, struct mpxy_mm_forward, channel);

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

/** Read the MM shared memory reported by MM_GET_ATTRIBUTES */
static int mpxy_mm_parse_shmem(const void *fdt, int nodeoff,
			       struct rpmi_mm_attributes *mma)
{
	const fdt32_t *val;
	u64 base, size;
	int len;

	val = fdt_getprop(fdt, nodeoff, "riscv,rpmi-mm-shmem", &len);
	if (!val || len < 16)
		return SBI_ENODEV;

	base = ((u64)fdt32_to_cpu(val[0]) << 32) | fdt32_to_cpu(val[1]);
	size = ((u64)fdt32_to_cpu(val[2]) << 32) | fdt32_to_cpu(val[3]);

	/* The RPMI attribute reporting the size is 32-bit */
	if (!size || size > 0xffffffffULL)
		return SBI_EINVAL;

	mma->shmem_addr_lo = (u32)base;
	mma->shmem_addr_hi = (u32)(base >> 32);
	mma->shmem_size = (u32)size;

	return SBI_OK;
}

/** Read the MPXY channel ID of the REQUEST_FORWARD channel to forward to */
static int mpxy_mm_parse_queue_id(const void *fdt, int nodeoff, u32 *queue_id)
{
	const fdt32_t *val;
	int coff, len;

	val = fdt_getprop(fdt, nodeoff, "opensbi-reqfwd-channel", &len);
	if (!val || len < 4)
		return SBI_ENODEV;

	coff = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*val));
	if (coff < 0)
		return SBI_EINVAL;

	if (fdt_node_check_compatible(fdt, coff,
				      "riscv,rpmi-mpxy-request-forward"))
		return SBI_EINVAL;

	val = fdt_getprop(fdt, coff, "riscv,sbi-mpxy-channel-id", &len);
	if (!val || len < 4)
		return SBI_EINVAL;

	*queue_id = fdt32_to_cpu(*val);

	return SBI_OK;
}

static int mpxy_mm_init(const void *fdt, int nodeoff,
			const struct fdt_match *match)
{
	struct mpxy_mm_forward *mm;
	const fdt32_t *val;
	int rc, len;

	val = fdt_getprop(fdt, nodeoff, "riscv,sbi-mpxy-channel-id", &len);
	if (!val || len < 4)
		return SBI_ENODEV;

	mm = sbi_zalloc(sizeof(*mm));
	if (!mm)
		return SBI_ENOMEM;

	mm->channel.channel_id = fdt32_to_cpu(*val);
	mm->channel.owner_domain = &root;

	rc = mpxy_mm_parse_queue_id(fdt, nodeoff, &mm->queue_id);
	if (rc)
		goto fail_free;

	if (mm->queue_id == mm->channel.channel_id) {
		rc = SBI_EINVAL;
		goto fail_free;
	}

	rc = mpxy_mm_parse_shmem(fdt, nodeoff, &mm->mma);
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
	{ .compatible = "riscv,rpmi-mpxy-mm-forward" },
	{},
};

const struct fdt_driver fdt_mpxy_rpmi_mm_forward = {
	.experimental = true,
	.match_table = mpxy_mm_match,
	.init = mpxy_mm_init,
};
