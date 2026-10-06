# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_ETH_KSZ8851SNL eth_ksz8851snl.c)
zephyr_library_include_directories(${ZEPHYR_BASE}/subsys/net/l2)
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/ethernet)
