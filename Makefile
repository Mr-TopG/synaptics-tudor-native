CC ?= cc
CPPFLAGS += -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -pthread -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wformat=2
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
PKG_CONFIG ?= pkg-config
PYTHON ?= python3
CRYPTO_CFLAGS = $(shell $(PKG_CONFIG) --cflags libcrypto)
CRYPTO_LIBS = $(shell $(PKG_CONFIG) --libs libcrypto)

.PHONY: all check pairing session capture check-session check-pairing check-interop install uninstall clean dist
all: build/tudor-native

build:
	mkdir -p build

build/tudor-native: src/probe.c src/protocol.c src/protocol.h src/usb.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/probe.c src/protocol.c

build/test-protocol: tests/test_protocol.c src/protocol.c src/protocol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_protocol.c src/protocol.c

build/test-probe: tests/test_probe.c src/probe.c src/protocol.c src/protocol.h src/usb.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_probe.c src/protocol.c

check: build/test-protocol build/test-probe
	./build/test-protocol
	./build/test-probe

pairing: build/tudor-pair

session: build/tudor-session

capture: build/tudor-capture

.PHONY: analyze check-analyze check-preview check-processing check-enrollment
analyze: build/tudor-analyze

build/tudor-analyze: src/analyze.c src/protocol.c src/protocol.h src/frame_decode.c src/frame_decode.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/analyze.c src/protocol.c src/frame_decode.c -lm

check-analyze: build/tudor-analyze
	$(PYTHON) tests/test_analyze.py ./build/tudor-analyze

check-preview: build/tudor-analyze
	$(PYTHON) tests/test_preview.py ./build/tudor-analyze

check-processing:
	$(PYTHON) tests/test_image_processing.py

check-enrollment:
	$(PYTHON) tests/test_enrollment_lab.py
	$(PYTHON) tests/test_enrollment_collection.py
	$(PYTHON) tests/test_native_probe_cli.py
	$(PYTHON) tests/test_fingerprint_round.py

.PHONY: image-score check-image-score check-image-score-reference
image-score: build/libtudor-image-score.so

.PHONY: verification check-verification
verification: build/libtudor-verification.so

VERIFICATION_SRC = src/verification.c src/image_quality.c src/image_bank.c src/image_score.c
VERIFICATION_HDR = src/verification.h src/image_quality.h src/image_bank.h src/image_score.h

build/libtudor-verification.so: $(VERIFICATION_SRC) $(VERIFICATION_HDR) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ $(VERIFICATION_SRC) -lm

build/test-verification: tests/test_verification.c $(VERIFICATION_SRC) $(VERIFICATION_HDR) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_verification.c $(VERIFICATION_SRC) -lm

check-verification: build/test-verification build/libtudor-verification.so
	./build/test-verification
	$(PYTHON) tests/test_verification_cli.py

build/libtudor-image-score.so: src/image_score.c src/image_score.h src/image_bank.c src/image_bank.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ src/image_score.c src/image_bank.c -lm

build/test-image-score: tests/test_image_score.c src/image_score.c src/image_score.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_image_score.c src/image_score.c -lm

build/test-image-bank: tests/test_image_bank.c src/image_bank.c src/image_bank.h src/image_score.c src/image_score.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_image_bank.c src/image_bank.c src/image_score.c -lm

build/test-image-self-check: tests/test_image_self_check.c src/image_score.c src/image_score.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_image_self_check.c src/image_score.c -lm

build/test-image-parallel: tests/test_image_parallel.c src/image_bank.c src/image_bank.h src/image_score.c src/image_score.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTUDOR_BANK_TESTING $(LDFLAGS) -o $@ tests/test_image_parallel.c src/image_bank.c src/image_score.c -lm

check-image-score: build/test-image-score build/test-image-bank build/test-image-self-check build/test-image-parallel
	./build/test-image-score
	./build/test-image-bank
	./build/test-image-self-check
	./build/test-image-parallel

check-image-score-reference: build/libtudor-image-score.so
	$(PYTHON) tests/test_native_image_score.py ./build/libtudor-image-score.so

build/capture-bridge: tests/session_bridge.c src/capture.c src/capture_core.c src/capture_core.h src/tls.c src/tls.h src/state.c src/state.h src/certificate.c src/certificate.h src/protocol.c src/protocol.h src/usb.h src/frame_decode.c src/frame_decode.h src/image_quality.c src/image_quality.h src/image_score.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) -DTUDOR_CAPTURE_TEST $(LDFLAGS) -o $@ tests/session_bridge.c src/capture_core.c src/frame_decode.c src/image_quality.c src/tls.c src/state.c src/certificate.c src/protocol.c $(CRYPTO_LIBS) -lm

.PHONY: check-capture
check-capture: build/capture-bridge
	$(PYTHON) tests/test_capture.py ./build/capture-bridge

