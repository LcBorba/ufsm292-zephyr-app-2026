/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);


int main(void)
{
	
	//servidor_main();
	//sensor_main();
	//gateway_main();
	//supervisor_main(); 
	
	printk("OK\n");
	

	return 0;
}

