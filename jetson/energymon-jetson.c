/**
 * Energy reading for NVIDIA Jetson devices with TI INA3221 power sensors.
 *
 * Note: The kernel driver used is NOT the INA3221 driver in the mainline Linux kernel.
 *       Rather, it is from the NVIDIA Linux for Tegra (Linux4Tegra, L4T) tree, at
 *       'nvidia/drivers/staging/iio/meter/ina3221.c'.
 * See: https://developer.nvidia.com/embedded/linux-tegra
 *
 * @author Connor Imes
 * @date 2021-12-16
 */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "energymon.h"
#include "energymon-jetson.h"
#include "energymon-time-util.h"
#include "energymon-util.h"

/* PATH_MAX should be defined in limits.h */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef ENERGYMON_DEFAULT
#include "energymon-default.h"
int energymon_get_default(energymon* em) {
  return energymon_get_jetson(em);
}
#endif

#define NUM_RAILS_DEFAULT_MAX 6

#define INA3221X_DIR "/sys/bus/i2c/drivers/ina3221x"
#define INA3221X_DIR_TEMPLATE_BUS_ADDR INA3221X_DIR"/%s"
#define INA3221X_DIR_TEMPLATE_DEVICE INA3221X_DIR"/%s/%s"
#define INA3221X_FILE_TEMPLATE_RAIL_NAME INA3221X_DIR"/%s/%s/rail_name_%d"
#define INA3221X_FILE_TEMPLATE_POWER INA3221X_DIR"/%s/%s/in_power%d_input"
#define INA3221X_FILE_TEMPLATE_POLLING_DELAY INA3221X_DIR"/%s/%s/polling_delay_%d"

// Environment variable to request a minimum polling interval.
// Keeping this as an undocumented feature for now.
#define ENERGYMON_JETSON_INTERVAL_US "ENERGYMON_JETSON_INTERVAL_US"

// The hardware supports up to 3 channels per instance
#define INA3221_CHANNELS_MAX 3

// The hardware can refresh at microsecond granularity, with "conversion times" for both the shunt- and bus-voltage
// measurements ranging from 140 us to 8.244 ms, per the INA3221 data sheet.
// However, the sysfs interface reports polling delay at millisecond granularity, so use min = 1 ms.
#define INA3221_MIN_POLLING_DELAY_US 1000

// 100k seems to be an empirically good value with very low overhead
#define INA3221_DEFAULT_POLLING_DELAY_US 100000
// 10k might also be considered good (generally <1% CPU utilization on an AGX Xavier)

typedef struct energymon_jetson {
  // sensor update interval in microseconds
  unsigned long polling_delay_us;
  // thread variables
  pthread_t thread;
  int poll_sensors;
  // total energy estimate
  uint64_t total_uj;
  // sensor file descriptors
  size_t count;
  int fds[];
} energymon_jetson;

static int read_string(const char* file, char* str, size_t len) {
  int fd;
  ssize_t ret;
  size_t end;
  if ((fd = open(file, O_RDONLY)) < 0) {
    return -1;
  }
  if ((ret = read(fd, str, len)) > 0) {
    // strip trailing new line
    if ((end = strcspn(str, "\n")) < len) {
      str[end] = '\0';
    }
  }
  close(fd);
  return ret < 0 ? -1 : 0;
}

static long read_long(const char* file) {
  char data[64];
  int fd;
  int ret;
  if ((fd = open(file, O_RDONLY)) < 0) {
    return -1;
  }
  if (read(fd, data, sizeof(data)) > 0) {
    errno = 0;
    ret = strtol(data, NULL, 0);
    if (!ret && errno) {
      ret = -1;
    }
  } else {
    ret = -1;
  }
  close(fd);
  return ret;
}

static int ina3221x_try_read_rail_name(const char* bus_addr, const char* device, int channel, char* name, size_t len) {
  char file[PATH_MAX];
  snprintf(file, sizeof(file), INA3221X_FILE_TEMPLATE_RAIL_NAME, bus_addr, device, channel);
  return read_string(file, name, len);
}

static long ina3221x_try_read_polling_delay_us(const char* bus_addr, const char* device, int channel) {
  char file[PATH_MAX];
  snprintf(file, sizeof(file), INA3221X_FILE_TEMPLATE_POLLING_DELAY, bus_addr, device, channel);
  // TODO: output includes precision (e.g., "0 ms") - set endptr in the read and parse this
  return read_long(file) * 1000;
}

