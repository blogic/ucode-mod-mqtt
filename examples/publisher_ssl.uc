#!/usr/bin/env ucode

/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

import * as mqtt from "mqtt";
import * as uloop from "uloop";

let count = 0;
let timer = null;
let client = null;

// Initialize uloop
uloop.init();

// Connect to MQTT broker with SSL
client = mqtt.connect("test.mosquitto.org", 8883, "ucode-ssl-publisher", {
    on_connect: function(client) {
            print("Publisher connected to MQTT broker with SSL!\n");

            // Start publishing messages every second
            function publishMessage() {
                count++;
                let message = `Message #${count} from SSL publisher`;

                if (client.publish("test/ucode/counter", message, 0, false)) {
                    print(`Published: ${message}\n`);
                } else {
                    print("Failed to publish message\n");
                }
            }

            // Use interval for continuous publishing
            timer = uloop.interval(1000, publishMessage);
    },
    on_disconnect: function(client) {
        print("Publisher disconnected (SSL)\n");
        if (timer) {
            uloop.cancel(timer);
        }
    },
    on_publish: function(client, mid) {
        // Optional: confirm message was sent
        // print(`Message delivered with ID: ${mid}\n`);
    },
    tls: true,
    ca_cert: "./examples/mosquitto.org.crt"
});

if (client) {
    print("Connecting publisher to test.mosquitto.org:8883 with SSL...\n");

    // Run the event loop
    uloop.run();
} else {
    print("Failed to initiate connection\n");
}

// Cleanup
uloop.done();