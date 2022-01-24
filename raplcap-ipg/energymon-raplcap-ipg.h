/**
 * Read energy from Intel RAPL using the raplcap-ipg library.
 *
 * @author Connor Imes
 * @date 2018-05-19
 */
#ifndef _ENERGYMON_RAPLCAP_IPG_H_
#define _ENERGYMON_RAPLCAP_IPG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>
#include <stddef.h>
#include "energymon.h"

int energymon_init_raplcap_ipg(energymon* em);

uint64_t energymon_read_total_raplcap_ipg(const energymon* em);

int energymon_finish_raplcap_ipg(energymon* em);

char* energymon_get_source_raplcap_ipg(char* buffer, size_t n);

uint64_t energymon_get_interval_raplcap_ipg(const energymon* em);

uint64_t energymon_get_precision_raplcap_ipg(const energymon* em);

int energymon_is_exclusive_raplcap_ipg(void);

int energymon_get_raplcap_ipg(energymon* em);

#ifdef __cplusplus
}
#endif

#endif
