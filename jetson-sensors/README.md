# NVIDIA Jetson Energy Monitor for JetPack 5.x

This implementation of the `energymon` interface reads from TI INA3221 power monitors on NVIDIA Jetson systems running [Jetson Linux >= 34.1](https://developer.nvidia.com/embedded/linux-tegra).
The power monitors are polled at regular intervals to estimate energy consumption.

## Linking

To link with the appropriate library and its dependencies, use `pkg-config` to get the linker flags:

```sh
pkg-config --libs --static energymon-jetson-sensors
```

The `--static` flag is unnecessary when using dynamically linked libraries.
