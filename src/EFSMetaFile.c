#include <yajl/yajl_parse.h>
#include "strndup.h"
#include "async/async.h"
#include "fts.h"
#include "EarthFS.h"

#define META_MAX (1024 * 100)

struct EFSMetaFile {
	byte_t *buf;
	size_t len;

	uint64_t metaFileID;
};

static yajl_callbacks const callbacks;

static uint64_t add_metafile(DB_txn *const txn, EFSConnection const *const conn, uint64_t const fileID, strarg_t const targetURI);
static void add_metadata(DB_txn *const txn, EFSConnection const *const conn, uint64_t const metaFileID, strarg_t const field, strarg_t const value, size_t const vlen);
static void add_fulltext(DB_txn *const txn, EFSConnection const *const conn, uint64_t const metaFileID, strarg_t const str, size_t const len);


EFSMetaFileRef EFSMetaFileCreate(strarg_t const type) {
	if(!type) return NULL;
	if(0 != strcasecmp("text/efs-meta+json; charset=utf-8", type)) return NULL;

	EFSMetaFileRef meta = calloc(1, sizeof(struct EFSMetaFile));
	if(!meta) return NULL;
	meta->buf = malloc(META_MAX);
	meta->len = 0;
	return meta;
}
void EFSMetaFileFree(EFSMetaFileRef *const metaptr) {
	EFSMetaFileRef meta = *metaptr;
	if(!meta) return;
	FREE(&meta->buf);
	meta->len = 0;
	meta->metaFileID = 0;
	assert_zeroed(meta, 1);
	FREE(metaptr); meta = NULL;
}

err_t EFSMetaFileWrite(EFSMetaFileRef const meta, byte_t const *const buf, size_t const len) {
	if(!meta) return 0;
	if(meta->len >= META_MAX) return 0;
	size_t const use = MIN(META_MAX-meta->len, len);
	memcpy(meta->buf+meta->len, buf, use);
	meta->len += use;
	return 0;
}
err_t EFSMetaFileEnd(EFSMetaFileRef const meta) {
	if(!meta) return 0;
	return 0;
}

typedef enum {
	s_start = 0,
	s_top,
		s_field_value,
		s_field_array,
	s_end,
} parser_state;

typedef struct {
	EFSConnection const *conn;
	DB_txn *txn;
	int64_t metaFileID;
	strarg_t targetURI;
	parser_state state;
	str_t *field;
} parser_context;

err_t EFSMetaFileStore(EFSMetaFileRef const meta, uint64_t const fileID, strarg_t const fileURI, EFSConnection const *const conn, DB_txn *const txn) {
	if(!meta) return 0;
	if(meta->len < 3) return 0;

	byte_t *buf = NULL;
	size_t len = 0;
	strarg_t targetURI = NULL;
	for(index_t i = 0; i < MIN(URI_MAX+1, meta->len-3); ++i) {
		bool_t const crlfcrlf =
			'\r' == meta->buf[i+0] &&
			'\n' == meta->buf[i+1] &&
			'\r' == meta->buf[i+2] &&
			'\n' == meta->buf[i+3];
		bool_t const crcr =
			'\r' == meta->buf[i+0] &&
			'\r' == meta->buf[i+1];
		bool_t const lflf =
			'\n' == meta->buf[i+0] &&
			'\n' == meta->buf[i+1];
		if(!crlfcrlf && !crcr && !lflf) continue;
		if(i < 8) break; // Too short to be a valid URI.
		meta->buf[i] = '\0';
		targetURI = (strarg_t)meta->buf;
		buf = meta->buf + (i + 1);
		len = meta->len - (i + 1);
		break;
	}
	if(!buf || !len || !targetURI) {
		fprintf(stderr, "Invalid meta-file (missing target URI)\n");
		return 0; // TODO: Should this warrant an actual error? Or should we ignore it since we can still store the data?
	}

	meta->metaFileID = add_metafile(txn, conn, fileID, targetURI);

	parser_context context = {
		.conn = conn,
		.txn = txn,
		.metaFileID = meta->metaFileID,
		.targetURI = targetURI,
		.state = s_start,
		.field = NULL,
	};

	yajl_handle parser = yajl_alloc(&callbacks, NULL, &context);
	if(!parser) return -1;
	yajl_config(parser, yajl_allow_partial_values, (int)true);
	yajl_parse(parser, buf, len);
	yajl_status const status = yajl_complete_parse(parser);
	if(yajl_status_ok != status) {
		unsigned char *msg = yajl_get_error(parser, true, buf, len);
		fprintf(stderr, "%s", msg);
		yajl_free_error(parser, msg); msg = NULL;
		FREE(&context.field);
		yajl_free(parser); parser = NULL;
		return -1;
	}
	FREE(&context.field);
	yajl_free(parser); parser = NULL;
	return 0;
}
uint64_t EFSMetaFileGetID(EFSMetaFileRef const meta) {
	if(!meta) return 0;
	return meta->metaFileID;
}


