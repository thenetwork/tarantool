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
#include "request.h"
#include "engine.h"
#include "txn.h"
#include "tuple.h"
#include "index.h"
#include "space.h"
#include "schema.h"
#include "port.h"
#include "lua/call.h"
#include <errinj.h>
#include <fiber.h>
#include <scoped_guard.h>
#include <third_party/base64.h>
#include "authentication.h"
#include "user_def.h"
#include "iproto_constants.h"

enum dup_replace_mode
dup_replace_mode(uint32_t op)
{
	return op == IPROTO_INSERT ? DUP_INSERT : DUP_REPLACE_OR_INSERT;
}

static void
execute_replace(struct request *request, struct port *port)
{
	struct space *space = space_cache_find(request->space_id);
	struct txn *txn = txn_begin_stmt(request, space);

	access_check_space(space, PRIV_W);
	struct tuple *new_tuple = tuple_new(space->format, request->tuple,
					    request->tuple_end);
	TupleGuard guard(new_tuple);
	space_validate_tuple(space, new_tuple);
	enum dup_replace_mode mode = dup_replace_mode(request->type);

	txn_replace(txn, space, NULL, new_tuple, mode);
	txn_commit_stmt(txn, port);
}

static void
execute_update(struct request *request, struct port *port)
{
	struct space *space = space_cache_find(request->space_id);
	struct txn *txn = txn_begin_stmt(request, space);

	access_check_space(space, PRIV_W);
	Index *pk = index_find(space, 0);
	/* Try to find the tuple by primary key. */
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	primary_key_validate(pk->key_def, key, part_count);
	struct tuple *old_tuple = pk->findByKey(key, part_count);

	if (old_tuple == NULL) {
		txn_commit_stmt(txn, port);
		return;
	}

	/* Update the tuple. */
	struct tuple *new_tuple = tuple_update(space->format,
					       region_alloc_cb,
					       &fiber()->gc,
					       old_tuple, request->tuple,
					       request->tuple_end,
					       request->field_base);
	TupleGuard guard(new_tuple);
	space_validate_tuple(space, new_tuple);
	txn_replace(txn, space, old_tuple, new_tuple, DUP_INSERT);
	txn_commit_stmt(txn, port);
}

static void
execute_delete(struct request *request, struct port *port)
{
	struct space *space = space_cache_find(request->space_id);
	struct txn *txn = txn_begin_stmt(request, space);

	access_check_space(space, PRIV_W);

	/* Try to find tuple by primary key */
	Index *pk = index_find(space, 0);
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	primary_key_validate(pk->key_def, key, part_count);
	struct tuple *old_tuple = pk->findByKey(key, part_count);

	if (old_tuple != NULL)
		txn_replace(txn, space, old_tuple, NULL, DUP_REPLACE_OR_INSERT);
	txn_commit_stmt(txn, port);
}


static void
execute_select(struct request *request, struct port *port)
{
	struct space *space = space_cache_find(request->space_id);
	access_check_space(space, PRIV_R);
	Index *index = index_find(space, request->index_id);

	ERROR_INJECT_EXCEPTION(ERRINJ_TESTING);

	uint32_t found = 0;
	uint32_t offset = request->offset;
	uint32_t limit = request->limit;
	if (request->iterator >= iterator_type_MAX)
		tnt_raise(IllegalParams, "Invalid iterator type");
	enum iterator_type type = (enum iterator_type) request->iterator;

	const char *key = request->key;
	uint32_t part_count = key ? mp_decode_array(&key) : 0;

	struct iterator *it = index->position();
	key_validate(index->key_def, type, key, part_count);
	index->initIterator(it, type, key, part_count);
	auto iterator_guard =
		make_scoped_guard([=] { iterator_close(it); });

	struct tuple *tuple;
	while ((tuple = it->next(it)) != NULL) {
		if (offset > 0) {
			offset--;
			continue;
		}
		if (limit == found++)
			break;
		port_add_tuple(port, tuple);
	}
	if (! in_txn()) {
		 /* no txn is created, so simply collect garbage here */
		fiber_gc();
	}
}

void
execute_auth(struct request *request, struct port * /* port */)
{
	const char *user = request->key;
	uint32_t len = mp_decode_strl(&user);
	authenticate(user, len, request->tuple, request->tuple_end);
}

/** }}} */

void
request_create(struct request *request, uint32_t type)
{
	if (!iproto_type_is_dml(type))
		tnt_raise(LoggedError, ER_UNKNOWN_REQUEST_TYPE, type);
	static const request_execute_f execute_map[] = {
		NULL, execute_select, execute_replace, execute_replace,
		execute_update, execute_delete, box_lua_call,
		execute_auth,
	};
	memset(request, 0, sizeof(*request));
	request->type = type;
	request->execute = execute_map[type];
}

