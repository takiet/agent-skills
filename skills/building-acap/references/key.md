# License Key API

Copy-protect an application by verifying a signed license key that is tied to a specific
application name, application id, device, and version. Use it to gate functionality when a
valid license is not installed.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/licensekey/html/index.html

## Build Requirements

### Makefile

Linking is special — the license-check symbol is provided partly statically:

```make
PKGS   = glib-2.0
LDLIBS += -Wl,-Bstatic,-llicensekey_stat,-Bdynamic,-llicensekey -ldl
```

### Source files

```c
#include <licensekey.h>
```

### manifest.json
enable Axis copy protection and set the `appId`. Like AXParameter, this goes inside
`acapPackageConf`, **not** in the top-level `resources`:

```json
"acapPackageConf": {
  "setup": {
    "appName": "licensekey_handler",
    "appId": "0",
    "copyProtection": { "method": "axis" }
    // (copyProtection may also sit directly under acapPackageConf)
  }
}
```
`appId` is assigned by Axis when you register the application; `0` is only for local testing.

## API

A single call returns the license validity:

```c
int licensekey_verify(const char* app_name,
                      int app_id,
                      int major_version,
                      int minor_version);
// returns 1 if the license key is valid, otherwise not valid
```

## Usage pattern

Check at startup and then periodically (a license can be added/removed/expire while running):

```c
#define APP_ID        0
#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define CHECK_SECS    300      // re-check every 5 minutes

static gchar* app_name = NULL;

static gboolean check_license_status(void* data) {
    (void)data;
    if (licensekey_verify(app_name, APP_ID, MAJOR_VERSION, MINOR_VERSION) != 1)
        syslog(LOG_INFO, "Licensekey is invalid");   // disable features here
    else
        syslog(LOG_INFO, "Licensekey is valid");
    return TRUE;                                      // keep the timer going
}

int main(int argc, char* argv[]) {
    app_name = g_path_get_basename(argv[0]);          // == appName
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);

    check_license_status(NULL);                        // immediate check
    g_timeout_add_seconds(CHECK_SECS, check_license_status, NULL);

    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    g_free(app_name);
}
```

## Notes & gotchas

- `app_name` must match the manifest `appName`; deriving it from `argv[0]` via
  `g_path_get_basename()` guarantees this.
- **Re-verify periodically**, not just once — licenses can change at runtime.
- Only `1` means valid; treat every other value as invalid and degrade gracefully.
- The license key file itself is generated and signed via the Axis license infrastructure
  using the app id, device id and version; verification here is purely local.

## Related

- Gate a feature behind a parameter as well → [parameter.md](parameter.md)
