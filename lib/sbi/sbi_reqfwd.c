/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Marouene Boubakri
 *
 * Authors:
 *   Marouene Boubakri <marouene.boubakri@nxp.com>
 */

#include <sbi/riscv_barrier.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_reqfwd.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_timer.h>

/** List of registered forwarded message queues */
static SBI_LIST_HEAD(reqfwd_queue_list);

/** Current message of a queue, called with the queue lock held */
static struct sbi_reqfwd_message *reqfwd_current(struct sbi_reqfwd_queue *queue)
{
	if (sbi_list_empty(&queue->messages))
		return NULL;

	return sbi_list_first_entry(&queue->messages,
				    struct sbi_reqfwd_message, node);
}

int sbi_reqfwd_register_queue(struct sbi_reqfwd_queue *queue,
			      const struct sbi_domain *target)
{
	if (!queue || !target)
		return SBI_EINVAL;

	if (sbi_reqfwd_find_queue(target))
		return SBI_EALREADY;

	queue->target = target;
	SPIN_LOCK_INIT(queue->lock);
	SBI_INIT_LIST_HEAD(&queue->messages);
	queue->count = 0;

	sbi_list_add_tail(&queue->node, &reqfwd_queue_list);

	return 0;
}

void sbi_reqfwd_unregister_queue(struct sbi_reqfwd_queue *queue)
{
	if (!queue || !queue->target)
		return;

	sbi_list_del(&queue->node);
	queue->target = NULL;
}

struct sbi_reqfwd_queue *sbi_reqfwd_find_queue(const struct sbi_domain *target)
{
	struct sbi_reqfwd_queue *queue;

	sbi_list_for_each_entry(queue, &reqfwd_queue_list, node) {
		if (queue->target == target)
			return queue;
	}

	return NULL;
}

int sbi_reqfwd_send(struct sbi_reqfwd_queue *queue,
		    struct sbi_reqfwd_message *fmsg,
		    const void *msg, unsigned long msg_len,
		    void *rsp, unsigned long rsp_max_len,
		    unsigned long *rsp_len, unsigned long timeout_us)
{
	u64 start, ticks;

	if (!queue || !fmsg || !msg || !msg_len || !timeout_us)
		return SBI_EINVAL;

	fmsg->state = SBI_REQFWD_STATE_QUEUED;
	fmsg->msg = msg;
	fmsg->msg_len = msg_len;
	fmsg->rsp = rsp;
	fmsg->rsp_max_len = rsp ? rsp_max_len : 0;
	fmsg->rsp_len = 0;

	spin_lock(&queue->lock);
	sbi_list_add_tail(&fmsg->node, &queue->messages);
	queue->count++;
	spin_unlock(&queue->lock);

	start = sbi_timer_value();
	ticks = sbi_timer_compute_udelta(timeout_us);

	while (1) {
		spin_lock(&queue->lock);

		/*
		 * sbi_reqfwd_complete() removes the message from the list
		 * before releasing the lock, so once it is completed the
		 * target domain no longer refers to it.
		 */
		if (fmsg->state == SBI_REQFWD_STATE_COMPLETED) {
			spin_unlock(&queue->lock);
			if (rsp_len)
				*rsp_len = fmsg->rsp_len;
			return 0;
		}

		if ((sbi_timer_value() - start) >= ticks) {
			sbi_list_del(&fmsg->node);
			queue->count--;
			spin_unlock(&queue->lock);
			return SBI_ETIMEDOUT;
		}

		spin_unlock(&queue->lock);
		cpu_relax();
	}
}

int sbi_reqfwd_retrieve(struct sbi_reqfwd_queue *queue, unsigned long offset,
			void *buf, unsigned long buf_len,
			unsigned long *returned, unsigned long *remaining)
{
	struct sbi_reqfwd_message *fmsg;
	unsigned long len;
	int ret = 0;

	if (!queue || (buf_len && !buf))
		return SBI_EINVAL;

	spin_lock(&queue->lock);

	fmsg = reqfwd_current(queue);
	if (!fmsg) {
		ret = SBI_ENOENT;
		goto out;
	}

	if (offset > fmsg->msg_len) {
		ret = SBI_EINVAL;
		goto out;
	}

	len = fmsg->msg_len - offset;
	if (len > buf_len)
		len = buf_len;

	if (len)
		sbi_memcpy(buf, (const char *)fmsg->msg + offset, len);
	fmsg->state = SBI_REQFWD_STATE_RETRIEVED;

	if (returned)
		*returned = len;
	if (remaining)
		*remaining = fmsg->msg_len - offset - len;

out:
	spin_unlock(&queue->lock);
	return ret;
}

int sbi_reqfwd_complete(struct sbi_reqfwd_queue *queue, const void *rsp,
			unsigned long rsp_len, unsigned long *count)
{
	struct sbi_reqfwd_message *fmsg;
	int ret = 0;

	if (!queue || (rsp_len && !rsp))
		return SBI_EINVAL;

	spin_lock(&queue->lock);

	fmsg = reqfwd_current(queue);
	if (!fmsg || fmsg->state != SBI_REQFWD_STATE_RETRIEVED) {
		ret = SBI_ENOENT;
		goto out;
	}

	if (rsp_len > fmsg->rsp_max_len) {
		ret = SBI_EBAD_RANGE;
		goto out;
	}

	if (rsp_len)
		sbi_memcpy(fmsg->rsp, rsp, rsp_len);
	fmsg->rsp_len = rsp_len;
	fmsg->state = SBI_REQFWD_STATE_COMPLETED;

	sbi_list_del(&fmsg->node);
	queue->count--;

out:
	if (count)
		*count = queue->count;
	spin_unlock(&queue->lock);

	return ret;
}
