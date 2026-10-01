/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include <app/gateway/memory_table.h>

static struct gateway_memory memory;

static void *gateway_memory_setup(void)
{
        gateway_memory_init(&memory);
        return NULL;
}

static void gateway_memory_before(void *fixture)
{
        ARG_UNUSED(fixture);
        gateway_memory_init(&memory);
}

ZTEST(gateway_memory, test_empty_table)
{
        struct gateway_memory_entry entry;

        zassert_equal(gateway_memory_get(&memory, 10, &entry),
                      -ENOENT,
                      "empty table should not contain node 10");
}

ZTEST(gateway_memory, test_store_and_get_reading)
{
        struct sensor_reading reading = {
                .node_id = 7,
                .seq = 10,
                .light = 1234,
                .temp_c_x100 = 2350,
                .accel = { 1, 2, 3 },
                .uptime_ms = 5000,
                .flags = 1,
        };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &reading, 9000),
                   "update failed");

        zassert_ok(gateway_memory_get(&memory, 7, &entry),
                   "stored node was not found");

        zassert_true(entry.valid, "entry should be valid");
        zassert_equal(entry.reading.node_id, 7, "wrong node_id");
        zassert_equal(entry.reading.seq, 10, "wrong seq");
        zassert_equal(entry.reading.light, 1234, "wrong light");
        zassert_equal(entry.reading.temp_c_x100, 2350, "wrong temperature");
        zassert_equal(entry.reading.accel[0], 1, "wrong accel[0]");
        zassert_equal(entry.reading.accel[1], 2, "wrong accel[1]");
        zassert_equal(entry.reading.accel[2], 3, "wrong accel[2]");
        zassert_equal(entry.reading.uptime_ms, 5000, "wrong uptime");
        zassert_equal(entry.reading.flags, 1, "wrong flags");
        zassert_equal(entry.received_at_ms, 9000, "wrong reception timestamp");
}

ZTEST(gateway_memory, test_new_reading_replaces_old_reading)
{
        struct sensor_reading first = {
                .node_id = 3,
                .seq = 1,
                .light = 100,
        };
        struct sensor_reading second = {
                .node_id = 3,
                .seq = 2,
                .light = 200,
        };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &first, 1000),
                   "first update failed");

        zassert_ok(gateway_memory_update(&memory, &second, 2000),
                   "second update failed");

        zassert_ok(gateway_memory_get(&memory, 3, &entry),
                   "node was not found");

        zassert_equal(entry.reading.seq, 2,
                      "latest sequence was not stored");
        zassert_equal(entry.reading.light, 200,
                      "latest reading was not stored");
        zassert_equal(entry.received_at_ms, 2000,
                      "latest timestamp was not stored");
}

ZTEST(gateway_memory, test_nodes_are_independent)
{
        struct sensor_reading node_a = {
                .node_id = 1,
                .seq = 10,
                .light = 111,
        };
        struct sensor_reading node_b = {
                .node_id = 2,
                .seq = 20,
                .light = 222,
        };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &node_a, 100),
                   "node 1 update failed");
        zassert_ok(gateway_memory_update(&memory, &node_b, 200),
                   "node 2 update failed");

        zassert_ok(gateway_memory_get(&memory, 1, &entry),
                   "node 1 not found");
        zassert_equal(entry.reading.light, 111, "node 1 data changed");

        zassert_ok(gateway_memory_get(&memory, 2, &entry),
                   "node 2 not found");
        zassert_equal(entry.reading.light, 222, "node 2 data changed");
}

ZTEST(gateway_memory, test_node_id_boundaries)
{
        struct sensor_reading node_zero = {
                .node_id = 0,
                .seq = 1,
        };
        struct sensor_reading node_max = {
                .node_id = 255,
                .seq = 2,
        };
        struct gateway_memory_entry entry;

        zassert_ok(gateway_memory_update(&memory, &node_zero, 10),
                   "node 0 rejected");
        zassert_ok(gateway_memory_update(&memory, &node_max, 20),
                   "node 255 rejected");

        zassert_ok(gateway_memory_get(&memory, 0, &entry),
                   "node 0 not found");
        zassert_equal(entry.reading.seq, 1, "wrong node 0 data");

        zassert_ok(gateway_memory_get(&memory, 255, &entry),
                   "node 255 not found");
        zassert_equal(entry.reading.seq, 2, "wrong node 255 data");
}

ZTEST(gateway_memory, test_null_arguments)
{
        struct sensor_reading reading = { 0 };
        struct gateway_memory_entry entry;

        zassert_equal(gateway_memory_update(NULL, &reading, 0),
                      -EINVAL, "NULL memory accepted");
        zassert_equal(gateway_memory_update(&memory, NULL, 0),
                      -EINVAL, "NULL reading accepted");

        zassert_equal(gateway_memory_get(NULL, 0, &entry),
                      -EINVAL, "NULL memory accepted");
        zassert_equal(gateway_memory_get(&memory, 0, NULL),
                      -EINVAL, "NULL entry accepted");
}

ZTEST_SUITE(gateway_memory, NULL,
            gateway_memory_setup,
            gateway_memory_before,
            NULL, NULL);
