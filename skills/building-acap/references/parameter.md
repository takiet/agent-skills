# AXParameter — Parameter API

Read and modify application parameters (those declared in `manifest.json`) and read system-wide
parameters. Parameters appear on the app's Settings page in the device web UI and are reachable
via VAPIX `param.cgi`. Changes can be observed via callbacks.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/axparameter/html/index.html

## Build & manifest

```make
PKGS = glib-2.0 gio-2.0 axparameter
```

```c
#include <axsdk/axparameter.h>
```

Declare parameters under `acapPackageConf.configuration.paramConfig`:

```json
"configuration": {
  "paramConfig": [
    { "name": "IsCustomized", "default": "no",  "type": "bool:no,yes" },
    { "name": "BackupValue",  "default": "...",  "type": "hidden:string" }
  ]
}
```

Common `type` values: `bool:no,yes`, `string`, `hidden:string`, `int`, `int:min,max`,
`enum:a,b,c`. No `resources` entry is needed.

## Core object

`AXParameter*` — a handle scoped to your application group.

```c
GError* error = NULL;
AXParameter* handle = ax_parameter_new("myappname", &error);   // group = app name
// ...
ax_parameter_free(handle);
```

## Reading / writing

```c
// Read an app parameter (short name, no qualifier needed)
gchar* value;
ax_parameter_get(handle, "IsCustomized", &value, &error);
g_free(value);

// Read a system parameter (fully qualified)
gchar* serial;
ax_parameter_get(handle, "Properties.System.SerialNumber", &serial, &error);
g_free(serial);

// Write (last bool = "do_sync": persist to storage)
ax_parameter_set(handle, "BackupValue", "new value", TRUE, &error);
```

Both short (`"IsCustomized"`) and fully qualified (`"root.myappname.IsCustomized"`) names are
accepted; internally everything is qualified with the app group.

## Adding / removing at runtime

```c
ax_parameter_add(handle, "CustomValue", "initial", NULL, &error);   // 4th arg = type or NULL
ax_parameter_remove(handle, "CustomValue", &error);

// Enumerate all app parameters:
GList* list = ax_parameter_list(handle, &error);
for (GList* x = list; x; x = g_list_next(x)) { /* (gchar*)x->data */ g_free(x->data); }
g_list_free(list);
```
Runtime add/remove is reflected on the Settings page only after the Apps page is reloaded.

## Change callbacks

```c
static void on_change(const gchar* name, const gchar* value, gpointer user) {
    // name arrives fully qualified: "root.myappname.IsCustomized"
    // !! Do NOT call any ax_parameter_* function here — it deadlocks. !!
    // Defer real work: copy strings, schedule with g_timeout_add_seconds().
}

ax_parameter_register_callback(handle, "root.myappname.IsCustomized", on_change, handle, &error);
// You may register for a parameter that does not exist yet.
```
Requires a running `GMainLoop`.

## Notes & gotchas

- **Deadlock rule:** never call `ax_parameter_*` from inside a `register_callback` handler.
  Copy the incoming `name`/`value` (they are owned by the library) and do the work from a
  `g_timeout_add_seconds()` callback, which *is* allowed to call the API.
- The callback always receives the **fully qualified** parameter name.
- `bool:no,yes` guarantees the stored string is `"no"` or `"yes"`; you still interpret it in code.
- `ax_parameter_get` on a missing parameter fails — use `ax_parameter_list` or check the return.
- Free every `gchar*` from `ax_parameter_get` with `g_free`.

## Related

- Persisting larger data / files → [storage.md](storage.md)
