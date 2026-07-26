---
name: building-acap
description: >
  Develop ACAP (Axis Camera Application Platform) applications using the ACAP Native SDK. Use when implementing, testing, and reviewing applications running on Axis devices, and when helping developers understand ACAP development.
---

# Building ACAP apps

## Overview

Creating design docs with developers, planning the implementation, implementing, testing, and reviewing features, and deploying applications.

### Reference Documents
- [ACAP Native SDK Manuals](https://developer.axis.com/acap/)
- [ACAP Native SDK Examples](https://github.com/AxisCommunications/acap-native-sdk-examples/tree/main)

## When to use

- Setup a project, including preparations of manifest.json, Dockerfile, and so on.
- Building applications
  - Using ACAP SDK libraries (Edge Storage, Event, License Key, Overlay (Axoverlay2), Bounding Box, Parameter, Serial, Video Capture (VDO), Machine Learning (Larod), Device Data Hub)
- Deploying applications

## Workflow: skeleton first, then features

Before any feature code runs on the device, it has to survive a long, failure-prone toolchain: Docker cross-compilation → `.eap` packaging → upload/install on the device → start → run over SSH. Each link breaks for its own environmental reasons — wrong `ARCH`, a missing `manifest.json` user group, SSH or digest auth not configured, an incompatible OS version.
If you write the feature first and it won't install or start, you cannot tell whether the fault is in your code or in the environment, and you end up debugging both at once.

So build in two phases, and don't merge them:

**Phase 1 — Set up the environment with a walking skeleton.** Stand up a minimal app that only
logs `"Hello World"` and drive it through the pipeline: build → package `.eap` → install → start
→ confirm the log shows `"Hello World"`. The point isn't just a smoke test: installing the
skeleton registers the app on the device, which is what lets you run test binaries from the
installed package over SSH (`run.sh`) during Phase 2. So this step both proves the environment end-to-end and makes it
ready for iterative development. Follow [Set up a project](#set-up-a-project) and check every box
in [Setup Verification](#setup-verification).

**Phase 2 — Feature implementation.** Only once the skeleton is verified, implement features
incrementally against a known-good environment. Follow [Building Process](#building-process).

The most common mistake is skipping Phase 1 and starting on the feature directly. Even when the
user asks straight for a feature, if the project has no verified skeleton yet, do Phase 1 first —
it takes minutes and saves you from attributing an environment failure to your feature code.
The one time it's fine to skip is when a skeleton has already been installed and verified on the
device in this project; then go straight to Phase 2.

## Available scripts
- **`scripts/deploy.sh`** -- Deploy the app (eap file) to the device
  - Arguments:
    - A eap filename (.eap)
- **`scripts/control.sh`** -- Start, Stop, Restart, or Remove the app
  - Arguments:
    - `appName`
    - `start|stop|restart|remove`
- **`scripts/run.sh`** -- Run a test binary that's packaged into the installed app, over SSH; the output is saved to the `output` file in text or binary dependent on it. Build and install the eap first (`make build` + `deploy.sh`); this script only runs.
  - Arguments:
    - `appName` (the installed app that packages the test binary)
    - the test binary name (inside the installed package)
    - **-a "args"**: Optional. for passing additional arguments to the test binary
- **`scripts/view_log.sh`** -- View the log by the app 
  - Arguments:
    - `appName`

## Set up a project

**Phase 1 — the walking skeleton.** Complete this and pass Setup Verification before writing any
feature code (see [Workflow](#workflow-skeleton-first-then-features)).

- Ask the user for the target **ACAP SDK version** (e.g. `12.11.0`). It feeds two places: the
  build `VERSION` (Makefile/Dockerfile) and the manifest `compatibleOsVersions` (its major).
- Generate a minimal manifest.json unless the manifest file exists by asking the user for `appName` and ACAP libraries to be used among vdo, storage, and overlay. Use the known-good minimal template in [Manifest file](#manifest-file-manifestjson) (`schemaVersion` 2.1.0, `vendorId`, and `compatibleOsVersions` with `min`/`max` set to the SDK major) so the first build isn't blocked by manifest defaults.
- Generate Makefile and Dockerfile with `VERSION` set to the SDK version the user gave: See [references/setup.md](references/setup.md)
- Build the app that just says 'Hello World' via syslog, producing the `.eap`.
- Install the eap with `deploy.sh`. **Installing the app creates a dedicated SSH user for it on
  the device** — this is exactly why the skeleton has to be installed before any SSH testing is
  possible.
- Start the app and confirm the log shows 'Hello World' using the `view_log.sh` script. This goes
  over HTTP and needs no SSH yet, so it verifies the build/install/start path on its own.
  - If it fails, tell the user to look at `README` and check device access. If the issue is not
    device access, investigate what is wrong or missing based on the messages.
- Now hand the SSH setup to the user and wait for them before going further — this step is theirs,
  not yours:
  - Ask the user to confirm on the device that the app's SSH user was created, and to **set a
    password** for that user via the device UI.
  - Ask them to put the resulting SSH credentials into `.env` themselves. DO NOT read or write
    `.env` — just let the user do it.
  - Do not continue until the user reports this is done.
- Once the user confirms the SSH user exists and its password is set, verify SSH execution: run the
  installed binary over SSH with `run.sh <appName> <appName>` and check the output is 'Hello
  World'. When this passes, the project environment is ready for development and testing.

### Project Structure

```
project-root/
├── Dockerfile
├── Makefile          <- For build
└── app/
    ├── manifest.json
    ├── Makefile      <- For compiling C/C++ applications
    ├── LICENSE       <- License information including third party libraries
    ├── <appName>.c   <- main: appName must match with manifest.json
    └── <feature>.c   <- Individual modules/features
```

### Setup Verification

This is the gate between Phase 1 and Phase 2. Do not begin feature implementation until every box
is checked — an unchecked box means the environment, not your future feature, is the unknown.

- [ ] EAP file (.eap) generated by building the app successfully
- [ ] Install the app successfully
- [ ] Start the app successfully and check if the app output "Hello World"
- [ ] Run the app's binary from the installed package over SSH (`run.sh <appName> <appName>`) and check it outputs "Hello World"

## Building Process

**Phase 2 — feature implementation.** Only reach here once the walking skeleton has passed
[Setup Verification](#setup-verification) on the device.

### Writing application overview

- Writing source codes
- Writing Makefile
- Writing manifest.json
- Preparing `LICENSE` file

### Build command

Make sure the following points
* `appName` matches the value in `manifest.json`, the source file name (.c), and the binary name.
* Certain APIs (such as Larod and Overlay) require the corresponding resources declarations in `manifest.json`; otherwise, the application will fail to start.

```bash
make build               # aarch64
make ARCH=armv7hf build  # armv7hf
# build: build/<appName>.eap
```

### Unit / function testing

Test binaries the same way the app itself runs: build them into the `.eap` alongside the app,
install, and run them on the device from the installed location — not as loose executables copied
to `/tmp`. Exercising the actual installed artifact means you test it in the real deployed layout
on the device, with the same packaging and paths the app uses, rather than a one-off copy.

Flow: add the test binary to the app so `acap-build` packages it into the eap → `make build` →
`scripts/deploy.sh` to install → `scripts/run.sh <appName> <test-binary>` runs it over SSH and
captures the result in `output`.

### Makefile (app/Makefile)

```makefile
PROG = $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
OBJS = $(PROG).c
DEBUG_DIR = debug

CFLAGS += -Wall -Wextra -Wformat=2 -Wpointer-arith \
          -Wbad-function-cast -Wstrict-prototypes -Wmissing-prototypes -Werror

all: $(PROG)

$(PROG): $(OBJS)
        install -d $(DEBUG_DIR)
        $(CC) $^ $(CFLAGS) $(LIBS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
        cp $(DEBUG_DIR)/$@ .
        $(STRIP) $@

clean:
        rm -rf $(PROG) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
```
Extract `appName` from `manifest.json` using `jq`

### Manifest file (manifest.json)

- [Manifest file — getting started](https://developer.axis.com/acap/get-started/create-your-first-acap-application/#manifest-file)
- [Manifest specification (full schema)](https://developer.axis.com/acap/reference/manifest-schemas/general-info/)

Start from this minimal, known-good manifest and change only `appName`, `vendor`, and `vendorId`.
Getting these defaults right up front avoids the two most common time sinks — an outdated
`schemaVersion` and a `compatibleOsVersions` that is missing its `min`/`max`, both of which cause
confusing install/build failures:

```json
{
  "schemaVersion": "2.1.0",
  "acapPackageConf": {
    "setup": {
      "appName": "hello_world",
      "vendor": "My Company",
      "vendorId": "0123456ABC",   # A 10-digit hexadecimal number
      "version": "1.0.0",
      "runMode": "once",
      "compatibleOsVersions": [ { "min": "12", "max": "12" } ]  # list + string
    }
  }
}
```

Field notes:
- **`schemaVersion`** — use `2.1.0`. Older values like `1.3` are outdated and behave differently.
- **`appName`** — must match the binary and the `.c` source file names exactly.
- **`vendorId`** — the integer ID Axis assigns to a registered vendor; `0` is fine as a
  placeholder for local development, set your real ID before distribution.
- **`compatibleOsVersions`** — always an object with both `min` and `max`, set to the **SDK major
  version** you asked the user for at setup: e.g. SDK `12.11.x` → `min` and `max` both `12`.
  Omitting either, or a mismatched range, is a frequent cause of install failures. Keep this major
  in sync with the SDK version used for the build (Makefile/Dockerfile `VERSION`).
- **`runMode`** — `once` runs the app a single time when started, which suits a Hello World
  skeleton. Other options: `respawn` (kept running) or `never` (not started automatically).

Per-API declarations go under a **top-level `resources` key** — a sibling of `acapPackageConf`, not nested inside it. Group settings live at `resources.linux.user.groups` (not a top-level `linux.user.groups`), D-Bus methods at `resources.dbus.requiredMethods`, and so on. The table below lists each API's requirement using that full path. Placement relative to `acapPackageConf`:

```json
{
  "schemaVersion": "2.1.0",
  "resources": {
    "linux": { "user": { "groups": ["video"] } }
  },
  "acapPackageConf": {
    "setup": { "appName": "hello_world" }
  }
}
```

### Available APIs

See the following references (Usage, API specifications, Examples) when using the respective
APIs below. The last column lists what each API requires in `manifest.json` — omitting it is a common reason an app fails to start.

| API | Purpose | Reference | manifest.json requirement |
|---|---|---|---|
| VDO (Video Capture) | Capture frames (H.264/H.265/AV1/JPEG/YUV/RGB) | `references/vdo.md` | `resources.linux.user.groups: ["video"]` |
| Larod | Edge inference & image preprocessing | `references/larod.md` | `resources.linux.user.groups: ["video"]` + `resources.deepLearningProcessor` (DLPU chips) |
| Event (Axevent) | Event handling (stateless/stateful) | `references/event.md` | none |
| Axoverlay 2 | Overlay custom graphics onto streams | `references/overlay.md` | `resources.overlay {enabled, required}` |
| Bounding Box | Fast boxes/polylines for analytics results | `references/bbox.md` | `resources.dbus.requiredMethods` (Graphics2/Overlay2) + `resources.linux.user.groups: ["video"]` |
| Parameter (AXParameter) | Parameter management + change callbacks | `references/parameter.md` | `acapPackageConf.configuration.paramConfig[]` |
| Edge Storage (AXStorage) | Read/Write to SD card / NAS | `references/storage.md` | `resources.linux.user.groups: ["storage"]` |
| Serial Port | RS-232/422/485 | `references/serial.md` | `resources.linux.user.groups: ["admin"]` |
| Device Data Hub | Pub/Sub messaging between apps | `references/ddh.md` | `resources.deviceDataHub {enabled, accessControlList}` |
| License Key | Copy protection | `references/key.md` | `acapPackageConf.setup.copyProtection.method: "axis"` + `acapPackageConf.setup.appId` |

> The full list of platform APIs (recording search/playback, PTZ, message broker, HIDRAW,
> VAPIX access, bundled open-source libs) is at
> https://developer.axis.com/acap/reference/supported-apis/

## Deploy apps

Use `scripts/deploy.sh` script.

### Control apps (start, stop, restart, or remove)

Use `scripts/control.sh` script.

## View application logs

Use `scripts/view_log.sh` script.

## Implementation Rules

### Rule 1: Clearly Structured

Organize the source code into modules by feature. Group related functionality together, and avoid mixing unrelated features in the same file.

### Rule 2: Safe Defaults

New code should default to safe, conservative behavior:

### Rule 3: Rollback-Friendly

Each increment should be independently revertable:

- Additive changes (new files, new functions) are easy to revert
- Modifications to existing code should be minimal and focused
- Changes to `manifest.json` resource declarations should be introduced together with the code that uses them
- Avoid deleting something in one commit and replacing it in the same commit — separate them

## Red Flags

- Writing feature code before a "Hello World" skeleton has been installed, started, and verified on the device (Phase 1 / Setup Verification skipped)
- Read .env or .env.* files
- Resolve authentication issue when accessing devices. (Just let the user to resolve it)
- Deploy applications unless explicitly instructed to do so
- More than 300 lines of code written without running tests
- Multiple unrelated changes in a single increment
- "Let me just quickly add this too" scope expansion
- Skipping the test/verify step to move faster
- Build or tests broken between increments
- Large uncommitted changes accumulating
- Building abstractions before the third use case demands it
- Touching files outside the task scope "while I'm here"
- Creating new utility files for one-time operations
- Running the same build/test command twice in a row without any intervening code change

## Verification

After completing all increments for a task:

- [ ] The build is clean
- [ ] The feature works end-to-end as specified
- [ ] No uncommitted changes remain
