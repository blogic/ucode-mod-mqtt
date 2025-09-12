/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

#include "mqtt.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Helper Functions
 * -------------------------------------------------------------------------- */

/**
 * mqtt_vm_call() - Call a function already pushed on the VM stack
 * @vm: ucode VM context
 * @nargs: Number of arguments pushed after the function
 *
 * Wrapper around uc_vm_call() that mirrors the exception semantics of the
 * uloop module: on an exception the handler installed via uloop.guard() is
 * invoked when present, otherwise the event loop is terminated. The return
 * value only exists on the stack when no exception occurred.
 *
 * Return: true if the call completed without exception
 */
static bool mqtt_vm_call(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *exh, *val;

	if (uc_vm_call(vm, false, nargs) == EXCEPTION_NONE)
		return true;

	exh = uc_vm_registry_get(vm, "uloop.ex_handler");
	if (!ucv_is_callable(exh))
		goto error;

	val = uc_vm_exception_object(vm);
	uc_vm_stack_push(vm, ucv_get(exh));
	uc_vm_stack_push(vm, val);

	if (uc_vm_call(vm, false, 1) != EXCEPTION_NONE)
		goto error;

	ucv_put(uc_vm_stack_pop(vm));

	return false;

error:
	uloop_end();
	return false;
}

/**
 * invoke_callback() - Execute a ucode callback function
 * @client: MQTT client instance
 * @slot: Resource value slot holding the callback
 * @arg: Optional extra argument, ownership is transferred (may be NULL)
 *
 * Invokes the ucode callback stored in @slot, passing the client resource
 * as the first parameter followed by @arg if present. Exceptions raised by
 * the callback are handled by mqtt_vm_call().
 */
static void invoke_callback(mqtt_client_t *client, size_t slot, uc_value_t *arg)
{
	uc_value_t *callback = ucv_resource_value_get(client->res, slot);
	uc_vm_t *vm = client->vm;

	if (!ucv_is_callable(callback)) {
		ucv_put(arg);
		return;
	}

	uc_vm_stack_push(vm, ucv_get(callback));
	uc_vm_stack_push(vm, ucv_get(client->res));
	if (arg)
		uc_vm_stack_push(vm, arg);

	if (mqtt_vm_call(vm, arg ? 2 : 1))
		ucv_put(uc_vm_stack_pop(vm));
}

/**
 * error_set() - Record the last error message for mqtt.error()
 * @vm: ucode VM context
 * @msg: Error message to store
 */
static void error_set(uc_vm_t *vm, const char *msg) { uc_vm_registry_set(vm, "mqtt.last_error", ucv_string_new(msg)); }

/**
 * get_string_option() - Extract a string value from options object
 * @options: Options object to search
 * @key: Key to look up
 *
 * Returns a dynamically allocated copy of the string value or NULL if not found.
 *
 * Return: Allocated string copy or NULL
 */
static char *get_string_option(uc_value_t *options, const char *key)
{
	uc_value_t *val = ucv_object_get(options, key, NULL);
	if (val && ucv_type(val) == UC_STRING)
		return strdup(ucv_string_get(val));
	return NULL;
}

/**
 * set_callback_if_valid() - Store a callback from options in a resource slot
 * @res: Client resource object
 * @slot: Embedded value slot to store the callback in
 * @options: Options object
 * @key: Callback key name
 *
 * Callbacks live in embedded resource value slots because plain C structure
 * fields are invisible to the garbage collector and would be freed under it.
 */
static void set_callback_if_valid(uc_value_t *res, size_t slot, uc_value_t *options, const char *key)
{
	uc_value_t *cb = ucv_object_get(options, key, NULL);
	if (cb && ucv_is_callable(cb))
		ucv_resource_value_set(res, slot, ucv_get(cb));
}

/**
 * extract_callbacks() - Extract callback functions from options
 * @client: MQTT client to configure
 * @options: Options object containing callbacks
 *
 * Extracts and stores references to callback functions from the options object.
 * Callbacks supported: on_connect, on_reconnect, on_disconnect, on_message,
 * on_publish, on_error.
 */
