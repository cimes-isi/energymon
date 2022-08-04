/**
 * Energy reading for NVIDIA Jetson devices with TI INA3221 power sensors.
 *
 * @author Connor Imes
 * @date 2022-08-04
 */
#ifndef _ENERGYMON_JETSON_SENSORS_H_
#define _ENERGYMON_JETSON_SENSORS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>
#include <stddef.h>
#include "energymon.h"

int energymon_init_jetson_sensors(energymon* em);

uint64_t energymon_read_total_jetson_sensors(const energymon* em);

int energymon_finish_jetson_sensors(energymon* em);

char* energymon_get_source_jetson_sensors(char* buffer, size_t n);

uint64_t energymon_get_interval_jetson_sensors(const energymon* em);

uint64_t energymon_get_precision_jetson_sensors(const energymon* em);

int energymon_is_exclusive_jetson_sensors(void);

int energymon_get_jetson_sensors(energymon* em);

#ifdef __cplusplus
}
#endif

#endif