static int ina3221x_open_power_file(const char* bus_addr, const char* device, int channel) {
  char file[PATH_MAX];
  int fd;
  snprintf(file, sizeof(file), INA3221X_FILE_TEMPLATE_POWER, bus_addr, device, channel);
  if ((fd = open(file, O_RDONLY)) < 0) {
    perror(file);
  }
  return fd;
}

static int is_iio_device_dir(const struct dirent* entry) {
  // format: iio:deviceN
  return entry->d_type == DT_DIR
         && strlen(entry->d_name) > 10
         && entry->d_name[0] != '.'
         && entry->d_name[3] == ':';
}

static int is_i2c_bus_addr_dir(const struct dirent* entry) {
  // format: X-ABCDE
  return (entry->d_type == DT_LNK || entry->d_type == DT_DIR)
         && strlen(entry->d_name) > 2
         && entry->d_name[0] != '.'
         && entry->d_name[1] == '-';
}

static int ina3221x_walk_device_dir(const char* const* names, int* fds, size_t len, unsigned long* polling_delay_us_max,
                                    const char* bus_addr, const char* device) {
  // for each rail_name_X, open power file if its name is in list
  // there are 3 channels per device, but it's possible they aren't all connected
  char name[64];
  size_t i;
  int channel;
  long polling_delay_us;
  for (channel = 0; channel < INA3221_CHANNELS_MAX; channel++) {
    errno = 0;
    if (ina3221x_try_read_rail_name(bus_addr, device, channel, name, sizeof(name)) < 0) {
      if (errno == ENOENT) {
        // assume this means the channel isn't connected, which is not an error
        continue;
      }
      return -1;
    } else {
      for (i = 0; i < len; i++) {
        if (!strncmp(name, names[i], sizeof(name))) {
          if (fds[i]) {
            fprintf(stderr, "Duplicate sensor name: %s\n", name);
            errno = EEXIST;
            return -1;
          }
          if ((fds[i] = ina3221x_open_power_file(bus_addr, device, channel)) < 0) {
            return -1;
          }
          polling_delay_us = ina3221x_try_read_polling_delay_us(bus_addr, device, channel);
          if (polling_delay_us > 0 && (unsigned long) polling_delay_us > *polling_delay_us_max) {
            *polling_delay_us_max = (unsigned long) polling_delay_us;
          }
          break;
        }
      }
    }
  }
  return 0;
}

static int ina3221x_walk_bus_addr_dir(const char* const* names, int* fds, size_t len,
                                      unsigned long* polling_delay_us_max, const char* bus_addr) {
  // for each name format iio:deviceX
  DIR* dir;
  const struct dirent* entry;
  char path[PATH_MAX];
  int ret = 0;
  snprintf(path, sizeof(path), INA3221X_DIR_TEMPLATE_BUS_ADDR, bus_addr);
  if ((dir = opendir(path)) == NULL) {
    perror(path);
    return -1;
  }
  while ((entry = readdir(dir)) != NULL) {
    if (is_iio_device_dir(entry)) {
      if ((ret = ina3221x_walk_device_dir(names, fds, len, polling_delay_us_max, bus_addr, entry->d_name)) < 0) {
        break;
      }
    }
  }
  if (closedir(dir)) {
    perror(path);
  }
  return ret;
}

static int ina3221x_walk_i2c_drivers_dir(const char* const* names, int* fds, size_t len,
                                         unsigned long* polling_delay_us_max) {
  // for each name format X-ABCDE
  DIR* dir;
  const struct dirent* entry;
  int ret = 0;
  if ((dir = opendir(INA3221X_DIR)) == NULL) {
    perror(INA3221X_DIR);
    return -1;
  }
  while ((entry = readdir(dir)) != NULL) {
    if (is_i2c_bus_addr_dir(entry)) {
      if ((ret = ina3221x_walk_bus_addr_dir(names, fds, len, polling_delay_us_max, entry->d_name)) < 0) {
        break;
      }
    }
  }
  if (closedir(dir)) {
    perror(INA3221X_DIR);
  }
  return ret;
}

