# API reference

The module name is `mqtt`. It gives ucode scripts an MQTT client that runs on
the uloop event loop.

```ucode
import * as mqtt from "mqtt";
import * as uloop from "uloop";
```

The module needs a running event loop. Start uloop before you connect. Run
uloop to process broker traffic.

```ucode
uloop.init();
let client = mqtt.connect("broker.example.org");
uloop.run();
uloop.done();
```

## Module functions

### mqtt.connect(broker, port, client_id, options)

Create a client and start a connection to a broker.

The connection is asynchronous. The function returns before the broker
answers. The `on_connect` callback reports a successful connection.

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `broker` | string | required | Host name or IP address of the broker. |
| `port` | integer | 1883 | TCP port, 1 to 65535. |
| `client_id` | string | random | MQTT client identifier. libmosquitto generates one if you omit it. |
| `options` | object | none | Callbacks and connection settings. See [Options](#options). |

Returns the client object. Returns `null` if the module cannot start the
connection. Call `mqtt.error()` to read the reason.

The function raises a type exception in three cases:

- An argument has the wrong type.
- The port is outside 1 to 65535.
- An option has the wrong type.

### mqtt.error()

Return the last error message, then clear it.

Returns a string. Returns `null` if no error occurred since the last call.

## Options

Every option has a default, so you set only what you need.

The module validates each value. A value of the wrong type raises a type
exception. An integer value outside its range is clamped to the nearest bound.

### Callback options

| Option | Runs when |
| --- | --- |
| `on_connect` | The broker accepted the connection. |
| `on_reconnect` | The broker accepted a reconnection. `on_connect` runs if you omit this. |
| `on_disconnect` | The connection closed, for any reason. |
| `on_message` | A message arrived on a subscribed topic. |
| `on_publish` | The broker confirmed a published message. |
| `on_error` | The broker refused the connection, or the reconnect attempts ran out. |

See [Callbacks](#callbacks) for the arguments of each callback.

### Authentication

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `username` | string | none | User name for the broker. |
| `password` | string | none | Password for the broker. The module ignores it without a user name. |

### TLS

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `tls` | boolean | `false` | Encrypt the connection with TLS. |
| `ca_cert` | string | none | Path to a CA certificate file. |
| `ca_path` | string | none | Path to a directory of CA certificates. |
| `cert` | string | none | Path to the client certificate. |
| `key` | string | none | Path to the client private key. |

The module applies these certificate paths only if `tls` is `true`.

If you set `tls` without `ca_cert` and without `ca_path`, the module uses the
system trust store. It reads `/etc/ssl/certs/ca-certificates.crt` if that file
is readable. If not, it reads the directory `/etc/ssl/certs`.

### Connection

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `keepalive` | integer | 60 | Seconds between keepalive pings, 5 to 65535. |
| `debug` | boolean | `false` | Write libmosquitto log output to stderr. |

The lower bound of 5 seconds comes from libmosquitto, which refuses smaller
values.

### Last will

The broker publishes the last will if the client disconnects abnormally. A
clean `disconnect()` cancels the will.

| Member | Type | Default | Description |
| --- | --- | --- | --- |
| `topic` | string | required | Topic for the will message. |
| `payload` | string | empty | Content of the will message. |
| `qos` | integer | 0 | QoS level, 0 to 2. |
| `retain` | boolean | `false` | Ask the broker to retain the will message. |

```ucode
will: {
    topic: "clients/sensor1/status",
    payload: "offline",
    qos: 1,
    retain: true
}
```

### Reconnection

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `reconnect` | boolean | `true` | Reconnect after an unexpected disconnection. |
| `reconnect_delay` | integer | 5 | Seconds before the first attempt, 1 to 3600. |
| `reconnect_delay_max` | integer | 30 | Upper bound for the backoff delay, up to 86400 seconds. |
| `reconnect_attempts` | integer | 0 | Maximum attempts. 0 means no limit. |

See [Reconnection behaviour](#reconnection-behaviour) for the rules the module
applies.

### A full options object

```ucode
let client = mqtt.connect("broker.example.org", 8883, "sensor1", {
    on_connect: function(client) {
        client.subscribe("sensors/+/command", 1);
    },
    on_message: function(client, message) {
        printf("%s: %s\n", message.topic, message.payload);
    },
    on_error: function(client, message) {
        warn("mqtt: " + message + "\n");
    },

    username: "sensor1",
    password: "secret",

    tls: true,
    ca_cert: "/etc/ssl/certs/broker-ca.crt",

    keepalive: 30,

    will: {
        topic: "clients/sensor1/status",
        payload: "offline",
        retain: true
    },

    reconnect: true,
    reconnect_delay: 5,
    reconnect_delay_max: 60,
    reconnect_attempts: 0
});
```

## Client methods

Every method returns `false` if the client is no longer usable. Every method
that talks to the broker records a message for `mqtt.error()` on failure.

### client.publish(topic, message, qos, retain)

Publish a message to a topic.

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `topic` | string | required | Topic to publish to. |
| `message` | string | required | Payload. Binary content is allowed. |
| `qos` | integer | 0 | QoS level, 0 to 2. |
| `retain` | boolean | `false` | Ask the broker to retain the message. |

Returns `true` if libmosquitto queued the message. The `on_publish` callback
reports the delivery.

The payload keeps its full length. The module does not stop at an embedded
null byte.

Raises a type exception if the topic or the message is not a string. Raises a
type exception if the QoS is not an integer between 0 and 2.

### client.subscribe(topic, qos)

Subscribe to a topic.

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `topic` | string | required | Topic filter. Wildcards `+` and `#` are allowed. |
| `qos` | integer | 0 | Maximum QoS level, 0 to 2. |

Returns `true` if libmosquitto queued the request.

### client.unsubscribe(topic)

Stop a subscription.

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `topic` | string | required | The topic filter you subscribed with. |

Returns `true` if libmosquitto queued the request.

### client.disconnect()

Close the connection cleanly.

A clean disconnect turns off automatic reconnection and cancels a pending
reconnect attempt. The broker discards the last will.

Returns `true` on success.

### client.reconnect()

Connect to the broker again.

Use this method after `disconnect()`, or after the reconnect attempts ran out.
The method cancels a pending backoff timer and starts a fresh attempt. It also
resets the attempt counter.

Returns `true` if libmosquitto started the attempt.

### client.is_connected()

Report the connection state.

Returns `true` while the connection is up.

## Callbacks

Every callback receives the client object as its first argument. This lets one
script serve several connections with shared callback functions.

### on_connect(client) and on_reconnect(client)

The broker accepted the connection. Subscribe to your topics here, because a
reconnection clears the subscriptions of a clean session.

`on_reconnect` runs for the second connection and every later one. If you do
not set `on_reconnect`, `on_connect` runs instead.

### on_disconnect(client)

The connection closed. The callback runs for a clean disconnect and for a
connection loss.

### on_message(client, message)

A message arrived. The `message` object holds four members.

| Member | Type | Description |
| --- | --- | --- |
| `topic` | string | Topic the message arrived on. |
| `payload` | string | Message content, with its full length. |
| `qos` | integer | QoS level of the message. |
| `retain` | boolean | The broker sent this as a retained message. |

### on_publish(client, mid)

The broker confirmed a message. `mid` is the message identifier that
libmosquitto assigned.

### on_error(client, message)

The module reports a connection problem. `message` is a string. It holds the
CONNACK reason if the broker refused the connection. It holds
`reconnect attempts exhausted` if the attempt limit ran out.

## Errors

The module reports problems in two ways.

A wrong argument or a wrong option value raises an exception. Catch it with
`try`/`catch`. These faults come from the script, not from the network.

A failure of the broker or of the operating system does not raise an
exception. `mqtt.connect()` returns `null`, and the client methods return
`false`. Call `mqtt.error()` for the message.

```ucode
let client = mqtt.connect("broker.example.org");

if (!client)
    die("connect failed: " + mqtt.error());
```

An exception inside a callback goes to the handler that `uloop.guard()`
installed. Without a guard, the module ends the event loop.

## Reconnection behaviour

Automatic reconnection is on by default. The module drives it with its own
timer, because libmosquitto reconnects on its own only under
`mosquitto_loop_forever()`.

The rules are:

- A lost connection starts the backoff timer.
- A clean `disconnect()` does not start it.
- The delay starts at `reconnect_delay` and doubles per attempt.
- The delay never grows past `reconnect_delay_max`.
- A successful connection resets the attempt counter and the delay.

If `reconnect_attempts` is not 0, the module counts the attempts. When the
count reaches the limit, the module calls `on_error` with
`reconnect attempts exhausted` and stops. A later `reconnect()` call starts a
fresh cycle.

## Client lifetime

The module holds its own reference to a client while the connection is up, and
while a reconnect attempt is pending. Your script can therefore drop its
reference and keep the connection alive.

```ucode
uloop.init();

mqtt.connect("broker.example.org", 1883, "sensor1", {
    on_connect: function(client) {
        client.publish("sensors/sensor1/state", "up");
    }
});

uloop.run();
```

The module drops its reference once the client is idle. The garbage collector
then frees the client.
