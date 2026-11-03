CC = cc
AR = ar
CPPFLAGS = -Iinclude
CFLAGS = -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror
LDFLAGS =
LDLIBS =
PREFIX = /usr/local
BACKEND ?= auto
BACKING ?= auto
REMAP ?= auto
ALLOW_DISK ?= 0
MIN_COPY_PAGES ?= 4
MUTABLE_SNAPSHOTS ?= 1
SIGNAL_MODE ?= auto
RUN_PROBES ?= 1
BUILD_CPPFLAGS = $(CPPFLAGS) -Ibuild -DCOWCPY_HAVE_CONFIG
DETECTED_LIBS = $(shell cat build/cowcpy.libs 2>/dev/null)
SHARED_LDFLAGS ?= $(shell cat build/cowcpy.sharedflags 2>/dev/null)
LIBS = -pthread $(DETECTED_LIBS) $(LDLIBS)
SOURCES = src/cowcpy.c src/signal.c
HEADERS = include/cowcpy.h src/internal.h

.PHONY: all check clean install FORCE
all: build/libcowcpy.a build/libcowcpy.so build/example

build:
	mkdir -p build

build/cowcpy_config.h: tests/configure.sh tests/probe_publication.c tests/probe_mapped.c $(SOURCES) $(HEADERS) FORCE | build
	CC='$(CC)' CPPFLAGS='$(CPPFLAGS)' CFLAGS='$(CFLAGS)' LDFLAGS='$(LDFLAGS)' sh tests/configure.sh $@ '$(BACKEND)' '$(BACKING)' '$(REMAP)' '$(ALLOW_DISK)' '$(RUN_PROBES)' '$(SIGNAL_MODE)' '$(MIN_COPY_PAGES)' '$(MUTABLE_SNAPSHOTS)'

build/cowcpy.o: src/cowcpy.c $(HEADERS) build/cowcpy_config.h | build
	$(CC) $(BUILD_CPPFLAGS) $(CFLAGS) -pthread -fPIC -c $< -o $@

build/signal.o: src/signal.c $(HEADERS) build/cowcpy_config.h | build
	$(CC) $(BUILD_CPPFLAGS) $(CFLAGS) -pthread -fPIC -c $< -o $@

build/libcowcpy.a: build/cowcpy.o build/signal.o
	$(AR) rcs $@ $^

build/libcowcpy.so: build/cowcpy.o build/signal.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHARED_LDFLAGS) $^ $(LIBS) -o $@

build/example: examples/basic.c build/libcowcpy.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< build/libcowcpy.a $(LIBS) -o $@

build/test: tests/test_cowcpy.c build/libcowcpy.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< build/libcowcpy.a $(LIBS) -o $@

build/test-features: tests/test_features.c build/libcowcpy.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< build/libcowcpy.a $(LIBS) -o $@

build/test-threads: tests/test_threads.c build/libcowcpy.a
	$(CC) $(BUILD_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< build/libcowcpy.a $(LIBS) -o $@

build/test-signal: tests/test_cowcpy.c $(SOURCES) $(HEADERS) build/cowcpy_config.h
	$(CC) $(BUILD_CPPFLAGS) $(CFLAGS) -DCOWCPY_FORCE_SIGNAL $(SOURCES) $< $(LDFLAGS) $(LIBS) -o $@

build/test-threads-alias: tests/test_threads.c $(SOURCES) $(HEADERS) build/cowcpy_config.h
	$(CC) $(BUILD_CPPFLAGS) $(CFLAGS) -DCOWCPY_FORCE_SIGNAL -DCOWCPY_SIGNAL_DIRECT=0 -DCOWCPY_SIGNAL_LAZY=1 $(SOURCES) $< $(LDFLAGS) $(LIBS) -o $@

build/test-features-signal: tests/test_features.c $(SOURCES) $(HEADERS) build/cowcpy_config.h
	$(CC) $(BUILD_CPPFLAGS) $(CFLAGS) -DCOWCPY_FORCE_SIGNAL $(SOURCES) $< $(LDFLAGS) $(LIBS) -o $@

build/test-vm-failures: tests/test_vm_failures.c $(SOURCES) $(HEADERS) build/cowcpy_config.h
	$(CC) $(BUILD_CPPFLAGS) $(CFLAGS) -DCOWCPY_TEST_VM $(SOURCES) $< $(LDFLAGS) $(LIBS) -o $@

build/test-backing-shm: tests/test_backing.c $(SOURCES) $(HEADERS) build/cowcpy_config.h
	$(CC) $(BUILD_CPPFLAGS) -Isrc $(CFLAGS) -DCOWCPY_TEST_BACKING -DCOW_BACKING_REQUEST=2 -DTEST_SHM=1 $(SOURCES) $< $(LDFLAGS) $(LIBS) -o $@

build/test-backing-tmp: tests/test_backing.c $(SOURCES) $(HEADERS) build/cowcpy_config.h
	$(CC) $(BUILD_CPPFLAGS) -Isrc $(CFLAGS) -DCOWCPY_TEST_BACKING -DCOW_BACKING_REQUEST=3 -DTEST_SHM=0 $(SOURCES) $< $(LDFLAGS) $(LIBS) -o $@

build/test-publication: tests/probe_publication.c $(SOURCES) $(HEADERS) build/cowcpy_config.h
	$(CC) $(BUILD_CPPFLAGS) $(CFLAGS) -DCOWCPY_FORCE_SIGNAL -DCOWCPY_SIGNAL_DIRECT=0 -DCOWCPY_SIGNAL_LAZY=1 -DCOWCPY_TEST_HOOKS $(SOURCES) $< $(LDFLAGS) $(LIBS) -o $@

check: build/test build/test-features build/test-threads build/test-signal build/test-features-signal build/test-threads-alias build/test-publication build/test-vm-failures build/test-backing-shm build/test-backing-tmp
	./build/test
	./build/test-features
	./build/test-threads
	./build/test-signal
	./build/test-features-signal
	./build/test-threads-alias
	./build/test-publication || { status=$$?; test $$status -eq 77; }
	./build/test-vm-failures
	./build/test-backing-shm || { status=$$?; test $$status -eq 77; }
	./build/test-backing-tmp || { status=$$?; test $$status -eq 77; }

install: build/libcowcpy.a build/libcowcpy.so
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/cowcpy.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 build/libcowcpy.a build/libcowcpy.so $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf build
