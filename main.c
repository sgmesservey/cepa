#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <onion/onion.h>
#include <onion/handler.h>
#include <onion/https.h>
#include <onion/listen_point.h>
#include <onion/shortcuts.h>
#include <onion/exportlocal.h>
#include <onion/dict.h>
#include <sds.h>
#include <ezxml.h>
#include <duktape.h>
#include <utlist.h>
#include <uthash.h>
#include <sqlite3.h>

#define CEPA_DEFAULT_CHDIR  "/"
#define CEPA_DEFAULT_PORT   "8080"
#define CEPA_PATH_MAX       128
#define CEPA_USE_INDEX_HTML 1

typedef struct listelm {
	void *handle;
	struct listelm *next;
} listelm;

typedef struct {
	char *key;
	char *value;
	UT_hash_handle hh;
} header;

typedef struct {
	char *name;
	void *handle;
	UT_hash_handle hh;
} jslib;

typedef struct {
	onion_request *request;
	header *headers;
	sds buffer;
	const char *jslibpath;
	jslib *jslibs;
	int rc;
} context;

static int DONE = 0;
static char *JSLIBPATH = NULL;

static onion_connection_status index_handler(void *data, onion_request *request, onion_response *response);
static onion_connection_status js_handler(void *data, onion_request *request, onion_response *response);

static duk_int_t duk_require(duk_context *duk);
static duk_int_t duk_print(duk_context *duk);
static duk_int_t duk_is_secure(duk_context *duk);
static duk_int_t duk_set_code(duk_context *duk);
static duk_int_t duk_set_header(duk_context *duk);
static duk_int_t duk_get_method(duk_context *duk);
static duk_int_t duk_get_header(duk_context *duk);
static duk_int_t duk_get_path(duk_context *duk);
static duk_int_t duk_get_fullpath(duk_context *duk);
static duk_int_t duk_get_query(duk_context *duk);
static duk_int_t duk_get_post(duk_context *duk);
static duk_int_t duk_get_post2(duk_context *duk);
static duk_int_t duk_get_file(duk_context *duk);
static duk_int_t duk_get_cookie(duk_context *duk);

static duk_int_t duk_sqlite_factory(duk_context *duk);
static duk_int_t duk_sqlite_query(duk_context *duk);
static duk_int_t duk_sqlite_prepare(duk_context *duk);
static duk_int_t duk_sqlite_close(duk_context *duk);
static duk_int_t duk_sqlite_bind(duk_context *duk);
static duk_int_t duk_sqlite_execute(duk_context *duk);
static duk_int_t duk_sqlite_finalize(duk_context *duk);

static const duk_function_list_entry CGIBINDINGS[] = {
	{ "print",           duk_print,        DUK_VARARGS },
	{ "isSecure",        duk_is_secure,    0 },
	{ "setResponseCode", duk_set_code,     1 },
	{ "setHeader",       duk_set_header,   2 },
	{ "getMethod",       duk_get_method,   0 },
	{ "getHeader",       duk_get_header,   1 },
	{ "getPath",         duk_get_path,     0 },
	{ "getFullPath",     duk_get_fullpath, 0 },
	{ "getQuery",        duk_get_query,    1 },
	{ "getPost",         duk_get_post,     1 },
	{ "getPostMulti",    duk_get_post2,    2 },
	{ "getFile",         duk_get_file,     1 },
	{ "getCookie",       duk_get_cookie,   1 },
	{ NULL,              NULL,             0 }
};

static const duk_function_list_entry SQLITEBINDINGS[] = {
	{ "close",   duk_sqlite_close,   0 },
	{ "prepare", duk_sqlite_prepare, 1 },
	{ "query",   duk_sqlite_query,   2 },
	{ NULL,      NULL,               0 }
};

static void sig_handler(int sig) {
	DONE = 1;
}

static void close_modules(listelm *list) {
	listelm *e,*et;

	LL_FOREACH_SAFE(list,e,et) {
		LL_DELETE(list,e);
		dlclose(e->handle);
		free(e);
	}
}

