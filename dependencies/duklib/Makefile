CC=gcc
CFLAGS=-g -Wall -O2 -fomit-frame-pointer -mtune=native -I../sqlite-amalgamation-3110000

.PHONY: all

all: File.so

File.so: File.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

Mysql5.so: Mysql5.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -lmysqlclient

.PHONY: clean
clean:
	rm -f *.so

