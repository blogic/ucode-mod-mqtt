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

// Connect to MQTT broker with auto-reconnect
client = mqtt.connect("test.mosquitto.org", 1883, "ucode-auto-reconnect", {
    on_connect: function(client) {
        print("Connected to MQTT broker!\n");

        // Subscribe to the counter topic
        if (client.subscribe("test/ucode/counter", 0)) {
            print("Subscribed to test/ucode/counter\n");
            print("Waiting for messages...\n");
        } else {
            print("Failed to subscribe\n");
        }
    },
    on_reconnect: function(client) {
        print("Reconnected to MQTT broker!\n");

        // Re-subscribe after reconnection
        if (client.subscribe("test/ucode/counter", 0)) {
            print("Re-subscribed to test/ucode/counter\n");
        }
    },
    on_disconnect: function(client) {
        print("Disconnected from broker - will auto-reconnect in 5 seconds\n");
    },
    on_message: function(client, message) {
        let timestamp = time();
        print(`[${timestamp}] Received: ${message.payload}\n`);
    },
    // Enable automatic reconnection
    reconnect: true,
    reconnect_delay: 5,      // Wait 5 seconds between attempts
    reconnect_attempts: 10   // Try 10 times (use 0 for infinite)
});

if (client) {
    print("Connecting to test.mosquitto.org:1883 with auto-reconnect enabled...\n");

    // Run the event loop
    uloop.run();
} else {
    print("Failed to initiate connection\n");
}

// Cleanup
uloop.done();