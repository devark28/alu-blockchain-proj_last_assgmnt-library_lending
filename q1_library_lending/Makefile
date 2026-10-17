CC = gcc
CFLAGS = -Wall -Wextra
LIBS = -lcrypto
SOURCES = $(wildcard *.c)
PROGRAM = library_tracker

# Both builds always recompile, so a test build is never mistaken for a normal one.
.PHONY: all test clean reset

all:
	$(CC) $(CFLAGS) -o $(PROGRAM) $(SOURCES) $(LIBS)

# One "day" lasts 10 seconds, to see overdue loans and fines without waiting.
test:
	$(CC) $(CFLAGS) -DDAY_SECONDS=10 -o $(PROGRAM) $(SOURCES) $(LIBS)

clean:
	rm -f $(PROGRAM)

# Deletes the saved chain, pool, snapshots and every key, to start over.
reset:
	rm -rf *.dat *.tmp keys/