static void
request_set_uint(void *data, uint8_t key, uint32_t v)
{
	switch (key) {
	case IPROTO_SPACE_ID:
		((struct request *)data)->space_id = v;
		break;
	case IPROTO_INDEX_ID:
		((struct request *)data)->index_id = v;
		break;
	case IPROTO_OFFSET:
		((struct request *)data)->offset = v;
		break;
	case IPROTO_LIMIT:
		((struct request *)data)->limit = v;
		break;
	case IPROTO_ITERATOR:
		((struct request *)data)->iterator = v;
	default:
		break;
	}
}

static void
request_set_char(void *data, uint8_t key, const char *v, const char *v_end)
{
	switch (key) {
	case IPROTO_TUPLE:
		((struct request *)data)->tuple = v;
		((struct request *)data)->tuple_end = v_end;
		break;
	case IPROTO_KEY:
	case IPROTO_FUNCTION_NAME:
	case IPROTO_USER_NAME:
		((struct request *)data)->key = v;
		((struct request *)data)->key_end = v_end;
	default:
		break;
	}
}

static void
request_decode_cb(const char *data, uint32_t len, uint32_t type,
		  request_uint_f uint_f, request_char_f char_f, void *data_f)
{
	const char *end = data + len;
	uint64_t key_map = iproto_body_key_map[type];
	if (mp_typeof(*data) != MP_MAP || mp_check_map(data, end) > 0) {
error:
		tnt_raise(ClientError, ER_INVALID_MSGPACK, "packet body");
	}
	uint32_t size = mp_decode_map(&data);
	for (uint32_t i = 0; i < size; i++) {
		if (! iproto_body_has_key(data, end)) {
			mp_check(&data, end);
			mp_check(&data, end);
			continue;
		}
		unsigned char key = mp_decode_uint(&data);
		key_map &= ~iproto_key_bit(key);
		const char *value = data;
		if (mp_check(&data, end))
			goto error;
		if (iproto_key_type[key] != mp_typeof(*value))
			goto error;
		switch (key) {
		case IPROTO_SPACE_ID:
		case IPROTO_INDEX_ID:
		case IPROTO_OFFSET:
		case IPROTO_LIMIT:
		case IPROTO_ITERATOR:
			uint_f(data_f, key, mp_decode_uint(&value));
			break;
		case IPROTO_TUPLE:
		case IPROTO_KEY:
		case IPROTO_FUNCTION_NAME:
		case IPROTO_USER_NAME:
			char_f(data_f, key, value, data);
		default:
			break;
		}
	}
#ifndef NDEBUG
	if (data != end)
		tnt_raise(ClientError, ER_INVALID_MSGPACK, "packet end");
#endif
	if (key_map) {
		tnt_raise(ClientError, ER_MISSING_REQUEST_FIELD,
			  iproto_key_strs[__builtin_ffsll((long long) key_map) - 1]);
	}
}

void
request_decode(struct request *request, const char *data, uint32_t len)
{
	assert(request->execute != NULL);
	request_decode_cb(data, len, request->type, request_set_uint,
		request_set_char, request);
}

void
request_header_decode(struct xrow_header* xrow, request_uint_f uint_f,
		      request_char_f char_f, void *data)
{
	try {
		request_decode_cb(
			(const char *)xrow->body[0].iov_base, xrow->body[0].iov_len,
			xrow->type, uint_f, char_f, data);
	} catch (...) {

	}
}

int
request_encode(struct request *request, struct iovec *iov)
{
	int iovcnt = 1;
	const int HEADER_LEN_MAX = 32;
	uint32_t key_len = request->key_end - request->key;
	uint32_t len = HEADER_LEN_MAX + key_len;
	char *begin = (char *) region_alloc(&fiber()->gc, len);
	char *pos = begin + 1;     /* skip 1 byte for MP_MAP */
	int map_size = 0;
	if (true) {
		pos = mp_encode_uint(pos, IPROTO_SPACE_ID);
		pos = mp_encode_uint(pos, request->space_id);
		map_size++;
	}
	if (request->index_id) {
		pos = mp_encode_uint(pos, IPROTO_INDEX_ID);
		pos = mp_encode_uint(pos, request->index_id);
		map_size++;
	}
	if (request->key) {
		pos = mp_encode_uint(pos, IPROTO_KEY);
		memcpy(pos, request->key, key_len);
		pos += key_len;
		map_size++;
	}
	if (request->tuple) {
		pos = mp_encode_uint(pos, IPROTO_TUPLE);
		iov[1].iov_base = (void *) request->tuple;
		iov[1].iov_len = (request->tuple_end - request->tuple);
		iovcnt++;
		map_size++;
	}

	assert(pos <= begin + len);
	mp_encode_map(begin, map_size);
	iov[0].iov_base = begin;
	iov[0].iov_len = pos - begin;

	return iovcnt;
}