static void extract_callbacks(mqtt_client_t *client, uc_value_t *options)
{
	set_callback_if_valid(client->res, MQTT_SLOT_ON_CONNECT, options, "on_connect");
	set_callback_if_valid(client->res, MQTT_SLOT_ON_RECONNECT, options, "on_reconnect");
	set_callback_if_valid(client->res, MQTT_SLOT_ON_DISCONNECT, options, "on_disconnect");
	set_callback_if_valid(client->res, MQTT_SLOT_ON_MESSAGE, options, "on_message");
	set_callback_if_valid(client->res, MQTT_SLOT_ON_PUBLISH, options, "on_publish");
	set_callback_if_valid(client->res, MQTT_SLOT_ON_ERROR, options, "on_error");
}

/**
 * extract_reconnect_options() - Extract reconnection settings from options
 * @client: MQTT client to configure
 * @options: Options object containing reconnect settings (may be NULL)
 *
 * Configures automatic reconnection behavior including delay, max delay,
 * and attempt limits. Defaults: enabled, 5s delay, 30s max, infinite attempts.
 *
 * Return: false when an option failed validation
 */
static bool extract_reconnect_options(mqtt_client_t *client, uc_value_t *options)
{
	uc_value_t *val;

	val = ucv_object_get(options, "reconnect", NULL);
	client->auto_reconnect = val ? ucv_boolean_get(val) : true;

	if (!mqtt_uint_option_get(client->vm, options, "reconnect_delay", 5, 1, 3600, &client->reconnect_delay))
		return false;

	if (!mqtt_uint_option_get(client->vm, options, "reconnect_delay_max", 30, client->reconnect_delay, 86400,
	                          &client->reconnect_delay_max))
		return false;

	return mqtt_uint_option_get(client->vm, options, "reconnect_attempts", 0, 0, INT_MAX,
	                            &client->reconnect_attempts);
}

/* --------------------------------------------------------------------------
 * Resource Management
 * -------------------------------------------------------------------------- */

/**
 * client_active_set() - Acquire or drop the module's hold on the client
 * @client: MQTT client to update
 * @active: Whether the connection is in use by the event loop
 *
 * While a connection is active the resource may have no ucode-visible
 * references, yet mosquitto and uloop still hold pointers into it. Take an
 * own reference and mark the resource persistent so neither reference
 * counting nor the garbage collector can free it. Dropping the hold may
 * free the client immediately, so callers must not touch it afterwards.
 */
static void client_active_set(mqtt_client_t *client, bool active)
{
	if (client->active == active)
		return;

	client->active = active;

	if (active) {
		ucv_get(client->res);
		ucv_resource_persistent_set(client->res, true);
		return;
	}

	ucv_resource_persistent_set(client->res, false);
	ucv_put(client->res);
}

/**
 * client_release_cb() - Deferred release of an idle client
 * @t: Release timer embedded in the client
 *
 * Skips the release when the connection was resurrected (for example by a
 * reconnect() call from a callback) between scheduling and expiry.
 */
static void client_release_cb(struct uloop_timeout *t)
{
	mqtt_client_t *client = container_of(t, mqtt_client_t, release_timer);

	if (mosquitto_socket(client->mosq) >= 0 || client->reconnect_timer.pending)
		return;

	client_active_set(client, false);
}

/**
 * client_release_schedule() - Schedule dropping the module's client hold
 * @client: MQTT client that became idle
 *
 * The release must be deferred to a timer because it may run
 * mosquitto_destroy(), which is not allowed from within a mosquitto
 * callback such as on_disconnect.
 */
static void client_release_schedule(mqtt_client_t *client) { uloop_timeout_set(&client->release_timer, 0); }

static void mqtt_reconnect_schedule(mqtt_client_t *client);

/**
 * reconnect_timer_cb() - Backoff timer expiry, attempt the reconnect
 * @t: Reconnect timer embedded in the client
 *
 * Initiates an asynchronous reconnect. On immediate failure (for example
 * DNS resolution) the next attempt is scheduled; failures of an initiated
 * connect surface through on_disconnect which reschedules as well.
 */
