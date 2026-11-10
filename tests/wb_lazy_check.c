// runtime/wb_lazy_choice.h: whether the lazy write-back runs, by the player's setting, the platform and the GPU driver
// (runtime/vk_memory.c vk_mem_lazy_decide). Built and run by tests/wb_lazy_check.py; prints each failed check and
// exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "wb_lazy_choice.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// VkDriverId values (vulkan_core.h)
enum { AMD_PROPRIETARY = 1, AMD_OPEN_SOURCE = 2, MESA_RADV = 3, NVIDIA_PROPRIETARY = 4, INTEL_WINDOWS = 5, MESA_NVK = 24 };
enum { LINUX = 0, WINDOWS = 1 };

// driverInfo as the drivers report it: AMD's branch for RDNA 1 and 2 (an RX 6600's log), its driver for RDNA 3 and
// newer, NVIDIA's, Mesa's.
static const char RDNA12[] = "26.10.2 (AMD proprietary shader compiler)";
static const char RDNA34[] = "26.10.1 (LLPC)";
static const char NVIDIA[] = "617.42";
static const char MESA[] = "Mesa 26.0.1";

typedef struct { unsigned id; const char *info; const char *name; } Driver;
static const Driver drivers[] = {
    {AMD_PROPRIETARY, RDNA12, "AMD RDNA 1/2"}, {AMD_PROPRIETARY, RDNA34, "AMD RDNA 3+"}, {AMD_OPEN_SOURCE, "", "AMD open"},
    {MESA_RADV, MESA, "RADV"}, {NVIDIA_PROPRIETARY, NVIDIA, "NVIDIA"}, {INTEL_WINDOWS, "", "Intel"}, {MESA_NVK, MESA, "NVK"},
    {0, NULL, "unknown"},
};
enum { ND = sizeof drivers / sizeof *drivers };

static int has(const char *s, const char *part) { return s && strstr(s, part) != NULL; }

int main(void) {
    // Nothing set, Windows: on, except on AMD's driver branch for RDNA 1 and 2.
    WblChoice nv = wb_lazy_choice(NULL, WINDOWS, NVIDIA_PROPRIETARY, NVIDIA);
    CHECK(nv.on == 1, "Windows, NVIDIA's driver, nothing set: on");
    CHECK(has(nv.why, "default") && has(nv.why, "wb_lazy = 0"), "it says it is the default and how to turn it off: %s", nv.why);
    CHECK(wb_lazy_choice(NULL, WINDOWS, AMD_PROPRIETARY, RDNA34).on == 1, "Windows, AMD's driver for RDNA 3 and newer: on");
    CHECK(wb_lazy_choice(NULL, WINDOWS, INTEL_WINDOWS, "").on == 1, "Windows, Intel's driver, nothing set: on");
    CHECK(wb_lazy_choice(NULL, WINDOWS, 0, NULL).on == 1, "Windows, a driver that reports nothing: on, as 0.7.1 had it");
    WblChoice amd = wb_lazy_choice(NULL, WINDOWS, AMD_PROPRIETARY, RDNA12);
    CHECK(amd.on == 0, "Windows, AMD's driver for RDNA 1 and 2, nothing set: off");
    CHECK(has(amd.why, "AMD") && has(amd.why, "RX 5000 and 6000") && has(amd.why, "wb_lazy = 1"),
          "it names the driver's cards and how to turn it on: %s", amd.why);
    CHECK(has(amd.why, "frame rate"), "it says what off costs: %s", amd.why);
    CHECK(has(amd.why, "default"), "it says it is a default: %s", amd.why);

    // The branch is told by the driver and its driverInfo together, as vk_caps.c tells it: another driver whose
    // driverInfo happened to hold the words is not AMD's.
    CHECK(wbl_amd_rdna12(AMD_PROPRIETARY, RDNA12) && !wbl_amd_rdna12(AMD_PROPRIETARY, RDNA34), "the branch by driverInfo");
    CHECK(!wbl_amd_rdna12(AMD_PROPRIETARY, NULL) && !wbl_amd_rdna12(AMD_PROPRIETARY, ""), "no driverInfo: not the branch");
    CHECK(!wbl_amd_rdna12(NVIDIA_PROPRIETARY, RDNA12) && wb_lazy_choice(NULL, WINDOWS, MESA_RADV, RDNA12).on == 1,
          "the words alone, under another driver: not the branch");

    // An empty value is no value (cfg.c add: "key =" leaves the setting at its default).
    CHECK(wb_lazy_choice("", WINDOWS, AMD_PROPRIETARY, RDNA12).on == 0, "an empty value on the RDNA 1/2 branch: the default, off");
    CHECK(wb_lazy_choice("", WINDOWS, NVIDIA_PROPRIETARY, NVIDIA).on == 1, "an empty value on NVIDIA: the default, on");

    // Nothing set, elsewhere: off on every driver.
    for (int i = 0; i < ND; i++) {
        WblChoice c = wb_lazy_choice(NULL, LINUX, drivers[i].id, drivers[i].info);
        CHECK(c.on == 0 && has(c.why, "wb_lazy = 1"), "Linux, %s, nothing set: off, and how to turn it on: %s",
              drivers[i].name, c.why);
    }

    // What the player sets wins on every platform and driver, and the reason says it was set.
    for (int os = LINUX; os <= WINDOWS; os++)
        for (int i = 0; i < ND; i++) {
            WblChoice on = wb_lazy_choice("1", os, drivers[i].id, drivers[i].info);
            WblChoice off = wb_lazy_choice("0", os, drivers[i].id, drivers[i].info);
            CHECK(on.on == 1 && has(on.why, "set"), "os %d, %s, wb_lazy = 1: on (%s)", os, drivers[i].name, on.why);
            CHECK(off.on == 0 && has(off.why, "set"), "os %d, %s, wb_lazy = 0: off (%s)", os, drivers[i].name, off.why);
            CHECK(!has(on.why, "default") && !has(off.why, "default"), "os %d, %s: a set value is not a default", os,
                  drivers[i].name);
        }
    // Any value but 0 counts as on, as cfg_bool reads it for memory.c mem_init.
    CHECK(wb_lazy_choice("on", WINDOWS, AMD_PROPRIETARY, RDNA12).on == 1, "wb_lazy = on: on");

    CHECK(WBL_DRIVER_AMD == AMD_PROPRIETARY, "WBL_DRIVER_AMD is VK_DRIVER_ID_AMD_PROPRIETARY");

    // The game passes the platform it was built for.
#ifdef _WIN32
    CHECK(WBL_ON_WINDOWS == 1, "a Windows build says so");
#else
    CHECK(WBL_ON_WINDOWS == 0, "a build elsewhere says so");
#endif

    if (!fails) printf("wb lazy choice: all checks passed\n");
    return fails != 0;
}
