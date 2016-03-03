CC=gcc
CFLAGS=-Wall -O2 -fomit-frame-pointer -mtune=native -I../sqlite-amalgamation-3110000

.PHONY: all

all: File.so Sqlite3.so

File.so: File.c
	$(CC) $(CFLAGS) -shared -o $@ $<

Sqlite3.so: Sqlite3.c sqlite3.o
	$(CC) $(CFLAGS) -shared -o $@ $^

sqlite3.o: ../sqlite-amalgamation-3110000/sqlite3.c
	$(CC) $(CFLAGS) -c $<

