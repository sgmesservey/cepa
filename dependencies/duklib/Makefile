CC=gcc
CFLAGS=-Wall -O2 -fomit-frame-pointer -mtune=native -I../sqlite-amalgamation-3110000

.PHONY: all

#all: File.so Sqlite3.so
all: File.so

File.so: File.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

## deprecated, Sqlite functionality is baked in to cepa itself 
#Sqlite3.so: Sqlite3.c sqlite3.o
#	$(CC) $(CFLAGS) -shared -fPIC -o $@ $^

sqlite3.o: ../sqlite-amalgamation-3110000/sqlite3.c
	$(CC) $(CFLAGS) -fPIC -c $<

