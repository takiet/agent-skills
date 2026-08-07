# AXSerialPort — Serial Port API

Configure and use the device's external serial port(s) — RS-232, RS-422 and RS-485
(2- and 4-wire). You configure the port with the AX API, then read/write bytes through a
standard `GIOChannel` on the port's file descriptor.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/axserialport/html/index.html

## Build Requirements

### Makefile

```make
PKGS = glib-2.0 axserialport
```

### Source files

```c
#include <axsdk/axserialport.h>
```
Requires a running `GMainLoop`. Available settings are product-dependent — check the device
datasheet.

### manifest.json
needs elevated privileges (the example uses the `admin` group):

```json
"resources": {
  "linux": { "user": { "groups": ["admin"] } }
}
```

## Core object

`AXSerialConfig*` — configuration handle for one port.

## Configure a port

```c
GError* error = NULL;
guint port0 = 0;
AXSerialConfig* config = ax_serial_init(port0, &error);

ax_serial_port_enable(config,   AX_SERIAL_ENABLE,        NULL);
ax_serial_set_baudrate(config,  AX_SERIAL_B19200,        NULL);
ax_serial_set_bias(config,      AX_SERIAL_DISABLE,       NULL);
ax_serial_set_databits(config,  AX_SERIAL_DATABITS_8,    NULL);
ax_serial_set_parity(config,    AX_SERIAL_PARITY_NONE,   NULL);
ax_serial_set_portmode(config,  AX_SERIAL_RS485_4,       NULL);   // RS232 / RS485_2 / RS485_4 / RS422
ax_serial_set_stopbits(config,  AX_SERIAL_STOPBITS_1,    NULL);
ax_serial_set_termination(config, AX_SERIAL_DISABLE,     NULL);

ax_serial_sync_port_settings(config, &error);   // apply the configuration
```

Enum families: `AX_SERIAL_ENABLE/DISABLE`, `AX_SERIAL_B<rate>` (e.g. `B9600`, `B19200`),
`AX_SERIAL_DATABITS_7/8`, `AX_SERIAL_PARITY_NONE/EVEN/ODD`,
`AX_SERIAL_STOPBITS_1/2`, `AX_SERIAL_RS232/RS485_2/RS485_4/RS422`.

## Read / write via GIOChannel

```c
int fd = ax_serial_get_fd(config, &error);
GIOChannel* io = g_io_channel_unix_new(fd);
g_io_channel_set_encoding(io, NULL, &error);          // NULL => raw binary (default is UTF-8)

// Write
gsize written;
g_io_channel_write_chars(io, data, len, &written, &error);
g_io_channel_flush(io, &error);

// Read: watch for incoming data
g_io_add_watch(io, G_IO_IN, incoming_data_cb, user_data);
static gboolean incoming_data_cb(GIOChannel* ch, GIOCondition c, gpointer user) {
    gsize read; gchar buf[128];
    g_io_channel_read_chars(ch, buf, sizeof(buf), &read, &error);
    return TRUE;   // keep the watch
}
```

## Cleanup

```c
g_io_channel_shutdown(io, FALSE, NULL);
g_io_channel_unref(io);
ax_serial_cleanup(config);
```

## Notes & gotchas

- **Set encoding to raw** (`g_io_channel_set_encoding(io, NULL, ...)`) for binary protocols,
  otherwise GLib assumes UTF-8 and mangles bytes.
- Which port modes / baud rates / bias / termination are valid is **product-specific** — read
  the datasheet; unsupported settings fail at `ax_serial_sync_port_settings()`.
- For RS-485 half-duplex (`RS485_2`) the driver handles direction switching; flush after write.
- All configuration setters take a trailing `GError**`; the example passes `NULL` but you
  should check errors in production.

## Related

- Raw USB HID devices instead of serial → HIDRAW access (see supported-apis).
