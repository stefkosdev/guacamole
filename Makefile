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

# Full live path through Kin (http.service -> polykernel -> guacamole.service).
# Needs a running Kin; set KIN_SESSION=<kin_session cookie> for the HTTP layer.
integration:
	./scripts/integration-test-kin.sh

# Live viewer/video path (WS tunnel -> guacamole.service -> Docker VNC).
# Needs a running Kin, docker, and KIN_SESSION=<kin_session cookie>.
tunnel-test:
	./scripts/tunnel-test-kin.sh

# Real in-browser render: headless Chromium drives the deployed guac-viewer.js
# against the live tunnel and asserts the display paints.
# Needs a running Kin, docker, chromium, node, and KIN_SESSION=<kin_session cookie>.
# Run scripts/install-test-deps.sh once first.
browser-test:
	./scripts/browser-test-kin.sh

# Install the system libraries needed to BUILD guacamole (libguac + plugins).
install-build-deps:
	./scripts/install-build-deps.sh

# Install every test dependency (gcc/make, docker, chromium, node) + puppeteer-core.
install-deps:
	./scripts/install-test-deps.sh

# Full end-to-end test: a persisted connection drives a real VNC session
# (service under a minimal Kin manager stub; needs Docker + a built service).
e2e:
	./scripts/e2e-vnc.sh

# Bring a real VNC target up / down for end-to-end testing (needs Docker).
vnc-up:
	./scripts/setup-vnc-test-env.sh

vnc-down:
	./scripts/teardown-vnc-test-env.sh

clean:
	$(MAKE) -C $(SERVICE_DIR) clean
	$(MAKE) -C $(SERVICE_DIR)/tests clean

.PHONY: all service build-apps deb test test-unit integration tunnel-test browser-test install-build-deps install-deps e2e vnc-up vnc-down clean
