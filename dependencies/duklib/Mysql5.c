#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mysql/mysql.h>
#include <duktape.h>

static int mysqlClose(duk_context *duk) {
	int fin = duk_get_top(duk);
	MYSQL *conn;

	if (!fin) duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if (!duk_is_pointer(duk,-1)) return DUK_RET_INTERNAL_ERROR;
	conn = (MYSQL *)duk_get_pointer(duk,-1);
	duk_pop(duk);
	if (conn) mysql_close(conn);
	if (!fin) {
		duk_del_prop_string(duk,-1,"__PTR");
		duk_del_prop_string(duk,-1,"close");
		duk_del_prop_string(duk,-1,"query");
		duk_pop(duk);
		duk_push_true(duk);
		return 1;
	}
	return 0;
}

static int mysqlQuery(duk_context *duk) {
	const char *query,*err;
	MYSQL *conn;
	MYSQL_RES *result = NULL;
	MYSQL_ROW row;
	MYSQL_FIELD *field;
	unsigned int i,j,numFields;
	unsigned long l,*lengths;
	void *buff;

	query = duk_require_string(duk,0);
	duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	conn = duk_get_pointer(duk,-1);
	duk_pop_2(duk);
	if (conn == NULL) return DUK_RET_INTERNAL_ERROR;
	if (mysql_query(conn,query)) {
		duk_push_sprintf(duk,"mysql query error: %s",mysql_error(conn));
		goto fail;
	}
	result = mysql_store_result(conn);
	if (result == NULL) {
		err = mysql_error(conn);
		if (err && strlen(err) != 0) {
			duk_push_sprintf(duk,"mysql query error: %s",err);
			goto fail;
		}
		duk_push_number(duk,(duk_double_t)mysql_affected_rows(conn));
		goto pass;
	}
	numFields = mysql_num_fields(result);
	duk_push_array(duk);
	duk_push_array(duk);
	for (j = 0; j < numFields; j++) {
		field = mysql_fetch_field_direct(result,j);
		duk_push_string(duk,field->name);
		duk_put_prop_index(duk,-2,j);
	}
	duk_put_prop_index(duk,-2,0);
	i = 1;
	while ((row = mysql_fetch_row(result)) != NULL) {
		duk_push_array(duk);
		lengths = mysql_fetch_lengths(result);
		for (j = 0; j < numFields; j++) {
			field = mysql_fetch_field_direct(result,j);
			switch(field->type) {
				case MYSQL_TYPE_DECIMAL:
				case MYSQL_TYPE_TINY:
				case MYSQL_TYPE_SHORT:
				case MYSQL_TYPE_LONG:
				case MYSQL_TYPE_FLOAT:
				case MYSQL_TYPE_DOUBLE:
					duk_push_string(duk,row[j]);
					duk_to_number(duk,-1);
				break;
				case MYSQL_TYPE_TIMESTAMP:
					duk_push_lstring(duk,row[j],lengths[j]);
				break;
				case MYSQL_TYPE_LONGLONG:
				case MYSQL_TYPE_INT24:
					duk_push_string(duk,row[j]);
					duk_to_number(duk,-1);
				break;
				case MYSQL_TYPE_DATE:
				case MYSQL_TYPE_TIME:
				case MYSQL_TYPE_DATETIME:
				case MYSQL_TYPE_YEAR:
				case MYSQL_TYPE_NEWDATE:
					duk_push_lstring(duk,row[j],lengths[j]);
				break;
				case MYSQL_TYPE_BIT:
					l = strtoul(row[j],NULL,2);
					duk_push_number(duk,(duk_double_t)l);
				break;
				case MYSQL_TYPE_NEWDECIMAL:
					duk_push_string(duk,row[j]);
					duk_to_number(duk,-1);
				break;
				/* handled by default now
				case MYSQL_TYPE_ENUM:
				case MYSQL_TYPE_SET:
					duk_push_null(duk);
				*/
				break;
				case MYSQL_TYPE_BLOB:
				case MYSQL_TYPE_TINY_BLOB:
				case MYSQL_TYPE_MEDIUM_BLOB:
				case MYSQL_TYPE_LONG_BLOB:
					buff = duk_push_fixed_buffer(duk,lengths[j]);
					memcpy(buff,row[j],lengths[j]);
				break;
				case MYSQL_TYPE_VARCHAR:
				case MYSQL_TYPE_VAR_STRING:
					duk_push_lstring(duk,row[j],lengths[j]);
				break;
				case MYSQL_TYPE_STRING:
					duk_push_lstring(duk,row[j],lengths[j]);
				break;
				/* handled by default now
				case MYSQL_TYPE_GEOMETRY:
					duk_push_null(duk);
				break;
				*/
				default:
					duk_push_null(duk);
			} // end switch field->type
			duk_put_prop_index(duk,-2,j);
		} // end for columns
		duk_put_prop_index(duk,-2,i++);
	} // end while rows
pass:
	mysql_free_result(result);
	return 1;
fail:
	if (result) mysql_free_result(result);
	if (conn) mysql_close(conn);
	duk_throw(duk);
}

