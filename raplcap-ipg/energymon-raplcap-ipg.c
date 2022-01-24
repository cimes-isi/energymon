/**
 * Read energy from Intel RAPL using the raplcap-msr library.
 *
 * @author Connor Imes
 * @date 2018-05-19
 */
#include <errno.h>
#include <inttypes.h>
#include <raplcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "energymon.h"
#include "energymon-raplcap-ipg.h"
#include "energymon-util.h"

#ifdef ENERGYMON_DEFAULT
#include "energymon-default.h"
int energymon_get_default(energymon* em) {
  return energymon_get_raplcap_ipg(em);
}
#endif

typedef struct raplcap_ipg_info {
  double j_last;
  double j_max;
  uint32_t n_overflow;
} raplcap_ipg_info;

typedef struct energymon_raplcap_ipg {
  raplcap rc;
  uint32_t n_pkg;
  uint32_t n_die;
  uint32_t n_msrs;
  raplcap_ipg_info msrs[];
} energymon_raplcap_ipg;

int energymon_init_raplcap_ipg(energymon* em) {
  if (em == NULL || em->state != NULL) {
    errno = EINVAL;
    return -1;
  }

  int err_save;
  int supp;
  uint32_t i;
  uint32_t n_pkg;
  uint32_t n_die;
  uint32_t n_msrs;
  uint32_t max_die;
  uint32_t pkg;
  uint32_t die;
  if ((n_pkg = raplcap_get_num_packages(NULL)) == 0) {
    perror("raplcap_get_num_packages");
    return -1;
  }
  // currently only support systems with homogeneous package/die configurations
  for (pkg = 0; pkg < n_pkg; pkg++) {
    if ((n_die = raplcap_get_num_die(NULL, pkg)) == 0) {
      if (errno == ENOSYS) {
        // TODO: for now, assume single-die
        n_die = 1;
      } else {
        perror("raplcap_get_num_die");
        return -1;
      }
    }
    if (pkg == 0) {
      max_die = n_die;
    } else if (max_die != n_die) {
      fprintf(stderr, "energymon_init_raplcap_ipg: no support for heterogeneous package/die\n");
      errno = ENOSYS;
      return -1;
    }
  }
  n_msrs = n_pkg * n_die;

  energymon_raplcap_ipg* state = calloc(1, sizeof(energymon_raplcap_ipg) + (n_msrs * sizeof(raplcap_ipg_info)));
  if (state == NULL) {
    return -1;
  }
  state->n_msrs = n_msrs;
  state->n_pkg = n_pkg;
  state->n_die = n_die;

  if (raplcap_init(&state->rc)) {
    perror("raplcap_init");
    free(state);
    return -1;
  }

  for (pkg = 0; pkg < state->n_pkg; pkg++) {
    for (die = 0; die < state->n_die; die++) {
      i = pkg * die + die;
      // first check if zone is supported
      supp = raplcap_pd_is_zone_supported(&state->rc, pkg, die, RAPLCAP_ZONE_PACKAGE);
      if (supp < 0) {
        perror("raplcap_pd_is_zone_supported");
        goto fail;
      }
      if (supp == 0) {
        fprintf(stderr, "energymon_init_raplcap_ipg: Unsupported zone: PACKAGE\n");
        errno = EINVAL;
        goto fail;
      }
      // Note: max energy is specified in a different MSR than the zone's energy counter,
      // so this call might still work for unsupported zones (which is why we have to check for support first)
      if ((state->msrs[i].j_max = raplcap_pd_get_energy_counter_max(&state->rc, pkg, die, RAPLCAP_ZONE_PACKAGE)) < 0) {
        if (errno == ENOSYS) {
          // TODO: this is just for testing
          state->msrs[i].j_max = 1000000000;
        } else {
          perror("raplcap_pd_get_energy_counter_max");
          goto fail;
        }
      }
    }
  }

  em->state = state;
  return 0;

fail:
  err_save = errno;
  if (raplcap_destroy(&state->rc)) {
    perror("raplcap_destroy");
  }
  free(state);
  errno = err_save;
  return -1;
}

uint64_t energymon_read_total_raplcap_ipg(const energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return 0;
  }
  uint32_t pkg;
  uint32_t die;
  uint32_t i;
  double j;
  uint64_t total = 0;
  energymon_raplcap_ipg* state = (energymon_raplcap_ipg*) em->state;
  for (errno = 0, pkg = 0; pkg < state->n_pkg && !errno; pkg++) {
    for (die = 0; die < state->n_die && !errno; die++) {
      i = pkg * die + die;
      if ((j = raplcap_pd_get_energy_counter(&state->rc, pkg, die, RAPLCAP_ZONE_PACKAGE)) >= 0) {
        if (j < state->msrs[i].j_last) {
          state->msrs[i].n_overflow++;
        }
        state->msrs[i].j_last = j;
        total += (uint64_t) ((j + state->msrs[i].n_overflow * state->msrs[i].j_max) * 1000000.0);
      }
    }
  }
  return errno ? 0 : total;
}

int energymon_finish_raplcap_ipg(energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return -1;
  }
  int ret;
  energymon_raplcap_ipg* state = em->state;
  if ((ret = raplcap_destroy(&state->rc))) {
    perror("raplcap_destroy");
  }
  free(em->state);
  em->state = NULL;
  return ret;
}

char* energymon_get_source_raplcap_ipg(char* buffer, size_t n) {
  return energymon_strencpy(buffer, "Intel RAPL via libraplcap-ipg", n);
}

uint64_t energymon_get_interval_raplcap_ipg(const energymon* em) {
  if (em == NULL) {
    // we don't need to access em, but it's still a programming error
    errno = EINVAL;
    return 0;
  }
  return 1000;
}

uint64_t energymon_get_precision_raplcap_ipg(const energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return 0;
  }
  // TODO: can we determine this from raplcap-ipg?
  return 1;
}

int energymon_is_exclusive_raplcap_ipg(void) {
  return 0;
}

int energymon_get_raplcap_ipg(energymon* em) {
  if (em == NULL) {
    errno = EINVAL;
    return -1;
  }
  em->finit = &energymon_init_raplcap_ipg;
  em->fread = &energymon_read_total_raplcap_ipg;
  em->ffinish = &energymon_finish_raplcap_ipg;
  em->fsource = &energymon_get_source_raplcap_ipg;
  em->finterval = &energymon_get_interval_raplcap_ipg;
  em->fprecision = &energymon_get_precision_raplcap_ipg;
  em->fexclusive = &energymon_is_exclusive_raplcap_ipg;
  em->state = NULL;
  return 0;
}
