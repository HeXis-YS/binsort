
DEFS = -DXPT_PTHREADS # -DNDEBUG # use XPT_WINDOWS for native Windows threads
WARN = -ansi -pedantic -Wall -Wextra -Wno-unused-parameter
OPT = -O2
CFLAGS = $(DEFS) $(WARN) $(OPT)
CC = gcc # -g


all: binsort

binsort_exe = binsort
binsort_objs = xpthread.o simhash.o tinymt32.o binsort.o main.o

simhash.o: simhash.c simhash.h

xpthread.o: xpthread.c xpthread.h

binsort.o: binsort.h xpthread.h simhash.h tinymt32.h

main.o: binsort.h

$(binsort_exe): $(binsort_objs)
	$(CC) $(binsort_objs) -o $@ -lpthread -lm

clean:
	-rm $(binsort_objs) $(binsort_exe)


kdiff:
	-(a=$$(mktemp -du) && hg clone $$PWD $$a && kdiff3 $$a $$PWD; rm -rf $$a)
