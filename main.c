#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
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

typedef struct module {
	char *name;
	char *url;
	onion_handler *handler;
	void *handle;
	void (*destroy)(void);
	struct module *next;
} module;

typedef struct script {
	char *name;
	char *url;
	struct script *next;
} script;

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

typedef struct {
	char *path;
	void *bytecode;
	duk_size_t len;
	time_t modified;
	UT_hash_handle hh;
} bytecode;

typedef struct {
	char *key;
	void *value;
	timer_t timer;
	void (*ffn)(void *);
	UT_hash_handle hh;
} keyvalue;

typedef struct {
	onion *server;
	char *port;
	char *sslport;
	char *global_ext;
	script *scripts;
	char *scripts_path;
	char *jslib_path;
	module *modules;
	char *modules_path;
	int (*kv_set)(const char *key,void *value,void *ffn,int expiry,int nx);
	const char *(*kv_get)(const char *key);
} module_context;

static int DONE = 0;
static char *JSLIBPATH = NULL;
static keyvalue *kvs = NULL;
static bytecode *cached_scripts = NULL;
static pthread_rwlock_t kvs_lock;
static pthread_rwlock_t scripts_lock;

static int kv_set(const char *key,void *value,void *ffn,int expiry,int nx);
static const char *kv_get(const char *key);

static onion_connection_status index_handler(void *data, onion_request *request, onion_response *response);
static onion_connection_status js_handler(void *data, onion_request *request, onion_response *response);

static duk_int_t duk_modsearch(duk_context *duk);
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

static duk_int_t duk_kv_set(duk_context *duk);
static duk_int_t duk_kv_get(duk_context *duk);

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

static const duk_function_list_entry KVBINDINGS[] = {
	{ "get", duk_kv_get, 1 },
	{ "set", duk_kv_set, 4 },
	{ NULL,  NULL,       0 }
};

static void sig_handler(int sig) {
	DONE = 1;
}

static void close_modules(module *list) {
	module *m,*mt;

	LL_FOREACH_SAFE(list,m,mt) {
		LL_DELETE(list,m);
		if (m->destroy != NULL) m->destroy();
		dlclose(m->handle);
		free(m);
	}
}

static void timer_handler(int sig, siginfo_t *si, void *uc) {
	keyvalue *kv;
	char *key = si->si_value.sival_ptr;

	pthread_rwlock_rdlock(&kvs_lock);
	HASH_FIND(hh,kvs,key,strlen(key),kv);
	pthread_rwlock_unlock(&kvs_lock);

	if (kv) {
		pthread_rwlock_wrlock(&kvs_lock);
		HASH_DEL(kvs,kv);
		pthread_rwlock_unlock(&kvs_lock);
		free(kv->key);
		if (kv->ffn != NULL) kv->ffn(kv->value);
		timer_delete(kv->timer);
		free(kv);
	}
}

int main(int argc, char **argv) {
	onion *o;
	onion_handler *root,*files = NULL,*module_handler;
	onion_url *urls;
	onion_listen_point *ssl = NULL;
	ezxml_t xml,sub,node;
	const char *script_path,*global_script_ext,*libpath,*modpath;
	const char *url,*name,*sslport = NULL,*sslcert = NULL,*sslkey = NULL;
	sds data,docroot = NULL,global_regex;
	void *handle;
	int (*init)(const char *path);
	module_context mctx;
	onion_connection_status (*dynhandler)(void *,onion_request *,onion_response *);
	module *modules = NULL,*mod;
	script *scripts = NULL,*scr;
	struct sigaction sa;
	keyvalue *kv,*kvt;
	bytecode *bc,*bct;
	
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

	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = timer_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGRTMAX - 1,&sa,NULL) < 0) {
		fprintf(stderr,"failed to intialize key/value store timeout mechanism\n");
		return 1;
	}

	if (pthread_rwlock_init(&kvs_lock,NULL) < 0) {
		fprintf(stderr,"failed to initialize key/value store read/write lock\n");
		return 1;
	}

	if (pthread_rwlock_init(&scripts_lock,NULL) < 0) {
		fprintf(stderr,"failed to initialze script cache lock\n");
		return 1;
	}

	if ((o = onion_new(O_POOL | O_DETACH_LISTEN | O_NO_SIGTERM)) == NULL) {
		fprintf(stderr,"failed to initialize onion\n");
		return 1;
	}

	if ((urls = onion_url_new()) == NULL) {
		fprintf(stderr,"failed to initialize onion\n");
		return 1;
	}

