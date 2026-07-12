# Axevent — Event API

Send and receive events through the Axis device event system (the same system that drives
action rules, ONVIF, MQTT, recordings, etc.). Supports **stateless** events (momentary
triggers), **stateful/property** events (a value with a current state), and **data** events.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/axevent/html/index.html

## Build & manifest

```make
PKGS = glib-2.0 axevent
```

```c
#include <axsdk/axevent.h>
```
No special `manifest.json` resource is required. Axevent is built on GLib and needs a running
`GMainLoop`.

## Core objects

| Type | Meaning |
|---|---|
| `AXEventHandler` | Connection to the event system; declares, subscribes, sends. |
| `AXEventKeyValueSet` | The topic + key/value payload describing an event. |
| `AXEvent` | A concrete event instance (a value set + timestamp). |

## Key/value set building blocks

- `ax_event_key_value_set_new()` / `..._free()`
- `ax_event_key_value_set_add_key_value(set, key, ns, value, AX_VALUE_TYPE_*, &err)`
  — types: `AX_VALUE_TYPE_INT`, `_DOUBLE`, `_BOOL`, `_STRING`, `_ELEMENT`.
- Topic keys are conventionally `topic0`, `topic1`, `topic2` with a namespace such as
  `"tns1"` (ONVIF) or `"tnsaxis"` (Axis).
- Mark roles: `ax_event_key_value_set_mark_as_source()`,
  `..._mark_as_data()`, `..._mark_as_user_defined(set, key, ns, "wstype:xs:float", &err)`.

## Sending events (producer)

```c
// 1. Declare the event once
static guint setup_declaration(AXEventHandler* h, gdouble* start_value) {
    AXEventKeyValueSet* set = ax_event_key_value_set_new();
    ax_event_key_value_set_add_key_value(set, "topic0", "tns1", "Monitoring",     AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_key_value(set, "topic1", "tns1", "ProcessorUsage", AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_key_value(set, "Value",  NULL,  start_value,       AX_VALUE_TYPE_DOUBLE, NULL);
    ax_event_key_value_set_mark_as_data(set, "Value", NULL, NULL);

    guint declaration = 0;
    ax_event_handler_declare(h, set,
        FALSE,                                  // FALSE => stateful/property event
        &declaration,
        (AXDeclarationCompleteCallback)declaration_complete, start_value, NULL);
    ax_event_key_value_set_free(set);
    return declaration;
}

// 2. Send an instance (typically from a timer, after declaration_complete fires)
AXEventKeyValueSet* kv = ax_event_key_value_set_new();
ax_event_key_value_set_add_key_value(kv, "Value", NULL, &value, AX_VALUE_TYPE_DOUBLE, NULL);
AXEvent* event = ax_event_new2(kv, NULL);       // ax_event_new is deprecated
ax_event_key_value_set_free(kv);
ax_event_handler_send_event(handler, declaration_id, event, NULL);
ax_event_free(event);
```

- `ax_event_handler_declare(..., stateless, ...)`: pass `TRUE` for a **stateless** event,
  `FALSE` for a **stateful/property** event.
- The `AXDeclarationCompleteCallback` fires when the declaration is registered; only then may
  you start sending.

## Receiving events (consumer)

```c
static void on_event(guint subscription, AXEvent* event, guint* token) {
    const AXEventKeyValueSet* kv = ax_event_get_key_value_set(event);   // do NOT free kv
    gdouble value = 0;
    ax_event_key_value_set_get_double(kv, "Value", NULL, &value, NULL);
    syslog(LOG_INFO, "value=%f", value);
    ax_event_free(event);                                               // DO free the event
}

AXEventKeyValueSet* filter = ax_event_key_value_set_new();
ax_event_key_value_set_add_key_value(filter, "topic0", "tns1", "Monitoring",     AX_VALUE_TYPE_STRING, NULL);
ax_event_key_value_set_add_key_value(filter, "topic1", "tns1", "ProcessorUsage", AX_VALUE_TYPE_STRING, NULL);

guint subscription = 0;
ax_event_handler_subscribe(handler, filter, &subscription,
    (AXSubscriptionCallback)on_event, token, NULL);
ax_event_key_value_set_free(filter);
```
Getters: `ax_event_key_value_set_get_integer/_double/_boolean/_string()`. Leave out keys in
the filter to match more broadly.

## Lifecycle

```c
AXEventHandler* handler = ax_event_handler_new();
// declare / subscribe ...
g_main_loop_run(loop);
ax_event_handler_undeclare(handler, declaration_id, NULL);   // producer
ax_event_handler_unsubscribe(handler, subscription, NULL);   // consumer
ax_event_handler_free(handler);
```

## Notes & gotchas

- Requires a `GMainLoop` — callbacks never fire without it.
- In the subscription callback, **free the `AXEvent`** but **not** the `AXEventKeyValueSet`
  (it is owned by the event system until you unsubscribe).
- Use `ax_event_new2()`, not the deprecated `ax_event_new()`.
- Send events only after the declaration-complete callback has fired.
- To discover existing topics/keys on a device, inspect the event declarations in the web UI
  or VAPIX before writing a subscription filter.

## Related

- For high-rate scene metadata (objects, PTZ), prefer the Device Data Hub → [ddh.md](ddh.md).