int main(int argc, char **argv) {
	onion *o;
	onion_handler *root,*files = NULL;
	onion_url *urls;
	onion_listen_point *ssl = NULL;
	ezxml_t xml,sub,node;
	const char *script_path,*global_script_ext,*libpath,*modpath;
	const char *url,*name,*sslport = NULL,*sslcert = NULL,*sslkey = NULL;
	sds data,docroot = NULL,global_regex;
	void *handle;
	int (*init)(const char *path);
	onion_connection_status (*dynhandler)(void *,onion_request *,onion_response *);
	listelm *modules = NULL,*elm;

	if (argc == 1) {
		fprintf(stderr,"%s config_file\n",argv[0]);
		return 1;
	}

	if (signal(SIGTERM,sig_handler) == SIG_ERR) {
		fprintf(stderr,"failed to set handler for SIGTERM\n");
		return 1;
	}

	if (signal(SIGINT,sig_handler) == SIG_ERR) {
		fprintf(stderr,"failed to set handler for SIGINT\n");
		return 1;
	}

	if ((o = onion_new(O_THREADED | O_DETACH_LISTEN | O_NO_SIGTERM)) == NULL) {
		fprintf(stderr,"failed to initialize onion\n");
		return 1;
	}

	if ((urls = onion_url_new()) == NULL) {
		fprintf(stderr,"failed to initialize onion\n");
		return 1;
	}

#ifdef CEPA_USE_INDEX_HTML
	onion_url_add(urls,"",index_handler);
#endif

	xml = ezxml_parse_file(argv[1]);
	if (xml->name == NULL) {
		fprintf(stderr,"could not parse '%s'\n",argv[1]);
		return 1;
	}

	if (strcmp(xml->name,"server")) {
		fprintf(stderr,"configuration must have root node <server>\n");
		return 1;
	}

	if ((node = ezxml_child(xml,"docroot")) != NULL) {
		if ((files = onion_handler_export_local_new(node->txt)) == NULL) {
			fprintf(stderr,"error exporting local path '%s'\n",node->txt);
			return 1;
		}
		chdir(node->txt);
		docroot = sdsnew(node->txt);
	} else {
		chdir(CEPA_DEFAULT_CHDIR);
	}

	if ((node = ezxml_child(xml,"port")) != NULL) {
		onion_set_port(o,node->txt);
	} else {
		onion_set_port(o,CEPA_DEFAULT_PORT);
	}

	if ((sub = ezxml_child(xml,"scripts")) != NULL) {
		if ((script_path = ezxml_attr(sub,"path")) != NULL) {
			for (node = ezxml_child(sub,"script"); node != NULL; node = node->next) {
				url = ezxml_attr(node,"url");
				name = ezxml_attr(node,"name");
				if (url != NULL && name != NULL) {
					data = sdsempty();
					data = sdscatprintf(data,"!%s/%s",script_path,name);
					onion_url_add_with_data(urls,url,js_handler,data,sdsfree);
				}
			}
		}
		if ((global_script_ext = ezxml_attr(sub,"global")) != NULL) {
			if (docroot == NULL) {
				fprintf(stderr,"global js handler: missing <docroot> element\n");
				return 1;
			}
			global_regex = sdsempty();
			global_regex = sdscatprintf(global_regex,"^(.*)\\.%s$",global_script_ext);
			onion_url_add_with_data(urls,global_regex,js_handler,docroot,sdsfree);
			sdsfree(global_regex);
		}
		if ((libpath = ezxml_attr(sub,"libpath")) != NULL) {
			JSLIBPATH = strdup(libpath);
		}
	}

	if ((sub = ezxml_child(xml,"modules")) != NULL) {
		if ((modpath = ezxml_attr(sub,"path")) != NULL) {
			for (node = ezxml_child(sub,"module"); node != NULL; node = node->next) {
				url = ezxml_attr(node,"url");
				name = ezxml_attr(node,"name");
				if (url != NULL && name != NULL) {
					data = sdsempty();
					data = sdscatprintf(data,"%s/%s",modpath,name);
					if ((handle = dlopen(data,RTLD_NOW)) == NULL) {
						fprintf(stderr,"failed to open %s: '%s'\n",data,dlerror());
						close_modules(modules);
						return 1;
					}
					if ((init = dlsym(handle,"init")) == NULL) {
						fprintf(stderr,"failed to get init() from '%s': %s\n",data,dlerror());
						dlclose(handle);
						close_modules(modules);
						return 1;
					}
					if ((dynhandler = dlsym(handle,"handle")) == NULL) {
						fprintf(stderr,"failed to get handle() from '%s': %s\n",data,dlerror());
						dlclose(handle);
						close_modules(modules);
						return 1;
					}
					init(docroot);
					if ((elm = malloc(sizeof(listelm))) == NULL) {
						fprintf(stderr,"out of memory\n");
						close_modules(modules);
						return 1;
					}
					elm->handle = handle;
					LL_APPEND(modules,elm);
					onion_url_add(urls,url,dynhandler);
				}
			}
		}
	}

	if ((sub = ezxml_child(xml,"ssl")) != NULL) {
		if ((node = ezxml_child(sub,"port")) != NULL) sslport = node->txt;
		if ((node = ezxml_child(sub,"cert")) != NULL) sslcert = node->txt;
		if ((node = ezxml_child(sub,"key")) != NULL) sslkey = node->txt;
		if (sslport != NULL && sslcert != NULL && sslkey != NULL) {
			ssl = onion_https_new();
			// TODO: error checking. Returns an int, but onion docs are unclear what success or failure is
			onion_https_set_certificate(ssl,O_SSL_CERTIFICATE_KEY,sslcert,sslkey,NULL);
			onion_add_listen_point(o,NULL,sslport,ssl);
		} else {
			fprintf(stderr,"<ssl> present, but missing one or more of <port>, <cert>, <key>\n");
			close_modules(modules);
			return 1;
		}
	}

	root = onion_url_to_handler(urls);
	if (files != NULL) onion_handler_add(root,files);
	onion_set_root_handler(o,root);

	daemon(1,0);

	onion_listen(o);

	while (!DONE) if (sleep(1) == -1 && errno == EINTR) continue;

	onion_listen_stop(o);
	onion_free(o);
	if (ssl != NULL) onion_listen_point_free(ssl);
	close_modules(modules);
	if (JSLIBPATH != NULL) free(JSLIBPATH);
	return 0;
}

