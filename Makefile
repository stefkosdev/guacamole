# Guacamole — Kin Remote Desktop Gateway
# Build: guacamole.service (Kin system service)

SERVICE_DIR = services/guacamole.service

all: service

service:
	$(MAKE) -C $(SERVICE_DIR)

build-apps:
	./build-apps.sh

deb:
	./make-debian.sh

clean:
	$(MAKE) -C $(SERVICE_DIR) clean

.PHONY: all service build-apps deb clean
