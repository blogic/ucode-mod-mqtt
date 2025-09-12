/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

#include "mqtt.h"
#include <errno.h>
#include <unistd.h>

#define MQTT_MISC_INTERVAL_MS 1000

/**
 * mqtt_uloop_cb() - uloop file descriptor callback
 * @ufd: uloop file descriptor structure
 * @events: Event mask (ULOOP_READ, ULOOP_WRITE)
 *
 * Handles I/O events for the MQTT connection by calling mosquitto_loop
 * to process pending read/write operations, then resynchronises the
 * registered event mask with the resulting mosquitto state.
 */
void mqtt_uloop_cb(struct uloop_fd *ufd, unsigned int events)
{
	mqtt_client_t *client = container_of(ufd, mqtt_client_t, ufd);
	int rc;

	rc = mosquitto_loop(client->mosq, 0, 1);
	switch (rc) {
	case MOSQ_ERR_SUCCESS:
	/* Connection loss and refusal are reported through the callbacks */
	case MOSQ_ERR_CONN_LOST:
	case MOSQ_ERR_NO_CONN:
	case MOSQ_ERR_CONN_REFUSED:
		break;
	case MOSQ_ERR_ERRNO:
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			fprintf(stderr, "mosquitto_loop failed with errno: %s\n", strerror(errno));
		break;
	default:
		fprintf(stderr, "mosquitto_loop failed: %s (code: %d)\n", mosquitto_strerror(rc), rc);
		break;
	}

	mqtt_fd_events_update(client);
}

/**
 * mqtt_fd_events_update() - Synchronise uloop monitoring with mosquitto
 * @client: MQTT client to update
 *
 * Registers the current mosquitto socket with uloop, monitoring writes
 * only while mosquitto has pending output. Permanently monitoring
 * ULOOP_WRITE would busy-loop the process since a connected socket is
 * almost always writable. Handles socket replacement across reconnects by
 * re-registering when the file descriptor changed, and deregisters when
 * the connection is down.
 */
void mqtt_fd_events_update(mqtt_client_t *client)
{
	unsigned int events = ULOOP_READ;
	int sock = mosquitto_socket(client->mosq);

	if (sock < 0) {
		mqtt_cleanup_uloop(client);
		return;
	}

	if (mosquitto_want_write(client->mosq))
		events |= ULOOP_WRITE;

	if (client->ufd.registered) {
		if (client->ufd.fd == sock && client->ufd_events == events)
			return;
		uloop_fd_delete(&client->ufd);
	}

	client->ufd.fd = sock;
	client->ufd.cb = mqtt_uloop_cb;
	client->ufd_events = events;
	uloop_fd_add(&client->ufd, events);
}

/**
 * mqtt_misc_cb() - Periodic mosquitto housekeeping timer callback
 * @t: Misc timer embedded in the client
 *
 * mosquitto_loop() only runs on socket events, so an idle connection would
 * never send keepalive PINGREQ messages and the broker would drop it after
 * 1.5 times the keepalive interval. mosquitto_loop_misc() performs the
 * keepalive bookkeeping, retransmissions and dead peer detection.
 */
static void mqtt_misc_cb(struct uloop_timeout *t)
{
	mqtt_client_t *client = container_of(t, mqtt_client_t, misc_timer);

	mosquitto_loop_misc(client->mosq);
	mqtt_fd_events_update(client);
	uloop_timeout_set(t, MQTT_MISC_INTERVAL_MS);
}

/**
 * mqtt_misc_start() - Start the periodic mosquitto housekeeping timer
 * @client: MQTT client with an active connection attempt
 */
void mqtt_misc_start(mqtt_client_t *client)
{
	client->misc_timer.cb = mqtt_misc_cb;
	uloop_timeout_set(&client->misc_timer, MQTT_MISC_INTERVAL_MS);
}

/**
 * mqtt_cleanup_uloop() - Unregister MQTT client from uloop
 * @client: MQTT client to unregister
 *
 * Removes the MQTT client's file descriptor from uloop monitoring.
 */
void mqtt_cleanup_uloop(mqtt_client_t *client)
{
	if (client->ufd.registered)
		uloop_fd_delete(&client->ufd);
}