static onion_connection_status index_handler(void *data, onion_request *request, onion_response *response) {
	onion_shortcut_response_file("index.html",request,response);
	return OCS_PROCESSED;
}

static onion_connection_status js_handler(void *data, onion_request *request, onion_response *response) {
	const char *spath = data,*msg;
	char path[CEPA_PATH_MAX];
	context ctx;
	duk_context *duk = NULL;
	header *h,*ht;
	int len;
	jslib *j,*jt;
	
	if (*spath == '!') {
		snprintf(path,CEPA_PATH_MAX - 1,"%s",&spath[1]);
	} else {
		snprintf(path,CEPA_PATH_MAX - 1,"%s/%s",spath,onion_request_get_fullpath(request));
	}
	path[CEPA_PATH_MAX - 1] = '\0';

	ctx.request = request;
	ctx.headers = NULL;
	ctx.buffer = sdsempty();
	ctx.jslibs = NULL;
	ctx.rc = 200;

	if ((duk = duk_create_heap_default()) == NULL) {
		msg = "out of memory";
		goto FAIL;
	}

	duk_push_heap_stash(duk);
	duk_push_pointer(duk,&ctx);
	duk_put_prop_string(duk,-2,"__CTX");
	duk_pop(duk);

	duk_push_object(duk);
	duk_put_function_list(duk,-1,CGIBINDINGS);
	duk_put_global_string(duk,"cgi");

	duk_push_c_function(duk,duk_sqlite_factory,DUK_VARARGS);
	duk_put_global_string(duk,"sqlite");

	duk_push_global_object(duk);
	duk_del_prop_string(duk,-1,"print");
	duk_del_prop_string(duk,-1,"alert");
	duk_push_c_function(duk,duk_require,1);
	duk_put_prop_string(duk,-2,"require");
	duk_pop(duk);

	if (duk_peval_file(duk,path) != 0) {
		msg = duk_safe_to_string(duk,-1);
		goto FAIL;
	}

	duk_destroy_heap(duk);
	onion_response_set_code(response,ctx.rc);
	HASH_ITER(hh,ctx.headers,h,ht) {
		onion_response_set_header(response,h->key,h->value);
		HASH_DEL(ctx.headers,h);
		free(h->key);
		free(h->value);
		free(h);
	}
	len = sdslen(ctx.buffer);
	onion_response_set_length(response,len);
	if (len) onion_response_write(response,ctx.buffer,len);

	sdsfree(ctx.buffer);
	HASH_ITER(hh,ctx.jslibs,j,jt) {
		HASH_DEL(ctx.jslibs,j);
		free(j->name);
		dlclose(j->handle);
		free(j);
	}
	return OCS_PROCESSED;
FAIL:
	HASH_ITER(hh,ctx.headers,h,ht) {
		HASH_DEL(ctx.headers,h);
		free(h->key);
		free(h->value);
		free(h);
	}
	HASH_ITER(hh,ctx.jslibs,j,jt) {
		HASH_DEL(ctx.jslibs,j);
		free(j->name);
		dlclose(j->handle);
		free(j);
	}
	sdsfree(ctx.buffer);
	onion_shortcut_response(msg,500,request,response);
	if (duk != NULL) duk_destroy_heap(duk);
	return OCS_PROCESSED;
}