static void reconnect_timer_cb(struct uloop_timeout *t)
{
	mqtt_client_t *client = container_of(t, mqtt_client_t, reconnect_timer);

	client->reconnect_count++;
	client->is_reconnect = true;

	if (mosquitto_reconnect_async(client->mosq) == MOSQ_ERR_SUCCESS) {
		mqtt_fd_events_update(client);
		mqtt_misc_start(client);
		return;
	}

	mqtt_reconnect_schedule(client);
}

/**
 * mqtt_reconnect_schedule() - Arm the auto-reconnect backoff timer
 * @client: MQTT client that lost its connection
 *
 * With an external event loop libmosquitto never reconnects on its own;
 * mosquitto_reconnect_delay_set() only applies to mosquitto_loop_forever().
 * The module therefore drives reconnection through this timer. When the
 * configured attempt limit is exhausted, on_error is invoked and the
 * client is released.
 */
static void mqtt_reconnect_schedule(mqtt_client_t *client)
{
	if (client->reconnect_timer.pending)
		return;

	if (client->reconnect_attempts && client->reconnect_count >= client->reconnect_attempts) {
		invoke_callback(client, MQTT_SLOT_ON_ERROR, ucv_string_new("reconnect attempts exhausted"));
		client_release_schedule(client);
		return;
	}

	client->reconnect_timer.cb = reconnect_timer_cb;
	uloop_timeout_set(
	    &client->reconnect_timer,
	    mqtt_backoff_delay_get(client->reconnect_delay, client->reconnect_delay_max, client->reconnect_count) *
	        1000);
}

/**
 * mqtt_client_free() - Resource destructor for MQTT client
 * @ptr: Pointer to MQTT client structure
 *
 * Cleans up all C resources associated with an MQTT client. The structure
 * itself is embedded in the extended resource and the callback slots are
 * released by the VM, so neither is freed here.
 */
static void mqtt_client_free(void *ptr)
{
	mqtt_client_t *client = ptr;

	if (!client)
		return;

	uloop_timeout_cancel(&client->misc_timer);
	uloop_timeout_cancel(&client->reconnect_timer);
	uloop_timeout_cancel(&client->release_timer);

	if (client->ufd.registered)
		mqtt_cleanup_uloop(client);

	if (client->mosq)
		mosquitto_destroy(client->mosq);

	free(client->client_id);
	free(client->broker);
	free(client->username);
	free(client->password);
	free(client->ca_cert_file);
	free(client->ca_cert_path);
	free(client->cert_file);
	free(client->key_file);
}

/**
 * cleanup_all() - Module cleanup handler
 *
 * Called at process exit to cleanup mosquitto library.
 */
static void cleanup_all(void) { mosquitto_lib_cleanup(); }

/* --------------------------------------------------------------------------
 * Mosquitto Callbacks
 * -------------------------------------------------------------------------- */

/**
 * on_connect() - Mosquitto connection callback
 * @mosq: Mosquitto instance
 * @obj: User data pointer (mqtt_client_t)
 * @result: Connection result code (0 = success)
 *
 * Handles connection events, sets up uloop, and invokes appropriate user
 * callbacks: on_connect or on_reconnect on success, on_error with the
 * CONNACK reason when the broker refused the connection.
 */
static void on_connect(struct mosquitto *mosq, void *obj, int result)
{
	mqtt_client_t *client = (mqtt_client_t *)obj;
	size_t slot = MQTT_SLOT_ON_CONNECT;

	client->connected = (result == 0);

	if (!client->connected) {
		invoke_callback(client, MQTT_SLOT_ON_ERROR, ucv_string_new(mosquitto_connack_string(result)));
		return;
	}

	client->reconnect_count = 0;

	mqtt_fd_events_update(client);

	/* Fall back to on_connect if on_reconnect not available */
	if (client->is_reconnect && ucv_is_callable(ucv_resource_value_get(client->res, MQTT_SLOT_ON_RECONNECT)))
		slot = MQTT_SLOT_ON_RECONNECT;

	client->is_reconnect = false;
	invoke_callback(client, slot, NULL);
}

