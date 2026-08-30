/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Marouene Boubakri
 *
 * Authors:
 *   Marouene Boubakri <marouene.boubakri@nxp.com>
 */

#ifndef __SBI_REQFWD_H__
#define __SBI_REQFWD_H__

#include <sbi/riscv_locks.h>
#include <sbi/sbi_list.h>
#include <sbi/sbi_types.h>

struct sbi_domain;

/**
 * Queues of messages forwarded from one domain to another.
 *
 * A producer forwards a message to the queue of a target domain and waits
 * for the target domain to complete it. The oldest message of a queue is
 * the current message and it is the only one which can be retrieved and
 * completed.
 *
 * The producer and the target domain run on different HARTs, so the
 * message and response buffers must be reachable from M-mode on any HART
 * of the system. In other words they must be in firmware memory, not in
 * the memory of either domain.
 */

/** State of a forwarded message */
enum sbi_reqfwd_state {
	/** Queued and not retrieved yet */
	SBI_REQFWD_STATE_QUEUED = 0,
	/** At least partly retrieved by the target domain */
	SBI_REQFWD_STATE_RETRIEVED,
	/** Completed by the target domain, the response is available */
	SBI_REQFWD_STATE_COMPLETED,
};

/** A message forwarded to a target domain */
struct sbi_reqfwd_message {
	/** Node in the message list of a queue */
	struct sbi_dlist node;
	/** State of this message */
	enum sbi_reqfwd_state state;
	/** Message bytes */
	const void *msg;
	/** Number of message bytes */
	unsigned long msg_len;
	/** Buffer receiving the response */
	void *rsp;
	/** Size of the response buffer */
	unsigned long rsp_max_len;
	/** Number of response bytes written by the target domain */
	unsigned long rsp_len;
};

/** Queue of messages forwarded to one target domain */
struct sbi_reqfwd_queue {
	/** Node in the list of registered queues */
	struct sbi_dlist node;
	/** Domain consuming the messages of this queue */
	const struct sbi_domain *target;
	/** Lock protecting the message list */
	spinlock_t lock;
	/** Forwarded messages, oldest first */
	struct sbi_dlist messages;
	/** Number of messages in the list */
	unsigned long count;
	/**
	 * Optional callback telling the target domain that a message
	 * arrived in an empty queue. It is called without the queue lock
	 * held and must be set before the queue is registered.
	 */
	void (*notify)(struct sbi_reqfwd_queue *queue);
};

/**
 * Register the forwarded message queue of a target domain
 *
 * The optional notify callback of the queue must already be set, this
 * function does not touch it.
 *
 * @param queue message queue to register
 * @param target domain consuming the messages of the queue
 *
 * @return 0 on success and negative error code on failure
 */
int sbi_reqfwd_register_queue(struct sbi_reqfwd_queue *queue,
			      const struct sbi_domain *target);

/**
 * Unregister a forwarded message queue
 *
 * The queue must be empty.
 *
 * @param queue message queue to unregister
 */
void sbi_reqfwd_unregister_queue(struct sbi_reqfwd_queue *queue);

/**
 * Find the forwarded message queue of a target domain
 *
 * @param target domain consuming the messages of the queue
 *
 * @return pointer to the queue or NULL if the domain has no queue
 */
struct sbi_reqfwd_queue *sbi_reqfwd_find_queue(const struct sbi_domain *target);

/**
 * Forward a message and wait for the target domain to complete it
 *
 * @param queue message queue of the target domain
 * @param fmsg caller provided state tracking the forwarded message
 * @param msg message bytes, must stay valid until this function returns
 * @param msg_len number of message bytes
 * @param rsp buffer receiving the response, may be NULL
 * @param rsp_max_len size of the response buffer
 * @param rsp_len place to store the number of response bytes, may be NULL
 * @param timeout_us how long to wait for completion in microseconds
 *
 * @return 0 on success, SBI_ETIMEDOUT if the target domain did not
 * complete the message in time, and a negative error code on failure
 */
int sbi_reqfwd_send(struct sbi_reqfwd_queue *queue,
		    struct sbi_reqfwd_message *fmsg,
		    const void *msg, unsigned long msg_len,
		    void *rsp, unsigned long rsp_max_len,
		    unsigned long *rsp_len, unsigned long timeout_us);

/**
 * Copy a part of the current forwarded message of a queue
 *
 * The current message is marked as retrieved even if only a part of it
 * was copied, because the target domain may not need the whole message.
 *
 * @param queue message queue of the target domain
 * @param offset index of the first byte to copy
 * @param buf buffer receiving the copied bytes
 * @param buf_len size of the buffer
 * @param returned place to store the number of bytes copied
 * @param remaining place to store the number of bytes left after this call
 *
 * @return 0 on success, SBI_ENOENT if the queue has no current message,
 * and a negative error code on failure
 */
int sbi_reqfwd_retrieve(struct sbi_reqfwd_queue *queue, unsigned long offset,
			void *buf, unsigned long buf_len,
			unsigned long *returned, unsigned long *remaining);

/**
 * Complete the current forwarded message of a queue
 *
 * @param queue message queue of the target domain
 * @param rsp response bytes
 * @param rsp_len number of response bytes
 * @param count place to store the number of messages left behind
 *
 * @return 0 on success, SBI_ENOENT if the queue has no current message or
 * the current message was not retrieved, and a negative error code on
 * failure
 */
int sbi_reqfwd_complete(struct sbi_reqfwd_queue *queue, const void *rsp,
			unsigned long rsp_len, unsigned long *count);

#endif /* __SBI_REQFWD_H__ */
