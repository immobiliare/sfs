ifdef DEBUG
CFLAGS:=-Werror -fno-inline -ggdb -O0 $(CFLAGS)
CXXFLAGS:=-Werror -fno-inline -ggdb -O0 $(CXXFLAGS)
else
CFLAGS:=-O2 $(CFLAGS)
CXXFLAGS:=-O2 $(CXXFLAGS)
endif
CFLAGS+=-Wall -DHAVE_UTIMENSAT
CXXFLAGS+=-Wall -DHAVE_UTIMENSAT
LDFLAGS+=-pthread
CSRCS=sfs.c util.c batch.c setproctitle.c config.c inih/ini.c
CPPSRCS=set.cpp
COBJS=$(subst .c,.o,$(CSRCS))
CPPOBJS=$(subst .cpp,.o,$(CPPSRCS))
HDRS=sfs.h setproctitle.h set.h util.h batch.h config.h inih/ini.h
FUSE_FLAGS:=$(shell pkg-config fuse --cflags) $(shell pkg-config fuse --atleast-version=2.8 && echo '-DFUSE_28')
FUSE_LIBS:=$(shell pkg-config fuse --libs)

all: sfs

sfs: $(COBJS) $(CPPOBJS)
	$(CXX) -o sfs $(COBJS) $(CPPOBJS) $(LDFLAGS) $(FUSE_LIBS)

%.o: %.c $(HDRS)
	$(CC) -c -o $@ $< $(CFLAGS) $(FUSE_FLAGS)

%.o: %.cpp $(HDRS)
	# Do not use c++11 as standard, it won't build on squeeze
	$(CXX) -std=c++0x $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f sfs $(COBJS) $(CPPOBJS)

.PHONY: all clean
