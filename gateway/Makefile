PKGCONF ?= pkg-config
CFLAGS += -O2 -g $(shell $(PKGCONF) --cflags libdpdk)
LIBS += $(shell $(PKGCONF) --static --libs libdpdk) -lpthread -lfstack
HEADERS = ../common/shm.h ../common/error.h ../common/ring.h connection.h \
	  jhash.h listen_sock.h shm.h signal.h gwmap.h
SOURCES = main.c connection.c listen_sock.c shm.c signal.c gwmap.c

unimsg_gateway: ${SOURCES} ${HEADERS}
	$(CC) -o $@ ${CFLAGS} ${SOURCES} ${LIBS}

clean:
	rm -f unimsg_gateway