# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(sensor_frame.c)
zephyr_library_sources(radio_test_pattern.c)

# sensor_frame.c is the shared 802.15.4 codec (encode + decode);
# radio_test_pattern.c is the deterministic seq-keyed payload pinned by the
# host unit tests in tests/lib/radio. Raw-mode images must provide their own
# net_recv_data(): raw mode shrinks the net stack to net_pkt.c, which leaves
# that symbol undefined (the gateway bring-up defines it in src/main.c).