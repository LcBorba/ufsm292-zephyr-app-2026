/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_GATEWAY_MEMORY_TABLE_H_
#define APP_GATEWAY_MEMORY_TABLE_H_

#include <stdbool.h>
#include <stdint.h>

#include <app/lib/sensor_frame.h>

/* Number of stored nodes, not a limit on the uint8_t node ID range. */
#define GATEWAY_MEMORY_MAX_NODES 16

struct gateway_memory_entry {
        bool valid;
        struct sensor_reading reading;
        uint32_t received_at_ms;
};

struct gateway_memory {
        struct gateway_memory_entry entries[GATEWAY_MEMORY_MAX_NODES];
};

/* Serialize init/update/get when sharing the table between threads. */
void gateway_memory_init(struct gateway_memory *memory);

/* Returns -ENOSPC for a new node when full; existing nodes still update. */
int gateway_memory_update(struct gateway_memory *memory,
                          const struct sensor_reading *reading,
                          uint32_t received_at_ms);

int gateway_memory_get(struct gateway_memory *memory,
                       uint8_t node_id,
                       struct gateway_memory_entry *entry);

#endif
