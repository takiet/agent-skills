# Set up

## Contents

- [Makefile under the project root](#makefile-for-outputting-eap-files) — drives the Docker build
- [Dockerfile](#baseline-dockerfile-multi-stage) — cross-compiles and packages the `.eap`
- [Packaging extra files into the eap](#packaging-extra-files-into-the-eap) — test binaries,
  models, labels (`acap-build -a`)
- [app/Makefile](#appmakefile--compiling-the-application) — compiles the app itself (C and C++)

## Makefile for outputting eap files

`VERSION` is the ACAP SDK version you asked the user for at setup; its major must match
`compatibleOsVersions.min`/`max` in `manifest.json`.

> Recipe lines in every Makefile below must be indented with a **TAB**, not spaces. `make` reports
> a space-indented recipe as `*** missing separator.  Stop.`, which says nothing about indentation
> — so check this first if a copied Makefile refuses to run at all.

```makefile
# VERSION is the ACAP SDK version chosen at setup. Keep the comment on its own
# line: make keeps the spaces between a value and a trailing `#`, so writing
# `VERSION ?= 12.11.0   # ...` puts those spaces inside VERSION itself.
ARCH        ?= aarch64
VERSION     ?= 12.11.0
OUTPUT_DIR  := build

.PHONY: build clean

build:
	DOCKER_BUILDKIT=1 docker build \
	  --build-arg ARCH=$(ARCH) \
	  --build-arg VERSION=$(VERSION) \
	  --target export \
	  --output type=local,dest=$(OUTPUT_DIR) \
	  .

clean:
	rm -rf $(OUTPUT_DIR)
```

## Baseline Dockerfile (Multi stage)

```dockerfile
ARG ARCH=aarch64
ARG VERSION=12.11.0
ARG REPO=axisecp

FROM ${REPO}/acap-native-sdk:${VERSION}-${ARCH} AS builder
COPY app /opt/app/
WORKDIR /opt/app
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./

# Export the build output with no unnecessary layer left. This copies the whole
# app directory, so `build/` ends up holding the sources and `package.conf`
# beside the `.eap` — glob `build/*.eap` rather than assuming it is alone there.
FROM scratch AS export
COPY --from=builder /opt/app/* /
```

On an Apple Silicon host every build ends with
`InvalidBaseImagePlatform: ... was pulled with platform "linux/amd64", expected "linux/arm64"`.
The SDK image is amd64-only and runs under emulation; the cross-compiler still emits a genuine
`aarch64` binary. Expected, and there is nothing to fix.

## Packaging extra files into the eap

By default `acap-build ./` packages the application binary named in `manifest.json` and nothing
else. Anything additional — **test binaries, model files, label files, fonts** — goes in with
the repeatable **`-a`** flag. It's the single answer to "how do I get this file onto the device",
so reach for it rather than inventing a download step or copying files to `/tmp` by hand:

```dockerfile
RUN . /opt/axis/acapsdk/environment-setup* && \
    acap-build ./ -a test_capture \
                  -a models/yolov5s.tflite \
                  -a models/labels.txt
```

- `-a` may be given many times, and its argument may be a path into a subdirectory.
- Paths are relative to the app directory, and the file lands at the same relative path inside
  the installed app — so the app can open `models/labels.txt` relative to its own directory.
- `acap-build` only **packages**; it does not build. A test binary must also be produced by
  `app/Makefile`, so add it to the `all` target (see below) or it silently won't exist.

This is why test binaries are run from the installed package rather than scp'd to the device:
same packaging, same paths, same layout as the real app.

## app/Makefile — compiling the application

Extract `appName` from `manifest.json` with `jq` so the binary name can never drift from the
manifest — a mismatch there is one of the most common causes of an app that installs but won't
start.

The file holding `main()` is always **`main.c`** (or `main.cc`), never `<appName>.c`. What has to
carry the app's name is the *binary*, and `$(PROG)` supplies that at link time, so the source
filename is free to be a constant. That keeps a rename confined to `manifest.json` instead of also
touching a filename, an `#include`, and every `SRCS` line that mentions it.

### C

```makefile
PROG      = $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
SRCS      = main.c
DEBUG_DIR = debug

PKGS  = gio-2.0 gio-unix-2.0 vdostream

CFLAGS += -Wall -Wextra -Wformat=2 -Wpointer-arith \
          -Wbad-function-cast -Wstrict-prototypes -Wmissing-prototypes -Werror
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

all: $(PROG)

$(PROG): $(SRCS)
	install -d $(DEBUG_DIR)
	$(CC) $^ $(CFLAGS) $(LIBS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

clean:
	rm -rf $(PROG) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
```

### C++

The ACAP libraries are C, but the application can be C++ — and projects often specify it. Two
things change beyond `CC` → `CXX`:

- **Drop `-Wbad-function-cast`, `-Wstrict-prototypes` and `-Wmissing-prototypes`.** These are
  C-only options; a C++ compiler warns that they don't apply, and with `-Werror` that warning
  ends the build. It reads like a broken toolchain, but it's just three flags.
- **List the sources explicitly.** `SRCS = main.c` only works for a single-file app. Once the
  code is split into feature modules, and once there are test binaries linking a subset of them,
  compile to objects and link each binary from the objects it needs.

```makefile
PROG      = $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
SRCS      = main.cc capture.cc detector.cc overlay.cc
OBJS      = $(SRCS:.cc=.o)
TESTS     = test_capture test_detect
DEBUG_DIR = debug

PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream axoverlay2

# Note: no -Wbad-function-cast / -Wstrict-prototypes / -Wmissing-prototypes here.
CXXFLAGS += -Wall -Wextra -Wformat=2 -Wpointer-arith -Werror
CXXFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS   += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

# Test binaries must be built here; acap-build -a only packages what already exists.
all: $(PROG) $(TESTS)

$(PROG): $(OBJS)
	install -d $(DEBUG_DIR)
	$(CXX) $^ $(CXXFLAGS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

test_capture: test_capture.o capture.o
	$(CXX) $^ $(CXXFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

test_detect: test_detect.o detector.o
	$(CXX) $^ $(CXXFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -rf $(PROG) $(TESTS) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
```

### PKGS and pkg-config

Every API reference in this skill lists a `PKGS =` line; this is what to do with it. The
`PKG_CONFIG_PATH` must be passed through explicitly — the SDK environment sets it to the
cross-compilation sysroot, and if `make` doesn't forward it, `pkg-config` silently resolves
against the build host instead and you get header-not-found or link errors that look like a
missing SDK.

```makefile
PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream
CXXFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS   += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
```

List only what the code actually includes. A Hello World skeleton needs **no** packages at all —
syslog is in libc — so leave `PKGS` empty rather than carrying `vdostream` from the template into
an app that doesn't capture video yet, in the same spirit as not declaring unused manifest
resources. With `PKGS` empty, `pkg-config` prints `Please specify at least one package name on the
command line.` on every build; it's harmless and the build still succeeds, but wrap the calls if
you'd rather keep the log clean:

```makefile
CFLAGS += $(if $(PKGS),$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS)))
LDLIBS += $(if $(PKGS),$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS)))
```
