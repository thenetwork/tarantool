/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "ipc.h"
#include "fiber.h"
#include <stdlib.h>

static void
ipc_channel_create(struct ipc_channel *ch);

static void
ipc_channel_destroy(struct ipc_channel *ch);

struct ipc_channel *
ipc_channel_new(unsigned size)
{
	if (!size)
		size = 1;
	struct ipc_channel *res = (struct ipc_channel *)
		malloc(sizeof(struct ipc_channel) + sizeof(void *) * size);
	if (res == NULL)
		return NULL;
	res->size = size;
	ipc_channel_create(res);
	return res;
}

void
ipc_channel_delete(struct ipc_channel *ch)
{
	ipc_channel_destroy(ch);
	free(ch);
}

static void
ipc_channel_create(struct ipc_channel *ch)
{
	ch->beg = ch->count = 0;
	ch->closed = false;
	ch->close = NULL;
	ch->bcast = NULL;
	rlist_create(&ch->readers);
	rlist_create(&ch->writers);
}

static void
ipc_channel_destroy(struct ipc_channel *ch)
{
	while (!rlist_empty(&ch->writers)) {
		struct fiber *f =
			rlist_first_entry(&ch->writers, struct fiber, state);
		rlist_del_entry(f, state);
	}
	while (!rlist_empty(&ch->readers)) {
		struct fiber *f =
			rlist_first_entry(&ch->readers, struct fiber, state);
		rlist_del_entry(f, state);
	}
}

void *
ipc_channel_get_timeout(struct ipc_channel *ch, ev_tstamp timeout)
{
	if (ch->closed)
		return NULL;

	struct fiber *f;
	bool first_try = true;
	ev_tstamp started = ev_now(loop());
	void *res;
	/* channel is empty */
	while (ch->count == 0) {
		/* try to be in FIFO order */
		if (first_try) {
			rlist_add_tail_entry(&ch->readers, fiber(), state);
			first_try = false;
		} else {
			rlist_add_entry(&ch->readers, fiber(), state);
		}
		fiber_yield_timeout(timeout);
		rlist_del_entry(fiber(), state);

		/* broadcast message wakes us up */
		if (ch->bcast) {
			fiber_wakeup(ch->bcast);
			fiber_testcancel();
			res = ch->bcast_msg;
			goto exit;
		}

		fiber_testcancel();

		timeout -= ev_now(loop()) - started;
		if (timeout <= 0) {
			res = NULL;
			goto exit;
		}

		if (ch->closed) {
			res = NULL;
			goto exit;
		}
	}

	res = ch->item[ch->beg];
	if (++ch->beg >= ch->size)
		ch->beg -= ch->size;
	ch->count--;

	if (!rlist_empty(&ch->writers)) {
		f = rlist_first_entry(&ch->writers, struct fiber, state);
		rlist_del_entry(f, state);
		fiber_wakeup(f);
	}

exit:
	if (ch->closed && ch->close) {
		fiber_wakeup(ch->close);
		ch->close = NULL;
	}

	return res;
}

void *
ipc_channel_get(struct ipc_channel *ch)
{
	return ipc_channel_get_timeout(ch, TIMEOUT_INFINITY);
}

static void
ipc_channel_close_waiter(struct ipc_channel *ch, struct fiber *f)
{
	ch->close = fiber();

	while (ch->close) {
		fiber_wakeup(f);
		fiber_yield();
		ch->close = NULL;
		rlist_del_entry(fiber(), state);
		fiber_testcancel();
	}
}

void
ipc_channel_close(struct ipc_channel *ch)
{
	if (ch->closed)
		return;
	ch->closed = true;

	struct fiber *f;
	while(!rlist_empty(&ch->readers)) {
		f = rlist_first_entry(&ch->readers, struct fiber, state);
		ipc_channel_close_waiter(ch, f);
	}
	while(!rlist_empty(&ch->writers)) {
		f = rlist_first_entry(&ch->writers, struct fiber, state);
		ipc_channel_close_waiter(ch, f);
	}
	if (ch->bcast)
		fiber_wakeup(ch->bcast);
}

int
ipc_channel_put_timeout(struct ipc_channel *ch, void *data,
			ev_tstamp timeout)
{
	if (ch->closed) {
		errno = EBADF;
		return -1;
	}

	bool first_try = true;
	int res;
	unsigned i;
	ev_tstamp started = ev_now(loop());
	/* channel is full */
	while (ch->count >= ch->size) {

		/* try to be in FIFO order */
		if (first_try) {
			rlist_add_tail_entry(&ch->writers, fiber(), state);
			first_try = false;
		} else {
			rlist_add_entry(&ch->writers, fiber(), state);
		}

		fiber_yield_timeout(timeout);
		rlist_del_entry(fiber(), state);

		fiber_testcancel();

		timeout -= ev_now(loop()) - started;
		if (timeout <= 0) {
			errno = ETIMEDOUT;
			res = -1;
			goto exit;
		}

		if (ch->closed) {
			errno = EBADF;
			res = -1;
			goto exit;
		}
	}

	i = ch->beg;
	i += ch->count;
	ch->count++;
	if (i >= ch->size)
		i -= ch->size;

	ch->item[i] = data;
	if (!rlist_empty(&ch->readers)) {
		struct fiber *f;
		f = rlist_first_entry(&ch->readers, struct fiber, state);
		rlist_del_entry(f, state);
		fiber_wakeup(f);
	}
	res = 0;
exit:
	if (ch->closed && ch->close) {
		int save_errno = errno;
		fiber_wakeup(ch->close);
		ch->close = NULL;
		errno = save_errno;
	}
	return res;
}

void
ipc_channel_put(struct ipc_channel *ch, void *data)
{
	ipc_channel_put_timeout(ch, data, TIMEOUT_INFINITY);
}

int
ipc_channel_broadcast(struct ipc_channel *ch, void *data)
{
	/* do nothing at closed channel */
	if (ch->closed)
		return 0;

	/* broadcast in broadcast: marasmus */
	if (ch->bcast)
		return 0;

	/* there is no reader on channel */
	if (rlist_empty(&ch->readers)) {
		ipc_channel_put(ch, data);
		return 1;
	}

	unsigned readers = 0;
	struct fiber *f;
	rlist_foreach_entry(f, &ch->readers, state) {
		readers++;
	}

	unsigned cnt = 0;
	while (!rlist_empty(&ch->readers)) {
		if (ch->closed)
			break;
		f = rlist_first_entry(&ch->readers, struct fiber, state);

		ch->bcast_msg = data;
		ch->bcast = fiber();
		fiber_wakeup(f);
		fiber_yield();
		ch->bcast = NULL;
		rlist_del_entry(fiber(), state);
		fiber_testcancel();
		/* if any other reader was added don't wake it up */
		if (++cnt >= readers)
			break;
	}

	if (ch->closed && ch->close) {
		fiber_wakeup(ch->close);
		ch->close = NULL;
	}

	return cnt;
}
