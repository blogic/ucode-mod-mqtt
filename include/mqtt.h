/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

#ifndef UCODE_MQTT_H
#define UCODE_MQTT_H

#include <libubox/uloop.h>
#include <mosquitto.h>
#include <ucode/lib.h>
#include <ucode/types.h>
#include <ucode/util.h>
#include <ucode/vm.h>

/* Embedded resource value slots holding the user callbacks (GC-visible) */
enum {
	MQTT_SLOT_ON_CONNECT,
	MQTT_SLOT_ON_RECONNECT,
	MQTT_SLOT_ON_DISCONNECT,
	MQTT_SLOT_ON_MESSAGE,
	MQTT_SLOT_ON_PUBLISH,
	MQTT_SLOT_ON_ERROR,
	MQTT_SLOT_MAX,
};

typedef struct mqtt_client {
	struct mosquitto *mosq;
	struct uloop_fd ufd;
	unsigned int ufd_events; /* Event mask currently registered with uloop */
	struct uloop_timeout misc_timer;
	struct uloop_timeout reconnect_timer;
	struct uloop_timeout release_timer;
	uc_vm_t *vm;
	uc_value_t *res; /* resource handle for this client, owned while active */
	bool active;     /* module holds a reference and the persistent flag */
	bool connected;
	bool is_reconnect; /* Track if this is a reconnection */
	char *client_id;
	char *broker;
	int port;
	unsigned int keepalive; /* seconds */
	/* Reconnect settings */
	bool auto_reconnect;
	unsigned int reconnect_delay;     /* seconds */
	unsigned int reconnect_delay_max; /* max delay for exponential backoff */
	unsigned int reconnect_attempts;  /* 0 = infinite */
	unsigned int reconnect_count;     /* Current attempt count */
	/* TLS settings */
	char *ca_cert_file;
	char *ca_cert_path;
	char *cert_file;
	char *key_file;
	/* Credentials (saved for reconnect) */
	char *username;
	char *password;
} mqtt_client_t;

void mqtt_uloop_cb(struct uloop_fd *ufd, unsigned int events);
void mqtt_fd_events_update(mqtt_client_t *client);
void mqtt_misc_start(mqtt_client_t *client);
void mqtt_cleanup_uloop(mqtt_client_t *client);

unsigned int mqtt_backoff_delay_get(unsigned int delay, unsigned int delay_max, unsigned int count);
bool mqtt_uint_option_get(uc_vm_t *vm, uc_value_t *options, const char *key, unsigned int def, unsigned int min,
                          unsigned int max, unsigned int *dest);
bool mqtt_qos_arg_get(uc_vm_t *vm, uc_value_t *qos, int *dest);

#endif