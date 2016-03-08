CC=gcc
CFLAGS=-Wall -O2 -fomit-frame-pointer -mtune=native -I../sqlite-amalgamation-3110000

.PHONY: all

all: File.so

File.so: File.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

.PHONY: clean
clean:
	rm -f *.so

