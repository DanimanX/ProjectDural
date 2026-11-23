// Portable Linux pthread core-pinning helper for 4-core ARM64 handhelds
// (Allwinner H700: Core 0 main, Core 1 auxiliary/2D, Core 2 sound, Core 3 audio/aux).
//
// Pinning has to survive a firmware that leaves some cores offline. Spruce on the TrimUI
// Smart Pro boots with `online: 0,3` -- cores 1 and 2 hotplugged off -- and the previous
// helper then failed silently: every thread asked for a core that did not exist, got
// EINVAL, kept the affinity it inherited (core 0), and the whole 4-core pipeline ran on a
// single core. Measured on the device, that was the difference between ~27 and ~52 FPS:
// nothing to do with the GPU driver. So the request is now resolved against the cores that
// are actually online, and the outcome is reported instead of dropped.
#pragma once

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

namespace arm64 {

inline int cpu_count() {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? int(n) : 1;
}

// Is this logical CPU online? 1 = online, 0 = offline, -1 = unknown (no such file).
inline int cpu_is_online(int core) {
    char path[64];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/online", core);
    FILE *f = std::fopen(path, "r");
    if (!f) return core == 0 ? 1 : -1; // no online file: cpu0 is always on, others unknown
    int v = -1;
    if (std::fscanf(f, "%d", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}

// Resolve a role's preferred core against what is online. `avoid` lists the cores other
// roles own, used only when the preferred one is unavailable, so two threads do not end up
// sharing a CPU. Returns -1 when nothing is free (the caller then leaves affinity alone).
inline int resolve_core(int wanted, const int *avoid = nullptr, int avoid_n = 0) {
    const int n = cpu_count();
    if (n <= 1) return -1;
    auto usable = [&](int c) {
        if (c < 0 || c >= n) return false;
        for (int i = 0; i < avoid_n; ++i)
            if (avoid && avoid[i] == c) return false;
        return true;
    };
    if (usable(wanted) && cpu_is_online(wanted) != 0) return wanted;
    for (int c = 0; c < n; ++c)
        if (usable(c) && cpu_is_online(c) != 0) return c;
    return -1;
}

// Pin the calling thread. Returns the core it ended up on, or -1 if it could not be pinned.
inline int pin_current_thread(int wanted, const int *avoid = nullptr, int avoid_n = 0) {
    if (cpu_count() <= 1) return -1;
    const int core = resolve_core(wanted, avoid, avoid_n);
    if (core < 0) return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) != 0) return -1;
    if (core != wanted)
        std::fprintf(stderr, "[ProjectDural] core %d offline; thread pinned to core %d instead\n",
                     wanted, core);
    return core;
}

inline void set_current_thread_name(const char *name) {
#if defined(__linux__)
    if (name) ::pthread_setname_np(::pthread_self(), name);
#else
    (void)name;
#endif
}

} // namespace arm64
