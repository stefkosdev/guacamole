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
GUAC_RPATH = -Wl,-rpath,\$$ORIGIN/../../guacamole/lib

LDFLAGS = -L../../libraries -l:kin.library -Wl,-rpath,\$$ORIGIN/../../libraries $(GUAC_LDFLAGS) $(GUAC_RPATH) -lpthread -lcrypto

OBJS = service.o management.o tunnel.o

all: guacamole.service

# === libguac: main library (static) + protocol plugins (dynamic) ===
#
# This target downloads the Apache guacamole-server source (if not present),
# then builds libguac (the core library) as a static archive and each
# protocol plugin (libguac-client-*) as a shared object.
#
# After the build, libguac.so* is removed so only libguac.a remains
# for static linking into the service. The client .a files are removed
# so only .so files remain for runtime loading via guac_client_load_plugin().

guacamole-libguac-build:
	@set -e; if [ ! -d "$(GUAC_SRC)" ]; then \
		echo "Fetching guacamole-server source..."; \
		git clone --depth 1 https://github.com/apache/guacamole-server.git $(GUAC_SRC); \
	fi
	@set -e; if [ ! -f "$(GUAC_PREFIX)/lib/libguac.a" ]; then \
		if [ ! -f "$(GUAC_SRC)/config.status" ]; then \
			echo "Configuring guacamole-server..."; \
			cd $(GUAC_SRC) && autoreconf -fi && ./configure \
				--prefix=$(abspath $(GUAC_PREFIX)) \
				--with-freerdp-plugin-dir=$(abspath $(GUAC_PREFIX))/lib/freerdp \
				--enable-static --enable-shared \
				--disable-guacd --disable-guacenc --disable-guaclog; \
		fi; \
		echo "Building libguac (static) and protocol plugins (dynamic)..."; \
		$(MAKE) -C $(GUAC_SRC) -j$$(nproc); \
		$(MAKE) -C $(GUAC_SRC) install; \
		echo "Cleaning up: removing shared libguac (keeping static) and static client libs (keeping shared)..."; \
		rm -f $(GUAC_PREFIX)/lib/libguac.so $(GUAC_PREFIX)/lib/libguac.so.* 2>/dev/null || true; \
		for f in $(GUAC_PREFIX)/lib/libguac-terminal.so $(GUAC_PREFIX)/lib/libguac-terminal.so.*; do \
			[ -f "$$f" ] && rm -f "$$f" 2>/dev/null || true; \
		done; \
		for f in $(GUAC_PREFIX)/lib/libguac-client-*.a; do \
			[ -f "$$f" ] && rm -f "$$f" 2>/dev/null || true; \
		done; \
		echo "Done: libguac.a (static) + client plugins (shared) installed to $(GUAC_PREFIX)/lib/"; \
		ls -1 $(GUAC_PREFIX)/lib/libguac* 2>/dev/null || true; \
	fi

$(OBJS): | guacamole-libguac-build

guacamole.service: $(OBJS)
	$(CC) $(CFLAGS) $(GUAC_CFLAGS) -o $@ $^ $(LDFLAGS)

service.o: service.c management.h
	$(CC) $(CFLAGS) $(GUAC_CFLAGS) -c $< -o $@

management.o: management.c management.h
	$(CC) $(CFLAGS) $(GUAC_CFLAGS) -c $< -o $@

tunnel.o: tunnel.c tunnel.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o guacamole.service

distclean: clean
	rm -rf $(GUAC_PREFIX)

.PHONY: all clean distclean guacamole-libguac-build
