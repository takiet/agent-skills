# Device Data Hub (DDH) — Data producer/consumer API

Publish/subscribe messaging for sharing structured data between applications (and with the
device's built-in producers such as scene metadata). Producers register a **topic** and write
JSON samples; consumers subscribe with a **filter** and receive callbacks. DDH is the modern
replacement for the (deprecated) Message Broker API.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/device-data-hub/html/index.html

## Build Requirements

### Makefile

```make
PKGS = device-data-hub-client-c
```

### Source files

```c
#include <datahub/client.h>       // producer + common
#include <datahub/subscriber.h>   // consumer
```

### manifest.json
enable the resource and declare an **access-control list**. ACL usernames are
`acap-<appName>` of the peer apps that may read/write your topic:

```json
"resources": {
  "deviceDataHub": {
    "enabled": true,
    "accessControlList": [
      { "topics": ["com.example.objectdetector"],
        "usernames": ["acap-object_consumer"], "operations": ["read"] },
      { "topics": ["com.example.objectdetector"],
        "usernames": ["acap-object_detector"], "operations": ["all"] }
    ]
  }
}
```
Use reverse-DNS topic names (`com.example.<topic>`). `operations`: `read`, `write`, `all`.

## Core objects

| Type | Role |
|---|---|
| `DHClient` | Connection to the DDH core. |
| `DHTopic` / `DHTopicData` / `DHTopicSample` | Topic handle / a JSON payload / a received sample. |
| `DHWriter` | Producer endpoint for a topic. |
| `DHSubscriber` + `DHFilter` + `DHSubscribeOptions` | Consumer endpoint and its subscription config. |
| `DHError` | Error object; `dh_error_get_code()`, `dh_error_to_string()`. |

## Producer

```c
DHError* err = NULL;
DHClient* client = dh_client_create("Client for object_detector", &err);
dh_client_connect(client, &err);

// Get or create the topic (JSON definition with data_schema)
DHTopic* topic = dh_client_get_topic(client, TOPIC_NAME, &err);
if (err && dh_error_get_code(err) == DH_ERR_INVALID_TOPIC) {
    dh_error_destroy(err); err = NULL;
    topic = dh_client_create_topic(client, topic_definition_json, &err);
}

// Create a writer and register production
DHWriter* w = dh_client_create_writer(client, "producer_writer", TOPIC_NAME, &err);
dh_writer_set_consumer_match_update_callback(w, on_consumer_match_update, NULL);
DHProductionId id;
dh_writer_register_production(w, NULL, &id, &err);   // NULL = no instance keys

// Write a sample
DHTopicData* d = dh_topic_data_create();
dh_topic_data_set_json_data(d, "{\"object\":\"human\",\"distance\":100}", NULL);
DHTimestamp* ts = dh_timestamp_create();
dh_writer_write_data(w, NULL, d, ts, &err);
dh_timestamp_destroy(ts);
dh_topic_data_destroy(d);
```

The topic definition is a JSON string with `topic_name`, `description`, `version`, and a
`data_schema` (JSON-schema `type`/`properties`/`required`).

`on_consumer_match_update` tells the producer when a matching consumer appears
(`DH_CONSUMER_MATCH`) or leaves — a good signal to start/stop producing.

## Consumer

```c
DHClient* client = dh_client_create("Client for object_consumer", &err);
dh_client_connect(client, &err);

DHSubscriber* sub = dh_client_create_subscriber(client, "my subscriber", &err);
dh_subscriber_set_data_callback(sub, on_data_received, user_data, &err);

DHFilter* filter = dh_filter_create();
dh_filter_add_topic_name(filter, TOPIC_NAME, &err);

DHSubscribeOptions* opt = dh_subscribe_options_create();
dh_subscribe_options_add_filter(opt, filter, &err);
dh_filter_destroy(filter);
dh_subscribe_options_set_enable_data_updates(opt, true);

dh_subscriber_subscribe(sub, opt, &err);
dh_subscribe_options_destroy(opt);

// Callback
static void on_data_received(const DHTopicSample* sample, void* user) {
    const DHTopicData* td = dh_topic_sample_get_data(sample);
    const char* json = dh_topic_data_get_json_data(td);
    syslog(LOG_INFO, "got: %s", json);
}
```

## Cleanup

```c
// producer
dh_writer_destroy(w);
dh_client_delete_topic(client, TOPIC_NAME, &err);   // disconnect alone does NOT delete topic
dh_client_disconnect(client, &err);
dh_topic_destroy(topic);
dh_client_destroy(client);
// consumer
dh_subscriber_destroy(sub);
dh_client_disconnect(client, &err);
dh_client_destroy(client);
```

## Notes & gotchas

- **ACL is mandatory:** producer and each consumer must be listed by `acap-<appName>` username
  with appropriate `operations`, or access is denied.
- `dh_client_disconnect()` does **not** remove a topic; call `dh_client_delete_topic()`
  explicitly if you own it.
- Consumers don't need a `GMainLoop` — DDH runs its own dispatch thread; the example just
  `pause()`s the main thread. Guard shared state (the example uses `atomic_bool`).
- Handle `dh_*` errors consistently: check the `DHError**`, log `dh_error_to_string()`,
  then `dh_error_destroy()`.
- The device's own analytics (scene metadata) are consumable DDH topics — see the
  `consume-scene-metadata` example.

## Related

- Lower-rate / action-rule events → [event.md](event.md)