/**
 * on_disconnect() - Mosquitto disconnection callback
 * @mosq: Mosquitto instance
 * @obj: User data pointer (mqtt_client_t)
 * @result: Disconnection reason code (0 = client disconnect)
 *
 * Handles disconnection events, resynchronises uloop monitoring with the
 * closed socket and schedules the client release unless an automatic
 * reconnect is expected.
 */
static void on_disconnect(struct mosquitto *mosq, void *obj, int result)
{
	mqtt_client_t *client = (mqtt_client_t *)obj;
	client->connected = false;

	uloop_timeout_cancel(&client->misc_timer);
	mqtt_fd_events_update(client);

	invoke_callback(client, MQTT_SLOT_ON_DISCONNECT, NULL);

	if (client->auto_reconnect && result != 0)
		mqtt_reconnect_schedule(client);
	else
		client_release_schedule(client);
}

/**
 * on_message() - Mosquitto message receive callback
 * @mosq: Mosquitto instance
 * @obj: User data pointer (mqtt_client_t)
 * @message: Received MQTT message structure
 *
 * Processes incoming MQTT messages and invokes the user's on_message callback
 * with message details (topic, payload, QoS, retain).
 */
static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
	mqtt_client_t *client = (mqtt_client_t *)obj;
	uc_value_t *msg_obj;

	if (!ucv_is_callable(ucv_resource_value_get(client->res, MQTT_SLOT_ON_MESSAGE)))
		return;

	msg_obj = ucv_object_new(client->vm);
	ucv_object_add(msg_obj, "topic", ucv_string_new(message->topic));
	ucv_object_add(msg_obj, "payload", ucv_string_new_length(message->payload, message->payloadlen));
	ucv_object_add(msg_obj, "qos", ucv_int64_new(message->qos));
	ucv_object_add(msg_obj, "retain", ucv_boolean_new(message->retain));

	invoke_callback(client, MQTT_SLOT_ON_MESSAGE, msg_obj);
}

/**
 * on_publish() - Mosquitto publish complete callback
 * @mosq: Mosquitto instance
 * @obj: User data pointer (mqtt_client_t)
 * @mid: Message ID of published message
 *
 * Called when a message publication is complete. Invokes user's
 * on_publish callback with the message ID.
 */
static void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
	mqtt_client_t *client = (mqtt_client_t *)obj;

	invoke_callback(client, MQTT_SLOT_ON_PUBLISH, ucv_int64_new(mid));
}

/**
 * on_log() - Mosquitto logging callback
 * @mosq: Mosquitto instance
 * @obj: User data pointer (unused)
 * @level: Log level (MOSQ_LOG_*)
 * @str: Log message string
 *
 * Formats and outputs mosquitto library log messages to stderr.
 */
static void on_log(struct mosquitto *mosq, void *obj, int level, const char *str)
{
	const char *level_str;
	switch (level) {
	case MOSQ_LOG_DEBUG:
		level_str = "DEBUG";
		break;
	case MOSQ_LOG_INFO:
		level_str = "INFO";
		break;
	case MOSQ_LOG_NOTICE:
		level_str = "NOTICE";
		break;
	case MOSQ_LOG_WARNING:
		level_str = "WARNING";
		break;
	case MOSQ_LOG_ERR:
		level_str = "ERROR";
		break;
	default:
		level_str = "UNKNOWN";
		break;
	}
	fprintf(stderr, "[MOSQUITTO %s] %s\n", level_str, str);
}

/**
 * setup_mosquitto_callbacks() - Configure all mosquitto callbacks
 * @mosq: Mosquitto instance to configure
 *
 * Sets up all mosquitto library callbacks for connection, disconnection,
 * message receipt, publish completion, and logging.
 */
static void setup_mosquitto_callbacks(struct mosquitto *mosq)
{
	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_disconnect_callback_set(mosq, on_disconnect);
	mosquitto_message_callback_set(mosq, on_message);
	mosquitto_publish_callback_set(mosq, on_publish);
}

