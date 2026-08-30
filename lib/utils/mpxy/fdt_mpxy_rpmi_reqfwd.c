/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Marouene Boubakri
 *
 * Authors:
 *   Marouene Boubakri <marouene.boubakri@nxp.com>
 */

#include <libfdt.h>
#include <sbi/sbi_byteorder.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/mailbox/rpmi_msgprot.h>
#include <sbi_utils/mpxy/fdt_mpxy.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi.h>
#include <sbi_utils/mpxy/reqfwd_queue.h>

/** Implementation ID and version reported for this service group */
#define MPXY_REQFWD_IMPL_ID		0
#define MPXY_REQFWD_IMPL_VERSION	RPMI_VERSION(1, 0)

/** Service group version implemented here */
#define MPXY_REQFWD_SRVGRP_VERSION	RPMI_VERSION(1, 0)

/**
 * Default and minimum message data length of the channel. The minimum
 * has to hold the fixed part of a retrieve response plus one 4-byte word
 * of the forwarded message.
 */
#define MPXY_REQFWD_MSG_MAX_LEN_DEFAULT	4096
#define MPXY_REQFWD_MSG_MAX_LEN_MIN	16

/*
 * M-mode answers a retrieve or a complete without talking to anyone, so
 * the channel timeouts only need to be non-zero.
 */
#define MPXY_REQFWD_SEND_TIMEOUT_US	10
#define MPXY_REQFWD_COMPLETION_TIMEOUT_US 10

/** Fixed part of a REQFWD_RETRIEVE_CURRENT_MESSAGE response */
#define MPXY_REQFWD_RETRIEVE_RSP_SIZE	\
	sizeof(struct rpmi_reqfwd_retrieve_current_message_rsp)

/** An MPXY channel serving the RPMI REQUEST_FORWARD service group */
struct mpxy_reqfwd {
	struct mpxy_rpmi_channel_attrs msgprot_attrs;
	struct sbi_mpxy_channel channel;
	struct reqfwd_queue queue;
};

static int mpxy_reqfwd_read_attributes(struct sbi_mpxy_channel *channel,
				       u32 *outmem, u32 base_attr_id,
				       u32 attr_count)
{
	struct mpxy_reqfwd *rf =
		container_of(channel, struct mpxy_reqfwd, channel);

	return mpxy_rpmi_read_attrs(&rf->msgprot_attrs, outmem,
				    base_attr_id, attr_count);
}

/**
 * Notifications are not implemented.
 *
 * REQFWD_NEW_MESSAGE is an RPMI notification message and SBI MPXY
 * delivers such messages through an MSI or an SSE event, neither of
 * which is wired up here. Software waiting for forwarded messages polls
 * REQFWD_RETRIEVE_CURRENT_MESSAGE instead.
 */
