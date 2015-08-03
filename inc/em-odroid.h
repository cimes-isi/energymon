/**
 * Energy reading for an ODROID with INA231 power sensors.
 *
 * @author Connor Imes
 * @date 2014-06-30
 */
#ifndef _EM_ODROID_H_
#define _EM_ODROID_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "energymon.h"

int energymon_init_odroid(energymon* em);

unsigned long long energymon_read_total_odroid(const energymon* em);

int energymon_finish_odroid(energymon* em);

char* energymon_get_source_odroid(char* buffer);

unsigned long long energymon_get_interval_odroid(const energymon* em);

int energymon_get_odroid(energymon* impl);

#ifdef __cplusplus
}
#endif

#endif
