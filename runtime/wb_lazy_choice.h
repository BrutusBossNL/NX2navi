// Whether the lazy write-back runs (memory.c "Lazy write-back", vk_memory.c vk_mem_lazy_decide), from the player's
// setting, the platform and the GPU driver, with the reason the log gives. Own header so tests/wb_lazy_check.c checks
// it without Vulkan.
//
// The lazy write-back makes pages of guest memory inaccessible while a host-pointer import of them is bound to a buffer
// the GPU writes, and copies their bytes down from inside the access-fault handler of whichever thread touched them. A
// driver is free to handle both differently, so the default is on only where it has run:
//   - Windows, NVIDIA's driver: on. Measured and played there.
//   - Windows, AMD's driver for RDNA 3 and newer: on. Builds with this default were measured on an RX 7900 XT (driver
//     26.10.1) standing in Hyrule Field and walking through a village (vk_exec_xfer.c oq_take and vk_exec_draw.c
//     have numbers from that card). Whether fights were played on it is not recorded.
//   - Windows, AMD's driver branch for RDNA 1 and 2 (RX 5000 and 6000; its driverInfo reads "AMD proprietary shader
//     compiler", as vk_caps.c amd_sc tells it): off. Nobody has run it there under our eyes, and the one log of it
//     there is an RX 6600 that lost its device 3.5 minutes into play (146 fetches in the 1,200 frames before; the
//     GPU's last mark was inside a draw pass, not a fetch). That log does not prove the lazy write-back the cause:
//     no failure path was found in the code, and cards with 8 GB lost their device the same way on NVIDIA before it
//     existed. A second report, of a game that ended in fights on an AMD card of unknown model and ran for hours with
//     a settings file that turns it off, changed several settings at once. Until one of these cards has run it with
//     a log to compare, what shaders write goes back at the end of every submission there, as before 0.7.1, at that
//     cost (about 90 fps against 65 standing in Hyrule Field on an RTX 5090; not measured on these cards).
//   - Windows, any other driver: on, as 0.7.1 shipped it. No report from one.
//   - elsewhere: off, as before. The fetch inside the fault handler has only run on Windows.
// A value the player sets wins everywhere: wb_lazy = 1 turns it on for an RX 6000 card, wb_lazy = 0 off for any.
#pragma once

#include <stdint.h>
#include <string.h>

#define WBL_DRIVER_AMD 1   // VkDriverId of AMD's own driver, VK_DRIVER_ID_AMD_PROPRIETARY (vk_memory.c asserts the value)
#ifdef _WIN32
#define WBL_ON_WINDOWS 1
#else
#define WBL_ON_WINDOWS 0
#endif

typedef struct {
    int on;
    const char *why;   // the end of the log line "mem: lazy write-back on|off: "
} WblChoice;

// AMD's driver branch for RDNA 1 and 2: its own driver whose driverInfo names the older shader compiler.
static inline int wbl_amd_rdna12(uint32_t driver_id, const char *driver_info) {
    return driver_id == WBL_DRIVER_AMD && driver_info && strstr(driver_info, "shader compiler") != NULL;
}

// set: the value the player gave NX2NAVI_WB_LAZY (the environment or nx2navi.ini), NULL or "" when they gave none.
// windows: the game runs on Windows (WBL_ON_WINDOWS). driver_id, driver_info: the device's VkDriverId and the
// driverInfo of its VkPhysicalDeviceDriverProperties.
static inline WblChoice wb_lazy_choice(const char *set, int windows, uint32_t driver_id, const char *driver_info) {
    if (set && *set) {
        if (!strcmp(set, "0")) return (WblChoice){0, "wb_lazy = 0 is set"};
        return (WblChoice){1, "wb_lazy is set to on"};
    }
    if (!windows)
        return (WblChoice){0, "the default outside Windows, where its fetch inside the access-fault handler has not run "
                              "(wb_lazy = 1 turns it on)"};
    if (wbl_amd_rdna12(driver_id, driver_info))
        return (WblChoice){0, "the default on AMD's driver for RX 5000 and 6000 cards, where it has not been tried by "
                              "us and a report had the game lose its device with it on; what shaders write goes back "
                              "at the end of every submission, which costs frame rate in open areas (wb_lazy = 1 turns "
                              "it on)"};
    return (WblChoice){1, "the default on Windows (wb_lazy = 0 turns it off)"};
}
