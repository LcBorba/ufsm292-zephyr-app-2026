# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(sensor_frame.c)
zephyr_library_sources(radio_test_pattern.c)

# Raw mode shrinks the net stack to net_pkt.c, leaving net_recv_data()
# undefined: the shared testbed receive path provides it. Non-raw images
# (e.g. the gateway custom L2) get net_recv_data() from the stack, so this
# must stay raw-only to avoid a duplicate symbol.
zephyr_library_sources_ifdef(CONFIG_IEEE802154_RAW_MODE radio_testbed.c)