/**
 * setup_tls() - Configure TLS/SSL for MQTT connection
 * @client: MQTT client to configure
 *
 * Sets up TLS using either custom certificates or system CA bundle.
 * Falls back to system certificates if no custom CA is specified.
 *
 * Return: MOSQ_ERR_SUCCESS on success, error code otherwise
 */
static int setup_tls(mqtt_client_t *client)
{
	int rc;

	if (client->ca_cert_file || client->ca_cert_path) {
		rc = mosquitto_tls_set(client->mosq, client->ca_cert_file, client->ca_cert_path, client->cert_file,
		                       client->key_file, NULL);
	}
	else if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0) {
		rc = mosquitto_tls_set(client->mosq, "/etc/ssl/certs/ca-certificates.crt", NULL, NULL, NULL, NULL);
	}
	else {
		rc = mosquitto_tls_set(client->mosq, NULL, "/etc/ssl/certs", NULL, NULL, NULL);
	}

	if (rc != MOSQ_ERR_SUCCESS)
		error_set(client->vm, mosquitto_strerror(rc));

	return rc;
}

/* --------------------------------------------------------------------------
 * Module Functions
 * -------------------------------------------------------------------------- */

/**
 * setup_will() - Configure the last will testament from options
 * @client: MQTT client to configure
 * @options: Options object (may be NULL)
 *
 * Parses the will option object with topic (string, required), payload
 * (string), qos (0-2) and retain (boolean) members and registers it with
 * mosquitto before connecting.
 *
 * Return: false when the will could not be validated or set
 */
static bool setup_will(mqtt_client_t *client, uc_value_t *options)
{
	uc_value_t *will = ucv_object_get(options, "will", NULL);

	if (!will)
		return true;

	if (ucv_type(will) != UC_OBJECT) {
		uc_vm_raise_exception(client->vm, EXCEPTION_TYPE, "option 'will' must be an object");
		return false;
	}

	uc_value_t *topic = ucv_object_get(will, "topic", NULL);
	uc_value_t *payload = ucv_object_get(will, "payload", NULL);
	uc_value_t *retain = ucv_object_get(will, "retain", NULL);

	if (!topic || ucv_type(topic) != UC_STRING) {
		uc_vm_raise_exception(client->vm, EXCEPTION_TYPE, "will topic must be a string");
		return false;
	}

	if (payload && ucv_type(payload) != UC_STRING) {
		uc_vm_raise_exception(client->vm, EXCEPTION_TYPE, "will payload must be a string");
		return false;
	}

	int qos_val;
	if (!mqtt_qos_arg_get(client->vm, ucv_object_get(will, "qos", NULL), &qos_val))
		return false;

	int rc = mosquitto_will_set(client->mosq, ucv_string_get(topic), payload ? ucv_string_length(payload) : 0,
	                            payload ? ucv_string_get(payload) : NULL, qos_val,
	                            retain ? ucv_boolean_get(retain) : false);

	if (rc != MOSQ_ERR_SUCCESS) {
		error_set(client->vm, mosquitto_strerror(rc));
		return false;
	}

	return true;
}

/**
 * uc_mqtt_connect() - Create and connect MQTT client
 * @vm: ucode VM context
 * @nargs: Number of arguments
 *
 * Creates a new MQTT client resource and initiates connection to broker.
 * Arguments: broker (string), port (int), client_id (string), options (object)
 * Options may include callbacks, credentials, TLS settings, and reconnect config.
 *
 * Return: Client resource object or NULL on failure
 */
