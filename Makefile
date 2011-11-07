#-----------------------------------------------------------------------------
# configurable section
#-----------------------------------------------------------------------------

# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# POSIX:
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

CC = $(CROSS_COMPILE)gcc
PLATFORM_DEFS = -DXPT_PTHREADS
PLATFORM_LIBS = -lpthread -lm

# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# For Windows using MingW:
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

# CC = $(CROSS_COMPILE)gcc -mno-cygwin
# PLATFORM_DEFS = -DXPT_WINDOWS
# PLATFORM_LIBS = # empty
# CROSS_COMPILE = i686-pc-mingw32-

#-----------------------------------------------------------------------------

WARN = -Wall -Wextra -Wno-unused-parameter # -ansi -pedantic 
OPT = -O2
CFLAGS = $(PLATFORM_DEFS) $(WARN) $(OPT) # -g


all: binsort

binsort_exe = binsort
binsort_objs = xpthread.o simhash.o tinymt32.o binsort.o main.o

simhash.o: simhash.c simhash.h

xpthread.o: xpthread.c xpthread.h

binsort.o: binsort.h xpthread.h simhash.h tinymt32.h

main.o: binsort.h

$(binsort_exe): $(binsort_objs)
	$(CC) $(binsort_objs) -o $@ $(PLATFORM_LIBS)

clean:
	-rm $(binsort_objs) $(binsort_exe)


kdiff:
	-(a=$$(mktemp -du) && hg clone $$PWD $$a && kdiff3 $$a $$PWD; rm -rf $$a)
