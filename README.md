# ucode-mqtt

MQTT for ucode scripts.

The module wraps libmosquitto and drives it from uloop. One script can
therefore talk to a broker and serve its other events at the same time.

```ucode
#!/usr/bin/env ucode

import * as mqtt from "mqtt";
import * as uloop from "uloop";

uloop.init();

let client = mqtt.connect("test.mosquitto.org", 1883, "ucode-demo", {
    on_connect: function(client) {
        client.subscribe("test/ucode/+");
        client.publish("test/ucode/hello", "Hello from ucode");
    },
    on_message: function(client, message) {
        printf("%s: %s\n", message.topic, message.payload);
    }
});

if (!client)
    die("connect failed: " + mqtt.error());

uloop.run();
uloop.done();
```

## What it gives you

- Several connections in one script, each with its own callbacks.
- Automatic reconnection, with exponential backoff and an optional
  attempt limit.
- TLS, with the system trust store or with your own certificates.
- Last will testament, keepalive, QoS 0 to 2 and retained messages.
- Binary payloads, kept at their full length.
- No blocking anywhere. Every operation runs on uloop.

## Install

The build needs cmake, libmosquitto, libubox and libucode.

```bash
sudo apt install libmosquitto-dev libubox-dev cmake
```

```bash
cmake -B build .
cmake --build build
cmake --install build
```

The install step writes `mqtt.so` into `lib/ucode/` under the install prefix.

## Run a script

Point ucode at the directory that holds the module. The path must be
absolute.

```bash
ucode -L "$(pwd)/build" demo.uc
```

## Where to read on

[API.md](API.md) documents every function, option and callback.

The `examples/` directory holds scripts you can run:

| Script | Shows |
| --- | --- |
| `basic.uc` | A first connection, one subscription, one message. |
| `publisher.uc`, `subscriber.uc` | The publish and subscribe pattern, split over two scripts. |
| `publisher_ssl.uc`, `subscriber_ssl.uc` | The same pair over TLS. |
| `subscriber_with_reconnect.uc` | The reconnection options at work. |

## Licence

GPL 2.0 only, see `LICENSE.txt`. Every source file carries an
`SPDX-License-Identifier: GPL-2.0-only` header. Copyright (C) 2026 John
Crispin <john@phrozen.org>.
