/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>

#include <app/gateway/memory_table.h>

void gateway_memory_init(struct gateway_memory *memory)
{
        if (memory == NULL) {
                return;
        }

        for (size_t i = 0; i < GATEWAY_MEMORY_MAX_NODES; i++) {
                memory->entries[i].valid = false;
        }
}

int gateway_memory_update(struct gateway_memory *memory,
                          const struct sensor_reading *reading,
                          uint32_t received_at_ms)
{
        if (memory == NULL || reading == NULL) {
                return -EINVAL;
        }

        memory->entries[reading->node_id].valid = true;
        memory->entries[reading->node_id].reading = *reading;
        memory->entries[reading->node_id].received_at_ms = received_at_ms;

        return 0;
}

int gateway_memory_get(struct gateway_memory *memory,
                       uint8_t node_id,
                       struct gateway_memory_entry *entry)
{
        if (memory == NULL || entry == NULL) {
                return -EINVAL;
        }

        if (!memory->entries[node_id].valid) {
                return -ENOENT;
        }

        *entry = memory->entries[node_id];

        return 0;
}