static duk_int_t duk_require(duk_context *duk) {
	context *ctx;
	const char *name;
	char path[CEPA_PATH_MAX],*ext;
	void *handle;
	int (*init)(duk_context *duk);
	jslib *lib;
	sds errmsg;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	if (JSLIBPATH == NULL) {
		duk_push_string(duk,"no library path");
		duk_throw(duk);
	}

	name = duk_require_string(duk,0);
	if ((ext = strrchr(name,'.')) == NULL) {
		duk_push_string(duk,"missing library extension");
		duk_throw(duk);
	}
	ext++;
	if (*ext == '\0' || (strcmp(ext,"js") && strcmp(ext,"so"))) {
		duk_push_string(duk,"invalid library extension");
		duk_throw(duk);
	}
	HASH_FIND(hh,ctx->jslibs,name,strlen(name),lib);
	if (lib != NULL) return 0;
	snprintf(path,CEPA_PATH_MAX - 1,"%s/%s",JSLIBPATH,name);
	if (strcmp(ext,"js") == 0) {
		if (duk_peval_file(duk,path) == 0) duk_pop(duk);
		else duk_throw(duk);
	} else {
		if ((handle = dlopen(path,RTLD_NOW)) == NULL) {
			duk_push_string(duk,dlerror());
			duk_throw(duk);
		}
		if ((init = dlsym(handle,"init")) == NULL) {
			duk_push_string(duk,dlerror());
			dlclose(handle);
			duk_throw(duk);
		}
		if (init(duk)) {
			dlclose(handle);
			errmsg = sdsempty();
			errmsg = sdscatprintf(errmsg,"library %s failed to initialize",name);
			duk_push_string(duk,errmsg);
			sdsfree(errmsg);
			duk_throw(duk);
		}
		if ((lib = malloc(sizeof(jslib))) == NULL) {
			dlclose(handle);
			duk_push_string(duk,"out of memory");
			duk_throw(duk);
		}
		if ((lib->name = strdup(name)) == NULL) {
			dlclose(handle);
			free(lib);
			duk_push_string(duk,"out of memory");
			duk_throw(duk);
		}
		lib->handle = handle;
		HASH_ADD_KEYPTR(hh,ctx->jslibs,lib->name,strlen(lib->name),lib);
	}
	return 0;
}

static duk_int_t duk_is_secure(duk_context *duk) {
	context *ctx;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	if (onion_request_is_secure(ctx->request)) duk_push_true(duk);
	else duk_push_false(duk);
	return 1;
}