#ifdef CEPA_USE_INDEX_HANDLER
	onion_url_add(urls,"",index_handler);
#endif

	mctx.server = o;
	mctx.sslport = NULL;
	mctx.global_ext = NULL;
	mctx.scripts = NULL;
	mctx.scripts_path = NULL;
	mctx.modules = NULL;
	mctx.modules_path = NULL;
	mctx.jslib_path = NULL;
	mctx.kv_set = kv_set;
	mctx.kv_get = kv_get;

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
		mctx.port = strdup(node->txt); // TODO null check
	} else {
		onion_set_port(o,CEPA_DEFAULT_PORT);
		mctx.port = strdup(CEPA_DEFAULT_PORT); // TODO null check
	}

	if ((sub = ezxml_child(xml,"scripts")) != NULL) {
		if ((script_path = ezxml_attr(sub,"path")) != NULL) {
			mctx.scripts_path = strdup(script_path); // TODO null check
			for (node = ezxml_child(sub,"script"); node != NULL; node = node->next) {
				url = ezxml_attr(node,"url");
				name = ezxml_attr(node,"name");
				if (url != NULL && name != NULL) {
					scr = malloc(sizeof(script)); // TODO null check
					scr->name = strdup(name); // TODO null check
					scr->url = strdup(url); // TODO null check
					LL_APPEND(scripts,scr);
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
			if (strlen(global_script_ext) == 0) global_script_ext = "jsx";
			global_regex = sdsempty();
			global_regex = sdscatprintf(global_regex,"^(.*)\\.%s$",global_script_ext);
			onion_url_add_with_data(urls,global_regex,js_handler,docroot,sdsfree);
			mctx.global_ext = strdup(global_script_ext); // TODO null check
			sdsfree(global_regex);
		}
		if ((libpath = ezxml_attr(sub,"libpath")) != NULL) {
			JSLIBPATH = strdup(libpath); // TODO null check
		}
	}

	if ((sub = ezxml_child(xml,"modules")) != NULL) {
		if ((modpath = ezxml_attr(sub,"path")) != NULL) {
			mctx.modules_path = strdup(modpath); // TODO null check
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
					if ((mod = malloc(sizeof(module))) == NULL) {
						fprintf(stderr,"out of memory\n");
						close_modules(modules);
						return 1;
					}
					module_handler = onion_handler_new(dynhandler,&mctx,NULL);
					onion_url_add_handler(urls,url,module_handler);
					mod->name = strdup(name); // TODO null check
					mod->url = strdup(url); // TODO null check
					mod->handler = module_handler;
					mod->handle = handle;
					mod->destroy = dlsym(handle,"destroy"); // not an error if NULL
					LL_APPEND(modules,mod);
				}
			}
		}
	}

	if ((sub = ezxml_child(xml,"ssl")) != NULL) {
		if ((node = ezxml_child(sub,"port")) != NULL) {
			sslport = node->txt;
			mctx.sslport = strdup(sslport); // TODO null check
		}
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
	HASH_ITER(hh,kvs,kv,kvt) {
		HASH_DEL(kvs,kv);
		timer_delete(kv->timer);
		free(kv->key);
		if (kv->ffn != NULL) kv->ffn(kv->value);
		free(kv);
	}
	HASH_ITER(hh,cached_scripts,bc,bct) {
		HASH_DEL(cached_scripts,bc);
		free(bc->bytecode);
		free(bc);
	}
	pthread_rwlock_destroy(&kvs_lock);
	pthread_rwlock_destroy(&scripts_lock);
	free(mctx.port);
	free(mctx.sslport);
	free(mctx.modules_path);
	free(mctx.global_ext);
	return 0;
}