static int mpxy_reqfwd_enable_notification(struct mpxy_reqfwd *rf,
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

static int mpxy_reqfwd_retrieve(struct mpxy_reqfwd *rf,
				void *tx, u32 tx_len,
				void *rx, u32 rx_max_len,
				unsigned long *ack_len)
{
	const struct rpmi_reqfwd_retrieve_current_message_req *req = tx;
	struct rpmi_reqfwd_retrieve_current_message_rsp *rsp = rx;
	unsigned long returned = 0, remaining = 0;
	u32 start_index, max_len;
	s32 status;
	int ret;

	if (tx_len < sizeof(*req) ||
	    rx_max_len < MPXY_REQFWD_RETRIEVE_RSP_SIZE)
		return SBI_EINVAL;

	/* The request and the response share the shared memory */
	start_index = le32_to_cpu(req->start_index);

	max_len = rx_max_len;
	if (max_len > rf->channel.attrs.msg_data_maxlen)
		max_len = rf->channel.attrs.msg_data_maxlen;
	max_len -= MPXY_REQFWD_RETRIEVE_RSP_SIZE;

	/* An RPMI message data length is always a multiple of 4 bytes */
	max_len &= ~0x3U;

	ret = reqfwd_queue_retrieve(&rf->queue, start_index,
				    rsp->request_message, max_len,
				    &returned, &remaining);
	switch (ret) {
	case 0:
		status = RPMI_SUCCESS;
		break;
	case SBI_ENOENT:
		status = RPMI_ERR_NO_DATA;
		break;
	default:
		status = RPMI_ERR_INVALID_PARAM;
		break;
	}

	rsp->status = cpu_to_le32(status);
	rsp->remaining = cpu_to_le32(remaining);
	rsp->returned = cpu_to_le32(returned);
	*ack_len = MPXY_REQFWD_RETRIEVE_RSP_SIZE + returned;

	return SBI_OK;
}

static int mpxy_reqfwd_complete(struct mpxy_reqfwd *rf,
				void *tx, u32 tx_len,
				void *rx, u32 rx_max_len,
				unsigned long *ack_len)
{
	struct rpmi_reqfwd_complete_current_message_rsp *rsp = rx;
	unsigned long count = 0;
	s32 status;
	int ret;

	if (rx_max_len < sizeof(*rsp))
		return SBI_EINVAL;

	/*
	 * The response data is copied out of the shared memory before the
	 * response of this service is written back into it.
	 */
	ret = reqfwd_queue_complete(&rf->queue, tx, tx_len, &count);
	switch (ret) {
	case 0:
		status = RPMI_SUCCESS;
		break;
	case SBI_EBAD_RANGE:
		/* The response does not fit the buffer of the producer */
		status = RPMI_ERR_BAD_RANGE;
		break;
	default:
		status = RPMI_ERR_NO_DATA;
		break;
	}

	rsp->status = cpu_to_le32(status);
	rsp->num_messages = cpu_to_le32(count);
	*ack_len = sizeof(*rsp);

	return SBI_OK;
}

static int mpxy_reqfwd_send_message(struct sbi_mpxy_channel *channel,
				    u32 message_id, void *tx, u32 tx_len,
				    void *rx, u32 rx_max_len,
				    unsigned long *ack_len)
{
	struct mpxy_reqfwd *rf =
		container_of(channel, struct mpxy_reqfwd, channel);

	if (!rx || !ack_len)
		return SBI_EINVAL;

	switch (message_id) {
	case RPMI_REQFWD_SRV_ENABLE_NOTIFICATION:
		return mpxy_reqfwd_enable_notification(rf, tx, tx_len, rx,
						       rx_max_len, ack_len);
	case RPMI_REQFWD_SRV_RETRIEVE_CURRENT_MESSAGE:
		return mpxy_reqfwd_retrieve(rf, tx, tx_len, rx,
					    rx_max_len, ack_len);
	case RPMI_REQFWD_SRV_COMPLETE_CURRENT_MESSAGE:
		return mpxy_reqfwd_complete(rf, tx, tx_len, rx,
					    rx_max_len, ack_len);
	default:
		break;
	}

	return SBI_ENOTSUPP;
}

static int mpxy_reqfwd_init(const void *fdt, int nodeoff,
			    const struct fdt_match *match)
{
	struct mpxy_reqfwd *rf;
	const fdt32_t *val;
	u32 msg_max_len;
	int rc, len;

	val = fdt_getprop(fdt, nodeoff, "riscv,sbi-mpxy-channel-id", &len);
	if (!val || len < 4)
		return SBI_ENODEV;

	rf = sbi_zalloc(sizeof(*rf));
	if (!rf)
		return SBI_ENOMEM;

	rf->channel.channel_id = fdt32_to_cpu(*val);
	rf->channel.owner_domain = &root;

	msg_max_len = MPXY_REQFWD_MSG_MAX_LEN_DEFAULT;
	val = fdt_getprop(fdt, nodeoff, "riscv,sbi-mpxy-msg-max-len", &len);
	if (val && len >= 4)
		msg_max_len = fdt32_to_cpu(*val);
	if (msg_max_len < MPXY_REQFWD_MSG_MAX_LEN_MIN || (msg_max_len & 0x3)) {
		rc = SBI_EINVAL;
		goto fail_free;
	}

	rf->channel.read_attributes = mpxy_reqfwd_read_attributes;
	rf->channel.send_message_with_response = mpxy_reqfwd_send_message;

	rf->channel.attrs.msg_proto_id = SBI_MPXY_MSGPROTO_RPMI_ID;
	rf->channel.attrs.msg_proto_version = RPMI_VERSION(1, 0);
	rf->channel.attrs.msg_data_maxlen = msg_max_len;
	rf->channel.attrs.msg_send_timeout = MPXY_REQFWD_SEND_TIMEOUT_US;
	rf->channel.attrs.msg_completion_timeout =
		MPXY_REQFWD_COMPLETION_TIMEOUT_US;

	rf->msgprot_attrs.servicegrp_id = RPMI_SRVGRP_REQUEST_FORWARD;
	rf->msgprot_attrs.servicegrp_ver = MPXY_REQFWD_SRVGRP_VERSION;
	rf->msgprot_attrs.impl_id = MPXY_REQFWD_IMPL_ID;
	rf->msgprot_attrs.impl_ver = MPXY_REQFWD_IMPL_VERSION;

	/*
	 * A producer names the queue it forwards to by the ID of the
	 * channel the messages are retrieved from.
	 */
	rc = reqfwd_queue_register(&rf->queue, rf->channel.channel_id);
	if (rc)
		goto fail_free;

	rc = sbi_mpxy_register_channel(&rf->channel);
	if (rc)
		goto fail_unregister_queue;

	return SBI_OK;

fail_unregister_queue:
	reqfwd_queue_unregister(&rf->queue);
fail_free:
	sbi_free(rf);
	return rc;
}

static const struct fdt_match mpxy_reqfwd_match[] = {
	{ .compatible = "riscv,rpmi-mpxy-request-forward" },
	{},
};

const struct fdt_driver fdt_mpxy_rpmi_reqfwd = {
	.experimental = true,
	.match_table = mpxy_reqfwd_match,
	.init = mpxy_reqfwd_init,
};