static duk_int_t duk_print(duk_context *duk) {
	context *ctx;
	duk_idx_t i,j;
	const char *str;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	j = duk_get_top(duk);
	for (i = 0; i < j; i++) {
		str = duk_safe_to_string(duk,i);
		ctx->buffer = sdscat(ctx->buffer,str);
	}
	return 0;
}

static duk_int_t duk_set_code(duk_context *duk) {
	context *ctx;
	duk_int_t rc;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	rc = duk_require_int(duk,0);
	ctx->rc = (int)rc;
	return 0;
}

static duk_int_t duk_set_header(duk_context *duk) {
	context *ctx;
	const char *key,*value;
	header *h;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	key = duk_require_string(duk,0);
	value = duk_safe_to_string(duk,1);
	if (strcmp(value,"null") == 0 || strcmp(value,"undefined") == 0) value = NULL;

	HASH_FIND(hh,ctx->headers,key,strlen(key),h);

	if (h != NULL) {
		free(h->value);
		if (value != NULL) {
			if ((h->value = strdup(value)) == NULL) goto FAIL;
		} else {
			HASH_DEL(ctx->headers,h);
			free(h->key);
			free(h);
		}
	} else {
		if ((h = malloc(sizeof(header))) == NULL) goto FAIL;
		if ((h->key = strdup(key)) == NULL) goto FAIL;
		if ((h->value = strdup(value)) == NULL) goto FAIL;
		HASH_ADD_KEYPTR(hh,ctx->headers,h->key,strlen(h->key),h);
	}
	return 0;
FAIL:
	duk_push_string(duk,"out of memory");
	duk_throw(duk);
}

static duk_int_t duk_get_method(duk_context *duk) {
	context *ctx;
	int method;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	method = onion_request_get_flags(ctx->request);
	method &= OR_METHODS;
	switch (method) {
		case OR_GET:
			duk_push_string(duk,"GET");
		break;
		case OR_PUT:
			duk_push_string(duk,"PUT");
		break;
		case OR_POST:
			duk_push_string(duk,"POST");
		break;
		case OR_DELETE:
			duk_push_string(duk,"DELETE");
		break;
		case OR_HEAD:
			duk_push_string(duk,"HEAD");
		break;
		default:
			return 0;
	}
	return 1;
}

static duk_int_t duk_get_header(duk_context *duk) {
	context *ctx;
	const char *key,*value;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	key = duk_require_string(duk,0);
	value = onion_request_get_header(ctx->request,key);
	duk_push_string(duk,value);
	return 1;
}

static duk_int_t duk_get_path(duk_context *duk) {
	context *ctx;
	const char *path;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	path = onion_request_get_path(ctx->request);
	duk_push_string(duk,path);
	return 1;
}

static duk_int_t duk_get_fullpath(duk_context *duk) {
	context *ctx;
	const char *path;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	path = onion_request_get_fullpath(ctx->request);
	duk_push_string(duk,path);
	return 1;
}

static duk_int_t duk_get_query(duk_context *duk) {
	context *ctx;
	const char *key,*value;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	key = duk_require_string(duk,0);
	value = onion_request_get_query(ctx->request,key);
	duk_push_string(duk,value);
	return 1;
}

static duk_int_t duk_get_post(duk_context *duk) {
	context *ctx;
	const char *key,*value;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	key = duk_require_string(duk,0);
	value = onion_request_get_post(ctx->request,key);
	duk_push_string(duk,value);
	return 1;
}

