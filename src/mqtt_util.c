/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */

#include "mqtt.h"

/**
 * mqtt_backoff_delay_get() - Compute the exponential backoff delay
 * @delay: Base delay in seconds
 * @delay_max: Upper bound for the delay in seconds
 * @count: Number of attempts made so far
 *
 * Doubles the base delay per attempt made so far, capped at @delay_max.
 *
 * Return: Delay in seconds before the next reconnect attempt
 */
unsigned int mqtt_backoff_delay_get(unsigned int delay, unsigned int delay_max, unsigned int count)
{
	for (unsigned int i = 0; i < count && delay < delay_max; i++)
		delay *= 2;

	if (delay > delay_max)
		delay = delay_max;

	return delay;
}

/**
 * mqtt_uint_option_get() - Extract a bounded unsigned integer option
 * @vm: ucode VM context
 * @options: Options object (may be NULL)
 * @key: Option key name
 * @def: Default when the option is absent
 * @min: Lower bound, values below are clamped
 * @max: Upper bound, values above are clamped
 * @dest: Result storage
 *
 * Return: false when the option is present but not an integer, in which
 * case a type exception has been raised
 */
bool mqtt_uint_option_get(uc_vm_t *vm, uc_value_t *options, const char *key, unsigned int def, unsigned int min,
                          unsigned int max, unsigned int *dest)
{
	uc_value_t *val = ucv_object_get(options, key, NULL);
	int64_t n;

	if (!val) {
		*dest = def;
		return true;
	}

	if (ucv_type(val) != UC_INTEGER) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "option '%s' must be an integer", key);
		return false;
	}

	n = ucv_int64_get(val);
	if (n < (int64_t)min)
		n = min;
	if (n > (int64_t)max)
		n = max;
	*dest = n;
	return true;
}

/**
 * mqtt_qos_arg_get() - Validate an optional QoS argument
 * @vm: ucode VM context
 * @qos: QoS argument value (may be NULL)
 * @dest: Result storage, 0 when the argument is absent
 *
 * Return: false when the argument is invalid, in which case a type
 * exception has been raised
 */
bool mqtt_qos_arg_get(uc_vm_t *vm, uc_value_t *qos, int *dest)
{
	*dest = 0;

	if (!qos)
		return true;

	if (ucv_type(qos) != UC_INTEGER || ucv_int64_get(qos) < 0 || ucv_int64_get(qos) > 2) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "qos must be an integer between 0 and 2");
		return false;
	}

	*dest = ucv_int64_get(qos);
	return true;
}
