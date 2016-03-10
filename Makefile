CC=gcc
INCLS=-Idependencies/uthash -Idependencies/sds -Idependencies/duktape -Idependencies/ezxml -Idependencies/sqlite-amalgamation-3110000
CFLAGS=-Wall -g $(INCLS) -O2 -fomit-frame-pointer -mtune=native
LDLIBS=-lm -lgnutls -lduk -ldl -lonion -lrt -lpthread
SQLFTS=-DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_PARENTHESIS

cepa: main.c sds.o duktape.o ezxml.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

sds.o: dependencies/sds/sds.c
	$(CC) $(CFLAGS) -c $<

duktape.o: dependencies/duktape/duktape.c
	$(CC) $(CFLAGS) -c $<

ezxml.o: dependencies/ezxml/ezxml.c
	$(CC) $(CFLAGS) -c $<

sqlite3.o: dependencies/sqlite-amalgamation-3110000/sqlite3.c
	$(CC) $(CFLAGS) $(SQLFTS) -c $<

.PHONY: example
example: example/cepa/dependencies/baz.c
	$(CC) -shared -fPIC -o example/cepa/dependencies/baz.so $^
	$(MAKE) -C dependencies/duklib
	cp dependencies/duklib/*.so example/cepa/lib

.PHONY: run
run: cepa
	./cepa example.xml

.PHONY: clean
clean:
	rm -f cepa
	rm -f *.o
	$(MAKE) -C dependencies/duklib clean

