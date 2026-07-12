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

## Set up a project

- Prepare Makefile and Dockerfile: See [references/setup.md](references/setup.md)

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

## Building Process

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
# output: output/<appName>.eap
```

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

Every manifest needs at least `schemaVersion`, and under `acapPackageConf.setup`: `appName`
(must match the binary and source file names), `vendor`, `version`, `runMode`, and
`compatibleOsVersions`. Add per-API `resources` as listed in the table below.

### Available APIs

See the following references (Usage, API specifications, Examples) when using the respective
APIs below. The last column lists what each API requires in `manifest.json` — omitting it is a
common reason an app fails to start.

| API | Purpose | Reference | manifest.json requirement |
|---|---|---|---|
| VDO (Video Capture) | Capture frames (H.264/H.265/AV1/JPEG/YUV/RGB) | `references/vdo.md` | `linux.user.groups: ["video"]` |
| Larod | Edge inference & image preprocessing | `references/larod.md` | `video` group + `deepLearningProcessor` (DLPU chips) |
| Event (Axevent) | Event handling (stateless/stateful) | `references/event.md` | none |
| Axoverlay 2 | Overlay custom graphics onto streams | `references/overlay.md` | `resources.overlay {enabled, required}` |
| Bounding Box | Fast boxes/polylines for analytics results | `references/bbox.md` | `dbus.requiredMethods` (Graphics2/Overlay2) + `video` group |
| Parameter (AXParameter) | Parameter management + change callbacks | `references/parameter.md` | `configuration.paramConfig[]` |
| Edge Storage (AXStorage) | Read/Write to SD card / NAS | `references/storage.md` | `linux.user.groups: ["storage"]` |
| Serial Port | RS-232/422/485 | `references/serial.md` | `linux.user.groups: ["admin"]` |
| Device Data Hub | Pub/Sub messaging between apps | `references/ddh.md` | `resources.deviceDataHub {enabled, accessControlList}` |
| License Key | Copy protection | `references/key.md` | `copyProtection.method: "axis"` + `appId` |

> The full list of platform APIs (recording search/playback, PTZ, message broker, HIDRAW,
> VAPIX access, bundled open-source libs) is at
> https://developer.axis.com/acap/reference/supported-apis/


## Deploy apps

Use digest auth (`--anyauth` / `--digest`); `appName` below is the value from `manifest.json`.

```bash
# Install (field name is packfile)
curl --anyauth -u <user>:<password> \
  -F packfile=@output/<appName>.eap \
  "http://<device-ip>/axis-cgi/applications/upload.cgi"

# Start / stop / restart / remove: action=start|stop|restart|remove
curl --anyauth -u <user>:<password> \
  "http://<device-ip>/axis-cgi/applications/control.cgi?action=start&package=<appName>"
```

## View application logs

### via HTTP
```bash
curl --anyauth -u <user>:<password> \
  "http://<device-ip>/axis-cgi/admin/systemlog.cgi?appname=<appName>"
```

### via SSH (once SSH is enabled on the device)
```bash
# ssh user@host, then follow the app's journal by syslog identifier (appName)
ssh <ssh-user>@<device-ip> "journalctl -t <appName> -f"
```


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