static void multi_callback(void *data, const char *key, const char *value) {
	duk_context *duk = data;
	const char *current_key;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CALLBACK_ERROR");
	if (duk_is_null_or_undefined(duk,-1)) {
		duk_pop(duk);
	} else {
		duk_pop_2(duk);
		return;
	}
	duk_get_prop_string(duk,-1,"__CURRENT_KEY");
	current_key = duk_get_string(duk,-1);
	duk_pop(duk);
	if (!strcmp(current_key,key)) {
		duk_get_prop_string(duk,-1,"__CURRENT_CALLBACK");
		duk_swap(duk,-1,-2);
		duk_pop(duk);
		duk_push_string(duk,value);
		if (duk_pcall(duk,1) == 0) {
			duk_pop(duk);
		} else {
			duk_push_heap_stash(duk);
			duk_swap(duk,-1,-2);
			duk_put_prop_string(duk,-2,"__CURRENT_ERROR");
			duk_pop(duk);
		}
	} else {
		duk_pop(duk);
	}
}

static duk_int_t duk_get_post2(duk_context *duk) {
	context *ctx;
	const char *key;
	const onion_dict *dict;
	
	duk_push_heap_stash(duk);            // <ARG0> <ARG1> <STASH>
	duk_get_prop_string(duk,-1,"__CTX"); // <ARG0> <ARG1> <STASH> <PTR>
	ctx = duk_get_pointer(duk,-1); 
	duk_pop(duk);                        // <ARG0> <ARG1> <STASH>

	key = duk_require_string(duk,0);
	key = key; // -Wall dodge
	duk_require_function(duk,1);
	
	duk_dup(duk,0);                                   // <ARG0> <ARG1> <STASH> <ARG0>
	duk_put_prop_string(duk,-2,"__CURRENT_KEY");      // <ARG0> <ARG1> <STASH>
	duk_dup(duk,1);                                   // <ARG0> <ARG1> <STASH> <ARG1>
	duk_put_prop_string(duk,-2,"__CURRENT_CALLBACK"); // <ARG0> <ARG1> <STASH>
	duk_pop(duk);                                     // <ARG0> <ARG1>

	dict = onion_request_get_post_dict(ctx->request);
	onion_dict_preorder(dict,multi_callback,duk);

	duk_push_heap_stash(duk);
	duk_del_prop_string(duk,-1,"__CURRENT_KEY");
	duk_del_prop_string(duk,-1,"__CURRENT_CALLBACK");
	duk_get_prop_string(duk,-1,"__CALLBACK_ERROR");
	duk_swap_top(duk,-2);
	duk_pop(duk);
	if (duk_is_undefined(duk,-1)) duk_pop(duk);
	else duk_throw(duk);

	return 0;
}

static duk_int_t duk_get_file(duk_context *duk) {
	context *ctx;
	const char *key,*value;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	key = duk_require_string(duk,0);
	value = onion_request_get_file(ctx->request,key);
	duk_push_string(duk,value);
	return 1;
}

static duk_int_t duk_get_cookie(duk_context *duk) {
	context *ctx;
	const char *key,*value;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	key = duk_require_string(duk,0);
	value = onion_request_get_cookie(ctx->request,key);
	duk_push_string(duk,value);
	return 1;
}

static duk_int_t duk_sqlite_factory(duk_context *duk) {
	const char *path;
	sqlite3 *db;
	int flags = SQLITE_OPEN_READWRITE,timeout = 50;
	
	path = duk_require_string(duk,0);
	if (*path == '\0') return DUK_RET_API_ERROR;
	if (duk_get_top(duk) > 1) timeout = duk_require_int(duk,1);
	if (duk_get_top(duk) > 2) if (duk_get_boolean(duk,2)) flags &= SQLITE_OPEN_CREATE;

	if (sqlite3_open_v2(path,&db,flags,NULL) != SQLITE_OK) {
		duk_push_string(duk,sqlite3_errmsg(db));
		duk_throw(duk);
	}

	sqlite3_busy_timeout(db,timeout);

	duk_push_object(duk);
	duk_push_pointer(duk,db);
	duk_put_prop_string(duk,-2,"__PTR");
	duk_put_function_list(duk,-1,SQLITEBINDINGS);
	duk_push_c_function(duk,duk_sqlite_close,1);
	duk_set_finalizer(duk,-2);

	return 1;
}

