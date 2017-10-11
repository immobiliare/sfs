CFLAGS=-Wall -DHAVE_UTIMENSAT
LDFLAGS=-pthread
ifdef DEBUG
CFLAGS+=-Werror -fno-inline -ggdb -O0
else
CFLAGS+=-O2
endif
CSRCS=sfs.c util.c batch.c setproctitle.c config.c inih/ini.c
CPPSRCS=set.cpp
COBJS=$(subst .c,.o,$(CSRCS))
CPPOBJS=$(subst .cpp,.o,$(CPPSRCS))
HDRS=sfs.h setproctitle.h set.h util.h batch.h config.h inih/ini.h
CFLAGS+=$(shell pkg-config fuse --atleast-version=2.8 && echo ' -DFUSE_28 ')

all: sfs

sfs: $(COBJS) $(CPPOBJS)
	g++ -o sfs $(COBJS) $(CPPOBJS) $(LDFLAGS) `pkg-config fuse --libs`

%.o: %.c $(HDRS)
	gcc -c -o $@ $< $(CFLAGS) `pkg-config fuse --cflags`

%.o: %.cpp $(HDRS)
	# Do not use c++11 as standard, it won't build on squeeze
	g++ -std=c++0x $(CFLAGS) -c -o $@ $<

clean:
	rm -f sfs $(COBJS) $(CPPOBJS)

.PHONY: all clean
