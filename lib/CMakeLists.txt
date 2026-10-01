# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_CUSTOM custom)
add_subdirectory_ifdef(CONFIG_RADIO radio)
add_subdirectory_ifdef(CONFIG_GATEWAY_MEMORY gateway_memory)
