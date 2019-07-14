CFLAGS=-Wall -Werror -O3 \
	-DHAVE_COPY_FILE_RANGE \
	-DHAVE_POSIX_FALLOCATE \
	-DHAVE_UTIMENSAT \
	-DHAVE_LIBULOCKMGR \
	-DHAVE_SETXATTR \
	-DHAVE_FSTATAT

LDFLAGS=-pthread -lulockmgr
ifdef DEBUG
CFLAGS+=-fno-inline -ggdb -O0
else
CFLAGS+=-Ofast
endif
CSRCS=sfs.c util.c batch.c setproctitle.c config.c inih/ini.c
CPPSRCS=set.cpp
COBJS=$(subst .c,.o,$(CSRCS))
CPPOBJS=$(subst .cpp,.o,$(CPPSRCS))
HDRS=sfs.h setproctitle.h set.h util.h batch.h config.h inih/ini.h
CFLAGS+=$(shell pkg-config fuse3 --atleast-version=3.0 && echo ' -DFUSE_30 ') -lulockmgr

all: sfs

sfs: $(COBJS) $(CPPOBJS)
	g++ -o sfs $(COBJS) $(CPPOBJS) $(LDFLAGS) `pkg-config fuse3 --libs`

%.o: %.c $(HDRS)
	gcc -c -o $@ $< $(CFLAGS) `pkg-config fuse3 --cflags`

%.o: %.cpp $(HDRS)
	# Do not use c++11 as standard, it won't build on squeeze
	g++ -std=c++0x $(CFLAGS) -c -o $@ $<

clean:
	rm -f sfs $(COBJS) $(CPPOBJS)

.PHONY: all clean
