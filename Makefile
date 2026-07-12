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

# Run the test suite (management/persistence tests + optional socket smoke test).
test:
	./scripts/run-tests.sh

# Just the dependency-free management/persistence tests.
test-unit:
	$(MAKE) -C $(SERVICE_DIR)/tests run

# Bring a real VNC target up / down for end-to-end testing (needs Docker).
vnc-up:
	./scripts/setup-vnc-test-env.sh

vnc-down:
	./scripts/teardown-vnc-test-env.sh

clean:
	$(MAKE) -C $(SERVICE_DIR) clean
	$(MAKE) -C $(SERVICE_DIR)/tests clean

.PHONY: all service build-apps deb test test-unit vnc-up vnc-down clean
