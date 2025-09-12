#!/usr/bin/env ucode

/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

import * as mqtt from "mqtt";
import * as uloop from "uloop";

let client = null;

// Initialize uloop
uloop.init();

// Connect to MQTT broker
client = mqtt.connect("test.mosquitto.org", 1883, "ucode-subscriber", {
    on_connect: function(client) {
        print("Subscriber connected to MQTT broker!\n");

        // Subscribe to the counter topic
        if (client.subscribe("test/ucode/counter", 0)) {
            print("Subscribed to test/ucode/counter\n");
            print("Waiting for messages...\n");
        } else {
            print("Failed to subscribe\n");
            uloop.end();
        }
    },
    on_disconnect: function(client) {
        print("Subscriber disconnected\n");
    },
    on_message: function(client, message) {
        let timestamp = time();
        print(`[${timestamp}] Received: ${message.payload}\n`);
    }
});

if (client) {
    print("Connecting subscriber to test.mosquitto.org:1883...\n");

    // Run the event loop
    uloop.run();
} else {
    print("Failed to initiate connection\n");
}

// Cleanup
uloop.done();