static int yajl_null(parser_context *const context) {
	return false;
}
static int yajl_boolean(parser_context *const context, int const flag) {
	return false;
}
static int yajl_number(parser_context *const context, strarg_t const str, size_t const len) {
	return false;
}
static int yajl_string(parser_context *const context, strarg_t const str, size_t const len) {
	switch(context->state) {
		case s_field_value:
		case s_field_array: {
			if(0 == strcmp("fulltext", context->field)) {
				add_fulltext(
					context->txn,
					context->conn,
					context->metaFileID,
					str, len);
			} else {
				add_metadata(
					context->txn,
					context->conn,
					context->metaFileID,
					context->field,
					str, len);
			}
			if(s_field_value == context->state) {
				FREE(&context->field);
				context->state = s_top;
			}
			return true;
		}
		default:
			return false;
	}
}
static int yajl_start_map(parser_context *const context) {
	switch(context->state) {
		case s_start:
			context->state = s_top;
			return true;
		default:
			return false;
	}
}
static int yajl_map_key(parser_context *const context, strarg_t const key, size_t const len) {
	switch(context->state) {
		case s_top:
			assertf(!context->field, "Already parsing field");
			context->field = strndup(key, len);
			context->state = s_field_value;
			return true;
		default:
			assertf(0, "Unexpected map key in state %d", context->state);
			return false;
	}
}
static int yajl_end_map(parser_context *const context) {
	switch(context->state) {
		case s_top:
			context->state = s_end;
			return true;
		default:
			assertf(0, "Unexpected map end in state %d", context->state);
			return false;
	}
}
static int yajl_start_array(parser_context *const context) {
	switch(context->state) {
		case s_field_value:
			context->state = s_field_array;
			return true;
		default:
			return false;
	}
}
static int yajl_end_array(parser_context *const context) {
	switch(context->state) {
		case s_field_array:
			FREE(&context->field);
			context->state = s_top;
			return true;
		default:
			assertf(0, "Unexpected array end in state %d", context->state);
			return false;
	}
}
static yajl_callbacks const callbacks = {
	.yajl_null = (int (*)())yajl_null,
	.yajl_boolean = (int (*)())yajl_boolean,
	.yajl_number = (int (*)())yajl_number,
	.yajl_string = (int (*)())yajl_string,
	.yajl_start_map = (int (*)())yajl_start_map,
	.yajl_map_key = (int (*)())yajl_map_key,
	.yajl_end_map = (int (*)())yajl_end_map,
	.yajl_start_array = (int (*)())yajl_start_array,
	.yajl_end_array = (int (*)())yajl_end_array,
};

