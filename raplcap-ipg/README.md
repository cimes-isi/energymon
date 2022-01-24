# RAPLCap IPG Energy Monitor

This implementation of the `energymon` interface wraps the [raplcap-ipg](https://github.com/powercap/raplcap) library.


## Prerequisites

First, you must be using a system that supports Intel RAPL.

Install [raplcap-ipg](https://github.com/powercap/raplcap), including development header files.

See the raplcap-ipg documentation for more information on prerequisites like Intel Power Gadget.


## Linking

To link with the library:

```
pkg-config --libs --static energymon-raplcap-ipg
```