static uc_value_t *uc_mqtt_connect(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *broker = uc_fn_arg(0);
	uc_value_t *port = uc_fn_arg(1);
	uc_value_t *client_id = uc_fn_arg(2);
	uc_value_t *options = uc_fn_arg(3);

	if (!broker || ucv_type(broker) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "broker must be a string");
		return NULL;
	}

	if (port && ucv_type(port) != UC_INTEGER) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "port must be an integer");
		return NULL;
	}

	int64_t port_val = port ? ucv_int64_get(port) : 1883;
	if (port_val < 1 || port_val > 65535) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "port must be between 1 and 65535");
		return NULL;
	}

	if (client_id && ucv_type(client_id) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "client_id must be a string");
		return NULL;
	}

	if (options && ucv_type(options) != UC_OBJECT) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "options must be an object");
		return NULL;
	}

	mqtt_client_t *client;
	uc_value_t *res =
	    ucv_resource_create_ex(vm, "mqtt.client", (void **)&client, MQTT_SLOT_MAX, sizeof(mqtt_client_t));
	if (!res)
		return NULL;

	client->vm = vm;
	client->res = res;
	client->release_timer.cb = client_release_cb;
	client->broker = strdup(ucv_string_get(broker));
	client->port = port_val;

	const char *cid = client_id ? ucv_string_get(client_id) : NULL;
	client->client_id = cid ? strdup(cid) : NULL;

	client->mosq = mosquitto_new(client->client_id, true, client);
	if (!client->mosq) {
		error_set(vm, strerror(errno));
		ucv_put(res);
		return NULL;
	}

	setup_mosquitto_callbacks(client->mosq);

	extract_callbacks(client, options);

	if (!extract_reconnect_options(client, options)) {
		ucv_put(res);
		return NULL;
	}

	/* Store credentials for reconnect */
	uc_value_t *username = ucv_object_get(options, "username", NULL);
	uc_value_t *password = ucv_object_get(options, "password", NULL);

	if (username && ucv_type(username) == UC_STRING) {
		client->username = strdup(ucv_string_get(username));
		client->password =
		    (password && ucv_type(password) == UC_STRING) ? strdup(ucv_string_get(password)) : NULL;
		mosquitto_username_pw_set(client->mosq, client->username, client->password);
	}

	client->ca_cert_file = get_string_option(options, "ca_cert");
	client->ca_cert_path = get_string_option(options, "ca_path");
	client->cert_file = get_string_option(options, "cert");
	client->key_file = get_string_option(options, "key");

	uc_value_t *tls = ucv_object_get(options, "tls", NULL);
	if (tls && ucv_type(tls) == UC_BOOLEAN && ucv_boolean_get(tls)) {
		if (setup_tls(client) != MOSQ_ERR_SUCCESS) {
			ucv_put(res);
			return NULL;
		}
	}

	uc_value_t *debug = ucv_object_get(options, "debug", NULL);
	if (debug && ucv_boolean_get(debug))
		mosquitto_log_callback_set(client->mosq, on_log);

	/* libmosquitto rejects keepalive values below 5 seconds */
	if (!mqtt_uint_option_get(vm, options, "keepalive", 60, 5, 65535, &client->keepalive)) {
		ucv_put(res);
		return NULL;
	}

	if (!setup_will(client, options)) {
		ucv_put(res);
		return NULL;
	}

	int rc = mosquitto_connect_async(client->mosq, client->broker, client->port, client->keepalive);

	if (rc != MOSQ_ERR_SUCCESS) {
		error_set(vm, rc == MOSQ_ERR_ERRNO ? strerror(errno) : mosquitto_strerror(rc));
		ucv_put(res);
		return NULL;
	}

	mqtt_fd_events_update(client);
	if (!client->ufd.registered) {
		error_set(vm, "failed to register mosquitto socket with uloop");
		ucv_put(res);
		return NULL;
	}

	mqtt_misc_start(client);
	client_active_set(client, true);
	return res;
}

/* --------------------------------------------------------------------------
 * Client Methods
 * -------------------------------------------------------------------------- */

/**
 * validate_client() - Validate client and mosquitto instance
 * @client: Client to validate
 *
 * Return: true if valid, false otherwise
 */
static inline bool validate_client(mqtt_client_t *client) { return client && client->mosq; }

/**
 * uc_mqtt_client_disconnect() - Disconnect MQTT client from broker
 * @vm: ucode VM context
 * @nargs: Number of arguments (unused)
 *
 * Sends a clean disconnect to the MQTT broker.
 *
 * Return: Boolean true on success, false on failure
 */
