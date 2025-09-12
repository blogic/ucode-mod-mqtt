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
client = mqtt.connect("test.mosquitto.org", 1883, "ucode-test-client", {
    on_connect: function(client) {
        print("Connected to MQTT broker!\n");

        // Subscribe to a test topic
        if (client.subscribe("test/ucode/+", 0)) {
            print("Subscribed to test/ucode/+\n");
        }

        // Publish a test message
        if (client.publish("test/ucode/hello", "Hello from ucode!", 0, false)) {
            print("Published test message\n");
        }
    },
    on_disconnect: function(client) {
        print("Disconnected from broker\n");
    },
    on_message: function(client, message) {
        print("Received message:\n");
        print("  Topic: " + message.topic + "\n");
        print("  Payload: " + message.payload + "\n");
        print("  QoS: " + message.qos + "\n");
        print("  Retain: " + message.retain + "\n");
    },
    on_publish: function(client, mid) {
        print("Message published with ID: " + mid + "\n");
    }
});

if (client) {
    print("Connecting to test.mosquitto.org:1883...\n");

    // Run the event loop
    uloop.run();
} else {
    print("Failed to initiate connection\n");
}

// Cleanup
uloop.done();