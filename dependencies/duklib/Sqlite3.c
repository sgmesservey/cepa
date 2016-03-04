#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include <duktape.h>

static int sqliteClose(duk_context *duk) {
	int fin = duk_get_top(duk),rc = -1;
	sqlite3 *conn;

	if (!fin) duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if (!duk_is_pointer(duk,-1)) return DUK_RET_INTERNAL_ERROR;
	conn = duk_get_pointer(duk,-1);
	duk_pop(duk);
	if (conn) rc = sqlite3_close(conn);
	if (!fin) {
		duk_del_prop_string(duk,-1,"__PTR");
		duk_del_prop_string(duk,-1,"close");
		duk_del_prop_string(duk,-1,"query");
		duk_pop(duk);
		if (rc == SQLITE_OK) duk_push_true(duk);
		else duk_push_false(duk);
		return 1;
	}
	return 0;
}

static int sqliteQuery(duk_context *duk) {
	const char *query;
	sqlite3 *conn;
	sqlite3_stmt *stmt = NULL;
	int rc,done,row,count,index;
	const void *blob;
	void *buff;

	query = duk_require_string(duk,0);
	if (*query == '\0') return DUK_RET_API_ERROR;
	duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if (!duk_is_pointer(duk,-1)) return DUK_RET_INTERNAL_ERROR;
	conn = duk_get_pointer(duk,-1);
	duk_pop_2(duk);
	if (!conn) return DUK_RET_INTERNAL_ERROR;
	rc = sqlite3_prepare(conn,query,-1,&stmt,NULL);
	if (rc != SQLITE_OK) {
		duk_push_sprintf(duk,"sqlite query error: %s",sqlite3_errmsg(conn));
		goto FAIL;
	}
	done = count = 0;
	row = 1;
	duk_push_array(duk);
	while(!done) {
		rc = sqlite3_step(stmt);
		switch (rc) {
			case SQLITE_BUSY:
				duk_push_string(duk,"sqlite query error: SQLITE_BUSY");
				goto FAIL;
			break;
			case SQLITE_DONE:
				done = 1;
			break;
			case SQLITE_ROW:
				if (count == 0) {
					count = sqlite3_column_count(stmt);
					if (count == 0) {
						duk_pop(duk);
						duk_push_int(duk,sqlite3_changes(conn));
						goto PASS;
					}
					if (duk_get_int(duk,1)) {
						duk_push_array(duk);
						for (index = 0; index < count; index++) {
							duk_push_string(duk,sqlite3_column_name(stmt,index));
							duk_put_prop_index(duk,-2,index);
						}
						duk_put_prop_index(duk,-2,0);
					}
				}
				duk_push_array(duk);
				for (index = 0; index < count; index++) {
					switch (sqlite3_column_type(stmt,index)) {
						case SQLITE_INTEGER:
							duk_push_number(duk,(duk_double_t)sqlite3_column_int64(stmt,index));
						break;
						case SQLITE_FLOAT:
							duk_push_number(duk,(duk_double_t)sqlite3_column_double(stmt,index));
						break;
						case SQLITE_TEXT:
							duk_push_string(duk,(char *)sqlite3_column_text(stmt,index));
						break;
						case SQLITE_BLOB:
							rc = sqlite3_column_bytes(stmt,index);
							if (rc > 0) {
								buff = duk_push_fixed_buffer(duk,rc);
								blob = sqlite3_column_blob(stmt,index);
								memcpy(buff,blob,rc);
							} else {
								duk_push_null(duk);
							}
						break;
						default:
							duk_push_null(duk);
					} // end switch column type
					duk_put_prop_index(duk,-2,index);
				} // end iterating columns
				duk_put_prop_index(duk,-2,row++);
			break;
			case SQLITE_ERROR:
				duk_push_sprintf(duk,"sqlite query error: %s",sqlite3_errmsg(conn));
				goto FAIL;
			break;
			default:
				duk_push_string(duk,"sqlite api error: SQLITE unknown error returned");
				goto FAIL;
		}
	}
PASS:
	sqlite3_finalize(stmt);
	return 1;
FAIL:
	if (stmt) sqlite3_finalize(stmt);
	duk_throw(duk);
}

static int sqliteTimeout(duk_context *duk) {
	int t;
	sqlite3 *conn;

	t = duk_require_int(duk,0);
	duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if (!duk_is_pointer(duk,-1)) return DUK_RET_INTERNAL_ERROR;
	conn = duk_get_pointer(duk,-1);
	duk_pop_2(duk);
	sqlite3_busy_timeout(conn,t);
	duk_push_true(duk);
	return 1;
}

static int sqliteObject(duk_context *duk) {
	const char *name;
	int rc,timeout = 50;
	sqlite3 *conn;

	name = duk_require_string(duk,0);
	if (*name == '\0') return DUK_RET_API_ERROR;
	if (duk_get_top(duk) > 1) {
		timeout = duk_get_int(duk,1);
	}
	rc = SQLITE_OPEN_READWRITE;
	if (duk_get_top(duk) > 2) {
		if (duk_get_boolean(duk,2)) rc &= SQLITE_OPEN_CREATE;
	}
	rc = sqlite3_open_v2(name,&conn,rc,NULL);
	if (conn == NULL) {
		duk_push_string(duk,"out of memory!");
		goto FAIL;
	}
	if (rc != SQLITE_OK) {
		duk_push_sprintf(duk,"sqlite3 error: %s",sqlite3_errmsg(conn));
		goto FAIL;
	}
	sqlite3_busy_timeout(conn,timeout);
	duk_push_object(duk);
	duk_push_pointer(duk,conn);
	duk_put_prop_string(duk,-2,"__PTR");
	duk_push_c_function(duk,sqliteClose,0);
	duk_put_prop_string(duk,-2,"close");
	duk_push_c_function(duk,sqliteQuery,2);
	duk_put_prop_string(duk,-2,"query");
	duk_push_c_function(duk,sqliteTimeout,1);
	duk_put_prop_string(duk,-2,"timeout");
	duk_push_c_function(duk,sqliteClose,1);
	duk_set_finalizer(duk,-2);
	return 1;
FAIL:
	if (conn) sqlite3_close(conn);
	duk_throw(duk);
}

int init(duk_context *duk) {
	duk_push_c_function(duk,sqliteObject,1);
	duk_put_prop_string(duk,-1,"Sqlite");
	return 0;
}
