CC = gcc
CFLAGS = -Wall -g -pthread -I../../libraries/kin
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)

GUAC_PREFIX_REL = ../../dependencies/guacamole-server-install
GUAC_SRC_REL = ../../dependencies/guacamole-server-source
GUAC_PREFIX := $(abspath $(GUAC_PREFIX_REL))
GUAC_SRC := $(abspath $(GUAC_SRC_REL))

GUAC_CFLAGS = -I$(GUAC_PREFIX)/include
GUAC_LDFLAGS = -L$(GUAC_PREFIX)/lib -Wl,-Bstatic -lguac -Wl,-Bdynamic -lcairo -lpng -ljpeg -luuid -lwebp
GUAC_RPATH = -Wl,-rpath,\$$ORIGIN/../dependencies/guacamole-server-install/lib

LDFLAGS = -L../../libraries -l:kin.library -Wl,-rpath,\$$ORIGIN/../libraries $(GUAC_LDFLAGS) $(GUAC_RPATH) -lpthread

OBJS = service.o management.o

all: guacamole-server-build guacamole.service

guacamole-server-build:
	@if [ ! -f "$(GUAC_PREFIX)/lib/libguac.so" ]; then \
		if [ ! -d "$(GUAC_SRC)" ]; then \
			echo "Fetching guacamole-server source..."; \
			git clone --depth 1 https://github.com/apache/guacamole-server.git $(GUAC_SRC); \
		fi; \
		echo "Configuring guacamole-server..."; \
		cd $(GUAC_SRC) && autoreconf -fi && ./configure --prefix=$(abspath $(GUAC_PREFIX)) --disable-guacd --disable-guacenc --disable-guaclog --disable-kubernetes; \
		echo "Building guacamole-server..."; \
		$(MAKE) -C $(GUAC_SRC) -j$$(nproc); \
		$(MAKE) -C $(GUAC_SRC) install; \
	fi

guacamole.service: $(OBJS)
	$(CC) $(CFLAGS) $(GUAC_CFLAGS) -o $@ $^ $(LDFLAGS)

service.o: service.c management.h
	$(CC) $(CFLAGS) $(GUAC_CFLAGS) -c $< -o $@

management.o: management.c management.h
	$(CC) $(CFLAGS) $(GUAC_CFLAGS) -c $< -o $@

clean:
	rm -f *.o guacamole.service

distclean: clean
	rm -rf $(GUAC_PREFIX)

.PHONY: all clean distclean guacamole-server-build
