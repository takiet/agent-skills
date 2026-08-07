# AXStorage — Edge Storage API

Discover and use mounted storage devices (SD card, NAS shares) to read and write application
data. AXStorage is fully **event-driven and asynchronous**: you subscribe to each storage
device and react to availability/writability/full/exiting events.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/axstorage/html/index.html

## Build Requirements

### Makefile

```make
PKGS = glib-2.0 gio-2.0 axstorage
```

### Source files

```c
#include <axsdk/axstorage.h>
```
Requires a running `GMainLoop`.

### manifest.json
the app user must be in the `storage` group:

```json
"resources": {
  "linux": { "user": { "groups": ["storage"] } }
}
```

## Core object & types

- `AXStorage*` — handle to one set-up storage device.
- `AXStorageType` — `LOCAL` (SD card), `EXTERNAL` (NAS), etc. via `ax_storage_get_type()`.
- Event types for `ax_storage_get_status()`:
  `AX_STORAGE_AVAILABLE_EVENT`, `AX_STORAGE_WRITABLE_EVENT`,
  `AX_STORAGE_FULL_EVENT`, `AX_STORAGE_EXITING_EVENT`.

## Workflow

```
ax_storage_list()  ──►  for each id: ax_storage_subscribe(id, subscribe_cb)
                                            │
                          subscribe_cb: read ax_storage_get_status(...)
                            if writable & !full & !exiting & !setup:
                                ax_storage_setup_async(id, setup_cb)
                            if exiting & setup:
                                ax_storage_release_async(storage, release_cb)
                                            │
                          setup_cb: ax_storage_get_storage_id / _get_path / _get_type
                                    → now safe to read/write files under the path
```

```c
// 1. Enumerate devices
GError* error = NULL;
GList* disks = ax_storage_list(&error);         // list of gchar* storage_id (free each)

// 2. Subscribe to each device's events
guint sub = ax_storage_subscribe(storage_id, subscribe_cb, user_data, &error);

// 3. In subscribe_cb, check status and set up if usable
static void subscribe_cb(gchar* storage_id, gpointer user, GError* err) {
    gboolean writable = ax_storage_get_status(storage_id, AX_STORAGE_WRITABLE_EVENT, &err);
    gboolean full     = ax_storage_get_status(storage_id, AX_STORAGE_FULL_EVENT,     &err);
    gboolean exiting  = ax_storage_get_status(storage_id, AX_STORAGE_EXITING_EVENT,  &err);
    if (exiting)                        ax_storage_release_async(storage, release_cb, storage_id, &err);
    else if (writable && !full)         ax_storage_setup_async(storage_id, setup_cb, NULL, &err);
}

// 4. In setup_cb, the AXStorage* and path become available
static void setup_disk_cb(AXStorage* storage, gpointer user, GError* err) {
    gchar* id   = ax_storage_get_storage_id(storage, &err);
    gchar* path = ax_storage_get_path(storage, &err);       // e.g. /var/spool/storage/SD_DISK
    // build "<path>/mydata.log" and use normal file I/O (g_fopen, fwrite, ...)
    g_free(id); g_free(path);
}
```

## Writing files

The `path` from `ax_storage_get_path()` is a normal directory. Use standard/GLib file I/O:

```c
gchar* filename = g_strdup_printf("%s/%s.log", path, name);
FILE* f = g_fopen(filename, "a");
g_fprintf(f, "…\n");
fclose(f);
g_free(filename);
```
Only write when the device is `available && writable && !full && setup`.

## Cleanup

```c
ax_storage_release_async(storage, release_cb, user_data, &error);  // finish I/O first!
ax_storage_unsubscribe(subscription_id, &error);
```

## Notes & gotchas

- **Everything is async** — the `AXStorage*` handle only exists inside/after `setup_cb`; store
  it in your own per-disk struct keyed by `storage_id`.
- **Finish all reads/writes before `ax_storage_release_async()`.**
- Always gate writes on the current event flags; a card can be removed at any time
  (`EXITING`) — handle it by releasing.
- It is advised to retry `ax_storage_list()` and `ax_storage_setup_async()` on failure.
- A `subscription_id` of `0` from `ax_storage_subscribe` means failure.

## Related

- Small config values instead of files → [parameter.md](parameter.md)
