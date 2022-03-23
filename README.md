# RoCC (Robust Congestion Control)

Linux kernel module for RoCC

Run `make` and then `sudo insmod tcp_rocc.ko` to install the module.

Change `#undef ROCC_DEBUG` to `#define ROCC_DEBUG` in `tcp_rocc.c` to enable some debug logging.

Note, it may take a while after the last TCP flow using RoCC ended before `sudo rmmod tcp_rocc` works because the socket will wait for a timeout before closing.
