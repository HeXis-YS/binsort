
DEFS = -DXPT_PTHREADS -DNDEBUG
WARN = -Wall -Wextra -Wno-unused-parameter
OPT = -O3 # -march=core2 -mtune=generic
CFLAGS = $(DEFS) $(WARN) $(OPT) -fpic -DPIC 
CC = gcc


all: binsort

binsort_exe = binsort
binsort_objs = simhash.o xpthread.o binsort.o tinymt32.o

simhash.o: simhash.c simhash.h

xpthread.o: xpthread.c xpthread.h

binsort.o: xpthread.h simhash.h tinymt32.h



$(binsort_exe): $(binsort_objs)
	$(CC) $(binsort_objs) -o $@ -lpthread -lm

clean:
	-rm $(binsort_objs) $(binsort_exe)

kdiff:
	-(a=$$(mktemp -du) && hg clone $$PWD $$a && kdiff3 $$a $$PWD; rm -rf $$a)
