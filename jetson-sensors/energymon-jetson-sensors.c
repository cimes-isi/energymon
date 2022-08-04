/**
 * Energy reading for NVIDIA Jetson devices with TI INA3221 power sensors.
 *
 * @author Connor Imes
 * @date 2022-08-04
 */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// libsensors
#include <sensors.h>
#include <error.h>

#include "energymon.h"
#include "energymon-jetson-sensors.h"
#include "energymon-time-util.h"
#include "energymon-util.h"

#ifdef ENERGYMON_DEFAULT
#include "energymon-default.h"
int energymon_get_default(energymon* em) {
  return energymon_get_jetson_sensors(em);
}
#endif



// Environment variable to not call libsensors static lifecycle functions.
// Keeping this as an undocumented feature for now.
#define ENERGYMON_JETSON_SENSORS_SKIP_LIFECYCLE "ENERGYMON_JETSON_SENSORS_SKIP_LIFECYCLE"

// Environment variable to use a non-NULL sensors init file.
// Keeping this as an undocumented feature for now.
#define ENERGYMON_JETSON_SENSORS_INIT_FILE "ENERGYMON_JETSON_SENSORS_INIT_FILE"


#define INA3221_CHAN_NAME_LEN 64

#define JETSON_DEV_NAME_LEN 64
#define JETSON_INA3221_DEVS_MAX 2

typedef struct ina3221_chan_cfg {
  size_t id;
  char name[INA3221_CHAN_NAME_LEN];
} ina3221_chan_cfg;

#define INA3221_CHAN_CFG_INIT(_id, _name) { \
  .id = _id, \
  .name = _name }

typedef struct ina3221_dev_cfg {
  int bus;
  int addr;
  ina3221_chan_cfg channels[3];
} ina3221_dev_cfg;

static ina3221_dev_cfg DEVS_XAVIER_NX[JETSON_INA3221_DEVS_MAX] = {
  {
    .bus = 7,
    .addr = 0x40,
    .channels = {
      INA3221_CHAN_CFG_INIT(1, "VDD_IN"),
      INA3221_CHAN_CFG_INIT(2, "VDD_CPU_GPU_CV"),
      INA3221_CHAN_CFG_INIT(3, "VDD_SOC")
    },
  },
};

static ina3221_dev_cfg DEVS_AGX_XAVIER[JETSON_INA3221_DEVS_MAX] = {
  {
    .bus = 1,
    .addr = 0x40,
    .channels = {
      INA3221_CHAN_CFG_INIT(1, "GPU"),
      INA3221_CHAN_CFG_INIT(2, "CPU"),
      INA3221_CHAN_CFG_INIT(3, "SOC")
    },
  },
  {
    .bus = 1,
    .addr = 0x41,
    .channels = {
      INA3221_CHAN_CFG_INIT(1, "CV"),
      INA3221_CHAN_CFG_INIT(2, "VDDRQ"),
      INA3221_CHAN_CFG_INIT(3, "SYS5V")
    },
  },
};

static ina3221_dev_cfg DEVS_AGX_ORIN[JETSON_INA3221_DEVS_MAX] = {
  {
    .bus = 1,
    .addr = 0x40,
    .channels = {
      INA3221_CHAN_CFG_INIT(1, "VDD_GPU_SOC"),
      INA3221_CHAN_CFG_INIT(2, "VDD_CPU_CV"),
      INA3221_CHAN_CFG_INIT(3, "VIN_SYS_5V0")
    },
  },
  {
    .bus = 1,
    .addr = 0x41,
    .channels = {
      INA3221_CHAN_CFG_INIT(1, "NC"),
      INA3221_CHAN_CFG_INIT(2, "VDDQ_VDD2_1V8AO"),
      INA3221_CHAN_CFG_INIT(3, "NC")
    },
  },
};

