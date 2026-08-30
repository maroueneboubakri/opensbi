/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Marouene Boubakri
 *
 * Authors:
 *   Marouene Boubakri <marouene.boubakri@nxp.com>
 */

#ifndef __REQFWD_QUEUE_H__
#define __REQFWD_QUEUE_H__

#include <sbi/riscv_locks.h>
#include <sbi/sbi_list.h>
#include <sbi/sbi_types.h>

/**
 * Queue of RPMI messages forwarded to a software agent which serves the
 * RPMI REQUEST_FORWARD service group.
 *
 * A producer forwards a message to a queue and waits for the agent to
 * complete it. The oldest message of a queue is its current message and
 * it is the only one which can be retrieved and completed.
 *
 * The producer and the agent run on different HARTs, so the message and
 * response buffers must be reachable from M-mode on any HART of the
 * system. In other words they must be in firmware memory.
 */

/** State of a forwarded message */
enum reqfwd_state {
	/** Queued and not retrieved yet */
	REQFWD_STATE_QUEUED = 0,
	/** At least partly retrieved by the agent */
	REQFWD_STATE_RETRIEVED,
	/** Completed by the agent, the response is available */
	REQFWD_STATE_COMPLETED,
};

/** A forwarded message */
struct reqfwd_message {
	/** Node in the message list of a queue */
	struct sbi_dlist node;
	/** State of this message */
	enum reqfwd_state state;
	/** Message bytes */
	const void *msg;
	/** Number of message bytes */
	unsigned long msg_len;
	/** Buffer receiving the response */
	void *rsp;
	/** Size of the response buffer */
	unsigned long rsp_max_len;
	/** Number of response bytes written by the agent */
	unsigned long rsp_len;
};

/** Queue of messages forwarded to one agent */
struct reqfwd_queue {
	/** Node in the list of registered queues */
	struct sbi_dlist node;
	/** Identifier of this queue, unique across registered queues */
	u32 id;
	/** Lock protecting the message list */
	spinlock_t lock;
	/** Forwarded messages, oldest first */
	struct sbi_dlist messages;
	/** Number of messages in the list */
	unsigned long count;
};

/**
 * Register a forwarded message queue
 *
 * @param queue message queue to register
 * @param id identifier producers use to find the queue, in practice the
 * MPXY channel ID of the REQUEST_FORWARD channel serving it
 *
 * @return 0 on success and negative error code on failure
 */
int reqfwd_queue_register(struct reqfwd_queue *queue, u32 id);

/**
 * Unregister a forwarded message queue
 *
 * The queue must be empty.
 *
 * @param queue message queue to unregister
 */
void reqfwd_queue_unregister(struct reqfwd_queue *queue);

/**
 * Find a registered forwarded message queue
 *
 * @param id identifier the queue was registered with
 *
 * @return pointer to the queue or NULL if there is no such queue
 */
struct reqfwd_queue *reqfwd_queue_find(u32 id);

/**
 * Forward a message and wait for the agent to complete it
 *
 * @param queue message queue to forward to
 * @param fmsg caller provided state tracking the forwarded message
 * @param msg message bytes, must stay valid until this function returns
 * @param msg_len number of message bytes
 * @param rsp buffer receiving the response, may be NULL
 * @param rsp_max_len size of the response buffer
 * @param rsp_len place to store the number of response bytes, may be NULL
 * @param timeout_us how long to wait for completion in microseconds
 *
 * @return 0 on success, SBI_ETIMEDOUT if the agent did not complete the
 * message in time, and a negative error code on failure
 */
int reqfwd_queue_send(struct reqfwd_queue *queue,
		      struct reqfwd_message *fmsg,
		      const void *msg, unsigned long msg_len,
		      void *rsp, unsigned long rsp_max_len,
		      unsigned long *rsp_len, unsigned long timeout_us);

/**
 * Copy a part of the current forwarded message of a queue
 *
 * The current message is marked as retrieved even if only a part of it
 * was copied, because the agent may not need the whole message.
 *
 * @param queue message queue to retrieve from
 * @param offset index of the first byte to copy
 * @param buf buffer receiving the copied bytes
 * @param buf_len size of the buffer
 * @param returned place to store the number of bytes copied
 * @param remaining place to store the number of bytes left after this call
 *
 * @return 0 on success, SBI_ENOENT if the queue has no current message,
 * and a negative error code on failure
 */
int reqfwd_queue_retrieve(struct reqfwd_queue *queue, unsigned long offset,
			  void *buf, unsigned long buf_len,
			  unsigned long *returned, unsigned long *remaining);

/**
 * Complete the current forwarded message of a queue
 *
 * @param queue message queue to complete in
 * @param rsp response bytes
 * @param rsp_len number of response bytes
 * @param count place to store the number of messages left behind
 *
 * @return 0 on success, SBI_ENOENT if the queue has no current message or
 * the current message was not retrieved, and a negative error code on
 * failure
 */
int reqfwd_queue_complete(struct reqfwd_queue *queue, const void *rsp,
			  unsigned long rsp_len, unsigned long *count);

#endif /* __REQFWD_QUEUE_H__ */