static uc_value_t *uc_mqtt_client_disconnect(uc_vm_t *vm, size_t nargs)
{
	mqtt_client_t *client = uc_fn_thisval("mqtt.client");

	if (!validate_client(client))
		return ucv_boolean_new(false);

	int rc = mosquitto_disconnect(client->mosq);

	if (client->reconnect_timer.pending) {
		uloop_timeout_cancel(&client->reconnect_timer);
		client_release_schedule(client);
	}

	if (rc != MOSQ_ERR_SUCCESS)
		error_set(vm, mosquitto_strerror(rc));
	return ucv_boolean_new(rc == MOSQ_ERR_SUCCESS);
}

/**
 * uc_mqtt_client_is_connected() - Check MQTT connection status
 * @vm: ucode VM context
 * @nargs: Number of arguments (unused)
 *
 * Checks whether the client is currently connected to the broker.
 *
 * Return: Boolean true if connected, false otherwise
 */
static uc_value_t *uc_mqtt_client_is_connected(uc_vm_t *vm, size_t nargs)
{
	mqtt_client_t *client = uc_fn_thisval("mqtt.client");

	return ucv_boolean_new(client && client->connected);
}

/**
 * uc_mqtt_client_publish() - Publish message to MQTT topic
 * @vm: ucode VM context
 * @nargs: Number of arguments
 *
 * Publishes a message to the specified topic.
 * Arguments: topic (string), message (string), qos (int), retain (bool)
 *
 * Return: Boolean true on success, false on failure
 */
static uc_value_t *uc_mqtt_client_publish(uc_vm_t *vm, size_t nargs)
{
	mqtt_client_t *client = uc_fn_thisval("mqtt.client");
	uc_value_t *topic = uc_fn_arg(0);
	uc_value_t *message = uc_fn_arg(1);
	uc_value_t *qos = uc_fn_arg(2);
	uc_value_t *retain = uc_fn_arg(3);

	if (!topic || ucv_type(topic) != UC_STRING || !message || ucv_type(message) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "topic and message must be strings");
		return NULL;
	}

	int qos_val;
	if (!mqtt_qos_arg_get(vm, qos, &qos_val))
		return NULL;

	if (!validate_client(client))
		return ucv_boolean_new(false);

	const char *topic_str = ucv_string_get(topic);
	const char *message_str = ucv_string_get(message);
	size_t message_len = ucv_string_length(message);
	bool retain_val = retain ? ucv_boolean_get(retain) : false;

	int rc = mosquitto_publish(client->mosq, NULL, topic_str, message_len, message_str, qos_val, retain_val);

	mqtt_fd_events_update(client);
	if (rc != MOSQ_ERR_SUCCESS)
		error_set(vm, mosquitto_strerror(rc));
	return ucv_boolean_new(rc == MOSQ_ERR_SUCCESS);
}

/**
 * uc_mqtt_client_subscribe() - Subscribe to MQTT topic
 * @vm: ucode VM context
 * @nargs: Number of arguments
 *
 * Subscribes to the specified topic with optional QoS level.
 * Arguments: topic (string), qos (int)
 *
 * Return: Boolean true on success, false on failure
 */
static uc_value_t *uc_mqtt_client_subscribe(uc_vm_t *vm, size_t nargs)
{
	mqtt_client_t *client = uc_fn_thisval("mqtt.client");
	uc_value_t *topic = uc_fn_arg(0);
	uc_value_t *qos = uc_fn_arg(1);

	if (!topic || ucv_type(topic) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "topic must be a string");
		return NULL;
	}

	int qos_val;
	if (!mqtt_qos_arg_get(vm, qos, &qos_val))
		return NULL;

	if (!validate_client(client))
		return ucv_boolean_new(false);

	const char *topic_str = ucv_string_get(topic);

	int rc = mosquitto_subscribe(client->mosq, NULL, topic_str, qos_val);

	mqtt_fd_events_update(client);
	if (rc != MOSQ_ERR_SUCCESS)
		error_set(vm, mosquitto_strerror(rc));
	return ucv_boolean_new(rc == MOSQ_ERR_SUCCESS);
}