typedef struct jetson_dev_cfg {
  char name[JETSON_DEV_NAME_LEN];
  size_t n_ina3221_devs;
  ina3221_dev_cfg *ina3221_devs;
  // identifies by indexes which INA3221 devices and channels to open by default
  // TODO: might be easier to just add a flag array[3] to ina3221_dev_cfg? Does this break struct generality?
  int dev_chan_default_idxs[JETSON_INA3221_DEVS_MAX][3];
} jetson_dev_cfg;

static const jetson_dev_cfg JETSON_DEVS[] = {
  {
    .name = "Jetson Xavier NX Series",
    .n_ina3221_devs = 1,
    .ina3221_devs = DEVS_XAVIER_NX,
    .dev_chan_default_idxs = {{0},},
  },
  {
    .name = "Jetson AGX Xavier Series",
    .n_ina3221_devs = 2,
    .ina3221_devs = DEVS_AGX_XAVIER,
    .dev_chan_default_idxs = {{0, 1, 2}, {0, 1, 2}},
  },
  {
    .name = "Jetson AGX Orin Series",
    .n_ina3221_devs = 2,
    .ina3221_devs = DEVS_AGX_ORIN,
    // TODO: determine correct defaults
    .dev_chan_default_idxs = {{2}, {1}},
  },
};





static int init_libsensors(void) {
  const char* input;
  FILE* f = NULL;
  int rc;
  int err_save;
  if (getenv(ENERGYMON_JETSON_SENSORS_SKIP_LIFECYCLE)) {
    return 0;
  }
  if ((input = getenv(ENERGYMON_JETSON_SENSORS_INIT_FILE))) {
    if (!(f = fopen(input, "r"))) {
      fprintf(stderr, "fopen: %s: %s\n", input, strerror(errno));
      return -1;
    }
  }
  if ((rc = sensors_init(f))) {
    fprintf(stderr, "sensors_init: %s\n", sensors_strerror(rc));
    if (!errno) {
      // we don't really know the error, but we'll encourage user to retry
      errno = EAGAIN;
    }
  }
  if (f) {
    err_save = errno;
    if (fclose(f)) {
      fprintf(stderr, "fclose: %s: %s\n", input, strerror(errno));
    }
    errno = err_save;
  }
  return rc;
}

static void cleanup_libsensors(void) {
  if (!getenv(ENERGYMON_JETSON_SENSORS_SKIP_LIFECYCLE)) {
    sensors_cleanup();
  }
}





/**
 * Open all sensor files and start the thread to poll the sensors.
 */
int energymon_init_jetson_sensors(energymon* em) {
  if (em == NULL || em->state != NULL) {
    errno = EINVAL;
    return -1;
  }
  // TODO
  return -1;
}

int energymon_finish_jetson_sensors(energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return -1;
  }
  // TODO
  return -1;
}

uint64_t energymon_read_total_jetson_sensors(const energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return 0;
  }
  // TODO
  return 0;
}

char* energymon_get_source_jetson_sensors(char* buffer, size_t n) {
  return energymon_strencpy(buffer, "NVIDIA Jetson JetPack 5.x INA3221 Power Monitors", n);
}

uint64_t energymon_get_interval_jetson_sensors(const energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return 0;
  }
  // TODO
  return 0;
}

uint64_t energymon_get_precision_jetson_sensors(const energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return 0;
  }
  // milliwatts at refresh interval
  uint64_t prec = energymon_get_interval_jetson_sensors(em) / 1000;
  return prec ? prec : 1;
}

int energymon_is_exclusive_jetson_sensors(void) {
  return 0;
}

int energymon_get_jetson_sensors(energymon* em) {
  if (em == NULL) {
    errno = EINVAL;
    return -1;
  }
  em->finit = &energymon_init_jetson_sensors;
  em->fread = &energymon_read_total_jetson_sensors;
  em->ffinish = &energymon_finish_jetson_sensors;
  em->fsource = &energymon_get_source_jetson_sensors;
  em->finterval = &energymon_get_interval_jetson_sensors;
  em->fprecision = &energymon_get_precision_jetson_sensors;
  em->fexclusive = &energymon_is_exclusive_jetson_sensors;
  em->state = NULL;
  return 0;
}