// mysql(host,user,pass,db,port|sock[,key,cert]);
static int mysqlObject(duk_context *duk) {
	const char *user,*pass,*host = NULL,*db = NULL,*sock = NULL,*key = NULL,*cert = NULL;
	unsigned int port = 3306;
	MYSQL *conn = NULL;

	if (duk_is_string(duk,0)) {
		host = duk_get_string(duk,0);
		if (*host == '\0') host = NULL;
	}
	user = duk_require_string(duk,1);
	pass = duk_require_string(duk,2);
	if (duk_is_string(duk,3)) {
		db = duk_get_string(duk,3);
		if (*db == '\0') db = NULL;
	}
	if (duk_is_number(duk,4)) {
		port = (unsigned int)duk_get_int(duk,4);
	} else if (duk_is_string(duk,4)) {
		sock = duk_get_string(duk,4);
		if (*sock == '\0') sock = NULL;
		port = 0;
	}

	if (!duk_is_undefined(duk,5)) key = duk_get_string(duk,5);
	if (!duk_is_undefined(duk,6)) cert = duk_get_string(duk,6);
	if ((key && !cert) || (!key && cert)) {
		duk_push_string(duk,"must specify key AND certificate");
		goto fail;
	}

	conn = mysql_init(NULL);
	if (conn == NULL) {
		duk_push_string(duk,"mysql init failed");
		goto fail;
	}
	if (key) mysql_ssl_set(conn,key,cert,NULL,NULL,NULL);
	if (mysql_real_connect(conn,host,user,pass,db,port,sock,0L) == NULL) {
		duk_push_sprintf(duk,"mysql connect failed: %s",mysql_error(conn));
		goto fail;
	}
	duk_push_object(duk);
	duk_push_pointer(duk,conn);
	duk_put_prop_string(duk,-2,"__PTR");
	duk_push_c_function(duk,mysqlClose,0);
	duk_put_prop_string(duk,-2,"close");
	duk_push_c_function(duk,mysqlQuery,1);
	duk_put_prop_string(duk,-2,"query");
	duk_push_c_function(duk,mysqlClose,1);
	duk_set_finalizer(duk,-2);
	return 1;
fail:
	if (conn) mysql_close(conn);
	duk_throw(duk);
}

duk_int_t init(duk_context *duk) {
	duk_push_c_function(duk,mysqlObject,4);
	duk_push_int(duk,5);
	duk_put_prop_string(duk,-2,"MAJOR");
	duk_push_int(duk,5);
	duk_put_prop_string(duk,-2,"RELEASE");
	duk_push_int(duk,40);
	duk_put_prop_string(duk,-2,"VERSION");
	duk_put_global_string(duk,"Mysql");
	return 0;
}