/**
 * uc_mqtt_client_unsubscribe() - Unsubscribe from MQTT topic
 * @vm: ucode VM context
 * @nargs: Number of arguments
 *
 * Unsubscribes from the specified topic.
 * Arguments: topic (string)
 *
 * Return: Boolean true on success, false on failure
 */
static uc_value_t *uc_mqtt_client_unsubscribe(uc_vm_t *vm, size_t nargs)
{
	mqtt_client_t *client = uc_fn_thisval("mqtt.client");
	uc_value_t *topic = uc_fn_arg(0);

	if (!topic || ucv_type(topic) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "topic must be a string");
		return NULL;
	}

	if (!validate_client(client))
		return ucv_boolean_new(false);

	const char *topic_str = ucv_string_get(topic);
	int rc = mosquitto_unsubscribe(client->mosq, NULL, topic_str);

	mqtt_fd_events_update(client);
	if (rc != MOSQ_ERR_SUCCESS)
		error_set(vm, mosquitto_strerror(rc));
	return ucv_boolean_new(rc == MOSQ_ERR_SUCCESS);
}

/**
 * uc_mqtt_client_reconnect() - Manually trigger reconnection
 * @vm: ucode VM context
 * @nargs: Number of arguments (unused)
 *
 * Initiates a manual reconnection to the MQTT broker.
 * Uses mosquitto's async reconnect to maintain the existing instance.
 *
 * Return: Boolean true on success, false on failure
 */
static uc_value_t *uc_mqtt_client_reconnect(uc_vm_t *vm, size_t nargs)
{
	mqtt_client_t *client = uc_fn_thisval("mqtt.client");

	if (!validate_client(client))
		return ucv_boolean_new(false);

	uloop_timeout_cancel(&client->reconnect_timer);
	uloop_timeout_cancel(&client->release_timer);
	client_active_set(client, true);
	client->reconnect_count = 0;
	client->is_reconnect = true;

	int rc = mosquitto_reconnect_async(client->mosq);

	if (rc == MOSQ_ERR_SUCCESS) {
		mqtt_fd_events_update(client);
		mqtt_misc_start(client);
		return ucv_boolean_new(true);
	}

	error_set(vm, mosquitto_strerror(rc));
	client_release_schedule(client);
	return ucv_boolean_new(false);
}

/**
 * uc_mqtt_error() - Retrieve and clear the last error message
 * @vm: ucode VM context
 * @nargs: Number of arguments (unused)
 *
 * Return: Last error message string or NULL if no error occurred
 */
static uc_value_t *uc_mqtt_error(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *err = uc_vm_registry_get(vm, "mqtt.last_error");

	if (!err)
		return NULL;

	err = ucv_get(err);
	uc_vm_registry_delete(vm, "mqtt.last_error");
	return err;
}

/* --------------------------------------------------------------------------
 * Module Registration
 * -------------------------------------------------------------------------- */

static const uc_function_list_t mqtt_client_methods[] = {
    {"disconnect", uc_mqtt_client_disconnect},     {"reconnect", uc_mqtt_client_reconnect},
    {"is_connected", uc_mqtt_client_is_connected}, {"publish", uc_mqtt_client_publish},
    {"subscribe", uc_mqtt_client_subscribe},       {"unsubscribe", uc_mqtt_client_unsubscribe},
};

static const uc_function_list_t module_functions[] = {
    {"connect", uc_mqtt_connect},
    {"error", uc_mqtt_error},
};

/**
 * uc_module_entry() - Module initialization entry point
 * @vm: ucode VM context
 * @scope: Module scope object
 *
 * Initializes the MQTT module, registers types and functions,
 * and sets up cleanup handlers.
 */
void uc_module_entry(uc_vm_t *vm, uc_value_t *scope) __attribute__((used));
void uc_module_entry(uc_vm_t *vm, uc_value_t *scope)
{
	mosquitto_lib_init();

	uc_type_declare(vm, "mqtt.client", mqtt_client_methods, mqtt_client_free);

	uc_function_list_register(scope, module_functions);

	atexit(cleanup_all);
}