static void close_and_clear_fds(int* fds, size_t max_fds) {
  int err_save = errno;
  size_t i;
  for (i = 0; i < max_fds; i++) {
    if (fds[i] > 0) {
      close(fds[i]);
      fds[i] = 0;
    }
  }
  errno = err_save;
}

/*
 * There doesn't seem to be a consistent approach to determine the Jetson model by direct system inquiry.
 * Instead, we use a try/catch heuristic to find default rails with an ordered search.
 * This only works if the name set in an earlier array isn't a subset of any name set in later arrays searched.
 * It's important to order the search s.t. this invariant isn't violated, o/w sensors will be skipped.
 */
// The TX1 and most TX2 models have power power rails VDD_IN for the main board and VDD_MUX for the carrier board
static const char* DEFAULT_RAIL_NAMES_TX[NUM_RAILS_DEFAULT_MAX] = {"VDD_IN", "VDD_MUX"};
// The Xavier NX and TX2 NX have parent power rail VDD_IN
static const char* DEFAULT_RAIL_NAMES_VDD_IN[NUM_RAILS_DEFAULT_MAX] = {"VDD_IN"};
// The Nano has parent power rail POM_5V_IN
static const char* DEFAULT_RAIL_NAMES_POM_5V_IN[NUM_RAILS_DEFAULT_MAX] = {"POM_5V_IN"};
// The AGX Xavier doesn't have a parent power rail - all its rails are in parallel
static const char* DEFAULT_RAIL_NAMES_AGX_XAVIER[NUM_RAILS_DEFAULT_MAX] = {"GPU", "CPU", "SOC", "CV", "VDDRQ", "SYS5V"};
static const char* const* DEFAULT_RAIL_NAMES[] = {
  DEFAULT_RAIL_NAMES_TX,
  DEFAULT_RAIL_NAMES_VDD_IN,
  DEFAULT_RAIL_NAMES_POM_5V_IN,
  DEFAULT_RAIL_NAMES_AGX_XAVIER,
};
static int ina3221x_walk_i2c_drivers_dir_for_default(int* fds, size_t* n_fds, unsigned long* polling_delay_us_max) {
  size_t i;
  size_t j;
#ifndef NDEBUG
  size_t max_fds = *n_fds;
#endif
  assert(max_fds == NUM_RAILS_DEFAULT_MAX);
  for (i = 0; i < sizeof(DEFAULT_RAIL_NAMES) / sizeof(DEFAULT_RAIL_NAMES[0]); i++) {
    for (*n_fds = 0; *n_fds < NUM_RAILS_DEFAULT_MAX && DEFAULT_RAIL_NAMES[i][*n_fds] != NULL; (*n_fds)++) {
      // *n_fds is being incremented
    }
    assert(*n_fds > 0);
    assert(*n_fds <= max_fds);
    if (ina3221x_walk_i2c_drivers_dir(DEFAULT_RAIL_NAMES[i], fds, *n_fds, polling_delay_us_max)) {
      // cleanup is done in the calling function
      return -1;
    }
    if (fds[0] > 0) {
      // check if all channels are found
      for (j = 0; j < *n_fds; j++) {
        if (fds[j] <= 0) {
          close_and_clear_fds(fds, *n_fds);
          break; // continue outer loop
        }
      }
      return 0;
    }
  }
  errno = ENODEV;
  return -1;
}

/**
 * pthread function to poll the sensor(s) at regular intervals.
 */
static void* jetson_poll_sensors(void* args) {
  energymon_jetson* state = (energymon_jetson*) args;
  char cdata[8];
  unsigned long sum_mw;
  size_t i;
  uint64_t exec_us;
  uint64_t last_us;
  int err_save;
  if (!(last_us = energymon_gettime_us())) {
    // must be that CLOCK_MONOTONIC is not supported
    perror("jetson_poll_sensors");
    return (void*) NULL;
  }
  energymon_sleep_us(state->polling_delay_us, &state->poll_sensors);
  while (state->poll_sensors) {
    // read individual sensors
    for (sum_mw = 0, errno = 0, i = 0; i < state->count && !errno; i++) {
      if (pread(state->fds[i], cdata, sizeof(cdata), 0) > 0) {
        sum_mw += strtoul(cdata, NULL, 0);
      }
    }
    err_save = errno;
    exec_us = energymon_gettime_elapsed_us(&last_us);
    if (err_save) {
      errno = err_save;
      perror("jetson_poll_sensors: skipping power sensor reading");
    } else {
      state->total_uj += (uint64_t) (sum_mw * exec_us / 1000);
    }
    // sleep for the update interval of the sensors
    if (state->poll_sensors) {
      energymon_sleep_us(state->polling_delay_us, &state->poll_sensors);
    }
    errno = 0;
  }
  return (void*) NULL;
}