static onion_connection_status index_handler(void *data, onion_request *request, onion_response *response) {
	onion_shortcut_response_file("index.html",request,response);
	return OCS_PROCESSED;
}

static onion_connection_status js_handler(void *data, onion_request *request, onion_response *response) {
	const char *spath = data,*msg;
	char path[CEPA_PATH_MAX];
	struct stat astat;
	context ctx;
	duk_context *duk = NULL;
	header *h,*ht;
	int len,insert = 0,compiled = 0;
	jslib *j,*jt;
	sds errfull = NULL;
	bytecode *bc;
	void *code;

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

	if (stat(path,&astat) == -1) {
		len = errno;
		errfull = sdsempty();
		errfull = sdscatprintf(errfull,"failed to load %s: %s",path,strerror(len));
		msg = errfull;
		goto FAIL;
	}

	pthread_rwlock_rdlock(&scripts_lock);
	HASH_FIND(hh,cached_scripts,path,strlen(path),bc);
	pthread_rwlock_unlock(&scripts_lock);

	if (bc == NULL) {
		if ((bc = malloc(sizeof(bytecode))) == NULL) {
			msg = "out of memory";
			goto FAIL;
		}
		if ((bc->path = strdup(path)) == NULL) {
			free(bc);
			msg = "out of memory";
			goto FAIL;
		}
		bc->modified = 0;
		bc->bytecode = NULL;
		insert = 1;
	}
	if (bc->modified < astat.st_mtime) {
		if (duk_pcompile_file(duk,0,path) != 0) {
			msg = duk_get_string(duk,-1);
			goto FAIL;
		}
		duk_dump_function(duk);
		code = duk_get_buffer_data(duk,-1,(duk_size_t *)&bc->len);
		pthread_rwlock_wrlock(&scripts_lock);
		if (bc->bytecode != NULL) free(bc->bytecode);
		if ((bc->bytecode = malloc(bc->len)) == NULL) {
			free(bc->path);
			free(bc);
			pthread_rwlock_unlock(&scripts_lock);
			msg = "out of memory";
			goto FAIL;
		}
		memcpy(bc->bytecode,code,bc->len);
		bc->modified = astat.st_mtime;
		if (insert) HASH_ADD_KEYPTR(hh,cached_scripts,bc->path,strlen(bc->path),bc);
		pthread_rwlock_unlock(&scripts_lock);
		duk_pop(duk);
		compiled = 1;
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

	duk_push_object(duk);
	duk_put_function_list(duk,-1,KVBINDINGS);
	duk_put_global_string(duk,"kv");

	duk_push_global_object(duk);
	duk_del_prop_string(duk,-1,"print");
	duk_del_prop_string(duk,-1,"alert");
	duk_get_prop_string(duk,-1,"Duktape");
	duk_push_c_function(duk,duk_modsearch,4);
	duk_put_prop_string(duk,-2,"modSearch");
	duk_pop_2(duk);


	/*
	if (duk_peval_file(duk,path) != 0) {
		errfull = sdsempty();
		if (duk_is_object(duk,-1)) {
			duk_get_prop_string(duk,-1,"fileName");
			errfull = sdscat(errfull,duk_safe_to_string(duk,-1));
			duk_pop(duk);
			errfull = sdscat(errfull," : ");
			duk_get_prop_string(duk,-1,"lineNumber");
			errfull = sdscat(errfull,duk_safe_to_string(duk,-1));
			duk_pop(duk);
			errfull = sdscat(errfull," : ");
			duk_get_prop_string(duk,-1,"message");
			errfull = sdscat(errfull,duk_safe_to_string(duk,-1));
			duk_pop(duk);
			msg = errfull;
		} else {
			msg = duk_safe_to_string(duk,-1);
		}
		goto FAIL;
	}
	*/

	code = duk_push_buffer(duk,bc->len,0);
	memcpy(code,bc->bytecode,bc->len);
	duk_load_function(duk);
	if (duk_pcall(duk,0) != 0) {
		errfull = sdsempty();
		if (duk_is_object(duk,-1)) {
			duk_get_prop_string(duk,-1,"fileName");
			errfull = sdscat(errfull,duk_safe_to_string(duk,-1));
			duk_pop(duk);
			errfull = sdscat(errfull," : ");
			duk_get_prop_string(duk,-1,"lineNumber");
			errfull = sdscat(errfull,duk_safe_to_string(duk,-1));
			duk_pop(duk);
			errfull = sdscat(errfull," : ");
			duk_get_prop_string(duk,-1,"message");
			errfull = sdscat(errfull,duk_safe_to_string(duk,-1));
			duk_pop(duk);
			msg = errfull;
		} else {
			msg = duk_safe_to_string(duk,-1);
		}
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
	if (compiled) onion_response_set_header(response,"Compiled","true");
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
	if (msg) onion_shortcut_response(msg,500,request,response);
	else onion_shortcut_response("unknown error",500,request,response);
	if (duk != NULL) duk_destroy_heap(duk);
	if (errfull != NULL) sdsfree(errfull);
	return OCS_PROCESSED;
}

static duk_int_t duk_modsearch(duk_context *duk) {
	context *ctx;
	const char *name;
	char path[CEPA_PATH_MAX];
	struct stat astat;
	void *handle = NULL;
	duk_int_t (*init)(duk_context *duk);
	jslib *lib;

	duk_push_heap_stash(duk);
	duk_get_prop_string(duk,-1,"__CTX");
	ctx = duk_get_pointer(duk,-1);
	duk_pop_2(duk);

	if (JSLIBPATH == NULL) {
		duk_push_string(duk,"no library path");
		duk_throw(duk);
	}
	name = duk_require_string(duk,0);
	HASH_FIND(hh,ctx->jslibs,name,strlen(name),lib);
	if (lib != NULL) return 0;
	if ((lib = malloc(sizeof(jslib))) == NULL) {
		duk_push_string(duk,"out of memory");
		duk_throw(duk);
	}
	if ((lib->name = strdup(name)) == NULL) {
		free(lib);
		duk_push_string(duk,"out of memory");
		duk_throw(duk);
	}
	lib->handle = NULL;
	snprintf(path,CEPA_PATH_MAX - 1,"%s/%s.js",JSLIBPATH,name);
	path[CEPA_PATH_MAX - 1] = '\0';
	if (stat(path,&astat) == -1) {
		snprintf(path,CEPA_PATH_MAX - 1,"%s/%s.so",JSLIBPATH,name);
		path[CEPA_PATH_MAX - 1] = '\0';
		if (stat(path,&astat) == -1) {
			duk_push_sprintf(duk,"module %s not found",name);
			goto FAIL;
		}
		if ((handle = dlopen(path,RTLD_NOW)) == NULL) {
			duk_push_sprintf(duk,"failed to load native module %s: %s",name,dlerror());
			goto FAIL;
		}
		if ((init = dlsym(handle,"init")) == NULL) {
			duk_push_sprintf(duk,"failed to load native module %s: %s",name,dlerror());
			goto FAIL;
		}
		duk_push_c_function(duk,init,2);
		duk_dup(duk,2);
		duk_dup(duk,3);
		if (duk_pcall(duk,2) != 0) {
			goto FAIL;
		}
		duk_pop(duk);
		lib->handle = handle;
	} else {
		duk_push_string_file(duk,path);
		return 1;
	}
	return 0;
FAIL:
	free(lib->name);
	free(lib);
	duk_throw(duk);
	return 0; // compiler bodge
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
		if (value == NULL) return 0;
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
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,timeout = 50;
	
	path = duk_require_string(duk,0);
	if (*path == '\0') return DUK_RET_API_ERROR;
	if (duk_get_top(duk) > 1) timeout = duk_require_int(duk,1);
	if (duk_get_top(duk) > 2) if (duk_get_boolean(duk,2)) flags |= SQLITE_OPEN_CREATE;

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

static duk_int_t duk_kv_get(duk_context *duk) {
	const char *key;
	duk_size_t len;
	keyvalue *kv;

	key = duk_require_lstring(duk,0,&len);
	
	pthread_rwlock_rdlock(&kvs_lock);
	HASH_FIND(hh,kvs,key,len,kv);
	pthread_rwlock_unlock(&kvs_lock);

	if (kv != NULL) {
		duk_push_string(duk,kv->value);
		return 1;
	}
	return 0;
}

static duk_int_t duk_kv_set(duk_context *duk) {
	const char *key,*value;
	duk_size_t len;
	duk_int_t expiry;
	duk_bool_t nxflag;
	keyvalue *kv;
	struct sigevent sev;
	struct itimerspec its;
	int err;
	char *temp;

	key = duk_require_lstring(duk,0,&len);
	value = duk_get_string(duk,1);
	expiry = duk_get_int(duk,2);
	nxflag = duk_get_boolean(duk,3);

	pthread_rwlock_rdlock(&kvs_lock);
	HASH_FIND(hh,kvs,key,len,kv);
	pthread_rwlock_unlock(&kvs_lock);

	if (kv == NULL) {
		if (value == NULL) {
			duk_push_false(duk);
		} else {
			if ((kv = malloc(sizeof(keyvalue))) == NULL) {
				duk_push_string(duk,"out of memory");
				duk_throw(duk);
			}
			if ((kv->key = strdup(key)) == NULL) {
				free(kv);
				duk_push_string(duk,"out of memory");
				duk_throw(duk);
			}
			if ((kv->value = strdup(value)) == NULL) {
				free(kv->key);
				free(kv);
				duk_push_string(duk,"out of memory");
				duk_throw(duk);
			}
			if (expiry > 0) {
				sev.sigev_notify = SIGEV_SIGNAL;
				sev.sigev_signo = SIGRTMAX - 1;
				sev.sigev_value.sival_ptr = kv->key;
				if (timer_create(CLOCK_REALTIME,&sev,&kv->timer) == -1) {
					err = errno;
					free(kv->key);
					free(kv);
					duk_push_sprintf(duk,"failed to create timer: %s",strerror(err));
					duk_throw(duk);
				}
				its.it_value.tv_sec = expiry;
				its.it_value.tv_nsec = 0;
				its.it_interval.tv_sec = 0;
				its.it_interval.tv_nsec = 0;
				if (timer_settime(kv->timer,0,&its,NULL) == -1) {
					err = errno;
					free(kv->key);
					free(kv);
					timer_delete(kv->timer);
					duk_push_sprintf(duk,"failed to set timer: %s",strerror(err));
					duk_throw(duk);
				} 
			} else {
				kv->timer = NULL;
			}
			kv->ffn = free;
			pthread_rwlock_wrlock(&kvs_lock);
			HASH_ADD_KEYPTR(hh,kvs,kv->key,len,kv);
			pthread_rwlock_unlock(&kvs_lock);
			duk_push_true(duk);
		}
	} else {
		if (value == NULL) {
			pthread_rwlock_wrlock(&kvs_lock);
			HASH_DEL(kvs,kv);
			pthread_rwlock_unlock(&kvs_lock);
			if (kv->timer != NULL) timer_delete(kv->timer);
			free(kv->key);
			free(kv);
			duk_push_true(duk);
		} else {
			if (nxflag) {
				duk_push_false(duk);
			} else {
				if ((temp = strdup(value)) == NULL) {
					duk_push_string(duk,"out of memory");
					duk_throw(duk);
				}
				pthread_rwlock_wrlock(&kvs_lock);
				free(kv->value);
				kv->value = temp;
				pthread_rwlock_unlock(&kvs_lock);
				if (expiry < 0) {
					timer_delete(kv->timer);
					kv->timer = NULL;
				} else if (expiry > 0) {
					its.it_value.tv_sec = expiry;
					its.it_value.tv_nsec = 0;
					its.it_interval.tv_sec = 0;
					its.it_interval.tv_nsec = 0;
					if (timer_settime(kv->timer,0,&its,NULL) == -1) {
						err = errno;
						duk_push_sprintf(duk,"failed to set timer: %s",strerror(err));
						duk_throw(duk);
					}
				}
				duk_push_true(duk);
			}
		}
	}
	return 1;
}

static const char *kv_get(const char *key) {
	keyvalue *kv;

	if (key == NULL) return NULL;
	pthread_rwlock_rdlock(&kvs_lock);
	HASH_FIND(hh,kvs,key,strlen(key),kv);
	pthread_rwlock_unlock(&kvs_lock);
	if (kv == NULL) return NULL;
	return (const char *)kv->value;
}

static int kv_set(const char *key, void *value, void *ffn, int expiry, int nx) {
	keyvalue *kv;
	struct sigevent sev;
	struct itimerspec its;

	if (key == NULL) return 0;
	pthread_rwlock_rdlock(&kvs_lock);
	HASH_FIND(hh,kvs,key,strlen(key),kv);
	pthread_rwlock_unlock(&kvs_lock);

	if (kv == NULL) {
		if (value == NULL) return 0;
		if ((kv = malloc(sizeof(keyvalue))) == NULL) return 0;
		if ((kv->key = strdup(key)) == NULL) {
			free(kv);
			return 0;
		}
		kv->value = value;
		if (expiry > 0) {
			sev.sigev_notify = SIGEV_SIGNAL;
			sev.sigev_signo = SIGRTMAX - 1;
			sev.sigev_value.sival_ptr = kv->key;
			if (timer_create(CLOCK_REALTIME,&sev,&kv->timer) == -1) {
				free(kv->value);
				free(kv->key);
				free(kv);
				return 0;
			}
			its.it_value.tv_sec = expiry;
			its.it_value.tv_nsec = 0;
			its.it_interval.tv_sec = 0;
			its.it_interval.tv_nsec = 0;
			if (timer_settime(kv->timer,0,&its,NULL) == -1) {
				timer_delete(kv->timer);
				free(kv->value);
				free(kv->key);
				free(kv);
				return 0;
			}
		} else {
			kv->timer = NULL;
		}
		kv->ffn = ffn;
		pthread_rwlock_wrlock(&kvs_lock);
		HASH_ADD_KEYPTR(hh,kvs,kv->key,strlen(kv->key),kv);
		pthread_rwlock_unlock(&kvs_lock);
		return 1;
	}
	if (value == NULL) {
		pthread_rwlock_wrlock(&kvs_lock);
		HASH_DEL(kvs,kv);
		pthread_rwlock_unlock(&kvs_lock);
		timer_delete(kv->timer);
		if (kv->ffn != NULL) kv->ffn(kv->value);
		free(kv->key);
		free(kv);
		return 1;
	}
	if (nx > 0) return 0;
	if (expiry < 0) {
		timer_delete(kv->timer);
		kv->timer = NULL;
	} else if (expiry > 0) {
		its.it_value.tv_sec = expiry;
		its.it_value.tv_nsec = 0;
		its.it_interval.tv_sec = 0;
		its.it_interval.tv_nsec = 0;
		if (timer_settime(kv->timer,0,&its,NULL) == -1) return 0;
	}
	pthread_rwlock_wrlock(&kvs_lock);
	if (kv->ffn != NULL) kv->ffn(kv->value);
	kv->value = value;
	kv->ffn = ffn;
	pthread_rwlock_unlock(&kvs_lock);
	return 1;
}
