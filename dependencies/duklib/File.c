#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <duktape.h>

static int fileClose(duk_context *duk) {
	int argc = duk_get_top(duk),err = 0;
	FILE *file = NULL;
	char ebuff[256];

	if (argc == 0) duk_push_this(duk);
	if (duk_get_prop_string(duk,-1,"__PTR")) {
		file = (FILE *)duk_get_pointer(duk,-1);
		duk_pop(duk);
	}
	if (file) {
		if (fclose(file)) err = errno;
	}
	if (argc == 0) {
		if (err) { 
			duk_pop(duk);
			strerror_r(err,ebuff,256);
			duk_push_sprintf(duk,"file close failed: %s",ebuff);
			duk_throw(duk);
		}
		duk_del_prop_string(duk,-1,"__PTR");
		duk_del_prop_string(duk,-1,"gets");
		duk_del_prop_string(duk,-1,"puts");
		duk_pop(duk);
		duk_push_true(duk);
		return 1;
	}
	return 0;
}

static int fileGets(duk_context *duk) {
	FILE *file;
	char line[256],*nl,ebuff[256];

	duk_push_this(duk);
	if (duk_get_prop_string(duk,-1,"__PTR")) {
		file = (FILE *)duk_get_pointer(duk,-1);
		duk_pop_2(duk);
		if (fgets(line,256,file) == NULL) {
			if (feof(file)) {
				duk_push_false(duk);
				return 1;
			} else {
				strerror_r(errno,ebuff,256);
				duk_push_sprintf(duk,"file gets failed: %s",ebuff);
				duk_throw(duk);
			}
		}
		nl = strrchr(line,'\n');
		if (nl) *nl = '\0';
		duk_push_string(duk,line);
		return 1;
	}
	duk_pop(duk);
	return DUK_RET_INTERNAL_ERROR;
}

static int filePuts(duk_context *duk) {
	duk_idx_t argc = duk_get_top(duk),i;
	FILE *file;
	char ebuff[256];

	duk_push_this(duk);
	if (duk_get_prop_string(duk,-1,"__PTR")) {
		file = (FILE *)duk_get_pointer(duk,-1);
		duk_pop_2(duk);
		for (i = 0; i < argc; i++) {
			duk_safe_to_string(duk,i);
			if (fprintf(file,"%s",duk_get_string(duk,i)) < 0) {
				strerror_r(errno,ebuff,256);
				duk_push_sprintf(duk,"file puts failed: %s",ebuff);
				duk_throw(duk);
			}
		}
		duk_push_true(duk);
		return 1;
	}
	duk_pop(duk);
	return DUK_RET_INTERNAL_ERROR;
}

static int fileSlurp(duk_context *duk) {
	const char *name = duk_require_string(duk,0);
	duk_push_string_file(duk,name);
	return 1;
}

static int fileObject(duk_context *duk) {
	const char *name,*mode;
	FILE *file;
	char ebuff[256];

	name = duk_require_string(duk,0);
	mode = duk_require_string(duk,1);


	if (strlen(name) == 0 || strlen(mode) == 0) return DUK_RET_API_ERROR;

	file = fopen(name,mode);
	if (file == NULL) {
		strerror_r(errno,ebuff,256);
		duk_push_sprintf(duk,"file open failed: %s",ebuff);
		duk_throw(duk);
	}

	duk_push_object(duk);
	duk_push_pointer(duk,file);
	duk_put_prop_string(duk,-2,"__PTR");
	duk_push_c_function(duk,fileClose,0);
	duk_put_prop_string(duk,-2,"close");
	duk_push_c_function(duk,fileGets,0);
	duk_put_prop_string(duk,-2,"gets");
	duk_push_c_function(duk,filePuts,DUK_VARARGS);
	duk_put_prop_string(duk,-2,"puts");
	duk_push_c_function(duk,fileClose,1);
	duk_set_finalizer(duk,-2);
	return 1;
}

static int fileRename(duk_context *duk) {
	char ebuff[256];
	const char *from;
	const char *to;

	from = duk_require_string(duk,0);
	to = duk_require_string(duk,1);
	if (rename(from,to)) {
		strerror_r(errno,ebuff,256);
		duk_push_sprintf(duk,"move from %s to %s failed: %s",from,to,ebuff);
		duk_throw(duk);
	}
	duk_push_true(duk);
	return 1;
}

static int fileCopy(duk_context *duk) {
	const char *from,*to;
	int fdin,fdout;
	struct stat astat;
	char *src,*dst,ebuff[256];

	from = duk_require_string(duk,0);
	to = duk_require_string(duk,1);

	if (stat(from,&astat) < 0) {
		strerror_r(errno,ebuff,256);
		duk_push_sprintf(duk,"could not stat %s: %s",from,ebuff);
		duk_throw(duk);
	}
	if ((fdin = open(from,O_RDONLY)) < 0) {
		strerror_r(errno,ebuff,256);
		duk_push_sprintf(duk,"error opening %s: %s",from,ebuff);
		duk_throw(duk);
	}
	if ((fdout = open(to,O_RDWR | O_CREAT | O_TRUNC,astat.st_mode)) < 0) {
		strerror_r(errno,ebuff,256);
		close(fdin);
		duk_push_sprintf(duk,"error opening %s: %s",to,ebuff);
		duk_throw(duk);
	}
	if (lseek(fdout,astat.st_size - 1,SEEK_SET) < 0) {
		strerror_r(errno,ebuff,256);
		close(fdin);
		close(fdout);
		duk_push_sprintf(duk,"error seeking %s: %s",to,ebuff);
		duk_throw(duk);
	}
	if (write(fdout,"",1) < 0) {
		strerror_r(errno,ebuff,256);
		close(fdin);
		close(fdout);
		duk_push_sprintf(duk,"error writing %s: %s",to,ebuff);
		duk_throw(duk);
	}
	if ((src = mmap(0,astat.st_size,PROT_READ, MAP_SHARED, fdin,0)) == (void *)-1) {
		strerror_r(errno,ebuff,256);
		close(fdin);
		close(fdout);
		duk_push_sprintf(duk,"mmap %s: %s",from,ebuff);
		duk_throw(duk);
	}
	if ((dst = mmap(0,astat.st_size,PROT_READ | PROT_WRITE, MAP_SHARED, fdout, 0)) == (void *)-1) {
		strerror_r(errno,ebuff,256);
		close(fdin);
		close(fdout);
		munmap(src,astat.st_size);
		duk_push_sprintf(duk,"mmap %s: %s",to,ebuff);
		duk_throw(duk);
	}
	memcpy(dst,src,astat.st_size);
	munmap(src,astat.st_size);
	munmap(dst,astat.st_size);
	duk_push_true(duk);
	return 1;
}

int init(duk_context *duk) {
	duk_push_c_function(duk,fileObject,2);
	duk_push_c_function(duk,fileSlurp,1);
	duk_put_prop_string(duk,-2,"slurp");
	duk_push_c_function(duk,fileRename,2);
	duk_put_prop_string(duk,-2,"rename");
	duk_push_c_function(duk,fileCopy,2);
	duk_put_prop_string(duk,-2,"copy");
	duk_put_global_string(duk,"File");
	return 0;
}