// Uses strtok_r to parse the input string into an array (str may be free'd afterward)
static char** strtok_to_arr_r(char* str, const char* delim, size_t *n) {
  char* tok;
  char* saveptr;
  char** arr = NULL;
  char** arr_realloc;
  *n = 0;
  tok = strtok_r(str, delim, &saveptr);
  while (tok) {
    // inefficient to realloc every iteration, but straightforward
    if (!(arr_realloc = realloc(arr, (*n + 1) * sizeof(char*)))) {
      goto fail;
    }
    arr = arr_realloc;
    if (!(arr[*n] = strdup(tok))) {
      goto fail;
    }
    (*n)++;
    tok = strtok_r(NULL, delim, &saveptr);
  }
  return arr;
fail:
  while (*n > 0) {
    (*n)--;
    free(arr[*n]);
  }
  free(arr);
  return NULL;
}

static void free_rail_names(char** rail_names, size_t n) {
  while (n > 0) {
    free(rail_names[--n]);
  }
  free(rail_names);
}

static char** get_rail_names(const char* rail_names_str, size_t* n) {
  char* tmp;
  char** rail_names;
  size_t i;
  size_t j;
  // duplicate rail_names_str b/c strtok_r modifies the input string
  if ((tmp = strdup(rail_names_str)) == NULL) {
    return NULL;
  }
  rail_names = strtok_to_arr_r(tmp, ",", n);
  free(tmp);
  if (!rail_names) {
    if (!errno) {
      // must be a bad env var value, o/w errno would be ENOMEM for problems in strtok_to_arr_r
      fprintf(stderr, "energymon_init_jetson: parsing error: "ENERGYMON_JETSON_RAIL_NAMES"=%s\n", rail_names_str);
      errno = EINVAL;
    }
    return NULL;
  }
  // disallow duplicate entries, otherwise we'd get duplicate power readings
  for (i = 0; i < *n - 1; i++) {
    for (j = i + 1; j < *n; j++) {
      if (!strcmp(rail_names[i], rail_names[j])) {
        fprintf(stderr, "energymon_init_jetson: duplicate rail name specified: %s\n",
                rail_names[i]);
        free_rail_names(rail_names, *n);
        *n = 0;
        errno = EINVAL;
        return NULL;
      }
    }
  }
  return rail_names;
}

static unsigned long get_polling_delay_us(unsigned long sysfs_polling_delay_us) {
  unsigned long us;
  const char* polling_interval_env = getenv(ENERGYMON_JETSON_INTERVAL_US);
  if (polling_interval_env) {
    // start with the value specified in the environment variable
    errno = 0;
    us = strtoul(polling_interval_env, NULL, 0);
    if (errno) {
      fprintf(stderr, "energymon_init_jetson: failed to parse environment variable value: "
              ENERGYMON_JETSON_INTERVAL_US"=%s\n", polling_interval_env);
      return 0;
    }
  } else {
    // start with the value captured from sysfs (might be 0)
    us = sysfs_polling_delay_us;
    // enforce a reasonable minimum default since actual update interval may be too fast
    if (us < INA3221_DEFAULT_POLLING_DELAY_US) {
      us = INA3221_DEFAULT_POLLING_DELAY_US;
    }
  }
  // always enforce an lower bound
  if (us < INA3221_MIN_POLLING_DELAY_US) {
    us = INA3221_MIN_POLLING_DELAY_US;
  }
  return us;
}

/**
 * Open all sensor files and start the thread to poll the sensors.
 */
