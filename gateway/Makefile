PKGCONF ?= pkg-config
CFLAGS += -O2 -g $(shell $(PKGCONF) --cflags libdpdk)
LIBS += $(shell $(PKGCONF) --static --libs libdpdk) \
	-Wl,--whole-archive,-lfstack,--no-whole-archive -Wl,--no-whole-archive \
	-lrt -lm -ldl -lcrypto -pthread -lnuma
HEADERS = ../common/jhash.h ../common/shm.h ../common/error.h ../common/ring.h \
	  connection.h listen_sock.h shm.h signal.h
SOURCES = main.c connection.c listen_sock.c shm.c signal.c

ifdef VANILLA_FSTACK
CFLAGS += -D VANILLA_FSTACK
endif

unimsg_gateway: ${SOURCES} ${HEADERS}
	$(CC) -o $@ ${CFLAGS} ${SOURCES} ${LIBS}

clean:
	rm -f unimsg_gateway