static int sqlite_execute(duk_context *duk, int headers, sqlite3 *db, sqlite3_stmt *stmt) {
	int rc,done,row,count,index;
	const void *blob;
	void *buff;

	done = row = count = 0;
	duk_push_array(duk);
	while (!done) {
		switch (sqlite3_step(stmt)) {
			case SQLITE_BUSY:
				duk_pop(duk);
				duk_push_string(duk,"sqlite timeout");
				rc = 0;
				done = 1;
			break;
			case SQLITE_DONE:
				rc = 1;
				done = 1;
			break;
			case SQLITE_ROW:
				if (count == 0) {
					count = sqlite3_column_count(stmt);
					if (count == 0) {
						duk_pop(duk);
						duk_push_int(duk,sqlite3_changes(db));
						rc = 1;
						goto END;
					}
					if (headers) {
						duk_push_array(duk);
						for (index = 0; index < count; index++) {
							duk_push_string(duk,sqlite3_column_name(stmt,index));
							duk_put_prop_index(duk,-2,index);
						}
						duk_put_prop_index(duk,-2,row++);
					}
				}
				duk_push_array(duk);
				for (index = 0; index < count; index++) {
					switch(sqlite3_column_type(stmt,index)) {
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
					} // end inner switch
					duk_put_prop_index(duk,-2,index);
				} // end for
				duk_put_prop_index(duk,-2,row++);
			break;
			default:
				duk_pop(duk);
				duk_push_string(duk,sqlite3_errmsg(db));
				rc = 0;
				done = 1;
		} // end outer switch
	} // end while
END:
	sqlite3_finalize(stmt);
	return rc;
}

static duk_int_t duk_sqlite_query(duk_context *duk) {
	const char *sql;
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int headers;

	sql = duk_require_string(duk,0);
	if (*sql == '\0') return DUK_RET_API_ERROR;
	headers = (duk_get_top(duk) > 1 && duk_get_boolean(duk,1));
	duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if ((db = duk_get_pointer(duk,-1)) == NULL) {
		duk_pop(duk);
		return DUK_RET_INTERNAL_ERROR;
	}
	duk_pop_2(duk);
	if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK) {
		duk_push_string(duk,sqlite3_errmsg(db));
		duk_throw(duk);
	}
	if (sqlite_execute(duk,headers,db,stmt) == 0) duk_throw(duk);
	return 1;
}

static duk_int_t duk_sqlite_prepare(duk_context *duk) {
	const char *sql;
	sqlite3 *db;
	sqlite3_stmt *stmt;

	sql = duk_require_string(duk,0);
	if (*sql == '\0') return DUK_RET_API_ERROR;
	duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if ((db = duk_get_pointer(duk,-1)) == NULL) return DUK_RET_INTERNAL_ERROR;
	duk_pop_2(duk);
	if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK) {
		duk_push_string(duk,sqlite3_errmsg(db));
		duk_throw(duk);
	}
	duk_push_object(duk);
	duk_push_pointer(duk,stmt);
	duk_put_prop_string(duk,-2,"__PTR");
	duk_push_pointer(duk,db);
	duk_put_prop_string(duk,-2,"__DB");
	duk_push_c_function(duk,duk_sqlite_bind,2);
	duk_put_prop_string(duk,-2,"bind");
	duk_push_c_function(duk,duk_sqlite_execute,1);
	duk_put_prop_string(duk,-2,"execute");
	duk_push_c_function(duk,duk_sqlite_finalize,1);
	duk_set_finalizer(duk,-2);
	return 1;
}

static duk_int_t duk_sqlite_close(duk_context *duk) {
	int fin,rc;
	sqlite3 *db;

	fin = duk_get_top(duk);
	if (!fin) duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if ((db = duk_get_pointer(duk,-1)) == NULL) {
		duk_pop(duk);
		if (!fin) duk_pop(duk);
		return 0;
	}
	duk_pop(duk);
	rc = sqlite3_close(db);
	if (!fin) {
		duk_del_prop_string(duk,-1,"__PTR");
		duk_del_prop_string(duk,-1,"close");
		duk_del_prop_string(duk,-1,"prepare");
		duk_del_prop_string(duk,-1,"query");
		if (rc != SQLITE_OK) {
			duk_push_string(duk,sqlite3_errstr(rc));
			duk_throw(duk);
		}
	} else {
		duk_pop(duk);
	}
	return 0;
}

