---
name: building-acap
description: >
  Develop ACAP (Axis Camera Application Platform) applications using the ACAP Native SDK. Use when implementing, testing, and reviewing applications running on Axis devices, and when helping developers understand ACAP development.
version: 0.1.0
license: MIT
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

**Run them with `bash`, from the project root.** Both halves matter, and each fails in a way that
points somewhere else:

- **`bash <script>`, not the script directly.** They are not all marked executable, so calling one
  directly gives `permission denied` — which reads like a device problem but isn't.
- **cwd must be the project root**, since each script sources the device credentials from `./.env`
  and reads the `.eap` by relative path. `.env missing` usually means cwd drifted, not that
  anything is wrong with the device. If the file genuinely doesn't exist yet, ask the user to
  create it (see `README`) rather than creating or editing it yourself.

- **`scripts/deploy.sh`** -- Deploy the app (eap file) to the device
  - Arguments:
    - A eap filename (.eap)
- **`scripts/setup_ssh.sh`** -- Set the password of the app's dedicated SSH user, so `run.sh` can
  log in. Run once after the first install; takes no arguments.
- **`scripts/control.sh`** -- Start, Stop, Restart, or Remove the app
  - Arguments:
    - `appName`
    - `start|stop|restart|remove`
- **`scripts/run.sh`** -- Run a test binary that's packaged into the installed app, over SSH; the output is saved to the `output` file in text or binary dependent on it. Build and install the eap first (`make build` + `deploy.sh`); this script only runs.
  - Arguments:
    - `appName` (the installed app that packages the test binary)
    - the test binary name (inside the installed package)
    - **-a "args"**: Optional. for passing additional arguments to the test binary
- **`scripts/view_log.sh`** -- View the log by the executable
  - Arguments:
    - `binary-name`: appName or test binary name

### Deploy/install traps

These cost real time and none of them announce themselves clearly:

- **`deploy.sh` resets the app's log.** Always go **deploy → run → read log**. Slipping a deploy
  in between running something and reading its log throws away exactly the output you wanted.
- **Don't `control.sh remove` between iterations — install over the top.** Removing the app
  deletes its dedicated SSH user *and that user's password*, so the user has to go back into the
  device UI and set a password again before you can run anything over SSH. Overwrite-install
  keeps it. Reserve `remove` for when you genuinely want a clean slate.
- **Upload failing with `Error: 27` means the `vendorId` doesn't match** the installed app of the
  same `appName`. The device refuses the swap. Keep `vendorId` fixed for the life of the project
  — that stability is what makes overwrite-install (and therefore the surviving SSH user)
  possible.

## Set up a project