static uint64_t add_metafile(DB_txn *const txn, EFSConnection const *const conn, uint64_t const fileID, strarg_t const targetURI) {
	uint64_t const metaFileID = db_next_id(txn, EFSMetaFileByID);
	assert(metaFileID);
	int rc;
	DB_val null = { 0, NULL };

	DB_VAL(metaFileID_key, DB_VARINT_MAX + DB_VARINT_MAX);
	db_bind_uint64(metaFileID_key, EFSMetaFileByID);
	db_bind_uint64(metaFileID_key, metaFileID);
	DB_VAL(metaFile_val, DB_VARINT_MAX + DB_INLINE_MAX);
	db_bind_uint64(metaFile_val, fileID);
	db_bind_string(txn, metaFile_val, targetURI);
	rc = db_put(txn, metaFileID_key, metaFile_val, DB_NOOVERWRITE_FAST);
	assert(!rc);

	DB_VAL(fileID_key, DB_VARINT_MAX * 3);
	db_bind_uint64(fileID_key, EFSFileIDAndMetaFileID);
	db_bind_uint64(fileID_key, fileID);
	db_bind_uint64(fileID_key, metaFileID);
	rc = db_put(txn, fileID_key, &null, DB_NOOVERWRITE_FAST);
	assert(!rc);

	DB_VAL(targetURI_key, DB_VARINT_MAX * 2 + DB_INLINE_MAX * 1);
	db_bind_uint64(targetURI_key, EFSTargetURIAndMetaFileID);
	db_bind_string(txn, targetURI_key, targetURI);
	db_bind_uint64(targetURI_key, metaFileID);
	rc = db_put(txn, targetURI_key, &null, DB_NOOVERWRITE_FAST);
	assert(!rc);

	return metaFileID;
}
static void add_metadata(DB_txn *const txn, EFSConnection const *const conn, uint64_t const metaFileID, strarg_t const field, strarg_t const value, size_t const vlen) {
	if(!vlen) return;
	DB_val null = { 0, NULL };
	int rc;

	DB_VAL(fwd, DB_VARINT_MAX * 2 + DB_INLINE_MAX * 2);
	db_bind_uint64(fwd, EFSMetaFileIDFieldAndValue);
	db_bind_uint64(fwd, metaFileID);
	db_bind_string(txn, fwd, field);
	db_bind_string_len(txn, fwd, value, vlen, false);
	rc = db_put(txn, fwd, &null, DB_NOOVERWRITE_FAST);
	assertf(DB_SUCCESS == rc || DB_KEYEXIST == rc, "Database error %s", db_strerror(rc));

	DB_VAL(rev, DB_VARINT_MAX * 2 + DB_INLINE_MAX * 2);
	db_bind_uint64(rev, EFSFieldValueAndMetaFileID);
	db_bind_string(txn, rev, field);
	db_bind_string_len(NULL, rev, value, vlen, false); // TODO: HACK
	db_bind_uint64(rev, metaFileID);
	rc = db_put(txn, rev, &null, DB_NOOVERWRITE_FAST);
	assertf(DB_SUCCESS == rc || DB_KEYEXIST == rc, "Database error %s", db_strerror(rc));
}
static void add_fulltext(DB_txn *const txn, EFSConnection const *const conn, uint64_t const metaFileID, strarg_t const str, size_t const len) {
	int rc;

	sqlite3_tokenizer_module const *fts = NULL;
	sqlite3_tokenizer *tokenizer = NULL;
	fts_get(&fts, &tokenizer);

	sqlite3_tokenizer_cursor *tcur = NULL;
	rc = fts->xOpen(tokenizer, str, len, &tcur);
	assert(SQLITE_OK == rc);

	DB_cursor *cursor = NULL;
	rc = db_cursor_open(txn, &cursor);
	assert(DB_SUCCESS == rc);

	for(;;) {
		strarg_t token;
		int tlen;
		int tpos; // TODO
		int ignored1, ignored2;
		rc = fts->xNext(tcur, &token, &tlen, &ignored1, &ignored2, &tpos);
		if(SQLITE_OK != rc) break;

		DB_VAL(token_val, DB_VARINT_MAX * 3 + DB_INLINE_MAX * 1);
		db_bind_uint64(token_val, EFSTermMetaFileIDAndPosition);
		db_bind_string_len(txn, token_val, token, tlen, false);
		db_bind_uint64(token_val, metaFileID);
		db_bind_uint64(token_val, 0); // TODO: Record tpos. Requires changes to EFSFulltextFilter so that each document only gets returned once, no matter how many times the token appears within it.
		DB_val null = { 0, NULL };
		rc = db_cursor_put(cursor, token_val, &null, DB_NOOVERWRITE_FAST);
		assert(DB_SUCCESS == rc || DB_KEYEXIST == rc);
	}

	db_cursor_close(cursor); cursor = NULL;

	fts->xClose(tcur); tcur = NULL;
}