build/tudor-capture: src/capture.c src/capture_core.c src/capture_core.h src/tls.c src/tls.h src/state.c src/state.h src/certificate.c src/certificate.h src/probe.c src/protocol.c src/protocol.h src/usb.h src/frame_decode.c src/frame_decode.h src/image_quality.c src/image_quality.h src/image_score.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) -DTUDOR_NO_MAIN $(LDFLAGS) -o $@ src/capture.c src/capture_core.c src/frame_decode.c src/image_quality.c src/tls.c src/state.c src/certificate.c src/probe.c src/protocol.c $(CRYPTO_LIBS) -lm

.PHONY: capture-core check-capture-core
capture-core: build/libtudor-capture.so

CAPTURE_CORE_SRC = src/capture_core.c src/frame_decode.c src/tls.c src/state.c src/certificate.c src/protocol.c
CAPTURE_CORE_HDR = src/capture_core.h src/frame_decode.h src/tls.h src/state.h src/certificate.h src/protocol.h

build/capture-core-bridge: tests/capture_core_bridge.c $(CAPTURE_CORE_SRC) $(CAPTURE_CORE_HDR) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) $(LDFLAGS) -o $@ tests/capture_core_bridge.c $(CAPTURE_CORE_SRC) $(CRYPTO_LIBS)

build/test-capture-api: tests/test_capture_api.c $(CAPTURE_CORE_SRC) $(CAPTURE_CORE_HDR) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) $(LDFLAGS) -o $@ tests/test_capture_api.c $(CAPTURE_CORE_SRC) $(CRYPTO_LIBS)

check-capture-core: build/capture-core-bridge build/test-capture-api
	./build/test-capture-api
	$(PYTHON) tests/test_capture_core.py ./build/capture-core-bridge

.PHONY: check-contact
check-contact: build/capture-core-bridge
	$(PYTHON) tests/test_contact.py ./build/capture-core-bridge

.PHONY: check-fprintd
# Requires the separate --service-test build and installed fprintd/dbus/Python bindings.
# Uses generated images, a private bus and a temporary store; no sensor access.
check-fprintd:
	/usr/bin/python3 tests/test_fprintd_service.py

.PHONY: service-bundle check-service-package
service-bundle:
	$(PYTHON) scripts/build-service-bundle.py

check-service-package:
	$(PYTHON) tests/test_service_package.py

build/libtudor-capture.so: $(CAPTURE_CORE_SRC) $(CAPTURE_CORE_HDR) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ $(CAPTURE_CORE_SRC) $(CRYPTO_LIBS)

build/tudor-session: src/session.c src/tls.c src/tls.h src/state.c src/state.h src/certificate.c src/certificate.h src/probe.c src/protocol.c src/protocol.h src/usb.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) -DTUDOR_NO_MAIN $(LDFLAGS) -o $@ src/session.c src/tls.c src/state.c src/certificate.c src/probe.c src/protocol.c $(CRYPTO_LIBS)

build/session-bridge: tests/session_bridge.c src/session.c src/tls.c src/tls.h src/state.c src/state.h src/certificate.c src/certificate.h src/protocol.c src/protocol.h src/usb.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) $(LDFLAGS) -o $@ tests/session_bridge.c src/tls.c src/state.c src/certificate.c src/protocol.c $(CRYPTO_LIBS)

build/test-tls: tests/test_tls.c src/tls.c src/tls.h src/certificate.c src/certificate.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) $(LDFLAGS) -o $@ tests/test_tls.c src/tls.c src/certificate.c $(CRYPTO_LIBS)

check-session: build/session-bridge build/test-tls
	./build/test-tls
	$(PYTHON) tests/test_session.py ./build/session-bridge

build/tudor-pair: src/pair.c src/certificate.c src/certificate.h src/probe.c src/protocol.c src/protocol.h src/usb.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) -DTUDOR_NO_MAIN $(LDFLAGS) -o $@ src/pair.c src/certificate.c src/probe.c src/protocol.c $(CRYPTO_LIBS)

build/test-certificate: tests/test_certificate.c src/certificate.c src/certificate.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) $(LDFLAGS) -o $@ tests/test_certificate.c src/certificate.c $(CRYPTO_LIBS)

build/test-pair: tests/test_pair.c src/pair.c src/certificate.c src/certificate.h src/protocol.c src/protocol.h src/usb.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) $(LDFLAGS) -o $@ tests/test_pair.c src/certificate.c src/protocol.c $(CRYPTO_LIBS)

check-pairing: build/test-certificate build/test-pair
	./build/test-certificate
	./build/test-pair

build/certificate-interop: tests/certificate_interop.c src/certificate.c src/certificate.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CRYPTO_CFLAGS) $(LDFLAGS) -o $@ tests/certificate_interop.c src/certificate.c $(CRYPTO_LIBS)

check-interop: build/certificate-interop
	$(PYTHON) tests/test_interop.py ./build/certificate-interop

install: build/tudor-native
	install -Dm755 build/tudor-native "$(DESTDIR)$(BINDIR)/tudor-native"
	@echo 'Installed diagnostic probe only; fingerprint login is not enabled.'

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/tudor-native"

dist:
	sh scripts/dist.sh

clean:
	rm -rf build