**Phase 1 — the walking skeleton.** Complete this and pass Setup Verification before writing any
feature code (see [Workflow](#workflow-skeleton-first-then-features)).

- Ask the user for the target **ACAP SDK version** (e.g. `12.11.0`). It feeds two places: the
  build `VERSION` (Makefile/Dockerfile) and the manifest `compatibleOsVersions` (its major).
- Generate a minimal manifest.json unless the manifest file exists by asking the user for `appName` and ACAP libraries to be used among vdo, storage, and overlay. Use the known-good minimal template in [Manifest file](#manifest-file-manifestjson) (`schemaVersion` 2.1.0, `vendorId`, and `compatibleOsVersions` with `min`/`max` set to the SDK major) so the first build isn't blocked by manifest defaults.
- Generate Makefile and Dockerfile with `VERSION` set to the SDK version the user gave: See [references/setup.md](references/setup.md)
- **Write `app/LICENSE`.** `acap-build` refuses to package without it — it stops with
  `Could not find a readable LICENSE file`, after the compile has already succeeded, so it reads
  like a packaging bug rather than a missing file. Any license text will do for local development.
- Build the app that just says 'Hello World', producing the `.eap`. Write the message to **both
  syslog and stdout** — the two verification steps below read different channels, and an app that
  only calls `syslog()` passes the log check and then leaves `output` empty forever:

  ```c
  openlog("<appName>", LOG_PID, LOG_USER);
  syslog(LOG_INFO, "Hello World");   // read by view_log.sh
  printf("Hello World\n");           // captured into `output` by run.sh
  ```
- Install the eap with `bash scripts/deploy.sh`. **Installing the app creates a dedicated SSH user for it on
  the device** — this is exactly why the skeleton has to be installed before any SSH testing is
  possible.
- Start the app and confirm the log shows 'Hello World' using `bash scripts/view_log.sh`. This goes
  over HTTP and needs no SSH yet, so it verifies the build/install/start path on its own.
  - If it fails, tell the user to look at `README` and check device access. If the issue is not
    device access, investigate what is wrong or missing based on the messages.
- Set SSH password by using `bash scripts/setup_ssh.sh`. If some error happens, inform the user instead of investing it. Just make sure the user confirm the contents in `.env` are correct, especially, if `APP_NAME` matches `appName`. DO NOT touch `.env`.
  - If the issue remains, ask the user to confirm on the device that the app's SSH user was created. If not, ask the user make sure that **Developer Mode** is configured correctly.
- Once SSH password is set successfully, verify SSH execution: run the installed binary over SSH
  with `bash scripts/run.sh <appName> <appName>` and check that the `output` file contains
  'Hello World'. When this passes, the project environment is ready for development and testing.
  - An **empty `output` with exit status 0** means the binary ran but wrote nothing to stdout —
    look for a `printf`, not for an SSH fault. Confirm it really ran by checking that
    `bash scripts/view_log.sh <appName>` gained a fresh syslog line; if it did, SSH is fine.

### Project Structure

```
project-root/
├── Dockerfile
├── Makefile               <- For build
├── .env                   <- Device credentials. The user's file — never read or write it
└── app/
    ├── manifest.json
    ├── Makefile           <- For compiling C/C++ applications
    ├── LICENSE            <- License information including third party libraries
    ├── <appName>.c        <- main: appName must match with manifest.json (.cc for C++)
    ├── <feature>.c        <- app-dependent: individual modules/features
    ├── test_<feature>.c   <- app-dependent: test binaries (needs acap-build -a)
    └── models/            <- app-dependent: model + labels (needs acap-build -a)
```

The app-dependent entries arrive with the feature that needs them. `acap-build` packages only the
app binary, so each also needs an `-a` flag — and test binaries need building as well, via the
`all` target of `app/Makefile`. See [references/setup.md](references/setup.md).

### Setup Verification

This is the gate between Phase 1 and Phase 2. Do not begin feature implementation until every box
is checked — an unchecked box means the environment, not your future feature, is the unknown.

- [ ] EAP file (.eap) generated by building the app successfully
- [ ] Install the app successfully
- [ ] Start the app successfully and see "Hello World" via `bash scripts/view_log.sh <appName>` (syslog)
- [ ] Run the app's binary from the installed package over SSH (`bash scripts/run.sh <appName> <appName>`) and find "Hello World" in the `output` file (stdout)

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
`bash scripts/deploy.sh` to install → `bash scripts/run.sh <appName> <test-binary>` runs it over
SSH and captures the result in `output`.

Concretely, that takes two changes — one to build the binary, one to package it:

```makefile
all: $(PROG) $(TEST_CAPTURE)      # app/Makefile: acap-build packages, it does not build
```
```dockerfile
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./ -a test_capture
```

`-a` repeats and takes subdirectory paths, and it's the same mechanism for shipping models,
labels and fonts — see [references/setup.md](references/setup.md).

Two conventions that make the results readable:

- **Results to stdout, diagnostics to stderr/syslog.** `run.sh` collects stdout into `output`, so
  keeping it to just the result means a test can emit binary data (a raw frame, a tensor dump)
  and still be logged normally.
- **Set the syslog ident to the binary's own name** in `openlog()`, not the appName. Then
  `bash scripts/view_log.sh <test-binary>` shows only that binary's log, instead of everything the app has
  ever written interleaved.

For inference and image pre-processing, build a **host-side reference** to check the device
against. The failures there are silent rather than loud — a wrong quantization scale or row-pitch
gives you a running app with wrong output — so a test that only proves "it didn't crash" proves
very little.

### Makefile (app/Makefile)

Templates for both C and C++, the `PKGS`/pkg-config expansion, and how to package extra files
into the `.eap` → [references/setup.md](references/setup.md).

Two things that cost time if you get them wrong: derive `PROG` from `manifest.json` with `jq`
rather than typing the name twice, and if the project is C++, remove the C-only warning options
(`-Wbad-function-cast`, `-Wstrict-prototypes`, `-Wmissing-prototypes`) — under `-Werror` they
break the build.

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
  "resources": {},               # empty on purpose — the slot each API fills later
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

- **`resources`** — start it as an empty object `{}` and leave it there. The skeleton needs no
  resources, but the key is where every API declaration will go, and creating it up front at the
  correct nesting level is what stops the most common manifest error: `resources` written *inside*
  `acapPackageConf` instead of beside it. With the slot already open you are filling in an
  existing object rather than deciding where to put a new one. Keep it empty until a feature
  actually needs something — an unused declaration is one more thing that can stop the app from
  starting.

Per-API declarations therefore go **into that top-level `resources` object**, a sibling of
`acapPackageConf`. Group settings live at `resources.linux.user.groups` (not a top-level
`linux.user.groups`), D-Bus methods at `resources.dbus.requiredMethods`, and so on. Each
reference file gives only the fragment that belongs inside `resources`; merge it into the empty
object rather than pasting a second `resources` key. The table below lists each API's requirement
using that full path.

### Available APIs

See the following references (Usage, API specifications, Examples) when using the respective
APIs below. The last column lists what each API requires in `manifest.json` — omitting it is a common reason an app fails to start.

| API | Purpose | Reference | manifest.json requirement |
|---|---|---|---|
| VDO (Video Capture) | Capture frames (H.264/H.265/AV1/JPEG/YUV/RGB) | `references/vdo.md` | `resources.linux.user.groups: ["video"]` |
| Larod | Edge inference & image preprocessing | `references/larod.md` | `resources.linux.user.groups: ["video"]` + `resources.deepLearningProcessor` — only if a job actually runs on the DLPU |
| Event (Axevent) | Event handling (stateless/stateful) | `references/event.md` | none |
| Axoverlay 2 | Overlay custom graphics onto streams — the only option if you need **text** | `references/overlay.md` | `resources.overlay {enabled, required}` (+ `linux.user.groups: ["gpu"]` for GPU rendering) |
| Bounding Box | Fast boxes/polylines for analytics results — geometry only, **cannot draw text** | `references/bbox.md` | `resources.dbus.requiredMethods` (Graphics2/Overlay2) + `resources.linux.user.groups: ["video"]` |
| Parameter (AXParameter) | Parameter management + change callbacks | `references/parameter.md` | `acapPackageConf.configuration.paramConfig[]` |
| Edge Storage (AXStorage) | Read/Write to SD card / NAS | `references/storage.md` | `resources.linux.user.groups: ["storage"]` |
| Serial Port | RS-232/422/485 | `references/serial.md` | `resources.linux.user.groups: ["admin"]` |
| Device Data Hub | Pub/Sub messaging between apps | `references/ddh.md` | `resources.deviceDataHub {enabled, accessControlList}` |
| License Key | Copy protection | `references/key.md` | `acapPackageConf.setup.copyProtection.method: "axis"` + `acapPackageConf.setup.appId` |

> The full list of platform APIs (recording search/playback, PTZ, message broker, HIDRAW,
> VAPIX access, bundled open-source libs) is at
> https://developer.axis.com/acap/reference/supported-apis/

## Deploy apps

`bash scripts/deploy.sh <file>.eap`

### Control apps (start, stop, restart, or remove)

`bash scripts/control.sh <appName> start|stop|restart|remove`

## View application logs

`bash scripts/view_log.sh <binary-name>`

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
- Trusting a **new measurement** — a pixel readback, a checksum, a custom probe — before checking
  it against a known answer. Draw a filled rectangle of known area and confirm the measurement
  reports it. Uncalibrated instruments don't just fail to help; they start overriding reality
- Dismissing what a **human observed** because your measurement disagrees. Suspect the measurement
  first, especially if you just wrote it. Concluding "the user misread it" on the strength of an
  unverified probe is how a real, reproducible bug gets recorded as nonexistent

## Verification

After completing all increments for a task:

- [ ] The build is clean
- [ ] The feature works end-to-end as specified
- [ ] No uncommitted changes remain