int energymon_init_jetson(energymon* em) {
  if (em == NULL || em->state != NULL) {
    errno = EINVAL;
    return -1;
  }

  unsigned long polling_delay_us = 0;
  size_t i;
  int err_save;
  size_t n_rails;
  char** rail_names = NULL;
  const char* rail_names_str = getenv(ENERGYMON_JETSON_RAIL_NAMES);
  if (rail_names_str) {
    if (!(rail_names = get_rail_names(rail_names_str, &n_rails))) {
      return -1;
    }
  } else {
    // look for a default rail below
    n_rails = NUM_RAILS_DEFAULT_MAX;
  }

  energymon_jetson* state = calloc(1, sizeof(energymon_jetson) + n_rails * sizeof(int));
  if (state == NULL) {
    goto fail_alloc;
  }
  state->count = n_rails;

  if (rail_names) {
    if (ina3221x_walk_i2c_drivers_dir((const char* const*) rail_names, state->fds, n_rails, &polling_delay_us)) {
      goto fail_rails;
    }
    for (i = 0; i < n_rails; i++) {
      if (state->fds[i] <= 0) {
        fprintf(stderr, "energymon_init_jetson: did not find requested rail: %s\n", rail_names[i]);
        errno = ENODEV;
        goto fail_rails;
      }
    }
    free_rail_names(rail_names, n_rails);
  } else {
    if (ina3221x_walk_i2c_drivers_dir_for_default(state->fds, &n_rails, &polling_delay_us)) {
      if (errno == ENODEV) {
        fprintf(stderr, "energymon_init_jetson: did not find default rail(s) - is this a supported model?\n"
                "Try setting "ENERGYMON_JETSON_RAIL_NAMES"\n");
      }
      goto fail_rails;
    }
    // possibly (even probably) reduce count
    state->count = n_rails;
  }

  em->state = state;

  state->polling_delay_us = get_polling_delay_us(polling_delay_us);
  if (!state->polling_delay_us) {
    err_save = errno;
    energymon_finish_jetson(em);
    errno = err_save;
    return -1;
  }

  // start sensors polling thread
  state->poll_sensors = 1;
  errno = pthread_create(&state->thread, NULL, jetson_poll_sensors, state);
  if (errno) {
    err_save = errno;
    energymon_finish_jetson(em);
    errno = err_save;
    return -1;
  }

  return 0;

fail_rails:
  close_and_clear_fds(state->fds, state->count);
  free(state);
fail_alloc:
  if (rail_names) {
    free_rail_names(rail_names, n_rails);
  }
  return -1;
}

int energymon_finish_jetson(energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return -1;
  }

  int err_save = 0;
  size_t i;
  energymon_jetson* state = (energymon_jetson*) em->state;

  if (state->poll_sensors) {
    // stop sensors polling thread and cleanup
    state->poll_sensors = 0;
#ifndef __ANDROID__
    pthread_cancel(state->thread);
#endif
    err_save = pthread_join(state->thread, NULL);
  }

  // close individual sensor files
  for (i = 0; i < state->count; i++) {
    if (state->fds[i] > 0 && close(state->fds[i])) {
      err_save = err_save ? err_save : errno;
    }
  }
  free(em->state);
  em->state = NULL;
  errno = err_save;
  return errno ? -1 : 0;
}

uint64_t energymon_read_total_jetson(const energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return 0;
  }
  errno = 0;
  return ((energymon_jetson*) em->state)->total_uj;
}

char* energymon_get_source_jetson(char* buffer, size_t n) {
  return energymon_strencpy(buffer, "NVIDIA Jetson INA3221 Power Monitors", n);
}

uint64_t energymon_get_interval_jetson(const energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return 0;
  }
  return ((energymon_jetson*) em->state)->polling_delay_us;
}

uint64_t energymon_get_precision_jetson(const energymon* em) {
  if (em == NULL || em->state == NULL) {
    errno = EINVAL;
    return 0;
  }
  // milliwatts at refresh interval
  uint64_t prec = energymon_get_interval_jetson(em) / 1000;
  return prec ? prec : 1;
}

int energymon_is_exclusive_jetson(void) {
  return 0;
}

int energymon_get_jetson(energymon* em) {
  if (em == NULL) {
    errno = EINVAL;
    return -1;
  }
  em->finit = &energymon_init_jetson;
  em->fread = &energymon_read_total_jetson;
  em->ffinish = &energymon_finish_jetson;
  em->fsource = &energymon_get_source_jetson;
  em->finterval = &energymon_get_interval_jetson;
  em->fprecision = &energymon_get_precision_jetson;
  em->fexclusive = &energymon_is_exclusive_jetson;
  em->state = NULL;
  return 0;
}