static duk_int_t duk_sqlite_bind(duk_context *duk) {
	sqlite3_stmt *stmt;
	int rc,index;
	const char *str;
	void *buff;
	duk_size_t len;

	index = duk_require_int(duk,0);
	duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if ((stmt = duk_get_pointer(duk,-1)) == NULL) {
		duk_pop(duk);
		return DUK_RET_INTERNAL_ERROR;
	}
	duk_pop_2(duk);
	if (duk_get_boolean(duk,2)) {
		rc = sqlite3_bind_int64(stmt,index,(sqlite3_int64)duk_get_int(duk,1));
	} else {
		switch (duk_get_type(duk,1)) {
			case DUK_TYPE_BOOLEAN:
				rc = sqlite3_bind_int64(stmt,index,(sqlite3_int64)duk_get_boolean(duk,1));
			break;
			case DUK_TYPE_NUMBER:
				rc = sqlite3_bind_double(stmt,index,(double)duk_get_number(duk,1));
			break;
			case DUK_TYPE_STRING:
				str = duk_get_lstring(duk,1,&len);
				rc = sqlite3_bind_text(stmt,index,str,(int)len,SQLITE_STATIC);
			break;
			case DUK_TYPE_OBJECT:
				duk_get_prop_string(duk,1,"byteLength");
				rc = duk_is_undefined(duk,-1);
				duk_pop(duk);
				if (!rc) goto BUFFER;
				duk_get_prop_string(duk,1,"toString");
				rc = duk_is_undefined(duk,-1);
				duk_pop(duk);
				if (rc) {
					rc = sqlite3_bind_text(stmt,index,duk_json_encode(duk,1),-1,SQLITE_STATIC);
				} else {
					duk_get_prop_string(duk,1,"toString");
					duk_call(duk,0);
					rc = sqlite3_bind_text(stmt,index,duk_to_string(duk,-1),-1,SQLITE_STATIC);
					duk_pop(duk);
				}
			break;
			case DUK_TYPE_BUFFER:
				BUFFER:
				buff = duk_get_buffer_data(duk,1,&len);
				rc = sqlite3_bind_blob(stmt,index,(const void *)buff,(int)len,SQLITE_STATIC);
			break;
			default:
				rc = sqlite3_bind_null(stmt,index);
		}
	}
	if (rc != SQLITE_OK) {
		duk_push_string(duk,sqlite3_errstr(rc));
		duk_throw(duk);
	}
	duk_push_true(duk);
	return 1;
}

static duk_int_t duk_sqlite_execute(duk_context *duk) {
	sqlite3 *db;
	sqlite3_stmt *stmt;
	int headers,rc;

	duk_push_this(duk);
	duk_get_prop_string(duk,-1,"__PTR");
	if ((stmt = duk_get_pointer(duk,-1)) == NULL) {
		duk_pop_2(duk);
		return DUK_RET_INTERNAL_ERROR;
	}
	duk_pop(duk);
	duk_get_prop_string(duk,-1,"__DB");
	if ((db = duk_get_pointer(duk,-1)) == NULL) {
		duk_pop_2(duk);
		return DUK_RET_INTERNAL_ERROR;
	}
	duk_pop(duk);
	headers = duk_get_boolean(duk,0);
	rc = sqlite_execute(duk,headers,db,stmt);
	duk_del_prop_string(duk,1,"__DB");
	duk_del_prop_string(duk,1,"__PTR");
	duk_push_undefined(duk);
	duk_set_finalizer(duk,1);
	if (rc == 0) duk_throw(duk);
	return 1;
}

static duk_int_t duk_sqlite_finalize(duk_context *duk) {
	sqlite3_stmt *stmt;

	duk_get_prop_string(duk,0,"__PTR");
	if ((stmt = duk_get_pointer(duk,-1)) != NULL) sqlite3_finalize(stmt);
	return 0;
}

