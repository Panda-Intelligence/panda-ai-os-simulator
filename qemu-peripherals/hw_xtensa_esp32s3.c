/*
 * ESP32S3 SoC and Machine
 *
 * Copyright (c) 2023-2024 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#ifndef __EMSCRIPTEN__
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#endif

#include "qemu/error-report.h"
#include "qemu/log.h"
#ifndef __EMSCRIPTEN__
static void mofei_crash_handler(int sig) {
  void* bt[32];
  int n = backtrace(bt, 32);
  fprintf(stderr, "\n[CRASH] Signal %d. %d frames:\n", sig, n);
  for (int i = 0; i < n; i++) {
    Dl_info info;
    if (dladdr(bt[i], &info) && info.dli_sname) {
      fprintf(stderr, "  [%d] %s (%s+0x%lx)\n", i, info.dli_sname, info.dli_fname,
              (unsigned long)((char*)bt[i] - (char*)info.dli_saddr));
    } else {
      fprintf(stderr, "  [%d] %p (???)\n", i, bt[i]);
    }
  }
  fflush(stderr);
  _exit(128 + sig);
}
static void __attribute__((constructor)) mofei_install_crash_handler(void) {
  struct sigaction sa = {0};
  sa.sa_handler = mofei_crash_handler;
  sa.sa_flags = SA_RESETHAND;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}
#endif
#include "chardev/char-fe.h"
#include "core-esp32s3/core-isa.h"
#include "cpu_esp32s3.h"
#include "elf.h"
#include "exec/address-spaces.h"
#include "exec/exec-all.h"
#include "hw/boards.h"
#include "hw/char/esp32s3_uart.h"
#include "hw/char/lilygo_gnss_chardev.h"
#include "hw/display/esp_rgb.h"
#include "hw/dma/esp32s3_gdma.h"
#include "hw/gpio/esp32s3_gpio.h"
#include "hw/hw.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/i2c/esp32_i2c.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/lilygo_bq27220.h"
#include "hw/i2c/lilygo_display_input.h"
#include "hw/i2c/lilygo_i2c_probe.h"
#include "hw/i2c/lilygo_power_rtc.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/misc/esp32c3_jtag.h"
#include "hw/misc/esp32s3_aes.h"
#include "hw/misc/esp32s3_cache.h"
#include "hw/misc/esp32s3_ds.h"
#include "hw/misc/esp32s3_hmac.h"
#include "hw/misc/esp32s3_pms.h"
#include "hw/misc/esp32s3_rng.h"
#include "hw/misc/esp32s3_rsa.h"
#include "hw/misc/esp32s3_rtc_cntl.h"
#include "hw/misc/esp32s3_sha.h"
#include "hw/misc/esp32s3_xts_aes.h"
#include "hw/misc/ssi_psram.h"
#include "hw/misc/unimp.h"
#include "hw/net/can/esp32s3_twai.h"
#include "hw/nvram/esp32s3_efuse.h"
#include "hw/qdev-properties.h"
#include "hw/sd/dwc_sdmmc.h"
#include "hw/sd/sd.h"
#include "hw/ssi/esp32s3_spi.h"
#include "hw/ssi/lilygo_sx1262.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "hw/timer/esp32s3_systimer.h"
#include "hw/timer/esp32s3_timg.h"
#include "hw/xtensa/esp32s3_clk.h"
#include "hw/xtensa/esp32s3_intc.h"
#include "hw/xtensa/xtensa_memory.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/datadir.h"
#include "qemu/memalign.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "sysemu/cpus.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"
#include "target/xtensa/cpu.h"

#define MOFEI_DEFAULT_FIRMWARE_ELF_PATH "apps/panda-os/device/build/panda_os.elf"
#define MOFEI_BROWSER_FIRMWARE_KERNEL_PATH "/murphy-os/murphy_os-kernel.img"
#define MOFEI_BROWSER_FIRMWARE_SYMBOLS_PATH "/murphy-os/murphy_os-symbols.txt"

static char* mofei_resolve_rom_binary(void) {
  const char* mofei_rom = getenv("MOFEI_ROM_BINARY");
  if (mofei_rom && mofei_rom[0]) {
    return g_strdup(mofei_rom);
  }
  return qemu_find_file(QEMU_FILE_TYPE_BIOS, "esp32s3_rev0_rom.bin");
}

/* Mofei peripheral pin map (verified from lib/hal/HalGPIO.h + MofeiDisplay.h) */
#define MOFEI_EPD_CS_GPIO 5
#define MOFEI_EPD_DC_GPIO 6
#define MOFEI_EPD_RST_GPIO 7
#define MOFEI_EPD_BUSY_GPIO 8
#define MOFEI_TOUCH_SDA_GPIO 13
#define MOFEI_TOUCH_SCL_GPIO 12
#define MOFEI_TOUCH_INT_GPIO 44

/* Physical button GPIO pins (active-low, pull-up) */
#define MOFEI_BTN_KEYLOCK_GPIO 0 /* MOFEI_KEY_LOCK */
#define MOFEI_BTN_KEY1_GPIO 1    /* MOFEI_KEY1 */
#define MOFEI_BTN_KEY2_GPIO 2    /* MOFEI_KEY2 */

#define S37UC_EPD_CS_GPIO 3
#define S37UC_EPD_DC_GPIO 2
#define S37UC_EPD_RST_GPIO 0
#define S37UC_EPD_BUSY_GPIO 1
#define S37UC_TOUCH_SDA_GPIO 13
#define S37UC_TOUCH_SCL_GPIO 12
#define S37UC_TOUCH_INT_GPIO 21
#define S37UC_BTN_SIDEKEY_GPIO 17
#define S37UC_BTN_SIDEKEY1_GPIO 46

#define LILYGO_SD_CS_GPIO 12
#define LILYGO_RADIO_CS_GPIO 46
#define LILYGO_TOUCH_INT_GPIO 3
#define LILYGO_TOUCH_RESET_GPIO 9

/* Mofei sim: CPU1 synchronization for flash boot.
 *
 * On real hardware, CPU0 boots first and unstalls CPU1; both cores then
 * handshake through shared volatile bools.  In QEMU, CPU1 is permanently
 * halted (powered off), so we simulate CPU1's side of the handshake by
 * writing 1 to the appropriate BSS variables after a short delay.
 *
 * The ESP-IDF boot sequence has two spin loops in cpu_start.c:
 *   1. start_other_core() spins on s_cpu_up[0] && s_cpu_up[1]
 *   2. call_start_cpu0()  spins on s_cpu_inited[0] && s_cpu_inited[1]
 * CPU1 also spins on s_resume_cores in call_start_cpu1(), but since CPU1
 * never runs we don't need to set that — CPU0 never checks it.
 *
 * Addresses are resolved at machine-init time from the firmware ELF.
 */
static struct {
  uint32_t s_cpu_up_1;
  uint32_t s_cpu_inited_1;
  uint32_t s_system_inited_1;
  uint32_t s_system_full_inited;
  uint32_t adc_patch_fns[16]; /* ADC functions to patch with entry+ret0 */
  int adc_patch_count;
  int delay_ms;
} mofei_cpu1_sync = {0, 0, 0, 0, {0}, 0, 500};

static QEMUTimer* mofei_debug_pc_timer;
static int mofei_debug_pc_dumps;

/* Periodic tick timer that keeps the simulated CPU alive.
 * The ESP32-S3 firmware uses `waiti` to sleep until the next timer
 * interrupt.  In QEMU the CCOMPARE/CCOUNT timer emulation may not
 * generate interrupts fast enough, so the CPU halts and never wakes.
 * This timer fires every 10 ms of virtual time and forces a level-1
 * software interrupt (INTSET bit 0) on CPU0 so it resumes execution.
 * Note: Esp32s3SocState is defined later, so we use first_cpu from
 * the global CPU list and operate on the CPUState/CPUXtensaState. */
static QEMUTimer* mofei_cpu_tick_timer;
static uint32_t mofei_cpu_tick_count;
#include "hw/xtensa/mofei-sim-addrs.h"

/* Global instance — accessible from translate.c and exc_helper.c via the header */
MofeiSimBoardKind mofei_sim_active_board = MOFEI_SIM_BOARD_MOFEI;
MofeiSimAddrs mofei_sim_addrs;
uint32_t mofei_sim_wrap_malloc_addr;
uint32_t mofei_sim_reset_display_update_control_addr;
uint32_t mofei_display_writeLutFull_addr;
uint32_t mofei_display_writeLutFast_addr;
uint32_t mofei_display_writeLutDu_addr;

static const char* mofei_sim_board_name(void) {
  if (mofei_sim_board_is_s37uc()) {
    return "s37uc";
  }
  if (mofei_sim_board_is_m5papers3()) {
    return "m5papers3";
  }
  if (mofei_sim_board_is_lilygo_t5s3_pro()) {
    return "lilygo-t5s3-pro";
  }
  return "mofei";
}

static bool mofei_sim_path_mentions_s37uc(const char* path) {
  return path && (g_strrstr(path, "s37uc") || g_strrstr(path, "S37UC"));
}

static bool mofei_sim_path_mentions_lilygo_t5s3_pro(const char* path) {
  return path && (g_strrstr(path, "lilygo-t5s3-pro") || g_strrstr(path, "LILYGO-T5S3-PRO"));
}

static bool mofei_sim_path_mentions_m5papers3(const char* path) {
  return path && (g_strrstr(path, "m5papers3") || g_strrstr(path, "M5PAPERS3"));
}
static void mofei_sim_select_board(const char* firmware_path) {
  const char* env_board = getenv("MOFEI_SIM_BOARD");
  if (env_board && env_board[0]) {
    if (g_ascii_strcasecmp(env_board, "s37uc") == 0) {
      mofei_sim_active_board = MOFEI_SIM_BOARD_S37UC;
      return;
    }
    if (g_ascii_strcasecmp(env_board, "lilygo-t5s3-pro") == 0) {
      mofei_sim_active_board = MOFEI_SIM_BOARD_LILYGO_T5S3_PRO;
      return;
    }
    if (g_ascii_strcasecmp(env_board, "m5papers3") == 0) {
      mofei_sim_active_board = MOFEI_SIM_BOARD_M5PAPERS3;
      return;
    }
    if (g_ascii_strcasecmp(env_board, "mofei") == 0) {
      mofei_sim_active_board = MOFEI_SIM_BOARD_MOFEI;
      return;
    }
    fprintf(stderr, "[QEMU-SIM] WARNING: unsupported MOFEI_SIM_BOARD=%s, falling back to firmware path/default\n",
            env_board);
  }

  if (mofei_sim_path_mentions_s37uc(firmware_path)) {
    mofei_sim_active_board = MOFEI_SIM_BOARD_S37UC;
  } else if (mofei_sim_path_mentions_lilygo_t5s3_pro(firmware_path)) {
    mofei_sim_active_board = MOFEI_SIM_BOARD_LILYGO_T5S3_PRO;
  } else if (mofei_sim_path_mentions_m5papers3(firmware_path)) {
    mofei_sim_active_board = MOFEI_SIM_BOARD_M5PAPERS3;
  } else {
    mofei_sim_active_board = MOFEI_SIM_BOARD_MOFEI;
  }
}

typedef struct MofeiAllocPatchSpec {
  uint8_t size_kind;
  uint8_t old_ptr_arg;
  uint8_t caps_arg;
} MofeiAllocPatchSpec;

static MofeiAllocPatchSpec mofei_alloc_patch_spec_for_name(const char* name) {
  const MofeiAllocPatchSpec none = {0, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_NONE};
  if (!name) {
    return none;
  }
  if (strcmp(name, "heap_caps_malloc") == 0 || strcmp(name, "heap_caps_malloc_base") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A2, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_A3};
  }
  if (strcmp(name, "heap_caps_malloc_default") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A2, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "heap_caps_calloc") == 0 || strcmp(name, "heap_caps_calloc_base") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_MUL_A2_A3, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_A4};
  }
  if (strcmp(name, "heap_caps_realloc") == 0 || strcmp(name, "heap_caps_realloc_base") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A3, MOFEI_ALLOC_ARG_A2, MOFEI_ALLOC_ARG_A4};
  }
  if (strcmp(name, "heap_caps_realloc_default") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A3, MOFEI_ALLOC_ARG_A2, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "heap_caps_malloc_prefer") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A2, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_A4};
  }
  if (strcmp(name, "heap_caps_calloc_prefer") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_MUL_A2_A3, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_A5};
  }
  if (strcmp(name, "heap_caps_realloc_prefer") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A3, MOFEI_ALLOC_ARG_A2, MOFEI_ALLOC_ARG_A5};
  }
  if (strcmp(name, "heap_caps_free") == 0 || strcmp(name, "vPortFree") == 0 || strcmp(name, "free") == 0 ||
      strcmp(name, "heap_caps_free_base") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_FREE, MOFEI_ALLOC_ARG_A2, MOFEI_ALLOC_ARG_NONE};
  }
  if (strcmp(name, "_ZdlPv") == 0 || strcmp(name, "_ZdlPvj") == 0 || strcmp(name, "_ZdaPv") == 0 ||
      strcmp(name, "_ZdaPvj") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_FREE, MOFEI_ALLOC_ARG_A2, MOFEI_ALLOC_ARG_NONE};
  }
  if (strcmp(name, "_free_r") == 0 || strcmp(name, "multi_heap_free") == 0 ||
      strcmp(name, "multi_heap_free_impl") == 0 || strcmp(name, "tlsf_free") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_FREE, MOFEI_ALLOC_ARG_A3, MOFEI_ALLOC_ARG_NONE};
  }
  if (strcmp(name, "malloc") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A2, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "calloc") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_MUL_A2_A3, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "_malloc_r") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A3, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "_calloc_r") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_MUL_A3_A4, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "realloc") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A3, MOFEI_ALLOC_ARG_A2, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "_realloc_r") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A4, MOFEI_ALLOC_ARG_A3, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "heap_caps_aligned_alloc") == 0 || strcmp(name, "heap_caps_aligned_alloc_base") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A3, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_A4};
  }
  if (strcmp(name, "multi_heap_malloc") == 0 || strcmp(name, "multi_heap_malloc_impl") == 0 ||
      strcmp(name, "tlsf_malloc") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A3, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_NONE};
  }
  if (strcmp(name, "tlsf_memalign_offs") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A4, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_NONE};
  }
  if (strcmp(name, "_Znwj") == 0 || strcmp(name, "_Znaj") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A2, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_DEFAULT_POLICY};
  }
  if (strcmp(name, "pvPortMalloc") == 0) {
    return (MofeiAllocPatchSpec){MOFEI_ALLOC_SIZE_A2, MOFEI_ALLOC_ARG_NONE, MOFEI_ALLOC_ARG_NONE};
  }
  return none;
}

static void mofei_add_alloc_patch(MofeiSimAddrs* a, uint32_t fn_addr, MofeiAllocPatchSpec spec, const char* name) {
  if (!a || fn_addr == 0 || spec.size_kind == 0) {
    return;
  }
  const uint32_t patch_addr = fn_addr + 3;
  for (int i = 0; i < a->alloc_patch_count; i++) {
    if (a->alloc_patch_addrs[i].addr == patch_addr) {
      return;
    }
  }
  if (a->alloc_patch_count >= MOFEI_ALLOC_PATCH_MAX) {
    fprintf(stderr, "[QEMU-DBG] WARNING: allocator patch table full, skipping %s @ 0x%08x\n", name ? name : "?",
            patch_addr);
    return;
  }
  a->alloc_patch_addrs[a->alloc_patch_count].addr = patch_addr;
  a->alloc_patch_addrs[a->alloc_patch_count].size_kind = spec.size_kind;
  a->alloc_patch_addrs[a->alloc_patch_count].old_ptr_arg = spec.old_ptr_arg;
  a->alloc_patch_addrs[a->alloc_patch_count].caps_arg = spec.caps_arg;
  a->alloc_patch_count++;
  if (name) {
    fprintf(stderr, "[QEMU-DBG] RETW+ALLOC %s size=%u old=%u caps=%u @ 0x%08x\n", name, spec.size_kind,
            spec.old_ptr_arg, spec.caps_arg, patch_addr);
  }
}

static void mofei_add_retw_patch(MofeiSimAddrs* a, uint32_t retw_pc, const char* name) {
  if (!a || retw_pc == 0) {
    return;
  }
  for (int i = 0; i < a->retw_patch_count; i++) {
    if (a->retw_patch_addrs[i] == retw_pc) {
      return;
    }
  }
  if (a->retw_patch_count >= MOFEI_RETW_PATCH_MAX) {
    fprintf(stderr, "[QEMU-DBG] WARNING: RETW patch table full, skipping %s @ 0x%08x\n", name ? name : "?", retw_pc);
    return;
  }
  a->retw_patch_addrs[a->retw_patch_count++] = retw_pc;
}

static void mofei_add_retw_normal_precheck(MofeiSimAddrs* a, uint32_t retw_pc, const char* name) {
  if (!a || retw_pc == 0) {
    return;
  }
  for (int i = 0; i < a->retw_normal_precheck_count; i++) {
    if (a->retw_normal_precheck_addrs[i] == retw_pc) {
      return;
    }
  }
  if (a->retw_normal_precheck_count >= MOFEI_RETW_NORMAL_PRECHECK_MAX) {
    fprintf(stderr, "[QEMU-DBG] WARNING: RETW normal-precheck table full, skipping %s @ 0x%08x\n", name ? name : "?",
            retw_pc);
    return;
  }
  a->retw_normal_precheck_addrs[a->retw_normal_precheck_count++] = retw_pc;
  if (name) {
    fprintf(stderr, "[QEMU-DBG] RETW+NORMAL-PRECHECK %s @ 0x%08x\n", name, retw_pc);
  }
}

static void mofei_cpu_tick_timer_cb(void* opaque) {
  CPUState* cpu0 = first_cpu;
  if (!cpu0) {
    return;
  }
  CPUXtensaState* env = &XTENSA_CPU(cpu0)->env;

  mofei_cpu_tick_count++;

  const char* debug_tick = getenv("MOFEI_SIM_DEBUG_TICK");
  if ((debug_tick != NULL && debug_tick[0] != '\0' && strcmp(debug_tick, "0") != 0) || mofei_cpu_tick_count <= 3 ||
      (mofei_cpu_tick_count % 1000) == 0) {
    uint32_t serial_console = 0;
    uint32_t serial_gnss = 0;
    uint8_t console_pending = 0;
    if (debug_tick != NULL && debug_tick[0] != '\0' && strcmp(debug_tick, "0") != 0) {
      if (mofei_sim_addrs.murphySimulatorShellContext_addr != 0) {
        address_space_read(&address_space_memory, mofei_sim_addrs.murphySimulatorShellContext_addr + 44,
                           MEMTXATTRS_UNSPECIFIED, &serial_console, sizeof(serial_console));
      }
      if (mofei_sim_addrs.g_mofeiSimConsolePending_addr != 0) {
        address_space_read(&address_space_memory, mofei_sim_addrs.g_mofeiSimConsolePending_addr, MEMTXATTRS_UNSPECIFIED,
                           &console_pending, sizeof(console_pending));
      }
      if (serial_console != 0) {
        address_space_read(&address_space_memory, serial_console + 32, MEMTXATTRS_UNSPECIFIED, &serial_gnss,
                           sizeof(serial_gnss));
      }
    }
    fprintf(stderr, "[TICK] #%u: pc=0x%08x a0=0x%08x sp=0x%08x halted=%d CCOUNT=0x%08x PS=0x%08x WB=%u WS=0x%x\n",
            mofei_cpu_tick_count, env->pc, env->regs[0], env->regs[1], cpu0->halted, env->sregs[CCOUNT], env->sregs[PS],
            env->sregs[WINDOW_BASE], env->sregs[WINDOW_START]);
    if (debug_tick != NULL && debug_tick[0] != '\0' && strcmp(debug_tick, "0") != 0) {
      fprintf(stderr, "[TICK-CONTEXT] serial_console=0x%08x serial_gnss=0x%08x console_pending=%u\n", serial_console,
              serial_gnss, console_pending);
    }
  }

  /* If CPU is halted, inject a software interrupt to wake it.
   * INTSET bit 0 is a software interrupt that triggers at level 1.
   * We also advance CCOUNT to give the firmware a sense of time. */
  if (cpu0->halted) {
    env->sregs[CCOUNT] += 10000;
    cpu0->halted = 0;
    qemu_cpu_kick(cpu0);
  }

  env->sregs[CCOUNT] += 1000;

  timer_mod(mofei_cpu_tick_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100); /* 100 ms */
}

static void mofei_debug_pc_timer_cb(void* opaque) {
  XtensaCPU* cpu = XTENSA_CPU(first_cpu);
  if (cpu) {
    CPUXtensaState* env = &cpu->env;
    static uint32_t last_caller = 0;
    uint32_t caller = env->regs[0];
    {
      fprintf(stderr, "[QEMU-DBG] PC=0x%08x A0=0x%08x CCOUNT=0x%08x halted=%d PS=0x%08x excm=%d\n", env->pc, caller,
              env->sregs[CCOUNT], CPU(cpu)->halted, env->sregs[PS], (env->sregs[PS] & PS_EXCM) ? 1 : 0);
      last_caller = caller;
    }
  }
  if (++mofei_debug_pc_dumps < 50) {
    timer_mod(mofei_debug_pc_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1);
  } else if (mofei_debug_pc_dumps < 300) {
    timer_mod(mofei_debug_pc_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 500);
  }
}

static void mofei_cpu1_done_timer_cb(void* opaque) {
  if (mofei_cpu1_sync.s_cpu_up_1) {
    uint8_t one = 1;
    /* Check s_cpu_up[0] first */
    uint8_t cpu0_val = 0;
    uint32_t s_cpu_up_0 = mofei_cpu1_sync.s_cpu_up_1 - 1;
    address_space_read(&address_space_memory, s_cpu_up_0, MEMTXATTRS_UNSPECIFIED, &cpu0_val, 1);
    if (!cpu0_val) {
      address_space_write(&address_space_memory, s_cpu_up_0, MEMTXATTRS_UNSPECIFIED, &one, 1);
    }
    address_space_write(&address_space_memory, mofei_cpu1_sync.s_cpu_up_1, MEMTXATTRS_UNSPECIFIED, &one, 1);
  }
  if (mofei_cpu1_sync.s_cpu_inited_1) {
    uint8_t one = 1;
    /* Also set s_cpu_inited[0] if needed */
    uint8_t cpu0_val = 0;
    uint32_t s_cpu_inited_0 = mofei_cpu1_sync.s_cpu_inited_1 - 1;
    address_space_read(&address_space_memory, s_cpu_inited_0, MEMTXATTRS_UNSPECIFIED, &cpu0_val, 1);
    if (!cpu0_val) {
      address_space_write(&address_space_memory, s_cpu_inited_0, MEMTXATTRS_UNSPECIFIED, &one, 1);
    }
    address_space_write(&address_space_memory, mofei_cpu1_sync.s_cpu_inited_1, MEMTXATTRS_UNSPECIFIED, &one, 1);
  }
  if (mofei_cpu1_sync.s_system_inited_1) {
    uint8_t one = 1;
    uint32_t s_system_inited_0 = mofei_cpu1_sync.s_system_inited_1 - 1;
    address_space_write(&address_space_memory, s_system_inited_0, MEMTXATTRS_UNSPECIFIED, &one, 1);
    address_space_write(&address_space_memory, mofei_cpu1_sync.s_system_inited_1, MEMTXATTRS_UNSPECIFIED, &one, 1);
  }
  if (mofei_cpu1_sync.s_system_full_inited) {
    uint8_t one = 1;
    address_space_write(&address_space_memory, mofei_cpu1_sync.s_system_full_inited, MEMTXATTRS_UNSPECIFIED, &one, 1);
  }

  /* Patch ADC calibration functions to return 0 immediately.
   * NOTE: This timer callback is currently unused (never scheduled).
   * ADC patches are applied during ELF loading instead. */
  for (int i = 0; i < mofei_cpu1_sync.adc_patch_count; i++) {
    if (mofei_cpu1_sync.adc_patch_fns[i]) {
      uint8_t patch[] = {0x36, 0x21, 0x00, 0x02, 0x0c, 0x1d, 0xf0};
      address_space_write(&address_space_memory, mofei_cpu1_sync.adc_patch_fns[i], MEMTXATTRS_UNSPECIFIED, patch,
                          sizeof(patch));
    }
  }

  /* Arm a periodic debug timer to dump CPU0 PC every second — DISABLED */
#if 0
    if (!mofei_debug_pc_timer) {
        mofei_debug_pc_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                             mofei_debug_pc_timer_cb, NULL);
    }
    mofei_debug_pc_dumps = 0;
    timer_mod(mofei_debug_pc_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
#endif
}

#define TYPE_ESP32S3_SOC "xtensa.esp32s3"
#define ESP32S3_SOC(obj) OBJECT_CHECK(Esp32s3SocState, (obj), TYPE_ESP32S3_SOC)

#define TYPE_ESP32S3_CPU XTENSA_CPU_TYPE_NAME("esp32s3")

enum {
  ESP32S3_MEMREGION_IROM,
  ESP32S3_MEMREGION_DROM,
  ESP32S3_MEMREGION_DRAM,
  ESP32S3_MEMREGION_IRAM,
  ESP32S3_MEMREGION_ICACHE,
  ESP32S3_MEMREGION_DCACHE,
  ESP32S3_MEMREGION_RTCSLOW,
  ESP32S3_MEMREGION_RTCFAST,
  ESP32S3_MEMREGION_FRAMEBUF,
};

static const struct MemmapEntry {
  hwaddr base;
  hwaddr size;
} esp32s3_memmap[] = {
    [ESP32S3_MEMREGION_DROM] = {0x3ff00000, 0x20000},
    [ESP32S3_MEMREGION_IROM] = {0x40000000, 0x60000},
    [ESP32S3_MEMREGION_DRAM] = {MOFEI_SIM_INTERNAL_DRAM_BASE,
                                MOFEI_SIM_EXTERNAL_RAM_LIMIT - MOFEI_SIM_INTERNAL_DRAM_BASE},
    [ESP32S3_MEMREGION_IRAM] = {0x40370000, 0x80000},
    [ESP32S3_MEMREGION_DCACHE] = {0x3c000000, ESP32S3_EXTMEM_REGION_SIZE},
    [ESP32S3_MEMREGION_ICACHE] = {0x42000000, ESP32S3_EXTMEM_REGION_SIZE},
    [ESP32S3_MEMREGION_RTCSLOW] = {0x50000000, 0x2000},
    [ESP32S3_MEMREGION_RTCFAST] = {0x600fe000, 0x2000},
    /* Virtual Framebuffer, used for the graphical interface */
    [ESP32S3_MEMREGION_FRAMEBUF] = {0x20000000, ESP_RGB_MAX_VRAM_SIZE},
};

#define ESP32S3_SOC_RESET_PROCPU 0x1
#define ESP32S3_SOC_RESET_APPCPU 0x2
#define ESP32S3_SOC_RESET_PERIPH 0x4
#define ESP32S3_SOC_RESET_DIG (ESP32S3_SOC_RESET_PROCPU | ESP32S3_SOC_RESET_APPCPU | ESP32S3_SOC_RESET_PERIPH)
#define ESP32S3_SOC_RESET_RTC 0x8
#define ESP32S3_SOC_RESET_ALL (ESP32S3_SOC_RESET_RTC | ESP32S3_SOC_RESET_DIG)

#define ESP32S3_IO_WARNING 0

typedef struct Esp32s3SocState {
  /*< private >*/
  DeviceState parent_obj;

  /*< public >*/
  XtensaCPU cpu[ESP32S3_CPU_COUNT];
  Esp32s3IntMatrixState intmatrix;
  ESP32S3UARTState uart[ESP32S3_UART_COUNT];
  Esp32I2CState i2c[ESP32S3_I2C_COUNT];
  ESP32S3GPIOState gpio;
  Esp32s3RngState rng;
  Esp32S3TWAIState twai;

  Esp32s3RtcCntlState rtc_cntl;

  BusState rtc_bus;
  BusState periph_bus;

  MemoryRegion cpu_specific_mem[ESP32S3_CPU_COUNT];
  ESP32S3SpiState spi1;
  DeviceState* mofei_spi2;       /* GPSPI2 — selected e-ink SPI master */
  DeviceState* mofei_epd;        /* selected e-ink panel */
  DeviceState* lilygo_radio;     /* SX1262 on shared GPSPI2 target 1 */
  DeviceState* lilygo_touch;     /* GT911 on upstream I2C0 */
  Chardev* lilygo_gnss;          /* deterministic GNSS peer on UART2 */
  DeviceState* mofei_i2c_bridge; /* GPIO_I2C bridge for selected touch controller */
  DeviceState* mofei_touch;      /* selected touch controller */
  uint64_t kernel_entry;         /* Non-zero when -kernel ELF loaded */
  ESP32S3CacheState cache;
  ESP32S3EfuseState efuse;
  ESP32S3ClockState clock;
  ESP32S3GdmaState gdma;
  ESP32S3ShaState sha;
  ESP32S3AesState aes;
  ESP32S3RsaState rsa;
  ESP32S3HmacState hmac;
  ESP32S3DsState ds;
  ESP32S3PmsState pms;

  ESP32S3XtsAesState xts_aes;
  ESP32S3TimgState timg[2];
  ESP32S3SysTimerState systimer;

  ESP32C3UsbJtagState jtag;
  ESPRgbState rgb;

  MemoryRegion iomem;
  DWCSDMMCState sdmmc;
  DeviceState* eth;
  SsiPsramState* psram;

  uint32_t requested_reset;
  uint32_t suppress_reset_count;
} Esp32s3SocState;

static I2CBus* esp32s3_lilygo_i2c_bus(Esp32s3SocState* s) {
  if (mofei_sim_board_is_lilygo_t5s3_pro() || mofei_sim_board_is_m5papers3()) {
    return I2C_BUS(qdev_get_child_bus(DEVICE(&s->i2c[0]), "i2c"));
  }
  return NULL;
}

/* Temporary macro to mark the CPU as in non-debugging mode */
#define A_ASSIST_DEBUG_CORE_0_DEBUG_MODE_REG 0x098

/* "QEMU" as a 32-bit value, can be used by the application to to check whether it is running in
 * QEMU or on real hardware */
#define RGB_QEMU_ORIGIN 0x51454d55
#define RGB_QEMU_ORIGIN_REG 0x3F8

static void remove_cpu_watchpoints(XtensaCPU* xcs) {
  for (int i = 0; i < MAX_NDBREAK; ++i) {
    if (xcs->env.cpu_watchpoint[i]) {
      cpu_watchpoint_remove_by_ref(CPU(xcs), xcs->env.cpu_watchpoint[i]);
      xcs->env.cpu_watchpoint[i] = NULL;
    }
  }
}

static void esp32s3_dig_reset(void* opaque, int n, int level) {
  Esp32s3SocState* s = ESP32S3_SOC(opaque);
  if (level) {
    if (s->suppress_reset_count > 0) {
      s->suppress_reset_count--;
      return;
    }
    s->requested_reset = ESP32S3_SOC_RESET_DIG;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
  }
}

static void esp32s3_cpu_reset(void* opaque, int n, int level) {
  Esp32s3SocState* s = ESP32S3_SOC(opaque);
  if (level) {
    s->requested_reset = (n == 0) ? ESP32S3_SOC_RESET_PROCPU : ESP32S3_SOC_RESET_APPCPU;
    ShutdownCause cause = (n == 0) ? SHUTDOWN_CAUSE_GUEST_RESET : SHUTDOWN_CAUSE_SUBSYSTEM_RESET;
    qemu_system_reset_request(cause);
  }
}

static void esp32s3_soc_reset(DeviceState* dev) {
  Esp32s3SocState* s = ESP32S3_SOC(dev);
  if (s->requested_reset == 0) {
    s->requested_reset = ESP32S3_SOC_RESET_ALL;
  }
  if (s->requested_reset & ESP32S3_SOC_RESET_PERIPH) {
    device_cold_reset(DEVICE(&s->intmatrix));
    for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
      device_cold_reset(DEVICE(&s->uart[i]));
    }
  }
  if (s->requested_reset & ESP32S3_SOC_RESET_PROCPU) {
    xtensa_select_static_vectors(&s->cpu[0].env, s->rtc_cntl.stat_vector_sel[0]);
    remove_cpu_watchpoints(&s->cpu[0]);
    if (s->cpu[0].env.kernel_entry) {
      reset_mmu(&s->cpu[0].env);
      s->cpu[0].env.sregs[WINDOW_START] = 1;
      s->cpu[0].env.sregs[WINDOW_BASE] = 0;
      s->cpu[0].env.sregs[PS] &= ~PS_EXCM;
      s->cpu[0].env.sregs[PS] |= PS_WOE;
      s->cpu[0].env.sregs[VECBASE] = 0x40000000;
      s->cpu[0].env.pc = s->cpu[0].env.kernel_entry;
      CPU(&s->cpu[0])->exception_index = -1;
      CPU(&s->cpu[0])->halted = 0;
    }
    fprintf(stderr, "[SOC] PROCPU reset: pc=0x%x kernel_entry=0x%lx\n", s->cpu[0].env.pc,
            (unsigned long)s->cpu[0].env.kernel_entry);
  }
  if (s->requested_reset & ESP32S3_SOC_RESET_APPCPU && (ESP32S3_CPU_COUNT > 1)) {
    xtensa_select_static_vectors(&s->cpu[1].env, s->rtc_cntl.stat_vector_sel[1]);
    remove_cpu_watchpoints(&s->cpu[1]);
    if (!CPU(&s->cpu[1])->start_powered_off) {
      cpu_reset(CPU(&s->cpu[1]));
    }
  }
  s->requested_reset = 0;
}

static void esp32s3_cpu_stall(void* opaque, int n, int level) {}

static void esp32s3_clk_update(void* opaque, int n, int level) {
  if (!level) {
    return;
  }
}

static void esp32s3_soc_add_periph_device(MemoryRegion* dest, void* dev, hwaddr dport_base_addr) {
  MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
  /* Use priority 1 so real peripherals shadow the catch-all iomem region
   * mapped at 0x60000000 (ESP32S3_IO_START_ADDR).  Without this, the
   * iomem handler silently drops all reads/writes to UART0, SPI, etc. */
  memory_region_add_subregion_overlap(dest, dport_base_addr, mr, 1);
  MemoryRegion* mr_apb = g_new(MemoryRegion, 1);
  char* name = g_strdup_printf("mr-apb-0x%08x", (uint32_t)dport_base_addr);
  memory_region_init_alias(mr_apb, OBJECT(dev), name, mr, 0, memory_region_size(mr));
  g_free(name);
}

#define MB (1024 * 1024)

static void esp32s3_init_spi_flash(Esp32s3SocState* ms, BlockBackend* blk) {
  DeviceState* spi_master = DEVICE(&ms->spi1);
  BusState* spi_bus = qdev_get_child_bus(spi_master, "spi");
  const char* flash_model = NULL;
  int64_t image_size = blk_getlength(blk);

  switch (image_size) {
    case 2 * MB:
      flash_model = "w25x16";
      break;
    case 4 * MB:
      flash_model = "gd25q32";
      break;
    case 8 * MB:
      flash_model = "gd25q64";
      break;
    case 16 * MB:
      flash_model = "is25lp128";
      break;
    default:
      error_report("Drive size error: only 2, 4, 8, and 16MB images are supported");
      return;
  }

  /* Create the SPI flash model */
  DeviceState* flash_dev = qdev_new(flash_model);
  qdev_prop_set_drive(flash_dev, "drive", blk);

  /* Realize the SPI flash, its "drive" (blk) property must already be set! */
  qdev_realize(flash_dev, spi_bus, &error_fatal);
  qdev_connect_gpio_out_named(spi_master, SSI_GPIO_CS, 0, qdev_get_gpio_in_named(flash_dev, SSI_GPIO_CS, 0));
}

static void esp32s3_machine_init_psram(Esp32s3SocState* ms, uint32_t size_mbytes) {
  /* PSRAM attached to SPI1, CS1 */
  DeviceState* spi_master = DEVICE(&ms->spi1);
  BusState* spi_bus = qdev_get_child_bus(spi_master, "spi");
  DeviceState* psram = qdev_new(TYPE_SSI_PSRAM);
  qdev_prop_set_uint32(psram, "size_mbytes", size_mbytes);
  qdev_prop_set_uint8(psram, "cs", 1);
  qdev_realize(psram, spi_bus, &error_fatal);
  ms->psram = SSI_PSRAM(psram);
  qdev_connect_gpio_out_named(spi_master, SSI_GPIO_CS, 1, qdev_get_gpio_in_named(psram, SSI_GPIO_CS, 0));
}

static void esp32s3_machine_init_sd(Esp32s3SocState* ss, BlockBackend* blk) {
  if (blk != NULL) {
    DeviceState* card;

    card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
    /* See the comment on not using sysbus-default in esp32_machine_init_i2c */
    DeviceState* sdmmc = DEVICE(&ss->sdmmc);
    SDBus* sd_bus = SD_BUS(qdev_get_child_bus(sdmmc, "sd-bus"));
    qdev_realize_and_unref(card, BUS(sd_bus), &error_fatal);
  }
}

static void esp32s3_machine_attach_lilygo_spi_sd(DeviceState* spi2, BlockBackend* blk, int target) {
  SSIBus* spi_bus = (SSIBus*)qdev_get_child_bus(spi2, "spi");
  DeviceState* adapter = qdev_new("ssi-sd");
  qdev_prop_set_uint8(adapter, "cs", target);
  qdev_realize(adapter, BUS(spi_bus), &error_fatal);

  qemu_irq adapter_cs = qdev_get_gpio_in_named(adapter, SSI_GPIO_CS, 0);
  qemu_set_irq(adapter_cs, 1);
  qdev_connect_gpio_out_named(spi2, "target-select", target, adapter_cs);

  DeviceState* card = qdev_new(TYPE_SD_CARD_SPI);
  if (target == 0 && blk != NULL) qdev_prop_set_bit(card, "lilygo-storage", true);
  qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
  qdev_realize_and_unref(card, qdev_get_child_bus(adapter, "sd-bus"), &error_fatal);
}

static DeviceState* esp32s3_machine_init_lilygo_spi(DeviceState* spi2, BlockBackend* blk) {
  esp32s3_machine_attach_lilygo_spi_sd(spi2, blk, 0);
  fprintf(stderr, "[LILYGO-SPI] upstream ssi-sd media attached owner=sd cs=%d\n", LILYGO_SD_CS_GPIO);

  SSIBus* spi_bus = (SSIBus*)qdev_get_child_bus(spi2, "spi");
  DeviceState* radio = qdev_new(TYPE_LILYGO_SX1262);
  qdev_prop_set_uint8(radio, "cs", 1);
  qdev_realize(radio, BUS(spi_bus), &error_fatal);
  qemu_irq radio_cs = qdev_get_gpio_in_named(radio, SSI_GPIO_CS, 0);
  qemu_set_irq(radio_cs, 1);
  qdev_connect_gpio_out_named(spi2, "target-select", 1, radio_cs);
  fprintf(stderr, "[LILYGO-SPI] SX1262 attached owner=radio cs=%d\n", LILYGO_RADIO_CS_GPIO);
  return radio;
}

struct Esp32s3MachineState {
  MachineState parent;

  Esp32s3SocState esp32s3;
  DeviceState* flash_dev;
};
#define TYPE_ESP32S3_MACHINE MACHINE_TYPE_NAME("esp32s3")

static void esp32s3_init_openeth(Esp32s3SocState* ms) {
  MemoryRegion* mr = NULL;
  SysBusDevice* sbd = NULL;

  MemoryRegion* sys_mem = get_system_memory();

  /* Create a new OpenCores Ethernet component */
  DeviceState* open_eth_dev = qemu_create_nic_device("open_eth", true, NULL);
  if (!open_eth_dev) {
    return;
  }
  ms->eth = open_eth_dev;
  sbd = SYS_BUS_DEVICE(open_eth_dev);
  sysbus_realize(sbd, &error_fatal);

  /* OpenCores Ethernet has two memory regions: one for registers and one for descriptors,
   * we need to provide one I/O range for each of them */
  mr = sysbus_mmio_get_region(sbd, 0);
  memory_region_add_subregion_overlap(sys_mem, DR_REG_EMAC_BASE, mr, 0);
  mr = sysbus_mmio_get_region(sbd, 1);
  memory_region_add_subregion_overlap(sys_mem, DR_REG_EMAC_BASE + 0x400, mr, 0);

  sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(&ms->intmatrix), ETS_ETH_MAC_INTR_SOURCE));
}

static void esp32s3_soc_realize(DeviceState* dev, Error** errp) {
  Esp32s3SocState* s = ESP32S3_SOC(dev);
  MachineState* ms = MACHINE(qdev_get_machine());
  DeviceState* intmatrix_dev = DEVICE(&s->intmatrix);
  MemoryRegion* sys_mem = get_system_memory();

  const struct MemmapEntry* memmap = esp32s3_memmap;

  MemoryRegion* iram = g_new(MemoryRegion, 1);
  MemoryRegion* rtcslow = g_new(MemoryRegion, 1);
  MemoryRegion* rtcfast = g_new(MemoryRegion, 1);

  MemoryRegion* irom_cpu0 = NULL;
  for (int i = 0; i < ms->smp.cpus; ++i) {
    MemoryRegion* drom = g_new(MemoryRegion, 1);
    MemoryRegion* irom = g_new(MemoryRegion, 1);

    char name[20];
    snprintf(name, sizeof(name), "esp32s3.irom.cpu%d", i);
    memory_region_init_rom(irom, NULL, name, memmap[ESP32S3_MEMREGION_IROM].size, &error_fatal);
    memory_region_add_subregion(&s->cpu_specific_mem[i], memmap[ESP32S3_MEMREGION_IROM].base, irom);

    const hwaddr offset_in_orig = 0x40000;
    snprintf(name, sizeof(name), "esp32s3.drom.cpu%d", i);
    memory_region_init_alias(drom, NULL, name, irom, offset_in_orig, memmap[ESP32S3_MEMREGION_DROM].size);
    memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_DROM].base, drom);

    if (i == 0) {
      irom_cpu0 = irom;
    }
  }

  /* Low-address aliases for windowed-call return addresses.
   *
   * The Xtensa callx4/callx8/callx12 instructions encode the window
   * increment in bits [31:30] of the return address, leaving only 30 bits
   * for the actual PC.  When code above 0x40000000 executes a callx8,
   * the return address is truncated: e.g. callx8 from 0x40043CE1 stores
   * 0x80043CE4, and retw jumps to 0x00043CE4.  On real ESP32-S3 hardware
   * the ROM, IRAM, and cached flash are aliased at both their canonical
   * addresses and the 30-bit-truncated addresses.  QEMU must create the
   * same aliases.
   *
   * Affected regions:
   *   0x40000000 (IROM)    → alias at 0x00000000
   *   0x40370000 (IRAM)    → alias at 0x00370000
   *   0x42000000 (ICACHE)  → alias at 0x02000000  (added later)
   */
  if (irom_cpu0) {
    MemoryRegion* irom_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(irom_alias, OBJECT(s), "esp32s3.irom-low", irom_cpu0, 0,
                             memmap[ESP32S3_MEMREGION_IROM].size);
    /* Must be added to the CPU's own address space (cpu_specific_mem),
     * not sys_mem, because the CPU fetches instructions through its
     * private address space and the alias must be resolved at that level.
     * Use priority 1 to shadow the sys_mem alias inside cpu_specific_mem. */
    memory_region_add_subregion_overlap(&s->cpu_specific_mem[0], 0x00000000, irom_alias, 1);
    fprintf(stderr, "[QEMU-DBG] IROM low-address alias at 0x00000000 (%lu KB)\n",
            (unsigned long)(memmap[ESP32S3_MEMREGION_IROM].size / 1024));
  }

  memory_region_init_ram(iram, NULL, "esp32s3.iram", memmap[ESP32S3_MEMREGION_IRAM].size, &error_fatal);
  memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_IRAM].base, iram);

  /* IRAM low-address alias (0x4037xxxx → 0x0037xxxx) */
  {
    MemoryRegion* iram_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(iram_alias, OBJECT(s), "esp32s3.iram-low", iram, 0, memmap[ESP32S3_MEMREGION_IRAM].size);
    memory_region_add_subregion_overlap(&s->cpu_specific_mem[0], 0x00370000, iram_alias, 1);
    fprintf(stderr, "[QEMU-DBG] IRAM low-address alias at 0x00370000 (%lu KB)\n",
            (unsigned long)(memmap[ESP32S3_MEMREGION_IRAM].size / 1024));
  }

  memory_region_init_ram(rtcslow, NULL, "esp32s3.rtcslow", memmap[ESP32S3_MEMREGION_RTCSLOW].size, &error_fatal);
  memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_RTCSLOW].base, rtcslow);

  memory_region_init_ram(rtcfast, NULL, "esp32s3.rtcfast", memmap[ESP32S3_MEMREGION_RTCFAST].size, &error_fatal);
  memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_RTCFAST].base, rtcfast);

  for (int i = 0; i < ms->smp.cpus; ++i) {
    qdev_realize(DEVICE(&s->cpu[i]), NULL, &error_fatal);
  }

  for (int i = 0; i < ESP32S3_CPU_COUNT; ++i) {
    char name[16];
    snprintf(name, sizeof(name), "cpu%d", i);
    object_property_set_link(OBJECT(&s->intmatrix), name, OBJECT(qemu_get_cpu(i)), &error_abort);
  }
  qdev_realize(DEVICE(&s->intmatrix), &s->periph_bus, &error_fatal);

  qdev_realize(DEVICE(&s->rtc_cntl), &s->rtc_bus, &error_fatal);
  esp32s3_soc_add_periph_device(sys_mem, &s->rtc_cntl, DR_REG_RTCCNTL_BASE);

  qdev_connect_gpio_out_named(DEVICE(&s->rtc_cntl), ESP32S3_RTC_DIG_RESET_GPIO, 0,
                              qdev_get_gpio_in_named(dev, ESP32S3_RTC_DIG_RESET_GPIO, 0));
  qdev_connect_gpio_out_named(DEVICE(&s->rtc_cntl), ESP32S3_RTC_CLK_UPDATE_GPIO, 0,
                              qdev_get_gpio_in_named(dev, ESP32S3_RTC_CLK_UPDATE_GPIO, 0));
  for (int i = 0; i < ms->smp.cpus; ++i) {
    qdev_connect_gpio_out_named(DEVICE(&s->rtc_cntl), ESP32S3_RTC_CPU_RESET_GPIO, i,
                                qdev_get_gpio_in_named(dev, ESP32S3_RTC_CPU_RESET_GPIO, i));
    qdev_connect_gpio_out_named(DEVICE(&s->rtc_cntl), ESP32S3_RTC_CPU_STALL_GPIO, i,
                                qdev_get_gpio_in_named(dev, ESP32S3_RTC_CPU_STALL_GPIO, i));
  }

  for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
    const hwaddr uart_base[] = {DR_REG_UART_BASE, DR_REG_UART1_BASE, DR_REG_UART2_BASE};
    qdev_realize(DEVICE(&s->uart[i]), &s->periph_bus, &error_fatal);
    esp32s3_soc_add_periph_device(sys_mem, &s->uart[i], uart_base[i]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart[i]), 0, qdev_get_gpio_in(intmatrix_dev, ETS_UART0_INTR_SOURCE + i));
  }

  for (int i = 0; i < ESP32S3_I2C_COUNT; ++i) {
    const hwaddr i2c_base[] = {DR_REG_I2C_EXT_BASE, DR_REG_I2C1_EXT_BASE};
    qdev_prop_set_bit(DEVICE(&s->i2c[i]), "esp32s3-compat", true);
    qdev_realize(DEVICE(&s->i2c[i]), &s->periph_bus, &error_fatal);
    esp32s3_soc_add_periph_device(sys_mem, &s->i2c[i], i2c_base[i]);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c[i]), 0, qdev_get_gpio_in(intmatrix_dev, ETS_I2C_EXT0_INTR_SOURCE + i));
  }

  qdev_realize(DEVICE(&s->sdmmc), &s->periph_bus, &error_fatal);
  esp32s3_soc_add_periph_device(sys_mem, &s->sdmmc, DR_REG_SDMMC_BASE);
  sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdmmc), 0, qdev_get_gpio_in(intmatrix_dev, ETS_SDIO_HOST_INTR_SOURCE));

  /* Emulation of APB_CTRL_DATE_REG, needed for ECO3 revision detection.
   * This is a small hack to avoid creating a whole new device just to emulate one
   * register.
   */
  const hwaddr apb_ctrl_regs = DR_REG_APB_CTRL_BASE;
  MemoryRegion* apbctrl_mem = g_new(MemoryRegion, 1);
  memory_region_init_ram(apbctrl_mem, NULL, "esp32s3.apbctrl", 0x400 /* bytes */, &error_fatal);
  memory_region_add_subregion(sys_mem, apb_ctrl_regs, apbctrl_mem);
  uint32_t apb_ctrl_date_reg_val = 0x16042000 | 0x80000000; /* MSB indicates ECO3 silicon revision */
  uint32_t qemu_sig = RGB_QEMU_ORIGIN;
  cpu_physical_memory_write(apb_ctrl_regs + 0x7c, &apb_ctrl_date_reg_val, 4);
  cpu_physical_memory_write(apb_ctrl_regs + RGB_QEMU_ORIGIN_REG, &qemu_sig, 4);

  qemu_register_reset((QEMUResetHandler*)esp32s3_soc_reset, dev);

  /* TWAI realization */
  {
    /* Initialize and realize the TWAI device */
    sysbus_realize(SYS_BUS_DEVICE(&s->twai), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->twai), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_TWAI_BASE, mr, 0);
    /* Connect TWAI interrupt to the interrupt matrix */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->twai), 0, qdev_get_gpio_in(intmatrix_dev, ETS_TWAI_INTR_SOURCE));
  }
}

static uint64_t esp32s3_io_read(void* opaque, hwaddr addr, unsigned int size) {
  if (addr >= 0x40000 && addr < 0x42000) {
    return 1u << 16;
  }
  return 0;
}

static void esp32s3_io_write(void* opaque, hwaddr addr, uint64_t value, unsigned int size) {
  if (addr >= 0x40000 && addr < 0x42000) {
    static int adc_wr_log = 0;
    if (adc_wr_log < 5) {
      fprintf(stderr, "[QEMU-DBG] ADC IO write: addr=0x%lx value=0x%lx size=%u\n", (unsigned long)addr,
              (unsigned long)value, size);
      adc_wr_log++;
    }
  }
#if ESP32S3_IO_WARNING
  warn_report("[ESP32-S3] Unsupported write $%08lx = %08lx\n", ESP32S3_IO_START_ADDR + addr, value);
#endif
}

/* Define operations for I/OS */
static const MemoryRegionOps esp32s3_io_ops = {
    .read = esp32s3_io_read,
    .write = esp32s3_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32s3_soc_init(Object* obj) {
  Esp32s3SocState* s = ESP32S3_SOC(obj);
  MachineState* ms = MACHINE(qdev_get_machine());
  char name[16];
  MemoryRegion* system_memory = get_system_memory();

  qbus_init(&s->periph_bus, sizeof(s->periph_bus), TYPE_SYSTEM_BUS, DEVICE(s), "esp32-periph-bus");
  qbus_init(&s->rtc_bus, sizeof(s->rtc_bus), TYPE_SYSTEM_BUS, DEVICE(s), "esp32-rtc-bus");

  for (int i = 0; i < ms->smp.cpus; ++i) {
    snprintf(name, sizeof(name), "cpu%d", i);

    object_initialize_child(obj, name, &s->cpu[i], TYPE_ESP32S3_CPU);
    // Allocate memory for TIE registers
    s->cpu[i].env.ext = qemu_memalign(16, sizeof(CPUXtensaEsp32s3State));

    if (i == 0) {
      s->cpu[i].env.sregs[PRID] = 0xcdcd;
    }
    if (i == 1) {
      s->cpu[i].env.sregs[PRID] = 0xabab;
    }

    snprintf(name, sizeof(name), "cpu%d-mem", i);
    memory_region_init(&s->cpu_specific_mem[i], NULL, name, UINT32_MAX);

    CPUState* cs = CPU(&s->cpu[i]);
    cs->num_ases = 1;
    cpu_address_space_init(cs, 0, "cpu-memory", &s->cpu_specific_mem[i]);

    MemoryRegion* cpu_view_sysmem = g_new(MemoryRegion, 1);
    snprintf(name, sizeof(name), "cpu%d-sysmem", i);
    memory_region_init_alias(cpu_view_sysmem, NULL, name, system_memory, 0, UINT32_MAX);
    memory_region_add_subregion_overlap(&s->cpu_specific_mem[i], 0, cpu_view_sysmem, 0);
    cs->memory = &s->cpu_specific_mem[i];
  }

  for (int i = 0; i < ESP32S3_UART_COUNT; ++i) {
    snprintf(name, sizeof(name), "uart%d", i);
    object_initialize_child(obj, name, &s->uart[i], TYPE_ESP32S3_UART);
  }

  for (int i = 0; i < ESP32S3_I2C_COUNT; ++i) {
    snprintf(name, sizeof(name), "i2c%d", i);
    object_initialize_child(obj, name, &s->i2c[i], TYPE_ESP32_I2C);
  }

  object_property_add_alias(obj, "serial0", OBJECT(&s->uart[0]), "chardev");
  object_property_add_alias(obj, "serial1", OBJECT(&s->uart[1]), "chardev");
  // object_property_add_alias(obj, "serial2", OBJECT(&s->uart[2]), "chardev");
  qdev_prop_set_chr(DEVICE(&s->uart[0]), "chardev", serial_hd(0));
  qdev_prop_set_chr(DEVICE(&s->uart[1]), "chardev", serial_hd(1));
  // qdev_prop_set_chr(DEVICE(&s->uart[2]), "chardev", serial_hd(2));

  object_initialize_child(obj, "intmatrix", &s->intmatrix, TYPE_ESP32S3_INTMATRIX);

  object_initialize_child(obj, "rtc_cntl", &s->rtc_cntl, TYPE_ESP32S3_RTC_CNTL);

  qdev_init_gpio_in_named(DEVICE(s), esp32s3_dig_reset, ESP32S3_RTC_DIG_RESET_GPIO, 1);
  qdev_init_gpio_in_named(DEVICE(s), esp32s3_cpu_reset, ESP32S3_RTC_CPU_RESET_GPIO, ESP32S3_CPU_COUNT);
  qdev_init_gpio_in_named(DEVICE(s), esp32s3_cpu_stall, ESP32S3_RTC_CPU_STALL_GPIO, ESP32S3_CPU_COUNT);
  qdev_init_gpio_in_named(DEVICE(s), esp32s3_clk_update, ESP32S3_RTC_CLK_UPDATE_GPIO, 1);

  object_initialize_child(obj, "twai", &s->twai, TYPE_ESP32S3_TWAI);

  object_initialize_child(obj, "sdmmc", &s->sdmmc, TYPE_DWC_SDMMC);
}

static Property esp32s3_soc_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32s3_soc_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);

  dc->realize = esp32s3_soc_realize;
  device_class_set_props(dc, esp32s3_soc_properties);
}

static const TypeInfo esp32s3_soc_info = {.name = TYPE_ESP32S3_SOC,
                                          .parent = TYPE_DEVICE,
                                          .instance_size = sizeof(Esp32s3SocState),
                                          .instance_init = esp32s3_soc_init,
                                          .class_init = esp32s3_soc_class_init};

static void esp32s3_soc_register_types(void) { type_register_static(&esp32s3_soc_info); }

type_init(esp32s3_soc_register_types)

    static uint64_t translate_phys_addr(void* opaque, uint64_t addr) {
  XtensaCPU* cpu = opaque;

  return cpu_get_phys_page_debug(CPU(cpu), addr);
}

OBJECT_DECLARE_SIMPLE_TYPE(Esp32s3MachineState, ESP32S3_MACHINE)

// -----------------------------------------------

static void esp32s3_soc_add_unimp_device(MemoryRegion* dest, const char* name, hwaddr dport_base_addr, size_t size) {
  create_unimplemented_device(name, dport_base_addr, size);
  char* name_apb = g_strdup_printf("%s-apb", name);
  create_unimplemented_device(name_apb, dport_base_addr + APB_REG_BASE, size);
  g_free(name_apb);
}

typedef struct MofeiFirmwareSymbol {
  uint32_t address;
  uint32_t size;
  char type;
  char* name;
} MofeiFirmwareSymbol;

static GArray* mofei_firmware_symbols;

static bool mofei_symbol_type_is_function(char type) {
  return type == 'T' || type == 't' || type == 'W' || type == 'w';
}

static void mofei_clear_firmware_symbols(void) {
  if (!mofei_firmware_symbols) {
    return;
  }
  for (guint i = 0; i < mofei_firmware_symbols->len; i++) {
    MofeiFirmwareSymbol* symbol = &g_array_index(mofei_firmware_symbols, MofeiFirmwareSymbol, i);
    g_free(symbol->name);
  }
  g_array_free(mofei_firmware_symbols, TRUE);
  mofei_firmware_symbols = NULL;
}

static const char* mofei_resolve_firmware_symbols_path(void) {
  const char* symbols_path = getenv("MOFEI_FIRMWARE_SYMBOLS");
  if (symbols_path && symbols_path[0]) {
    return symbols_path;
  }
  if (g_file_test(MOFEI_BROWSER_FIRMWARE_SYMBOLS_PATH, G_FILE_TEST_IS_REGULAR)) {
    return MOFEI_BROWSER_FIRMWARE_SYMBOLS_PATH;
  }
  return NULL;
}

static void mofei_load_firmware_symbols(void) {
  const char* symbols_path = mofei_resolve_firmware_symbols_path();
  if (!symbols_path || !symbols_path[0]) {
    return;
  }

  mofei_clear_firmware_symbols();

  gchar* contents = NULL;
  gsize contents_len = 0;
  if (!g_file_get_contents(symbols_path, &contents, &contents_len, NULL) || !contents) {
    fprintf(stderr, "[QEMU-SIM] WARNING: could not read firmware symbols '%s'\n", symbols_path);
    return;
  }

  mofei_firmware_symbols = g_array_new(FALSE, FALSE, sizeof(MofeiFirmwareSymbol));
  gchar** lines = g_strsplit(contents, "\n", -1);
  for (gchar** cursor = lines; cursor && *cursor; cursor++) {
    char* line = g_strstrip(*cursor);
    if (!line[0] || line[0] == '#') {
      continue;
    }

    unsigned int address = 0;
    unsigned int size = 0;
    char type = 0;
    int name_offset = 0;
    if (sscanf(line, "%x %x %c %n", &address, &size, &type, &name_offset) < 3 || name_offset <= 0) {
      continue;
    }

    char* name = g_strstrip(line + name_offset);
    if (!name[0]) {
      continue;
    }

    MofeiFirmwareSymbol symbol = {
        .address = (uint32_t)address,
        .size = (uint32_t)size,
        .type = type,
        .name = g_strdup(name),
    };
    g_array_append_val(mofei_firmware_symbols, symbol);
  }
  g_strfreev(lines);
  g_free(contents);

  fprintf(stderr, "[QEMU-SIM] Loaded %u firmware symbols from %s\n", mofei_firmware_symbols->len, symbols_path);
}

static const char* mofei_resolve_firmware_kernel_path(void) {
  const char* elf_path = getenv("MOFEI_FIRMWARE_ELF");
  if (elf_path && elf_path[0]) {
    return elf_path;
  }
  if (g_file_test(MOFEI_BROWSER_FIRMWARE_KERNEL_PATH, G_FILE_TEST_IS_REGULAR)) {
    return MOFEI_BROWSER_FIRMWARE_KERNEL_PATH;
  }
  return MOFEI_DEFAULT_FIRMWARE_ELF_PATH;
}

static const MofeiFirmwareSymbol* mofei_find_firmware_symbol(const char* sym_name) {
  if (!mofei_firmware_symbols || !sym_name || !sym_name[0]) {
    return NULL;
  }

  for (guint i = 0; i < mofei_firmware_symbols->len; i++) {
    const MofeiFirmwareSymbol* symbol = &g_array_index(mofei_firmware_symbols, MofeiFirmwareSymbol, i);
    if (symbol->address && symbol->name && strcmp(symbol->name, sym_name) == 0) {
      return symbol;
    }
  }

  return NULL;
}

/* Resolve a symbol's address from an ELF file's .symtab.
 * Returns the symbol value if found, or 0. */
static uint32_t mofei_resolve_elf_symbol(const uint8_t* elf_data, gsize elf_len, const char* sym_name) {
  const MofeiFirmwareSymbol* sidecar_symbol = mofei_find_firmware_symbol(sym_name);
  if (sidecar_symbol) {
    return sidecar_symbol->address;
  }

  if (!elf_data || elf_len < 52 || memcmp(elf_data, ELFMAG, 4) != 0) {
    return 0;
  }

  /* Read ELF header (assume LE — BE ELFs are pre-swapped by the caller) */
  uint32_t e_shoff = *(uint32_t*)(elf_data + 32);
  uint16_t e_shentsize = *(uint16_t*)(elf_data + 46);
  uint16_t e_shnum = *(uint16_t*)(elf_data + 48);

  /* Find SHT_SYMTAB (type 2) and its associated string table */
  uint32_t symtab_off = 0, symtab_size = 0, strtab_off = 0;
  for (int si = 0; si < e_shnum; si++) {
    uint32_t sh_off = e_shoff + si * e_shentsize;
    if (sh_off + 40 > elf_len) break;
    uint32_t sh_type = *(uint32_t*)(elf_data + sh_off + 4);
    if (sh_type == 2) { /* SHT_SYMTAB */
      symtab_off = *(uint32_t*)(elf_data + sh_off + 16);
      symtab_size = *(uint32_t*)(elf_data + sh_off + 20);
      uint32_t str_idx = *(uint32_t*)(elf_data + sh_off + 24);
      uint32_t str_sh_off = e_shoff + str_idx * e_shentsize;
      if (str_sh_off + 40 <= elf_len) {
        strtab_off = *(uint32_t*)(elf_data + str_sh_off + 16);
      }
      break;
    }
  }

  if (!symtab_off || !strtab_off) {
    return 0;
  }

  /* Iterate symbol table entries (Elf32_Sym = 16 bytes) */
  int n_syms = symtab_size / 16;
  int want_len = strlen(sym_name);
  for (int si = 0; si < n_syms; si++) {
    uint32_t sym_off = symtab_off + si * 16;
    if (sym_off + 16 > elf_len) break;
    uint32_t st_name = *(uint32_t*)(elf_data + sym_off);
    uint32_t st_value = *(uint32_t*)(elf_data + sym_off + 4);
    if (st_name == 0 || st_value == 0) continue;
    if (st_name + want_len >= elf_len) continue;
    if (strtab_off + st_name + want_len >= elf_len) continue;
    if (memcmp(elf_data + strtab_off + st_name, sym_name, want_len + 1) == 0) {
      return st_value;
    }
  }
  return 0;
}

static uint32_t mofei_resolve_elf_function_end(const uint8_t* elf_data, gsize elf_len, const char* sym_name) {
  const MofeiFirmwareSymbol* sidecar_symbol = mofei_find_firmware_symbol(sym_name);
  if (sidecar_symbol) {
    if (sidecar_symbol->size > 0) {
      return sidecar_symbol->address + sidecar_symbol->size;
    }

    uint32_t nearest_next = 0;
    for (guint i = 0; i < mofei_firmware_symbols->len; i++) {
      const MofeiFirmwareSymbol* candidate = &g_array_index(mofei_firmware_symbols, MofeiFirmwareSymbol, i);
      if (!mofei_symbol_type_is_function(candidate->type)) continue;
      if (candidate->address <= sidecar_symbol->address) continue;
      if (!nearest_next || candidate->address < nearest_next) {
        nearest_next = candidate->address;
      }
    }
    return nearest_next;
  }

  if (!elf_data || elf_len < 52 || memcmp(elf_data, ELFMAG, 4) != 0) {
    return 0;
  }

  uint32_t e_shoff = *(uint32_t*)(elf_data + 32);
  uint16_t e_shentsize = *(uint16_t*)(elf_data + 46);
  uint16_t e_shnum = *(uint16_t*)(elf_data + 48);

  uint32_t symtab_off = 0, symtab_size = 0, strtab_off = 0;
  for (int si = 0; si < e_shnum; si++) {
    uint32_t sh_off = e_shoff + si * e_shentsize;
    if (sh_off + 40 > elf_len) break;
    uint32_t sh_type = *(uint32_t*)(elf_data + sh_off + 4);
    if (sh_type == 2) { /* SHT_SYMTAB */
      symtab_off = *(uint32_t*)(elf_data + sh_off + 16);
      symtab_size = *(uint32_t*)(elf_data + sh_off + 20);
      uint32_t str_idx = *(uint32_t*)(elf_data + sh_off + 24);
      uint32_t str_sh_off = e_shoff + str_idx * e_shentsize;
      if (str_sh_off + 40 <= elf_len) {
        strtab_off = *(uint32_t*)(elf_data + str_sh_off + 16);
      }
      break;
    }
  }

  if (!symtab_off || !strtab_off) {
    return 0;
  }

  int n_syms = symtab_size / 16;
  int want_len = strlen(sym_name);
  uint32_t fn_start = 0;
  uint32_t fn_end = 0;

  for (int si = 0; si < n_syms; si++) {
    uint32_t sym_off = symtab_off + si * 16;
    if (sym_off + 16 > elf_len) break;
    uint32_t st_name = *(uint32_t*)(elf_data + sym_off);
    uint32_t st_value = *(uint32_t*)(elf_data + sym_off + 4);
    uint32_t st_size = *(uint32_t*)(elf_data + sym_off + 8);
    if (st_name == 0 || st_value == 0) continue;
    if (strtab_off + st_name + want_len >= elf_len) continue;
    if (memcmp(elf_data + strtab_off + st_name, sym_name, want_len + 1) == 0) {
      fn_start = st_value;
      if (st_size > 0) {
        fn_end = st_value + st_size;
      }
      break;
    }
  }

  if (!fn_start) {
    return 0;
  }
  if (fn_end > fn_start) {
    return fn_end;
  }

  uint32_t nearest_next = 0;
  for (int si = 0; si < n_syms; si++) {
    uint32_t sym_off = symtab_off + si * 16;
    if (sym_off + 16 > elf_len) break;
    uint32_t st_value = *(uint32_t*)(elf_data + sym_off + 4);
    uint8_t st_info = *(uint8_t*)(elf_data + sym_off + 12);
    if ((st_info & 0x0f) != 2) continue; /* STT_FUNC */
    if (st_value <= fn_start) continue;
    if (!nearest_next || st_value < nearest_next) {
      nearest_next = st_value;
    }
  }

  return nearest_next;
}

static uint32_t mofei_find_last_retw_in_elf_span(const uint8_t* elf_data, gsize elf_len, uint32_t fn_start,
                                                 uint32_t fn_end, uint32_t max_scan_len) {
  if (!elf_data || elf_len < 52 || !fn_start || fn_end <= fn_start) {
    return 0;
  }

  uint16_t phnum = *(uint16_t*)(elf_data + 44);
  uint32_t phoff = *(uint32_t*)(elf_data + 28);
  for (int pi = 0; pi < phnum; pi++) {
    uint32_t ph_base = phoff + pi * 32;
    if (ph_base + 32 > elf_len) break;
    uint32_t p_type = *(uint32_t*)(elf_data + ph_base);
    uint32_t p_offset = *(uint32_t*)(elf_data + ph_base + 4);
    uint32_t p_vaddr = *(uint32_t*)(elf_data + ph_base + 8);
    uint32_t p_filesz = *(uint32_t*)(elf_data + ph_base + 16);
    if (p_type != 1) continue; /* PT_LOAD */
    if (fn_start < p_vaddr || fn_start >= p_vaddr + p_filesz) continue;

    uint32_t off_in_seg = fn_start - p_vaddr;
    uint32_t avail = p_filesz - off_in_seg;
    uint32_t fn_size = fn_end - fn_start;
    uint32_t scan_len = avail < fn_size ? avail : fn_size;
    if (max_scan_len > 0 && scan_len > max_scan_len) scan_len = max_scan_len;
    if (scan_len < 2) return 0;
    if ((uint64_t)p_offset + off_in_seg + scan_len > elf_len) return 0;

    uint8_t* code = (uint8_t*)elf_data + p_offset + off_in_seg;
    for (int j = (int)scan_len - 2; j >= 0; j--) {
      if (code[j] == 0x1D && code[j + 1] == 0xF0) {
        return fn_start + (uint32_t)j;
      }
    }
    return 0;
  }
  return 0;
}

static void mofei_cpu_resume_timer_cb(void* opaque) {
  Esp32s3SocState* ss = ESP32S3_SOC(opaque);
  CPUState* cpu0 = CPU(&ss->cpu[0]);
  if (cpu0->stopped) {
    cpu_resume(cpu0);
    fprintf(stderr, "[QEMU] CPU0 resumed via safety timer\n");
  }
}

static void esp32s3_machine_init(MachineState* machine) {
  /* Read Mofei sim config from environment variables */
  const char* env_ms = getenv("MOFEI_CPU1_DONE_MS");
  if (env_ms) {
    mofei_cpu1_sync.delay_ms = (int)strtoul(env_ms, NULL, 0);
  }

  fprintf(stderr, "[QEMU] esp32s3_machine_init\n");

  /* Resolve CPU1 synchronization symbols from the firmware ELF.
   * For flash boot, CPU1 is permanently halted so we must simulate
   * its handshake with CPU0 by writing to the BSS bools that the
   * ESP-IDF boot code spins on.  We resolve the addresses from the
   * ELF .symtab so they stay correct across firmware rebuilds. */
  {
    const char* elf_path = mofei_resolve_firmware_kernel_path();
    mofei_sim_select_board(elf_path);
    mofei_load_firmware_symbols();
    fprintf(stderr, "[QEMU-SIM] selected simulator board: %s\n", mofei_sim_board_name());
    gsize elf_len = 0;
    guint8* elf_data = NULL;
    if (g_file_get_contents(elf_path, (gchar**)&elf_data, &elf_len, NULL) && elf_data && elf_len >= 52) {
      /* The Xtensa toolchain may produce big-endian ELF containers.
       * Our resolver expects LE, so detect and byte-swap the header
       * fields if needed (same logic as the ELF boot path below). */
      bool elf_is_be = (elf_data[5] == ELFDATA2MSB);
      if (elf_is_be) {
        uint16_t* h16;
        uint32_t* h32;
        h16 = (uint16_t*)(elf_data + 16);
        *h16 = __builtin_bswap16(*h16);
        h16 = (uint16_t*)(elf_data + 18);
        *h16 = __builtin_bswap16(*h16);
        h32 = (uint32_t*)(elf_data + 20);
        *h32 = __builtin_bswap32(*h32);
        h32 = (uint32_t*)(elf_data + 24);
        *h32 = __builtin_bswap32(*h32);
        h32 = (uint32_t*)(elf_data + 28);
        *h32 = __builtin_bswap32(*h32);
        h32 = (uint32_t*)(elf_data + 32);
        *h32 = __builtin_bswap32(*h32);
        h32 = (uint32_t*)(elf_data + 36);
        *h32 = __builtin_bswap32(*h32);
        h16 = (uint16_t*)(elf_data + 40);
        *h16 = __builtin_bswap16(*h16);
        h16 = (uint16_t*)(elf_data + 42);
        *h16 = __builtin_bswap16(*h16);
        h16 = (uint16_t*)(elf_data + 44);
        *h16 = __builtin_bswap16(*h16);
        h16 = (uint16_t*)(elf_data + 46);
        *h16 = __builtin_bswap16(*h16);
        h16 = (uint16_t*)(elf_data + 48);
        *h16 = __builtin_bswap16(*h16);
        h16 = (uint16_t*)(elf_data + 50);
        *h16 = __builtin_bswap16(*h16);
        /* Byte-swap program headers */
        uint16_t phnum = *(uint16_t*)(elf_data + 44);
        uint32_t phoff = *(uint32_t*)(elf_data + 28);
        for (int i = 0; i < phnum && phoff + (uint32_t)(i + 1) * 32 <= elf_len; i++) {
          uint32_t* ph = (uint32_t*)(elf_data + phoff + i * 32);
          for (int w = 0; w < 8; w++) {
            ph[w] = __builtin_bswap32(ph[w]);
          }
        }
        /* Byte-swap section headers for our resolver */
        uint32_t e_shoff = *(uint32_t*)(elf_data + 32);
        uint16_t e_shentsize = *(uint16_t*)(elf_data + 46);
        uint16_t e_shnum_val = *(uint16_t*)(elf_data + 48);
        for (int si = 0; si < e_shnum_val; si++) {
          uint32_t sh_off = e_shoff + si * e_shentsize;
          if (sh_off + 40 > elf_len) break;
          uint32_t* sh = (uint32_t*)(elf_data + sh_off);
          for (int w = 0; w < 10; w++) {
            sh[w] = __builtin_bswap32(sh[w]);
          }
        }
      }

      /* s_cpu_up is a volatile bool[2]; we need s_cpu_up[1] */
      uint32_t base = mofei_resolve_elf_symbol(elf_data, elf_len, "s_cpu_up");
      if (base) {
        mofei_cpu1_sync.s_cpu_up_1 = base + 1; /* [1] is one byte after [0] */
        mofei_sim_addrs.s_cpu_up_1_addr = mofei_cpu1_sync.s_cpu_up_1;
        fprintf(stderr, "[QEMU-DBG] Resolved s_cpu_up @ 0x%x → [1] at 0x%x\n", base, mofei_cpu1_sync.s_cpu_up_1);
      }

      /* s_cpu_inited is a volatile bool[2]; we need s_cpu_inited[1] */
      base = mofei_resolve_elf_symbol(elf_data, elf_len, "s_cpu_inited");
      if (base) {
        mofei_cpu1_sync.s_cpu_inited_1 = base + 1;
        mofei_sim_addrs.s_cpu_inited_1_addr = mofei_cpu1_sync.s_cpu_inited_1;
        fprintf(stderr, "[QEMU-DBG] Resolved s_cpu_inited @ 0x%x → [1] at 0x%x\n", base,
                mofei_cpu1_sync.s_cpu_inited_1);
      }

      base = mofei_resolve_elf_symbol(elf_data, elf_len, "s_system_inited");
      if (base) {
        mofei_cpu1_sync.s_system_inited_1 = base + 1;
        mofei_sim_addrs.s_system_inited_1_addr = mofei_cpu1_sync.s_system_inited_1;
        fprintf(stderr, "[QEMU-DBG] Resolved s_system_inited @ 0x%x → [1] at 0x%x\n", base,
                mofei_cpu1_sync.s_system_inited_1);
      }

      base = mofei_resolve_elf_symbol(elf_data, elf_len, "s_system_full_inited");
      if (base) {
        mofei_cpu1_sync.s_system_full_inited = base;
        mofei_sim_addrs.s_system_full_inited_addr = mofei_cpu1_sync.s_system_full_inited;
        fprintf(stderr, "[QEMU-DBG] Resolved s_system_full_inited @ 0x%x\n", base);
      }

      /* Patch adc_hal_self_calibration to return 0 immediately.
       * The function reads eFuse calibration data which is not emulated
       * in QEMU, causing an infinite loop in read_cal_channel.
       * Patch: entry a1, 0; movi.n a2, 0; retw.n — replaces the function body
       * with an immediate return of 0 (no calibration data). */
      base = mofei_resolve_elf_symbol(elf_data, elf_len, "adc_hal_self_calibration");
      if (base && mofei_cpu1_sync.adc_patch_count < 16) {
        mofei_cpu1_sync.adc_patch_fns[mofei_cpu1_sync.adc_patch_count++] = base;
        fprintf(stderr, "[QEMU-DBG] Resolved adc_hal_self_calibration @ 0x%x\n", base);
      }

      /* Also resolve ADC calibration functions for patching.
       * These functions read eFuse calibration data which is not emulated
       * in QEMU, causing infinite loops.
       * Only patch functions that are called via standard callN instructions.
       * read_cal_channel is the key function that loops on eFuse reads;
       * patching it allows the other ADC functions to run without looping. */
      {
        const char* adc_syms[] = {
            "read_cal_channel",
            "adc_hal_self_calibration",
        };
        for (int i = 0; i < (int)(sizeof(adc_syms) / sizeof(adc_syms[0])); i++) {
          base = mofei_resolve_elf_symbol(elf_data, elf_len, adc_syms[i]);
          if (base && mofei_cpu1_sync.adc_patch_count < 16) {
            mofei_cpu1_sync.adc_patch_fns[mofei_cpu1_sync.adc_patch_count++] = base;
            fprintf(stderr, "[QEMU-DBG] Resolved %s @ 0x%x\n", adc_syms[i], base);
          }
        }
      }

      /* Resolve simulator intercept addresses from the firmware ELF.
       * These are used by translate.c and exc_helper.c to dynamically
       * intercept firmware functions without hardcoding PC addresses. */
      {
        MofeiSimAddrs* a = &mofei_sim_addrs;
        memset(a, 0, sizeof(*a));

        struct {
          const char* name;
          uint32_t* out;
        } syms[] = {
            /* Core firmware functions */
            {"_Z5setupv", &a->setup_addr},
            {"_Z20setupDisplayAndFontsv", &a->setupDisplayAndFonts_addr},
            {"_Z4loopv", &a->loop_addr},
            {"app_main", &a->app_main_addr},
            {"murphySimulatorMain", &a->murphySimulatorMain_addr},
            {"murphySimulatorEnterRunLoop", &a->murphySimulatorEnterRunLoop_addr},
            {"_ZN8murphyos11DeviceShell18startSimulatorLoopERKNS0_7ContextE",
             &a->murphyDeviceShellStartSimulatorLoop_addr},
            {"_ZN8murphyos11DeviceShell16runSimulatorLoopERKNS0_7ContextE",
             &a->murphyDeviceShellRunSimulatorLoopContext_addr},
            {"_ZN8murphyos11DeviceShell16runSimulatorLoopEv", &a->murphyDeviceShellRunSimulatorLoopMethod_addr},
            {"_ZN8murphyos11DeviceShell27runSimulatorLoopWithContextERKNS0_7ContextE",
             &a->murphyDeviceShellRunSimulatorLoopWithContext_addr},
            {"murphyDeviceShellRunSimulatorLoop", &a->murphyDeviceShellRunSimulatorLoop_addr},
            {"_Z8loopTaskPv", &a->loopTask_addr},
            {"start_cpu0", &a->start_cpu0_addr},
            {"xTaskCreateUniversal", &a->xTaskCreateUniversal_addr},
            {"esp_cache_msync", &a->esp_cache_msync_addr},
            {"setjmp", &a->setjmp_addr},
            {"gfxSimulatorPublishFrameBuffer", &a->gfxSimulatorPublishFrameBuffer_addr},
            {"mofeiSimulatorPublishFramebuffer", &a->mofeiSimulatorPublishFramebuffer_addr},
            {"murphySimulatorBootFirstFrame", &a->murphySimulatorBootFirstFrame_addr},
            {"murphySimulatorPublishFramebuffer", &a->murphySimulatorPublishFramebuffer_addr},
            {"murphySimulatorTraceActivity", &a->murphySimulatorTraceActivity_addr},
            {"murphySimulatorHooksEnabled", &a->murphySimulatorHooksEnabled_addr},
            /* ActivityManager */
            {"_ZN15ActivityManager8goToBootEv", &a->goToBoot_addr},
            {"_ZN15ActivityManager6goHomeEv", &a->goHome_addr},
            {"_ZN15ActivityManager5beginEv", &a->activityManager_begin_addr},
            {"activityManager", &a->activityManager_addr},
            {"_ZZL19runMurphyDeviceMainvE5shell", &a->murphyDeviceShell_addr},
            {"_ZZL19runMurphyDeviceMainvE6runner", &a->murphyDeviceRunner_addr},
            {"_ZZL19runMurphyDeviceMainvE2fb", &a->murphyDeviceFramebuffer_addr},
            {"_ZZL19runMurphyDeviceMainvE7display", &a->murphyDeviceDisplay_addr},
            {"_ZZL19runMurphyDeviceMainvE10sleepInput", &a->murphyDeviceSleepInput_addr},
            {"_ZZL19runMurphyDeviceMainvE5power", &a->murphyDevicePower_addr},
            {"_ZZL19runMurphyDeviceMainvE10frontlight", &a->murphyDeviceFrontlight_addr},
            {"_ZZL19runMurphyDeviceMainvE7storage", &a->murphyDeviceStorage_addr},
            {"_ZZL19runMurphyDeviceMainvE9castStore", &a->murphyDeviceCastStore_addr},
            {"_ZZL19runMurphyDeviceMainvE5input", &a->murphyDeviceInput_addr},
            {"_ZZL19runMurphyDeviceMainvE11mappedInput", &a->murphyDeviceMappedInput_addr},
            {"_ZZL19runMurphyDeviceMainvE10debugInput", &a->murphyDeviceDebugInput_addr},
            {"_ZZL19runMurphyDeviceMainvE11remoteInput", &a->murphyDeviceRemoteInput_addr},
            {"_ZZL19runMurphyDeviceMainvE6window", &a->murphyDeviceWindow_addr},
            {"_ZN12_GLOBAL__N_1L5nowMsEv", &a->murphyDeviceNowMs_addr},
            {"_ZTVN8murphyos8InputEspE", &a->murphyInputEspVtable_addr},
            {"_ZTVN8murphyos13PortraitInputE", &a->murphyPortraitInputVtable_addr},
            {"_ZTVN8murphyos14SleepHoldInputE", &a->murphySleepHoldInputVtable_addr},
            {"_ZTVN8murphyos11RemoteInputE", &a->murphyRemoteInputVtable_addr},
            {"_ZTVN8murphyos14SyntheticInputE", &a->murphySyntheticInputVtable_addr},
            {"murphySimulatorShellContext", &a->murphySimulatorShellContext_addr},
            {"murphySimulatorRunner", &a->murphySimulatorRunner_addr},
            {"murphySimulatorFramebufferObject", &a->murphySimulatorFramebufferObject_addr},
            {"murphySimulatorDisplay", &a->murphySimulatorDisplay_addr},
            {"murphySimulatorSleepInput", &a->murphySimulatorSleepInput_addr},
            {"murphySimulatorPower", &a->murphySimulatorPower_addr},
            {"murphySimulatorFrontlight", &a->murphySimulatorFrontlight_addr},
            {"murphySimulatorStorage", &a->murphySimulatorStorage_addr},
            {"murphySimulatorUiFont", &a->murphySimulatorUiFont_addr},
            {"murphySimulatorBodyFont", &a->murphySimulatorBodyFont_addr},
            {"murphySimulatorCastStore", &a->murphySimulatorCastStore_addr},
            {"murphySimulatorRunnerInput", &a->murphySimulatorRunnerInput_addr},
            {"murphySimulatorRunnerWindow", &a->murphySimulatorRunnerWindow_addr},
            {"murphySimulatorRunnerNowMs", &a->murphySimulatorRunnerNowMs_addr},
            /* Display */
            {"display", &a->display_addr},
            {"renderer", &a->renderer_addr},
            {"simulatorFrameBuffer", &a->gfxSimulatorFrameBuffer_addr},
            {"murphySimulatorFramebuffer", &a->murphySimulatorFramebuffer_addr},
            {"murphySimulatorFramebufferStorage", &a->murphySimulatorFramebufferStorage_addr},
            /* SPI / MofeiDisplay */
            {"_ZN8SPIClass16beginTransactionE11SPISettings", &a->spi_beginTransaction_addr},
            {"_ZN8SPIClass14endTransactionEv", &a->spi_endTransaction_addr},
            {"_ZN8SPIClass8transferEh", &a->spi_transfer_addr},
            {"_ZN8SPIClass10writeBytesEPKhm", &a->spi_writeBytes_addr},
            {"_ZNK12MofeiDisplay11sendCommandEh", &a->sendCommand_addr},
            {"_ZNK12MofeiDisplay8sendDataEh", &a->sendData_byte_addr},
            {"_ZNK12MofeiDisplay8sendDataEPKhm", &a->sendData_buf_addr},
            {"_ZN8murphyos9EpdBusEsp12writeCommandEh", &a->epdBusWriteCommand_addr},
            {"_ZN8murphyos9EpdBusEsp9writeDataEPKhj", &a->epdBusWriteData_addr},
            {"_ZN8murphyos9EpdBusEsp8waitBusyEv", &a->epdBusWaitBusy_addr},
            {"_ZN12MofeiDisplay25resetDisplayUpdateControlEv", &mofei_sim_reset_display_update_control_addr},
            {"_ZN12MofeiDisplay12writeLutFullEv", &mofei_display_writeLutFull_addr},
            {"_ZN12MofeiDisplay12writeLutFastEv", &mofei_display_writeLutFast_addr},
            {"_ZN12MofeiDisplay10writeLutDuEv", &mofei_display_writeLutDu_addr},
            /* Skip/intercept */
            {"addApbChangeCallback", &a->addApbChangeCallback_addr},
            {"removeApbChangeCallback", &a->removeApbChangeCallback_addr},
            {"getApbFrequency", &a->getApbFrequency_addr},
            {"gpio_config", &a->gpio_config_addr},
            {"gpio_set_level", &a->gpio_set_level_addr},
            {"gpio_get_level", &a->gpio_get_level_addr},
            {"__pinMode", &a->pinMode_addr},
            {"__digitalWrite", &a->digitalWrite_addr},
            {"__digitalRead", &a->digitalRead_addr},
            {"pvPortMalloc", &a->pvPortMalloc_addr},
            {"__wrap_malloc", &mofei_sim_wrap_malloc_addr},
            {"heap_caps_malloc", &a->heap_caps_malloc_addr},
            {"heap_caps_malloc_base", &a->heap_caps_malloc_base_addr},
            {"heap_caps_malloc_default", &a->heap_caps_malloc_default_addr},
            {"heap_caps_malloc_prefer", &a->heap_caps_malloc_prefer_addr},
            {"malloc", &a->malloc_addr},
            {"calloc", &a->calloc_addr},
            {"realloc", &a->realloc_addr},
            {"_malloc_r", &a->malloc_r_addr},
            {"_calloc_r", &a->calloc_r_addr},
            {"_realloc_r", &a->realloc_r_addr},
            {"_Znwj", &a->operator_new_addr},
            {"_Znaj", &a->operator_new_array_addr},
            /* ROM/RTOS */
            {"vTaskStartScheduler", &a->vTaskStartScheduler_addr},
            {"__assert_func", &a->__assert_func_addr},
            {"xTaskCreatePinnedToCore", &a->xTaskCreatePinnedToCore_addr},
            /* Partition/OTA */
            {"esp_partition_find", &a->esp_partition_find_addr},
            {"esp_partition_next", &a->esp_partition_next_addr},
            {"esp_partition_find_first", &a->esp_partition_find_first_addr},
            {"esp_partition_verify", &a->esp_partition_verify_addr},
            {"esp_timer_get_time", &a->esp_timer_get_time_addr},
            {"systimer_hal_get_counter_value", &a->systimer_hal_get_counter_value_addr},
            {"esp_ota_get_running_partition", &a->esp_ota_get_running_partition_addr},
            {"esp_ota_get_next_update_partition", &a->esp_ota_get_next_update_partition_addr},
            {"__atomic_s32c1i_exchange_1", &a->atomic_s32c1i_exchange_1_addr},
            {"__atomic_fetch_add_2", &a->atomic_fetch_add_2_addr},
            {"__atomic_fetch_add_4", &a->atomic_fetch_add_4_addr},
            {"__atomic_s32c1i_compare_exchange_1", &a->atomic_s32c1i_compare_exchange_1_addr},
            {"__atomic_s32c1i_compare_exchange_4", &a->atomic_s32c1i_compare_exchange_4_addr},
            {"__atomic_compare_exchange_4", &a->atomic_compare_exchange_4_addr},
            /* NVS */
            {"nvs_flash_init", &a->nvs_flash_init_addr},
            {"nvs_flash_init_partition", &a->nvs_flash_init_partition_addr},
            {"nvs_open", &a->nvs_open_addr},
            {"nvs_open_from_partition", &a->nvs_open_from_partition_addr},
            {"nvs_get_blob", &a->nvs_get_blob_addr},
            {"nvs_get_i8", &a->nvs_get_i8_addr},
            {"nvs_get_u8", &a->nvs_get_u8_addr},
            {"nvs_get_u16", &a->nvs_get_u16_addr},
            {"nvs_get_u32", &a->nvs_get_u32_addr},
            {"nvs_set_blob", &a->nvs_set_blob_addr},
            {"nvs_set_i8", &a->nvs_set_i8_addr},
            {"nvs_set_u8", &a->nvs_set_u8_addr},
            {"nvs_set_u16", &a->nvs_set_u16_addr},
            {"nvs_set_u32", &a->nvs_set_u32_addr},
            /* OTA */
            {"esp_ota_get_app_partition_count", &a->esp_ota_get_app_partition_count_addr},
            {"esp_ota_begin", &a->esp_ota_begin_addr},
            {"esp_ota_end", &a->esp_ota_end_addr},
            {"esp_ota_write", &a->esp_ota_write_addr},
            /* Heap */
            {"heap_caps_calloc", &a->heap_caps_calloc_addr},
            {"heap_caps_calloc_base", &a->heap_caps_calloc_base_addr},
            {"heap_caps_calloc_prefer", &a->heap_caps_calloc_prefer_addr},
            {"heap_caps_realloc", &a->heap_caps_realloc_addr},
            {"heap_caps_realloc_base", &a->heap_caps_realloc_base_addr},
            {"heap_caps_realloc_default", &a->heap_caps_realloc_default_addr},
            {"heap_caps_realloc_prefer", &a->heap_caps_realloc_prefer_addr},
            {"heap_caps_get_free_size", &a->heap_caps_get_free_size_addr},
            {"heap_caps_get_largest_free_block", &a->heap_caps_get_largest_free_block_addr},
            {"heap_caps_get_total_size", &a->heap_caps_get_total_size_addr},
            {"heap_caps_get_allocated_size", &a->heap_caps_get_allocated_size_addr},
            {"_ZN8EspClass12getPsramSizeEv", &a->esp_get_psram_size_addr},
            {"_ZN8EspClass12getFreePsramEv", &a->esp_get_free_psram_addr},
            {"_ZN8EspClass16getMaxAllocPsramEv", &a->esp_get_max_alloc_psram_addr},
            /* Queue/RTOS */
            {"xQueueGenericSend", &a->xQueueGenericSend_addr},
            {"xQueueSemaphoreTake", &a->xQueueSemaphoreTake_addr},
            {"xQueueGiveMutexRecursive", &a->xQueueGiveMutexRecursive_addr},
            {"xQueueReceive", &a->xQueueReceive_addr},
            {"xQueueTakeMutexRecursive", &a->xQueueTakeMutexRecursive_addr},
            {"xQueueGenericCreate", &a->xQueueGenericCreate_addr},
            {"xQueueCreateMutex", &a->xQueueCreateMutex_addr},
            {"xQueueCreateWithCaps", &a->xQueueCreateWithCaps_addr},
            {"xSemaphoreCreateGenericWithCaps", &a->xSemaphoreCreateGenericWithCaps_addr},
            {"vPortYield", &a->vPortYield_addr},
            {"i2c_master_transmit", &a->i2c_master_transmit_addr},
            {"i2c_master_receive", &a->i2c_master_receive_addr},
            {"i2c_master_transmit_receive", &a->i2c_master_transmit_receive_addr},
            {"i2c_master_multi_buffer_transmit", &a->i2c_master_multi_buffer_transmit_addr},
            {"esp_intr_alloc", &a->esp_intr_alloc_addr},
            {"esp_intr_alloc_intrstatus", &a->esp_intr_alloc_intrstatus_addr},
            {"esp_intr_enable", &a->esp_intr_enable_addr},
            {"esp_intr_disable", &a->esp_intr_disable_addr},
            {"esp_intr_free", &a->esp_intr_free_addr},
            {"spi_device_polling_transmit", &a->spi_device_polling_transmit_addr},
            {"spi_device_transmit", &a->spi_device_transmit_addr},
            {"sdmmc_host_wait_for_event", &a->sdmmc_host_wait_for_event_addr},
            /* Log */
            {"_Z9logPrintfPKcS0_S0_z", &a->logPrintf_addr},
            {"__wrap_log_printf", &a->wrap_log_printf_addr},
            {"log_printf", &a->log_printf_addr},
            {"log_printfv", &a->log_printfv_addr},
            {"esp_log", &a->esp_log_addr},
            {"esp_log_va", &a->esp_log_va_addr},
            {"esp_log_write", &a->esp_log_write_addr},
            {"esp_log_writev", &a->esp_log_writev_addr},
            {"esp_log_impl_lock", &a->esp_log_impl_lock_addr},
            {"esp_log_impl_unlock", &a->esp_log_impl_unlock_addr},
            {"__env_lock", &a->__env_lock_addr},
            {"__env_unlock", &a->__env_unlock_addr},
            /* Pthread TLS used by libstdc++ C++ exception globals */
            {"pthread_key_create", &a->pthread_key_create_addr},
            {"pthread_getspecific", &a->pthread_getspecific_addr},
            {"pthread_setspecific", &a->pthread_setspecific_addr},
            {"__esp_system_init_fn_mbedtls_psa_crypto_init_fn", &a->esp_system_init_mbedtls_psa_crypto_addr},
            /* Sleep functions to skip in simulator (GPIO not emulated) */
            {"_Z14enterDeepSleepv", &a->enterDeepSleep_addr},
            {"_Z19enterAutoLightSleepv", &a->enterAutoLightSleep_addr},
            {"_Z23mofeiSimulatorTouchReadPtS_PhS0_", &a->mofeiSimulatorTouchRead_addr},
            {"mofeiSimulatorButtonRead", &a->mofeiSimulatorButtonRead_addr},
            {"mofeiSimulatorTraceFileBrowserDirectory", &a->mofeiSimulatorTraceFileBrowserDirectory_addr},
            {"mofeiSimulatorTraceRpipe", &a->mofeiSimulatorTraceRpipe_addr},
            {"mofeiSimulatorTraceOpdsFetch", &a->mofeiSimulatorTraceOpdsFetch_addr},
            {"mofeiSimulatorTraceOpdsSearch", &a->mofeiSimulatorTraceOpdsSearch_addr},
            {"g_mofeiSimButtonBits", &a->g_mofeiSimButtonBits_addr},
            {"g_mofeiSimButtonPressedEvents", &a->g_mofeiSimButtonPressedEvents_addr},
            {"g_mofeiSimButtonReleasedEvents", &a->g_mofeiSimButtonReleasedEvents_addr},
            {"g_mofeiSimTouchEventPending", &a->g_mofeiSimTouchEventPending_addr},
            {"g_mofeiSimTouchEventType", &a->g_mofeiSimTouchEventType_addr},
            {"g_mofeiSimTouchEventX", &a->g_mofeiSimTouchEventX_addr},
            {"g_mofeiSimTouchEventY", &a->g_mofeiSimTouchEventY_addr},
            {"g_mofeiSimConsolePending", &a->g_mofeiSimConsolePending_addr},
            {"g_mofeiSimConsoleLength", &a->g_mofeiSimConsoleLength_addr},
            {"g_mofeiSimConsoleBuffer", &a->g_mofeiSimConsoleBuffer_addr},
            {"g_mofeiSimPandaDebugRequestPending", &a->g_mofeiSimPandaDebugRequestPending_addr},
            {"g_mofeiSimPandaDebugRequestLength", &a->g_mofeiSimPandaDebugRequestLength_addr},
            {"g_mofeiSimPandaDebugRequestBuffer", &a->g_mofeiSimPandaDebugRequestBuffer_addr},
            {"g_mofeiSimPandaDebugResponsePending", &a->g_mofeiSimPandaDebugResponsePending_addr},
            {"g_mofeiSimPandaDebugResponseLength", &a->g_mofeiSimPandaDebugResponseLength_addr},
            {"g_mofeiSimPandaDebugResponseBuffer", &a->g_mofeiSimPandaDebugResponseBuffer_addr},
            {"mofeiSimulatorPandaDebugMailboxCapacity", &a->mofeiSimulatorPandaDebugMailboxCapacity_addr},
            {"mofeiSimulatorPandaDebugReset", &a->mofeiSimulatorPandaDebugReset_addr},
            {"mofeiSimulatorPandaDebugPump", &a->mofeiSimulatorPandaDebugPump_addr},
            /* ADC calibration (skip at translate time) */
            {"read_cal_channel", &a->read_cal_channel_addr},
            {"adc_hal_set_controller", &a->adc_hal_set_controller_addr},
            {"adc_hal_self_calibration", &a->adc_hal_self_calibration_addr},
            {"adc_hal_calibration_init", &a->adc_hal_calibration_init_addr},
            {"adc_hal_set_calibration_param", &a->adc_hal_set_calibration_param_addr},
            {"esp_efuse_read_field_blob", &a->esp_efuse_read_field_blob_addr},
            {"esp_efuse_read_field_blob$part$0", &a->esp_efuse_read_field_blob_part_addr},
            /* Render/activity trace points for simulator diagnosis */
            {"_ZN17DashboardActivity6renderEO10RenderLock", &a->dashboardRender_addr},
            {"_ZN16SettingsActivity6renderEO10RenderLock", &a->settingsRender_addr},
            {"_ZN19ButtonRemapActivity6renderEO10RenderLock", &a->buttonRemapRender_addr},
            {"_ZN25DeviceDiagnosticsActivity6renderEO10RenderLock", &a->deviceDiagnosticsRender_addr},
            {"_ZN25StatusBarSettingsActivity6renderEO10RenderLock", &a->statusBarSettingsRender_addr},
            {"_ZN16CalendarActivity6renderEO10RenderLock", &a->calendarRender_addr},
            {"_ZN20WeatherClockActivity6renderEO10RenderLock", &a->weatherClockRender_addr},
            {"_ZN17ArcadeHubActivity6renderEO10RenderLock", &a->arcadeHubRender_addr},
            {"_ZN16Game2048Activity6renderEO10RenderLock", &a->game2048Render_addr},
            {"_ZN19RecentBooksActivity6renderEO10RenderLock", &a->recentBooksRender_addr},
            {"_ZN19FileBrowserActivity6renderEO10RenderLock", &a->fileBrowserRender_addr},
            {"_ZN15AppletsActivity6renderEO10RenderLock", &a->appletsRender_addr},
            {"_ZN14LuaAppActivity6renderEO10RenderLock", &a->luaAppRender_addr},
            {"_ZN18ReadingHubActivity6renderEO10RenderLock", &a->readingHubRender_addr},
            {"_ZN16StudyHubActivity6renderEO10RenderLock", &a->studyHubRender_addr},
            {"_ZN23StudyCardsTodayActivity6renderEO10RenderLock", &a->studyCardsTodayRender_addr},
            {"_ZN22OpdsServerListActivity6renderEO10RenderLock", &a->opdsServerListRender_addr},
            {"_ZN23OpdsBookBrowserActivity6renderEO10RenderLock", &a->opdsBookBrowserRender_addr},
            {"_ZN20OpdsSettingsActivity6renderEO10RenderLock", &a->opdsSettingsRender_addr},
            {"_ZN18DictionaryActivity6renderEO10RenderLock", &a->dictionaryRender_addr},
            {"_ZN21KeyboardEntryActivity6renderEO10RenderLock", &a->keyboardEntryRender_addr},
            {"_ZN34EpubReaderChapterSelectionActivity6renderEO10RenderLock", &a->epubChapterSelectRender_addr},
            {"_ZN34EpubReaderPercentSelectionActivity6renderEO10RenderLock", &a->epubPercentSelectionRender_addr},
            {"_ZN25EpubSearchResultsActivity6renderEO10RenderLock", &a->epubSearchResultsRender_addr},
            {"_ZN24TxtSearchResultsActivity6renderEO10RenderLock", &a->txtSearchResultsRender_addr},
            {"_ZN20TxtBookmarksActivity6renderEO10RenderLock", &a->txtBookmarksRender_addr},
            {"_ZN21EpubBookmarksActivity6renderEO10RenderLock", &a->epubBookmarksRender_addr},
            {"_ZN27EpubReaderFootnotesActivity6renderEO10RenderLock", &a->epubReaderFootnotesRender_addr},
            {"_ZN21TtfFontSelectActivity6renderEO10RenderLock", &a->ttfFontSelectRender_addr},
            {"_ZN22TimeZoneSelectActivity6renderEO10RenderLock", &a->timeZoneSelectRender_addr},
            {"_ZN31TraditionalChineseFontsActivity6renderEO10RenderLock", &a->traditionalChineseFontsRender_addr},
            {"_ZN22LanguageSelectActivity6renderEO10RenderLock", &a->languageSelectRender_addr},
            {"_ZN22SleepWallpaperActivity6renderEO10RenderLock", &a->sleepWallpaperRender_addr},
            {"_ZN33ReaderFrontlightSelectionActivity6renderEO10RenderLock", &a->readerFrontlightSelectionRender_addr},
            {"_ZN14ReaderActivity6renderEO10RenderLock", &a->readerRender_addr},
            {"_ZN18EpubReaderActivity6renderEO10RenderLock", &a->epubReaderRender_addr},
            {"_ZN17TxtReaderActivity6renderEO10RenderLock", &a->txtReaderRender_addr},
            {"_ZN17XtcReaderActivity6renderEO10RenderLock", &a->xtcReaderRender_addr},
            {"_ZTVN5panda3app9HomeSceneE", &a->murphyVtableHomeScene_addr},
            {"_ZTVN5panda3app13SettingsSceneE", &a->murphyVtableSettingsScene_addr},
            {"_ZTVN5panda3app18WifiSelectionSceneE", &a->murphyVtableWifiSelectionScene_addr},
            {"_ZTVN5panda3app16FileBrowserSceneE", &a->murphyVtableFileBrowserScene_addr},
            {"_ZTVN5panda3app12LibrarySceneE", &a->murphyVtableLibraryScene_addr},
            {"_ZTVN5panda3app16RecentBooksSceneE", &a->murphyVtableRecentBooksScene_addr},
            {"_ZTVN5panda3app14ArcadeHubSceneE", &a->murphyVtableArcadeHubScene_addr},
            {"_ZTVN5panda3app13Game2048SceneE", &a->murphyVtableGame2048Scene_addr},
            {"_ZTVN5panda3app12_GLOBAL__N_112WeatherSceneE", &a->murphyVtableWeatherScene_addr},
            {"_ZTVN5panda3app12_GLOBAL__N_113CalendarSceneE", &a->murphyVtableCalendarScene_addr},
            {"_ZTVN5panda3app5study13StudyHubSceneE", &a->murphyVtableStudyHubScene_addr},
            {"_ZTVN5panda3app5study12_GLOBAL__N_115CardsTodaySceneE", &a->murphyVtableCardsTodayScene_addr},
            {"_ZTVN5panda3app5study12_GLOBAL__N_114QueueListSceneE", &a->murphyVtableStudyQueueScene_addr},
            {"_ZTVN5panda3app5study12_GLOBAL__N_116ReviewQueueSceneE", &a->murphyVtableStudyReviewQueueScene_addr},
            {"_ZTVN5panda3app5study12_GLOBAL__N_117ImportStatusSceneE", &a->murphyVtableStudyImportStatusScene_addr},
            {"_ZTVN5panda3app5study12_GLOBAL__N_19QuizSceneE", &a->murphyVtableStudyQuizScene_addr},
            {"_ZTVN5panda3app5study12_GLOBAL__N_111ReportSceneE", &a->murphyVtableStudyReportScene_addr},
            {"_ZTVN5panda3app5study12_GLOBAL__N_113RecoverySceneE", &a->murphyVtableStudyRecoveryScene_addr},
            {"_ZTVN5panda2ui13KeyboardSceneE", &a->murphyVtableKeyboardScene_addr},
            {"_ZTVN5panda2ui17ConfirmationSceneE", &a->murphyVtableConfirmationScene_addr},
            {"_ZTVN5panda3app11ReaderSceneE", &a->murphyVtableReaderScene_addr},
            {"_ZTVN5panda3app10SleepSceneE", &a->murphyVtableSleepScene_addr},
            {"_ZTVN5panda3app13PandaHubSceneE", &a->murphyVtablePandaHubScene_addr},
            {"_ZTVN5panda3app16PandaLuaAppSceneE", &a->murphyVtablePandaLuaAppScene_addr},
            {"_ZTVN5panda3app16ButtonRemapSceneE", &a->murphyVtableButtonRemapScene_addr},
            {"_ZTVN5panda3app16DiagnosticsSceneE", &a->murphyVtableDiagnosticsScene_addr},
            {"_ZTVN5panda3app15FontPickerSceneE", &a->murphyVtableFontPickerScene_addr},
            {"_ZTVN5panda3app15EnumEditorSceneE", &a->murphyVtableEnumEditorScene_addr},
            {"_ZTVN5panda3app16ToggleGroupSceneE", &a->murphyVtableToggleGroupScene_addr},
            {"_ZTVN5panda3app18SearchResultsSceneE", &a->murphyVtableSearchResultsScene_addr},
            {"_ZTVN5panda3app17BookmarkListSceneE", &a->murphyVtableBookmarkListScene_addr},
            {"_ZTVN5panda3app16ChapterListSceneE", &a->murphyVtableChapterListScene_addr},
            {"_ZTVN5panda3app16PercentJumpSceneE", &a->murphyVtablePercentJumpScene_addr},
            {"_ZTVN5panda3app27ReaderDictionaryResultSceneE", &a->murphyVtableDictionaryScene_addr},
            {"_ZTVN5panda3app11SudokuSceneE", &a->murphyVtableSudokuScene_addr},
            {"_ZTVN5panda3app15VirtualPetSceneE", &a->murphyVtableVirtualPetScene_addr},
            {"_ZNK11GfxRenderer8fillRectEiiiib", &a->fillRect_addr},
            {"_ZNK11GfxRenderer13displayBufferEN10HalDisplay11RefreshModeE", &a->displayBuffer_addr},
            {"_ZN12_GLOBAL__N_137SimulatorActivityManagerRenderBackend27renderActivitySynchronouslyEP8Activity",
             &a->renderActivitySync_addr},
            {"_ZNK11GfxRenderer8drawLineEiiiib", &a->drawLine_addr},
            {"_ZN12_GLOBAL__N_116fillPhysicalRectEPhiiiib", &a->fillPhysicalRect_addr},
            /* EPUB open pipeline trace points for simulator E2E diagnosis */
            {"_ZN4Epub4loadEbb", &a->epubLoad_addr},
            {"_ZNK4Epub13setupCacheDirEv", &a->epubSetupCacheDir_addr},
            {"_ZN7ZipFile4openEv", &a->zipFileOpen_addr},
            {"_ZN17BookMetadataCache10beginWriteEv", &a->bmcBeginWrite_addr},
            {"_ZN17BookMetadataCache19beginContentOpfPassEv", &a->bmcBeginContentOpfPass_addr},
            {"_ZN4Epub15parseContentOpfERN17BookMetadataCache12BookMetadataEb", &a->epubParseContentOpf_addr},
            {"_ZNK4Epub18findContentOpfFileEPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
             &a->epubFindContentOpfFile_addr},
            {"_ZNK4Epub24readItemContentsToStreamERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEER5Printj",
             &a->epubReadItemStream_addr},
            {"_ZNK4Epub28readItemContentsToUtf8StreamERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEER5Printj",
             &a->epubReadItemUtf8Stream_addr},
            {"_ZN7ZipFile16readFileToStreamEPKcR5Printj", &a->zipReadFileToStream_addr},
            {"_ZN16ContentOpfParser5writeEPKhj", &a->contentOpfWrite_addr},
            {"_ZN16ContentOpfParser5flushEv", &a->contentOpfFlush_addr},
            {"_ZN17BookMetadataCache16createSpineEntryESt17basic_string_viewIcSt11char_traitsIcEE",
             &a->bmcCreateSpineEntry_addr},
            {"_ZN17BookMetadataCache17endContentOpfPassEv", &a->bmcEndContentOpfPass_addr},
            {"_ZN4epub11expat_psram10xmlReallocEPvj", &a->expatPsramRealloc_addr},
            {"_ZN17BookMetadataCache12beginTocPassEv", &a->bmcBeginTocPass_addr},
            {"_ZN17BookMetadataCache8endWriteEv", &a->bmcEndWrite_addr},
            {"_ZN17BookMetadataCache12buildBookBinER7ZipFileRKNS_12BookMetadataE", &a->bmcBuildBookBin_addr},
            {"_ZNK17BookMetadataCache21cleanupBuildArtifactsEv", &a->bmcCleanupBuildArtifacts_addr},
        };

        int resolved_count = 0;
        for (int i = 0; i < (int)(sizeof(syms) / sizeof(syms[0])); i++) {
          uint32_t val = mofei_resolve_elf_symbol(elf_data, elf_len, syms[i].name);
          if (val) {
            *syms[i].out = val;
            resolved_count++;
            fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", syms[i].name, val);
          } else {
            fprintf(stderr, "[QEMU-SIM] WARNING: symbol not found: %s\n", syms[i].name);
          }
        }
        if (mofei_sim_board_is_s37uc()) {
          struct {
            const char* name;
            uint32_t* out;
          } s37uc_display_syms[] = {
              {"_ZNK13Uc8253Display11sendCommandEh", &a->sendCommand_addr},
              {"_ZNK13Uc8253Display8sendDataEh", &a->sendData_byte_addr},
              {"_ZNK13Uc8253Display8sendDataEPKhm", &a->sendData_buf_addr},
              {"_ZN13Uc8253Display10writeLut5sEv", &mofei_display_writeLutFull_addr},
              {"_ZN13Uc8253Display10writeLutGcEv", &mofei_display_writeLutFast_addr},
              {"_ZN13Uc8253Display10writeLutDuEv", &mofei_display_writeLutDu_addr},
          };
          for (int i = 0; i < (int)(sizeof(s37uc_display_syms) / sizeof(s37uc_display_syms[0])); i++) {
            if (*s37uc_display_syms[i].out) {
              continue;
            }
            uint32_t val = mofei_resolve_elf_symbol(elf_data, elf_len, s37uc_display_syms[i].name);
            if (val) {
              *s37uc_display_syms[i].out = val;
              resolved_count++;
              fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", s37uc_display_syms[i].name, val);
            } else {
              fprintf(stderr, "[QEMU-SIM] WARNING: symbol not found: %s\n", s37uc_display_syms[i].name);
            }
          }
        }
        a->memset_addr = MOFEI_ESP32S3_ROM_MEMSET_ADDR;
        a->memcpy_addr = MOFEI_ESP32S3_ROM_MEMCPY_ADDR;
        a->strcmp_addr = mofei_resolve_elf_symbol(elf_data, elf_len, "strcmp");
        if (!a->strcmp_addr) {
          a->strcmp_addr = MOFEI_ESP32S3_ROM_STRCMP_ADDR;
        }
        a->strlen_addr = mofei_resolve_elf_symbol(elf_data, elf_len, "strlen");
        if (!a->strlen_addr) {
          a->strlen_addr = MOFEI_ESP32S3_ROM_STRLEN_ADDR;
        }
        a->strdup_addr = mofei_resolve_elf_symbol(elf_data, elf_len, "strdup");
        a->cxa_guard_acquire_addr = mofei_resolve_elf_symbol(elf_data, elf_len, "__cxa_guard_acquire");
        a->cxa_guard_release_addr = mofei_resolve_elf_symbol(elf_data, elf_len, "__cxa_guard_release");
        a->heap_start_addr = mofei_resolve_elf_symbol(elf_data, elf_len, "_heap_start");
        if (!a->heap_start_addr) {
          a->heap_start_addr = mofei_resolve_elf_symbol(elf_data, elf_len, "_heap_low_start");
        }
        if (!a->heap_start_addr) {
          a->heap_start_addr = MOFEI_SIM_INTERNAL_HEAP_FALLBACK_BASE;
          fprintf(stderr, "[QEMU-SIM] WARNING: symbol not found: _heap_start/_heap_low_start; using 0x%08x\n",
                  a->heap_start_addr);
        }
        fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "ROM memset", a->memset_addr);
        fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "ROM memcpy", a->memcpy_addr);
        fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "libc/ROM strcmp", a->strcmp_addr);
        fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "libc/ROM strlen", a->strlen_addr);
        fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "__cxa_guard_acquire", a->cxa_guard_acquire_addr);
        fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "__cxa_guard_release", a->cxa_guard_release_addr);
        fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "linker heap start", a->heap_start_addr);
        if (a->strdup_addr) {
          fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "libc strdup", a->strdup_addr);
        } else {
          fprintf(stderr, "[QEMU-SIM] WARNING: symbol not found: strdup\n");
        }

        if (a->gfxSimulatorPublishFrameBuffer_addr) {
          a->gfxSimulatorPublishFrameBuffer_return =
              (a->gfxSimulatorPublishFrameBuffer_addr + MOFEI_GFX_SIMULATOR_PUBLISH_RETURN_OFFSET) |
              MOFEI_XTENSA_CALL8_RETURN_MARKER;
          fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "gfxSimulatorPublishFrameBuffer return",
                  a->gfxSimulatorPublishFrameBuffer_return);
        }
        if (a->mofeiSimulatorPublishFramebuffer_addr) {
          a->mofeiSimulatorPublishFramebuffer_retw =
              a->mofeiSimulatorPublishFramebuffer_addr + MOFEI_SIMULATOR_PUBLISH_RET_OFFSET;
          fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "mofeiSimulatorPublishFramebuffer RETW",
                  a->mofeiSimulatorPublishFramebuffer_retw);
        }
        if (a->murphySimulatorPublishFramebuffer_addr) {
          a->murphySimulatorPublishFramebuffer_retw =
              a->murphySimulatorPublishFramebuffer_addr + MOFEI_SIMULATOR_PUBLISH_RET_OFFSET;
          fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "murphySimulatorPublishFramebuffer RETW",
                  a->murphySimulatorPublishFramebuffer_retw);
        }
        {
          const char* run_device_main_sym = "_ZL19runMurphyDeviceMainv";
          const uint32_t fn_start = mofei_resolve_elf_symbol(elf_data, elf_len, run_device_main_sym);
          const uint32_t fn_end = mofei_resolve_elf_function_end(elf_data, elf_len, run_device_main_sym);
          if (fn_start && fn_end > fn_start) {
            const uint32_t scan_len = fn_end - fn_start;
            const uint32_t retw_pc = mofei_find_last_retw_in_elf_span(elf_data, elf_len, fn_start, fn_end, scan_len);
            if (retw_pc) {
              a->murphyRunDeviceMain_retw = retw_pc;
              fprintf(stderr, "[QEMU-SIM] Resolved %-45s → 0x%08x\n", "runMurphyDeviceMain final RETW", retw_pc);
            }
          }
          if (!a->murphyRunDeviceMain_retw) {
            fprintf(stderr, "[QEMU-SIM] WARNING: could not resolve runMurphyDeviceMain final RETW\n");
          }
        }
        if (a->murphySimulatorHooksEnabled_addr) {
          const uint8_t enabled = 1;
          if (address_space_write(&address_space_memory, a->murphySimulatorHooksEnabled_addr, MEMTXATTRS_UNSPECIFIED,
                                  &enabled, sizeof(enabled)) == MEMTX_OK) {
            fprintf(stderr, "[QEMU-SIM] Murphy OS simulator hooks enabled at 0x%08x\n",
                    a->murphySimulatorHooksEnabled_addr);
          }
        }

        /* Scan setupDisplayAndFonts for its RETW instruction.
         * The function has exactly one RETW (.n) near its end.
         * RETW.n encoding: 0x1D 0xF0 (narrow, 2 bytes)
         * Use the ELF function span so the scan cannot cross into the next
         * function when setup() is linked before setupDisplayAndFonts(). */
        if (a->setupDisplayAndFonts_addr) {
          uint32_t fn_start = a->setupDisplayAndFonts_addr;
          uint32_t fn_end = mofei_resolve_elf_function_end(elf_data, elf_len, "_Z20setupDisplayAndFontsv");
          if (fn_end <= fn_start) fn_end = fn_start + 2048;
          fprintf(stderr, "[QEMU-SIM] setupDisplayAndFonts span 0x%08x..0x%08x\n", fn_start, fn_end);

          uint32_t retw_pc = mofei_find_last_retw_in_elf_span(elf_data, elf_len, fn_start, fn_end, 2048);

          if (retw_pc) {
            a->setupDisplayAndFonts_retw = retw_pc;
            fprintf(stderr, "[QEMU-SIM] Resolved setupDisplayAndFonts RETW @ 0x%08x\n", retw_pc);
          } else {
            fprintf(stderr, "[QEMU-SIM] WARNING: could not find RETW in setupDisplayAndFonts\n");
          }
        }

        /* Mark as resolved if all critical symbols were found */
        a->resolved = (a->setup_addr && a->setupDisplayAndFonts_addr && a->setupDisplayAndFonts_retw && a->loop_addr &&
                       a->activityManager_addr);

        /* Pre-compute allocator RETW patch addresses for translate.c's RETW handler.
         * All these functions get their bodies patched at offset +3 with RETW
         * (the 'entry' instruction at offset 0 is preserved, RETW overwrites
         * offset 3-5).  So the RETW PC = function_addr + 3.
         * translate.c checks PC == function_addr + 3 to detect these patched RETWs.
         * Each entry also records which argument register contains the size. */
        {
          struct {
            const char* name;
            uint32_t addr;
          } alloc_fns[] = {
              {"pvPortMalloc", a->pvPortMalloc_addr},
              {"heap_caps_malloc", a->heap_caps_malloc_addr},
              {"heap_caps_malloc_base", a->heap_caps_malloc_base_addr},
              {"heap_caps_malloc_default", a->heap_caps_malloc_default_addr},
              {"heap_caps_malloc_prefer", a->heap_caps_malloc_prefer_addr},
              {"malloc", a->malloc_addr},
              {"_malloc_r", a->malloc_r_addr},
              {"_Znwj", a->operator_new_addr},
              {"_Znaj", a->operator_new_array_addr},
              {"calloc", a->calloc_addr},
              {"_calloc_r", a->calloc_r_addr},
              {"realloc", a->realloc_addr},
              {"_realloc_r", a->realloc_r_addr},
              {"heap_caps_calloc", a->heap_caps_calloc_addr},
              {"heap_caps_calloc_base", a->heap_caps_calloc_base_addr},
              {"heap_caps_calloc_prefer", a->heap_caps_calloc_prefer_addr},
              {"heap_caps_realloc", a->heap_caps_realloc_addr},
              {"heap_caps_realloc_base", a->heap_caps_realloc_base_addr},
              {"heap_caps_realloc_default", a->heap_caps_realloc_default_addr},
              {"heap_caps_realloc_prefer", a->heap_caps_realloc_prefer_addr},
              {"heap_caps_free", 0},
              {"vPortFree", 0},
              {"free", 0},
              {"_free_r", 0},
              {"multi_heap_free", 0},
              {"multi_heap_free_impl", 0},
              {"tlsf_free", 0},
          };
          for (int i = 0; i < (int)(sizeof(alloc_fns) / sizeof(alloc_fns[0])); i++) {
            mofei_add_alloc_patch(a, alloc_fns[i].addr, mofei_alloc_patch_spec_for_name(alloc_fns[i].name),
                                  alloc_fns[i].name);
          }
        }

        /* Pre-compute callN target addresses that return NULL.
         * These are used at call-translation time (not RETW time). */
        {
          uint32_t null_fns[] = {
              a->esp_partition_find_addr,
              a->esp_partition_next_addr,
              a->esp_partition_find_first_addr,
              a->esp_partition_verify_addr,
              a->esp_ota_get_running_partition_addr,
              a->esp_ota_get_next_update_partition_addr,
          };
          for (int i = 0; i < (int)(sizeof(null_fns) / sizeof(null_fns[0])); i++) {
            if (null_fns[i] && a->call_null_count < MOFEI_CALL_NULL_MAX) {
              a->call_null_addrs[a->call_null_count++] = null_fns[i];
            }
          }
        }

        /* Pre-compute callN target addresses that return ESP_FAIL */
        {
          uint32_t fail_fns[] = {
              a->nvs_flash_init_addr,
              a->nvs_flash_init_partition_addr,
              a->nvs_open_addr,
              a->nvs_open_from_partition_addr,
              a->nvs_get_blob_addr,
              a->nvs_get_i8_addr,
              a->nvs_get_u8_addr,
              a->nvs_get_u16_addr,
              a->nvs_get_u32_addr,
              a->nvs_set_blob_addr,
              a->nvs_set_i8_addr,
              a->nvs_set_u8_addr,
              a->nvs_set_u16_addr,
              a->nvs_set_u32_addr,
              a->esp_ota_get_app_partition_count_addr,
              a->esp_ota_begin_addr,
              a->esp_ota_end_addr,
              a->esp_ota_write_addr,
          };
          for (int i = 0; i < (int)(sizeof(fail_fns) / sizeof(fail_fns[0])); i++) {
            if (fail_fns[i] && a->call_fail_count < MOFEI_CALL_FAIL_MAX) {
              a->call_fail_addrs[a->call_fail_count++] = fail_fns[i];
            }
          }
        }

        fprintf(
            stderr,
            "[QEMU-SIM] Symbol resolution: %d/%d symbols, resolved=%s, alloc_patches=%d, call_null=%d, call_fail=%d\n",
            resolved_count, (int)(sizeof(syms) / sizeof(syms[0])), a->resolved ? "YES" : "NO (partial)",
            a->alloc_patch_count, a->call_null_count, a->call_fail_count);
      }

      g_free(elf_data);
    } else {
      fprintf(stderr,
              "[QEMU-DBG] WARNING: could not read firmware ELF '%s', "
              "CPU1 sync symbols not resolved\n",
              elf_path);
    }
  }

  DriveInfo* dinfo = drive_get(IF_MTD, 0, 0);
  BlockBackend* blk = NULL;
  if (dinfo) {
    /* MTD was given! We need to initialize and emulate SPI flash */
    qemu_log("Adding SPI flash device\n");
    blk = blk_by_legacy_dinfo(dinfo);
  } else {
    qemu_log("Not initializing SPI Flash\n");
  }
  DriveInfo* sd_dinfo = drive_get(IF_SD, 0, 0);
  BlockBackend* sd_blk = sd_dinfo != NULL ? blk_by_legacy_dinfo(sd_dinfo) : NULL;

  MemoryRegion* sys_mem = get_system_memory();
  Esp32s3MachineState* ms = ESP32S3_MACHINE(machine);
  object_initialize_child(OBJECT(ms), "soc", &ms->esp32s3, TYPE_ESP32S3_SOC);
  Esp32s3SocState* ss = ESP32S3_SOC(&ms->esp32s3);
  if (mofei_sim_board_is_lilygo_t5s3_pro()) {
    ss->lilygo_gnss = lilygo_gnss_chardev_create();
    qdev_prop_set_chr(DEVICE(&ss->uart[2]), "chardev", ss->lilygo_gnss);
  }

  MemoryRegion* dram = g_new(MemoryRegion, 1);
  const struct MemmapEntry* memmap = esp32s3_memmap;

  memory_region_init_ram(dram, NULL, "esp32s3.dram", memmap[ESP32S3_MEMREGION_DRAM].size, &error_fatal);
  memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_DRAM].base, dram);

  /* Inject a tiny stub function into DRAM that writes a character directly
   * to the UART0 TX FIFO register (0x60000000), bypassing all ROM init
   * checks.  Then set the ROM's "alternate putc" function pointer so
   * esp_rom_printf calls our stub instead of the ROM's own multi-layer
   * checked version.
   *
   * Stub layout (placed at 0x3FCE0000, 13 bytes, no literal pool):
   *   +0  entry a1, 16              -- 36 21 00
   *   +3  movi  a8, 0x60            -- 82 a0 60   (96 decimal)
   *   +6  slli  a8, a8, 24          -- 80 88 01   (a8 = 0x60000000)
   *   +9  s32i.n a2, a8, 0          -- 29 08     (write char to FIFO)
   *   +B  retw.n                    -- 1d f0
   *
   * The ROM esp_rom_printf calls through *0x3FCEF750 when its primary
   * putc pointer (*0x3FCEF754) is NULL.  Setting *0x3FCEF750 to the
   * stub's entry point makes all ROM printf output flow to the QEMU
   * UART0 model, which is connected to stdio.
   */
  {
    static const uint8_t uart_putc_stub[] = {
        0x36, 0x21, 0x00, /* entry a1, 16 */
        0x1D, 0xF0,       /* retw.n */
    };
    const uint32_t stub_addr = 0x3FCE0000;
    address_space_write(&address_space_memory, stub_addr, MEMTXATTRS_UNSPECIFIED, uart_putc_stub,
                        sizeof(uart_putc_stub));

    /* Set the ROM alternate putc pointer to NULL so ROM printf
     * skips character output entirely. This avoids deep call chains
     * through the UART stub that trigger window overflow exceptions. */
    const uint32_t rom_alt_putc_addr = 0x3FCEF750;
    const uint32_t null_ptr = 0;
    address_space_write(&address_space_memory, rom_alt_putc_addr, MEMTXATTRS_UNSPECIFIED, (const uint8_t*)&null_ptr, 4);

    fprintf(stderr, "[QEMU-DBG] UART putc stub at 0x%x, *0x%x = NULL (no output)\n", stub_addr, rom_alt_putc_addr);
  }

  memory_region_init_io(&ss->iomem, OBJECT(&ss->cpu[0]), &esp32s3_io_ops, NULL, "esp32s3.iomem", 0xd1000);
  memory_region_add_subregion(sys_mem, ESP32S3_IO_START_ADDR, &ss->iomem);

  // qdev_prop_set_chr(DEVICE(ss), "serial0", serial_hd(0));
  // qdev_prop_set_chr(DEVICE(ss), "serial1", serial_hd(1));
  // qdev_prop_set_chr(DEVICE(ss), "serial2", serial_hd(2));

  qdev_realize(DEVICE(ss), NULL, &error_fatal);

  I2CBus* lilygo_i2c_bus = esp32s3_lilygo_i2c_bus(ss);
  if (lilygo_i2c_bus != NULL) {
    i2c_slave_create_simple(lilygo_i2c_bus, TYPE_LILYGO_I2C_PROBE, LILYGO_I2C_PROBE_ADDRESS);
    ss->lilygo_touch = DEVICE(i2c_slave_create_simple(lilygo_i2c_bus, TYPE_LILYGO_GT911, LILYGO_GT911_ADDRESS));
    i2c_slave_create_simple(lilygo_i2c_bus, TYPE_LILYGO_PCA9535, LILYGO_PCA9535_ADDRESS);
    i2c_slave_create_simple(lilygo_i2c_bus, TYPE_LILYGO_TPS65185, LILYGO_TPS65185_ADDRESS);
    lilygo_bq27220_attach(lilygo_i2c_bus);
    lilygo_bq25896_attach(lilygo_i2c_bus);
    lilygo_pcf8563_attach(lilygo_i2c_bus);
    lilygo_pca9535_set_gnss_power_sink(lilygo_gnss_chardev_set_board_power);
    fprintf(stderr, "[QEMU-SIM] lilygo I2C controller bus ready base=0x%08x\n", DR_REG_I2C_EXT_BASE);
  }

  object_initialize_child(OBJECT(ss), "extmem", &ss->cache, TYPE_ESP32S3_CACHE);
  object_initialize_child(OBJECT(ss), "spi1", &ss->spi1, TYPE_ESP32S3_SPI);
  object_initialize_child(OBJECT(ss), "efuse", &ss->efuse, TYPE_ESP32S3_EFUSE);
  object_initialize_child(OBJECT(ss), "jtag", &ss->jtag, TYPE_ESP32C3_JTAG);
  object_initialize_child(OBJECT(ss), "gpio", &ss->gpio, TYPE_ESP32S3_GPIO);
  object_initialize_child(OBJECT(ss), "rng", &ss->rng, TYPE_ESP32S3_RNG);

  object_initialize_child(OBJECT(ss), "clock", &ss->clock, TYPE_ESP32S3_CLOCK);

  object_initialize_child(OBJECT(ss), "gdma", &ss->gdma, TYPE_ESP32S3_GDMA);
  object_initialize_child(OBJECT(ss), "sha", &ss->sha, TYPE_ESP32S3_SHA);
  object_initialize_child(OBJECT(ss), "aes", &ss->aes, TYPE_ESP32S3_AES);
  object_initialize_child(OBJECT(ss), "rsa", &ss->rsa, TYPE_ESP32S3_RSA);
  object_initialize_child(OBJECT(ss), "hmac", &ss->hmac, TYPE_ESP32S3_HMAC);
  object_initialize_child(OBJECT(ss), "ds", &ss->ds, TYPE_ESP32S3_DS);
  object_initialize_child(OBJECT(ss), "pms", &ss->pms, TYPE_ESP32S3_PMS);

  object_initialize_child(OBJECT(ss), "xts_aes", &ss->xts_aes, TYPE_ESP32S3_XTS_AES);
  object_initialize_child(OBJECT(ss), "timg0", &ss->timg[0], TYPE_ESP32S3_TIMG);
  object_initialize_child(OBJECT(ss), "timg1", &ss->timg[1], TYPE_ESP32S3_TIMG);
  object_initialize_child(OBJECT(ss), "systimer", &ss->systimer, TYPE_ESP32S3_SYSTIMER);
  object_initialize_child(OBJECT(ss), "rgb", &ss->rgb, TYPE_ESP_RGB);

  /* Board-specific SPI/display and legacy bit-bang touch peripherals. */
  ss->mofei_spi2 = qdev_new("esp32s3-gpspi2");
  object_property_set_link(OBJECT(ss->mofei_spi2), "gdma", OBJECT(&ss->gdma), &error_abort);
  if (mofei_sim_board_is_lilygo_t5s3_pro()) {
    qdev_prop_set_bit(ss->mofei_spi2, "route-chip-selects", true);
    ss->mofei_epd = qdev_new("ssd1677-gdeq0426");
    ss->mofei_touch = NULL;
    ss->mofei_i2c_bridge = NULL;
  } else if (mofei_sim_board_is_m5papers3()) {
    ss->mofei_epd = qdev_new("ssd1677-gdeq0426");
    ss->mofei_touch = NULL;
    ss->mofei_i2c_bridge = NULL;
  } else if (mofei_sim_board_is_s37uc()) {
    ss->mofei_i2c_bridge = qdev_new(TYPE_GPIO_I2C);
    ss->mofei_epd = qdev_new("uc8253c-s37uc");
    ss->mofei_touch = qdev_new("chsc6440-s37uc");
    qdev_prop_set_uint8(ss->mofei_touch, "address", 0x40);
  } else {
    ss->mofei_i2c_bridge = qdev_new(TYPE_GPIO_I2C);
    ss->mofei_epd = qdev_new("ssd1677-gdeq0426");
    ss->mofei_touch = qdev_new("ft6336u-mofei");
    qdev_prop_set_uint8(ss->mofei_touch, "address", 0x2E);
  }

  Chardev* mofei_chr = qemu_chr_find("mofei");
  if (mofei_chr && ss->mofei_epd != NULL) {
    qdev_prop_set_chr(ss->mofei_epd, "chardev", mofei_chr);
    fprintf(stderr, "[QEMU-DBG] Mofei chardev 'mofei' connected to %s display\n", mofei_sim_board_name());
  }

  DeviceState* intmatrix_dev = DEVICE(&ss->intmatrix);
  {
    /* Store the current Machine CPU in the interrupt matrix */
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->intmatrix), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_INTERRUPT_BASE, mr, 0);
  }

  /* Initialize OpenCores Ethernet controller now sicne it requires the interrupt matrix */
  esp32s3_init_openeth(ss);

  /* USB Serial JTAG realization */
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->jtag), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->jtag), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_USB_SERIAL_JTAG_BASE, mr, 0);
  }

  /* SPI1 controller (SPI Flash) */
  {
    ss->spi1.xts_aes = &ss->xts_aes;
    sysbus_realize(SYS_BUS_DEVICE(&ss->spi1), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->spi1), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI1_BASE, mr, 0);
    if (blk) {
      esp32s3_init_spi_flash(ss, blk);
    }
    if (machine->ram_size > 0) {
      esp32s3_machine_init_psram(ss, (uint32_t)(machine->ram_size / MiB));
    }
  }

  /* Mofei/s37uc GPSPI2 (SPI2) controller + selected e-ink/touch
   * Realize the SPI controller and connect peripherals on the bus,
   * but skip all GPIO wiring (DC/CS/RST/BUSY/INT lines) since the
   * Espressif QEMU fork does not emulate the GPIO matrix.
   */
  {
    /* GPSPI2 MMIO at DR_REG_SPI2_BASE */
    sysbus_realize(SYS_BUS_DEVICE(ss->mofei_spi2), &error_fatal);
    memory_region_add_subregion(sys_mem, DR_REG_SPI2_BASE, sysbus_mmio_get_region(SYS_BUS_DEVICE(ss->mofei_spi2), 0));

    sysbus_connect_irq(SYS_BUS_DEVICE(ss->mofei_spi2), 0, qdev_get_gpio_in(intmatrix_dev, ETS_SPI2_INTR_SOURCE));

    if (mofei_sim_board_is_lilygo_t5s3_pro() || mofei_sim_board_is_m5papers3()) {
      SSIBus* frame_ipc_bus = ssi_create_bus(DEVICE(ss), "lilygo-frame-ipc");
      qdev_realize(ss->mofei_epd, BUS(frame_ipc_bus), &error_fatal);
      if (mofei_sim_board_is_lilygo_t5s3_pro()) {
        ss->lilygo_radio = esp32s3_machine_init_lilygo_spi(ss->mofei_spi2, sd_blk);
        lilygo_pca9535_set_radio_power_sink(lilygo_sx1262_set_board_power);
      }
    } else {
      /* Selected e-ink display on GPSPI2's SSI bus. */
      BusState* spi2_bus = qdev_get_child_bus(ss->mofei_spi2, "spi");
      qdev_realize(ss->mofei_epd, spi2_bus, &error_fatal);

      /* Selected touch controller on the legacy bit-bang I2C bus. */
      sysbus_realize_and_unref(SYS_BUS_DEVICE(ss->mofei_i2c_bridge), &error_fatal);
      I2CBus* i2c_bus = (I2CBus*)qdev_get_child_bus(ss->mofei_i2c_bridge, "i2c");
      qdev_realize_and_unref(ss->mofei_touch, BUS(i2c_bus), &error_fatal);
      i2c_slave_set_address(I2C_SLAVE(ss->mofei_touch), mofei_sim_board_is_s37uc() ? 0x40 : 0x2E);
    }

    fprintf(stderr, "[QEMU-DBG] %s peripherals realized\n", mofei_sim_board_name());
    fflush(stderr);
  }

  /* (Extmem) Cache realization */
  {
    if (blk) {
      ss->cache.flash_blk = blk;
    } else if (machine->kernel_filename || machine->firmware) {
      /* Fix: When booting from ELF without a flash drive, ss->cache.flash_blk is NULL,
       * which leaves ss->cache.flash_mr uninitialized. The cache controller later
       * asserts on memory_region_get_ram_ptr(&s->flash_mr) when the firmware clears the cache.
       * Workaround: Initialize a dummy RAM region so it doesn't assert. */
      memory_region_init_ram(&ss->cache.flash_mr, OBJECT(&ss->cache), "esp32s3.cache.dummy_flash", 8 * MiB,
                             &error_fatal);
    }
    if (ss->psram) {
      ss->cache.psram = ss->psram;
    }
    ss->cache.xts_aes = &ss->xts_aes;
    sysbus_realize(SYS_BUS_DEVICE(&ss->cache), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->cache), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_EXTMEM_BASE, mr, 0);

    memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_DCACHE].base, &ss->cache.dcache);
    memory_region_add_subregion(sys_mem, memmap[ESP32S3_MEMREGION_ICACHE].base, &ss->cache.icache);
  }

  /* eFuses realization */
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->efuse), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->efuse), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_EFUSE_BASE, mr, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(&ss->efuse), 0, qdev_get_gpio_in(intmatrix_dev, ETS_EFUSE_INTR_SOURCE));
  }

  /* System clock realization */
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->clock), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->clock), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SYSTEM_BASE, mr, 0);
    /* Connect the IRQ lines to the interrupt matrix */
    for (int i = 0; i < ESP32S3_SYSTEM_CPU_INTR_COUNT; i++) {
      sysbus_connect_irq(SYS_BUS_DEVICE(&ss->clock), i, qdev_get_gpio_in(intmatrix_dev, ETS_FROM_CPU_INTR0_SOURCE + i));
    }
  }
  /* Timer Groups realization */
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->timg[0]), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->timg[0]), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_TIMERGROUP0_BASE, mr, 0);
    /* Connect the T0 interrupt line to the interrupt matrix */
    qdev_connect_gpio_out_named(DEVICE(&ss->timg[0]), ESP32S3_T0_IRQ_INTERRUPT, 0,
                                qdev_get_gpio_in(intmatrix_dev, ETS_TG0_T0_LEVEL_INTR_SOURCE));
    /* Connect the T1 interrupt line to the interrupt matrix */
    qdev_connect_gpio_out_named(DEVICE(&ss->timg[0]), ESP32S3_T1_IRQ_INTERRUPT, 0,
                                qdev_get_gpio_in(intmatrix_dev, ETS_TG0_T1_LEVEL_INTR_SOURCE));
    /* Connect the Watchdog interrupt line to the interrupt matrix */
    qdev_connect_gpio_out_named(DEVICE(&ss->timg[0]), ESP32S3_WDT_IRQ_INTERRUPT, 0,
                                qdev_get_gpio_in(intmatrix_dev, ETS_TG0_WDT_LEVEL_INTR_SOURCE));
  }
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->timg[1]), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->timg[1]), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_TIMERGROUP1_BASE, mr, 0);
    /* Connect the T0 interrupt line to the interrupt matrix */
    qdev_connect_gpio_out_named(DEVICE(&ss->timg[1]), ESP32S3_T0_IRQ_INTERRUPT, 0,
                                qdev_get_gpio_in(intmatrix_dev, ETS_TG1_T0_LEVEL_INTR_SOURCE));
    /* Connect the T1 interrupt line to the interrupt matrix */
    qdev_connect_gpio_out_named(DEVICE(&ss->timg[1]), ESP32S3_T1_IRQ_INTERRUPT, 0,
                                qdev_get_gpio_in(intmatrix_dev, ETS_TG1_T1_LEVEL_INTR_SOURCE));
    /* Connect the Watchdog interrupt line to the interrupt matrix */
    qdev_connect_gpio_out_named(DEVICE(&ss->timg[1]), ESP32S3_WDT_IRQ_INTERRUPT, 0,
                                qdev_get_gpio_in(intmatrix_dev, ETS_TG1_WDT_LEVEL_INTR_SOURCE));
  }

  /* System timer */
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->systimer), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->systimer), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SYSTIMER_BASE, mr, 0);
    for (int i = 0; i < ESP_SYSTIMER_IRQ_COUNT; i++) {
      sysbus_connect_irq(SYS_BUS_DEVICE(&ss->systimer), i,
                         qdev_get_gpio_in(intmatrix_dev, ETS_SYSTIMER_TARGET0_EDGE_INTR_SOURCE + i));
    }
  }

  /* GPIO realization */
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->gpio), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->gpio), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_GPIO_BASE, mr, 0);
  }

  /* Board GPIO wiring — connect GPIO pins to peripheral control lines. */
  {
    DeviceState* gpio_dev = DEVICE(&ss->gpio);
    if (mofei_sim_board_is_lilygo_t5s3_pro()) {
      qdev_connect_gpio_out_named(gpio_dev, "gpio-out", LILYGO_SD_CS_GPIO,
                                  qdev_get_gpio_in_named(ss->mofei_spi2, "chip-select", 0));
      qdev_connect_gpio_out_named(gpio_dev, "gpio-out", LILYGO_RADIO_CS_GPIO,
                                  qdev_get_gpio_in_named(ss->mofei_spi2, "chip-select", 1));
      qdev_connect_gpio_out_named(gpio_dev, "gpio-out", LILYGO_TOUCH_RESET_GPIO,
                                  qdev_get_gpio_in_named(ss->lilygo_touch, "reset", 0));
      qdev_connect_gpio_out_named(ss->lilygo_touch, "int", 0, qdev_get_gpio_in(gpio_dev, LILYGO_TOUCH_INT_GPIO));
    } else if (mofei_sim_board_is_m5papers3()) {
      /* PaperS3 input is exported through the shared framebuffer/touch bridge. */
    } else {
      const int epd_dc_gpio = mofei_sim_board_is_s37uc() ? S37UC_EPD_DC_GPIO : MOFEI_EPD_DC_GPIO;
      const int epd_rst_gpio = mofei_sim_board_is_s37uc() ? S37UC_EPD_RST_GPIO : MOFEI_EPD_RST_GPIO;
      const int epd_busy_gpio = mofei_sim_board_is_s37uc() ? S37UC_EPD_BUSY_GPIO : MOFEI_EPD_BUSY_GPIO;
      const int touch_sda_gpio = mofei_sim_board_is_s37uc() ? S37UC_TOUCH_SDA_GPIO : MOFEI_TOUCH_SDA_GPIO;
      const int touch_scl_gpio = mofei_sim_board_is_s37uc() ? S37UC_TOUCH_SCL_GPIO : MOFEI_TOUCH_SCL_GPIO;
      const int touch_int_gpio = mofei_sim_board_is_s37uc() ? S37UC_TOUCH_INT_GPIO : MOFEI_TOUCH_INT_GPIO;

      qdev_connect_gpio_out_named(gpio_dev, "gpio-out", epd_dc_gpio, qdev_get_gpio_in_named(ss->mofei_epd, "dc", 0));
      qdev_connect_gpio_out_named(gpio_dev, "gpio-out", epd_rst_gpio,
                                  qdev_get_gpio_in_named(ss->mofei_epd, "reset", 0));
      qdev_connect_gpio_out_named(ss->mofei_epd, "busy", 0, qdev_get_gpio_in(gpio_dev, epd_busy_gpio));
      qdev_connect_gpio_out_named(gpio_dev, "gpio-out", touch_sda_gpio, qdev_get_gpio_in(ss->mofei_i2c_bridge, 0));
      qdev_connect_gpio_out_named(gpio_dev, "gpio-out", touch_scl_gpio, qdev_get_gpio_in(ss->mofei_i2c_bridge, 1));
      qdev_connect_gpio_out(ss->mofei_i2c_bridge, 0, qdev_get_gpio_in(gpio_dev, touch_sda_gpio));
      qdev_connect_gpio_out_named(ss->mofei_touch, "int", 0, qdev_get_gpio_in(gpio_dev, touch_int_gpio));

      if (mofei_sim_board_is_s37uc()) {
        qdev_connect_gpio_out_named(ss->mofei_touch, "button-out", 0,
                                    qdev_get_gpio_in(gpio_dev, S37UC_BTN_SIDEKEY_GPIO));
        qdev_connect_gpio_out_named(ss->mofei_touch, "button-out", 1,
                                    qdev_get_gpio_in(gpio_dev, S37UC_BTN_SIDEKEY1_GPIO));
      } else {
        qdev_connect_gpio_out_named(ss->mofei_touch, "button-out", 0,
                                    qdev_get_gpio_in(gpio_dev, MOFEI_BTN_KEYLOCK_GPIO));
        qdev_connect_gpio_out_named(ss->mofei_touch, "button-out", 1, qdev_get_gpio_in(gpio_dev, MOFEI_BTN_KEY1_GPIO));
        qdev_connect_gpio_out_named(ss->mofei_touch, "button-out", 2, qdev_get_gpio_in(gpio_dev, MOFEI_BTN_KEY2_GPIO));
      }
    }

    fprintf(stderr, "[QEMU-DBG] %s GPIO wiring complete\n", mofei_sim_board_name());
    fflush(stderr);
  }

  {
    qdev_realize(DEVICE(&ss->rng), &ss->periph_bus, &error_fatal);
    esp32s3_soc_add_periph_device(sys_mem, &ss->rng, ESP32S3_RNG_BASE);
  }

  /* GDMA Realization */
  {
    object_property_set_link(OBJECT(&ss->gdma), "soc_mr", OBJECT(dram), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&ss->gdma), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->gdma), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_GDMA_BASE, mr, 0);
    /* Connect the IRQs to the Interrupt Matrix */
    for (int i = 0; i < ESP32S3_GDMA_CHANNEL_COUNT; i++) {
      qdev_connect_gpio_out_named(DEVICE(&ss->gdma), ESP_GDMA_IRQ_IN_NAME, i,
                                  qdev_get_gpio_in(intmatrix_dev, ETS_DMA_IN_CH0_INTR_SOURCE + i));
      qdev_connect_gpio_out_named(DEVICE(&ss->gdma), ESP_GDMA_IRQ_OUT_NAME, i,
                                  qdev_get_gpio_in(intmatrix_dev, ETS_DMA_OUT_CH0_INTR_SOURCE + i));
    }
  }

  /* SHA realization */
  {
    ss->sha.parent.gdma = ESP_GDMA(&ss->gdma);
    sysbus_realize(SYS_BUS_DEVICE(&ss->sha), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->sha), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SHA_BASE, mr, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(&ss->sha), 0, qdev_get_gpio_in(intmatrix_dev, ETS_SHA_INTR_SOURCE));
  }

  /* AES realization */
  {
    ss->aes.parent.gdma = ESP_GDMA(&ss->gdma);
    sysbus_realize(SYS_BUS_DEVICE(&ss->aes), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->aes), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_AES_BASE, mr, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(&ss->aes), 0, qdev_get_gpio_in(intmatrix_dev, ETS_AES_INTR_SOURCE));
  }
  /* RSA realization */
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->rsa), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->rsa), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_RSA_BASE, mr, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(&ss->rsa), 0, qdev_get_gpio_in(intmatrix_dev, ETS_RSA_INTR_SOURCE));
  }
  /* PMS realization */
  {
    sysbus_realize(SYS_BUS_DEVICE(&ss->pms), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->pms), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SENSITIVE_BASE, mr, 0);
  }

  /* HMAC realization */
  {
    ss->hmac.parent.efuse = ESP_EFUSE(&ss->efuse);
    qdev_realize(DEVICE(&ss->hmac), &ss->periph_bus, &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->hmac), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_HMAC_BASE, mr, 0);
  }

  /* Digital Signature realization */
  {
    ss->ds.parent.hmac = ESP_HMAC(&ss->hmac);
    ss->ds.parent.aes = ESP_AES(&ss->aes);
    ss->ds.parent.rsa = ESP_RSA(&ss->rsa);
    ss->ds.parent.sha = ESP_SHA(&ss->sha);
    qdev_realize(DEVICE(&ss->ds), &ss->periph_bus, &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->ds), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_DIGITAL_SIGNATURE_BASE, mr, 0);
  }
  /* XTS-AES realization */
  {
    ss->xts_aes.efuse = ESP_EFUSE(&ss->efuse);
    ss->xts_aes.clock = &ss->clock;
    qdev_realize(DEVICE(&ss->xts_aes), &ss->periph_bus, &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->xts_aes), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_AES_XTS_BASE, mr, 0);
  }

  /* RGB display realization */
  {
    /* Give the internal RAM memory region to the display */
    ss->rgb.intram = dram;
    sysbus_realize(SYS_BUS_DEVICE(&ss->rgb), &error_fatal);
    MemoryRegion* mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&ss->rgb), 0);
    memory_region_add_subregion_overlap(sys_mem, DR_REG_FRAMEBUF_BASE, mr, 0);
    memory_region_add_subregion_overlap(sys_mem, esp32s3_memmap[ESP32S3_MEMREGION_FRAMEBUF].base, &ss->rgb.vram, 0);
  }

  esp32s3_soc_add_unimp_device(sys_mem, "esp32s3.rmt", DR_REG_RMT_BASE, 0x1000);
  esp32s3_soc_add_unimp_device(sys_mem, "esp32s3.iomux", DR_REG_IO_MUX_BASE, 0x2000);

  if (!mofei_sim_board_is_lilygo_t5s3_pro() || mofei_sim_board_is_m5papers3()) {
    esp32s3_machine_init_sd(ss, sd_blk);
  }

  /* For flash boot, set static_vectors=1 (ROM reset vector at 0x40000400)
   * before any reset runs.  On power-on, the ESP32-S3 boots from ROM.
   * This must be set before cpu_reset() or the QOM system reset, because
   * xtensa_cpu_resetfn reads static_vectors to determine the initial PC. */
  if (!machine->kernel_filename && !machine->firmware) {
    xtensa_select_static_vectors(&ss->cpu[0].env, true);
  }

  /* Need MMU initialized prior to ELF loading,
   * so that ELF gets loaded into virtual addresses
   */
  cpu_reset(CPU(&ss->cpu[0]));

  const char* load_elf_filename = NULL;
  if (machine->firmware) {
    load_elf_filename = machine->firmware;
  }
  if (machine->kernel_filename) {
    qemu_log("Warning: both -bios and -kernel arguments specified. Only loading the the -kernel file.\n");
    load_elf_filename = machine->kernel_filename;
  }

  if (load_elf_filename) {
    /* Create ICACHE/DCACHE RAM overlays BEFORE loading the ELF so that
     * load_elf writes patched firmware data into the overlay RAM rather
     * than into the default (empty) cache model regions.  Without these
     * overlays, the cache model's IOMMU translates ICACHE reads to flash
     * (which has no ELF data), causing fetch errors (cause=14). */
    MemoryRegion* icache_ram = g_new(MemoryRegion, 1);
    MemoryRegion* dcache_ram = g_new(MemoryRegion, 1);
    memory_region_init_ram(icache_ram, NULL, "esp32s3.icache.ram", ESP32S3_EXTMEM_REGION_SIZE, &error_fatal);
    memory_region_init_ram(dcache_ram, NULL, "esp32s3.dcache.ram", ESP32S3_EXTMEM_REGION_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(sys_mem, esp32s3_memmap[ESP32S3_MEMREGION_ICACHE].base, icache_ram, 1);
    memory_region_add_subregion_overlap(sys_mem, esp32s3_memmap[ESP32S3_MEMREGION_DCACHE].base, dcache_ram, 1);
    fprintf(stderr, "[QEMU-DBG] ICACHE/DCACHE RAM overlays created at 0x%lx (%lu KB each)\n",
            (unsigned long)esp32s3_memmap[ESP32S3_MEMREGION_ICACHE].base,
            (unsigned long)(ESP32S3_EXTMEM_REGION_SIZE / 1024));

    MemoryRegion* icache_low_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(icache_low_alias, OBJECT(ss), "esp32s3.icache-low", icache_ram, 0,
                             ESP32S3_EXTMEM_REGION_SIZE);
    memory_region_add_subregion_overlap(&ss->cpu_specific_mem[0], 0x02000000, icache_low_alias, 1);
    fprintf(stderr, "[QEMU-DBG] ICACHE low-address alias at 0x02000000 (%lu KB)\n",
            (unsigned long)(ESP32S3_EXTMEM_REGION_SIZE / 1024));

    MemoryRegion* icache_cpu_alias = g_new(MemoryRegion, 1);
    MemoryRegion* dcache_cpu_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(icache_cpu_alias, OBJECT(ss), "esp32s3.icache-cpu", icache_ram, 0,
                             ESP32S3_EXTMEM_REGION_SIZE);
    memory_region_init_alias(dcache_cpu_alias, OBJECT(ss), "esp32s3.dcache-cpu", dcache_ram, 0,
                             ESP32S3_EXTMEM_REGION_SIZE);
    memory_region_add_subregion_overlap(&ss->cpu_specific_mem[0], esp32s3_memmap[ESP32S3_MEMREGION_ICACHE].base,
                                        icache_cpu_alias, 2);
    memory_region_add_subregion_overlap(&ss->cpu_specific_mem[0], esp32s3_memmap[ESP32S3_MEMREGION_DCACHE].base,
                                        dcache_cpu_alias, 2);
    fprintf(stderr, "[QEMU-DBG] ICACHE/DCACHE CPU-view aliases installed for firmware fetch/data overlays\n");

    /* Load ROM bootloader to IROM — it provides functions at 0x40000000+
     * (e.g. esp_rom_get_reset_reason, memset) that the firmware calls. */
    char* rom_binary = mofei_resolve_rom_binary();
    if (rom_binary == NULL) {
      error_report("Error: ROM binary not found (set MOFEI_ROM_BINARY or ensure pc-bios/esp32s3_rev0_rom.bin exists)");
      exit(1);
    }
    gsize rom_len = 0;
    guint8* rom_data = NULL;
    if (!g_file_get_contents(rom_binary, (gchar**)&rom_data, &rom_len, NULL) || rom_len == 0) {
      error_report("Error: could not read ROM binary '%s'", rom_binary);
      g_free(rom_binary);
      exit(1);
    }
    fprintf(stderr, "[QEMU-DBG] ROM bootloader: %lu bytes from %s at 0x%lx\n", (unsigned long)rom_len, rom_binary,
            (unsigned long)esp32s3_memmap[ESP32S3_MEMREGION_IROM].base);
    g_free(rom_binary);

    uint64_t image_entry = 0;
    uint64_t image_lowaddr = 0;
    int image_size = 0;

    /* Detect file type: .bin (ESP image) or .elf */
    bool is_esp_image = false;
    size_t fnlen = strlen(load_elf_filename);
    if (fnlen >= 4 && g_ascii_strcasecmp(load_elf_filename + fnlen - 4, ".bin") == 0) {
      is_esp_image = true;
    }

    if (is_esp_image) {
      /* Parse ESP32/ESP32-S3 firmware image (.bin).
       * Format: 8-byte header + 16-byte extended header +
       *         N segments (each: 4B addr + 4B len + data) */
      gsize bin_len = 0;
      guint8* bin_data = NULL;
      if (g_file_get_contents(load_elf_filename, (gchar**)&bin_data, &bin_len, NULL) && bin_len >= 8) {
        if (bin_data[0] != 0xE9) {
          error_report("Not a valid ESP image: magic 0x%02x", bin_data[0]);
          exit(1);
        }
        uint8_t seg_count = bin_data[1];
        memcpy(&image_entry, bin_data + 4, 4);
        image_size = (int)bin_len;

        /* Extended header is 16 bytes for ESP32-S3 */
        size_t off = 8 + 16;
        fprintf(stderr, "[QEMU-DBG] Loading ESP image: %s (%lu bytes, %d segments, entry=0x%lx)\n", load_elf_filename,
                (unsigned long)bin_len, seg_count, (unsigned long)image_entry);

        for (int si = 0; si < seg_count && off + 8 <= bin_len; si++) {
          uint32_t load_addr, data_len;
          memcpy(&load_addr, bin_data + off, 4);
          memcpy(&data_len, bin_data + off + 4, 4);
          off += 8;
          if (off + data_len > bin_len) {
            error_report("ESP image segment %d overflows file", si);
            exit(1);
          }
          if (si == 0) {
            image_lowaddr = load_addr;
          }
          /* Patch out problematic calls in call_start_cpu0 that
           * access un-emulated hardware registers.
           * Replace call8 (3 bytes) with or a0,a0,a0 (NOP, 0x200000). */
          if (load_addr == 0x40374000) { /* IRAM segment */
            struct {
              uint32_t off;
              const char* name;
            } patches[] = {
                {0x1D16, "call_start_cpu1 call8 -> NOP"},
            };
            /* 0x200000 = 'or a0, a0, a0' (true 24-bit NOP, LE bytes).
             * Do NOT use {0x02,0x00,0x00} — that's 'l8ui a0,a0,0'!
             * Do NOT use {0x20,0x00,0x00} — that's a different opcode! */
            uint8_t nop3[] = {0x00, 0x00, 0x20};
            for (int pi = 0; pi < 1; pi++) {
              if (patches[pi].off + 3 <= data_len) {
                memcpy(bin_data + off + patches[pi].off, nop3, 3);
                fprintf(stderr, "[QEMU-DBG] Patched %s at seg off 0x%x\n", patches[pi].name, patches[pi].off);
              }
            }
          }
          gchar* seg_name = g_strdup_printf("esp32s3.bin_s%d", si);
          rom_add_blob_fixed_as(seg_name, bin_data + off, data_len, load_addr, CPU(&ss->cpu[0])->as);
          g_free(seg_name);
          fprintf(stderr, "[QEMU-DBG]   seg %d: 0x%x bytes at 0x%x\n", si, data_len, load_addr);
          off += data_len;
        }
        ss->kernel_entry = image_entry;
        g_free(bin_data);
      } else {
        error_report("Error: could not read '%s'", load_elf_filename);
        exit(1);
      }
    } else {
      fprintf(stderr, "[QEMU-DBG] Loading ELF: %s\n", load_elf_filename);
      gsize elf_len = 0;
      gchar* elf_data = NULL;
      if (!g_file_get_contents(load_elf_filename, &elf_data, &elf_len, NULL)) {
        error_report("Error: could not read '%s'", load_elf_filename);
        exit(1);
      }

      /* Patch do_multicore_settings (entry at 0x40375d98) to retw.
       * IRAM loads at 0x40374000, so offset = 0x1D98 in the IRAM ELF segment.
       * We patch the raw ELF file by finding the PT_LOAD segment that
       * covers 0x40375d98 and adjusting the file offset. */
      /* Memory-set patches: find symbols by name and write a
       * non-zero value to their address in DRAM after loading.
       * Used to bypass dual-core synchronization: CPU1 is
       * halted in QEMU so the "other core started" flag never
       * gets set. We write 1 to it so CPU0 proceeds. */
      struct {
        const char* name;
        uint32_t value;
        int found;
        uint32_t addr;
      } memset_patches[] = {
          {"s_other_cpu_startup_done", 1, 0, 0},
      };
      int n_memset = sizeof(memset_patches) / sizeof(memset_patches[0]);

      if (elf_len >= 52 && memcmp(elf_data, ELFMAG, 4) == 0) {
        /* The Xtensa bare-metal toolchain can produce big-endian ELF
         * containers (EI_DATA=ELFDATA2MSB), while ESP firmware builds
         * usually produce little-endian ELFs (EI_DATA=ELFDATA2LSB).  The custom
         * patching code reads ELF header fields by direct uint16/uint32
         * dereference, which only works for LE on a LE host.  For BE
         * ELFs, byte-swap the header fields and flip EI_DATA to LE so
         * the downstream load_elf() call (with big_endian=0) accepts it.
         * The Xtensa instruction encoding is always little-endian, so
         * segment data bytes need no conversion. */
        const bool elf_is_be = ((uint8_t)elf_data[5] == ELFDATA2MSB);
        if (elf_is_be) {
          /* Byte-swap ELF header fields from BE to LE.
           * e_ident (bytes 0..15) is single-byte — no swap needed.
           * The rest of the header (bytes 16..51) is a mix of
           * uint16 and uint32 fields, so we must swap each by
           * its actual size, not as bulk uint32 words. */
          uint16_t* h16;
          uint32_t* h32;
          h16 = (uint16_t*)(elf_data + 16);
          *h16 = __builtin_bswap16(*h16); /* e_type */
          h16 = (uint16_t*)(elf_data + 18);
          *h16 = __builtin_bswap16(*h16); /* e_machine */
          h32 = (uint32_t*)(elf_data + 20);
          *h32 = __builtin_bswap32(*h32); /* e_version */
          h32 = (uint32_t*)(elf_data + 24);
          *h32 = __builtin_bswap32(*h32); /* e_entry */
          h32 = (uint32_t*)(elf_data + 28);
          *h32 = __builtin_bswap32(*h32); /* e_phoff */
          h32 = (uint32_t*)(elf_data + 32);
          *h32 = __builtin_bswap32(*h32); /* e_shoff */
          h32 = (uint32_t*)(elf_data + 36);
          *h32 = __builtin_bswap32(*h32); /* e_flags */
          h16 = (uint16_t*)(elf_data + 40);
          *h16 = __builtin_bswap16(*h16); /* e_ehsize */
          h16 = (uint16_t*)(elf_data + 42);
          *h16 = __builtin_bswap16(*h16); /* e_phentsize */
          h16 = (uint16_t*)(elf_data + 44);
          *h16 = __builtin_bswap16(*h16); /* e_phnum */
          h16 = (uint16_t*)(elf_data + 46);
          *h16 = __builtin_bswap16(*h16); /* e_shentsize */
          h16 = (uint16_t*)(elf_data + 48);
          *h16 = __builtin_bswap16(*h16); /* e_shnum */
          h16 = (uint16_t*)(elf_data + 50);
          *h16 = __builtin_bswap16(*h16); /* e_shstrndx */
          /* Byte-swap all program header fields */
          uint16_t phnum = *(uint16_t*)(elf_data + 44);
          uint32_t phoff = *(uint32_t*)(elf_data + 28);
          for (int i = 0; i < phnum && phoff + (i + 1) * 32 <= elf_len; i++) {
            uint32_t* ph = (uint32_t*)(elf_data + phoff + i * 32);
            for (int w = 0; w < 8; w++) {
              ph[w] = __builtin_bswap32(ph[w]);
            }
          }
          /* Mark as LE so load_elf() (big_endian=0) accepts it */
          elf_data[5] = ELFDATA2LSB;
          fprintf(stderr, "[QEMU-DBG] BE ELF detected — header fields byte-swapped to LE\n");
        }

/* ---- Dynamic ELF symbol-based patching ----
 *
 * Instead of hardcoding instruction addresses (which shift on
 * every firmware rebuild), we parse the ELF .symtab at load
 * time and look up functions by name.  Each matched function
 * gets its second instruction (entry+3) overwritten with RETW
 * so it returns immediately after setting up its window frame.
 *
 * This makes the simulator robust across firmware rebuilds.
 * The patch list below only contains function names, not
 * addresses.
 */

/* --- Resolve function addresses from ELF symbol table --- */
#define MAX_RETW_PATCHES 512
        struct {
          const char* name;
          uint32_t addr;
          int found;
          int return_success;
        } retw_dyn[MAX_RETW_PATCHES];
        int n_retw = 0;

        /* Functions that must preserve a caller-provided non-zero return
         * candidate or receive a TCG-level success override.  These get
         * patched with 'nop.n; retw.n' instead of a wide RETW so the
         * translate.c RETW handler can distinguish them at offset +5.
         *
         * NOTE: xQueueGenericCreate and xQueueCreateMutex are NOT listed
         * here because translate.c's RETW handler allocates unique queue
         * handles via mofei_bump_queue_create(). They keep plain RETW so
         * the TCG helper fires at PC=addr+3. Same for malloc-family
         * functions which need mofei_bump_malloc(). */
        static const char* retw_success_names[] = {
            /* FreeRTOS task creation — returns pdPASS (1) */
            "xTaskCreatePinnedToCore",
            "xTaskCreateStaticPinnedToCore",
            "xTaskGetSchedulerState",
            "xTaskGenericNotify",
            "prvInitialiseNewTask$constprop$0",
            "prvAddNewTaskToReadyList",
            "prvAddCurrentTaskToDelayedList",
            "prvCheckForValidListAndQueue",
            /* FreeRTOS queue/semaphore operations — returns pdPASS/pdTRUE.
             * (Creation functions handled by translate.c TCG helpers.) */
            "xQueueGenericReset",
            "xQueueGenericSend",
            "xQueueSemaphoreTake",
            "xQueueTakeMutexRecursive",
            "xQueueGiveMutexRecursive",
            "xQueueReceive",
            "xQueuePeek",
            "xQueueGenericSendFromISR",
            "xQueueGiveFromISR",
            "xQueueReceiveFromISR",
            "xQueueGenericGetStaticBuffers",
            "xQueueGenericCreateStatic",
            "xQueueCreateCountingSemaphore",
            "xQueueCreateCountingSemaphoreStatic",
            "xQueueCreateMutexStatic",
            /* FreeRTOS event groups — returns non-NULL handle or bits */
            "xEventGroupCreate",
            "xEventGroupSetBits",
            "xEventGroupWaitBits",
            /* FreeRTOS idle/timer memory — void but harmless */
            "vApplicationGetIdleTaskMemory",
            "vApplicationGetTimerTaskMemory",
            /* Partition table — returns non-NULL iterator/handle */
            "esp_partition_find",
            "esp_partition_find_first",
            /* SPI bus init — returns spi_t* handle; loops polling
             * SPI peripheral registers in QEMU. */
            "spiStartBus",
        };
        int n_retw_success = sizeof(retw_success_names) / sizeof(retw_success_names[0]);

        /* Functions that must RETW (return immediately).
         * Grouped by subsystem.  Each name is looked up in the ELF
         * .symtab; if found, entry+3 is overwritten with RETW. */
        static const char* retw_names[] = {
            /* Boot / cache */
            "do_multicore_settings",
            "cache_hal_init",
            "Cache_Suspend_DCache",
            "Cache_Suspend_ICache",
            "Cache_Freeze_ICache_Enable",
            "Cache_Freeze_DCache_Enable",
            "cache_hal_disable",
            /* PSRAM (no PSRAM in QEMU) */
            "esp_psram_impl_enable",
            "esp_psram_chip_init",
            "s_psram_chip_init",
            /* SPI flash (not fully emulated) */
            "bootloader_flash_execute_command_common",
            "bootloader_flash_update_id",
            "flash_init_state",
            "esp_mmu_map_init",
            /* Partition table (flash not emulated) */
            "esp_partition_find",
            "esp_partition_next",
            "esp_partition_find_first",
            "esp_partition_verify",
            /* NVS (needs partitions + flash) */
            "nvs_flash_init",
            "nvs_flash_init_partition",
            "nvs_open",
            "nvs_open_from_partition",
            "nvs_commit",
            "nvs_close",
            "nvs_get_blob",
            "nvs_get_i8",
            "nvs_get_u8",
            "nvs_get_u16",
            "nvs_get_u32",
            "nvs_set_blob",
            "nvs_set_i8",
            "nvs_set_u8",
            "nvs_set_u16",
            "nvs_set_u32",
            "nvs_erase_key",
            /* OTA (needs flash partitions) */
            "esp_ota_get_running_partition",
            "esp_ota_get_app_partition_count",
            "esp_ota_get_next_update_partition",
            "esp_ota_begin",
            "esp_ota_end",
            "esp_ota_write",
            "esp_ota_abort",
            /* GPIO (peripheral register access) — keep stubbed
             * because gpio_get_level loops on BUSY pin polling,
             * and the GPIO matrix is not fully emulated. Display
             * DC/CS/RST control is handled by the display-bypass
             * mechanism instead. */
            "esp_gpio_reserve",
            "gpio_pullup_en",
            "gpio_pullup_dis",
            "gpio_pulldown_en",
            "gpio_pulldown_dis",
            "gpio_set_intr_type",
            "gpio_intr_enable",
            "gpio_intr_disable",
            "gpio_input_enable",
            "gpio_input_disable",
            "gpio_output_enable",
            "gpio_output_disable",
            "gpio_od_enable",
            "gpio_od_disable",
            "gpio_set_level",
            "gpio_get_level",
            "gpio_set_direction",
            "gpio_config_as_analog",
            "gpio_reset_pin",
            "gpio_set_drive_capability",
            "gpio_iomux_input",
            "gpio_iomux_output",
            "gpio_sleep_sel_dis",
            "gpio_func_sel",
            /* ADC oneshot (flash-resident, skip hardware init) */
            "__analogInit",
            "analogReadMilliVolts",
            "__analogReadMilliVolts",
            "adc_oneshot_new_unit",
            "adc_oneshot_del_unit",
            "adc_oneshot_read",
            "adc_oneshot_config_channel",
            "adc_oneshot_get_calibrated_result",
            "adc_oneshot_channel_to_io",
            "adc_oneshot_io_to_channel",
            "adc_oneshot_hal_setup",
            "adc_oneshot_hal_init",
            "adc_oneshot_hal_convert",
            "adc_oneshot_hal_channel_config",
            /* Serial/Print (virtual dispatch crashes in QEMU).
             * Keep Print::write overloads native: HalFile inherits the base
             * bulk write implementation, which fans buffers out through the
             * concrete byte writer used by EPUB section streaming. */
            "_ZN5Print7printlnEPKc",
            "_ZN5Print5printEPKc",
            "_ZN5Print7printlnEv",
            "_ZN5Print6printfEPKcz",
            "_ZN5Print7vprintfEPKc13__va_list_tag",
            "_ZN5Print11printNumberEmh",
            "_ZN5Print5printEli",
            "_ZN5Print5printEii",
            "_ZN5Print5printEmi",
            "_ZN5Print5printEhi",
            "_ZN5Print7printlnERK9Printable",
            "_ZN5Print5printERK6String",
            "_ZN5Print5printEc",
            "_ZN5Print5printERK9Printable",
            "_ZN5Print17availableForWriteEv",
            "_ZN5Print5flushEv",
            "_ZN5HWCDC5beginEm",
            "_ZNK5HWCDCcvbEv",
            "_ZN14HardwareSerial5beginEm",
            /* SPI bus init — spiStartBus loops calling xQueueSemaphoreTake
             * and polling SPI peripheral status registers that never
             * change in QEMU.  RETW-patched to skip hardware init.
             * (Also in retw_success_names to return non-NULL handle.) */
            "spiStartBus",
            /* SPI: SPIClass transaction functions are intercepted at
             * the TCG level (translate.c) and never execute natively.
             * Keeping them commented out here so the intercepts work. */
            /* "_ZN8SPIClass5beginEaaaa", */
            /* "_ZN8SPIClass16beginTransactionE11SPISettings", */
            /* "_ZN8SPIClass14endTransactionEv", */
            /* "_ZN8SPIClass8transferEh", */
            /* "_ZN8SPIClass10writeBytesEPKhm", */
            /* "_ZN8SPIClass3endEv", */
            /* Touch driver (I2C bitbang loops in QEMU) */
            "_ZN16MofeiTouchDriver12detectOnPinsEii",
            "_ZN16MofeiTouchDriver12readRegisterEhPhh",
            /* MPU/PMS (locks out memory) */
            "esp_cpu_configure_region_protection",
            /* Atomic CAS (peripheral register) */
            "esp_cpu_compare_and_set",
            /* FreeRTOS critical sections */
            "xPortEnterCriticalTimeout",
            "vPortExitCritical",
            "xPortInIsrContext",
            "xTaskGetCurrentTaskHandle",
            /* Clock / RTC */
            "rtc_clk_cpu_freq_to_xtal",
            "rtc_clk_cpu_freq_to_pll_mhz",
            "rtc_clk_cpu_freq_set_config",
            "rtc_clk_cpu_freq_set_config_fast",
            "rtc_clk_cpu_freq_set_xtal",
            "rtc_clk_cpu_freq_mhz_to_config",
            "recalib_bbpll",
            "esp_clk_init",
            /* I2C register access (ROM) */
            "regi2c_ctrl_read_reg_mask",
            "regi2c_ctrl_write_reg",
            "regi2c_ctrl_write_reg_mask",
            /* MSPI timing */
            "mspi_timing_change_speed_mode_cache_safe",
            "mspi_timing_enter_low_speed_mode",
            "mspi_timing_enter_high_speed_mode",
            /* Heap / memory */
            "heap_caps_init",
            "__esp_system_init_fn_init_heap",
            "soc_get_available_memory_regions",
            /* libc (calls malloc -> abort when heap empty) */
            "__esp_system_init_fn_init_libc",
            /* Interrupt allocation */
            "esp_intr_alloc_intrstatus",
            "esp_intr_alloc",
            /* Time */
            "esp_rtc_get_time_us",
            "esp_timer_get_time",
            "esp_timer_impl_get_time",
            "systimer_hal_get_counter_value",
            "esp_system_get_time",
            /* Brownout */
            "esp_brownout_init",
            /* Peripheral clocks */
            "esp_perip_clk_init",
            /* Log timestamp (divide by zero) */
            "esp_log_early_timestamp",
            /* Error handler */
            "_esp_error_check_failed",
            /* CPU1 (powered off in QEMU) */
            "esp_cpu_unstall",
            "ets_set_appcpu_boot_addr",
            "call_start_cpu1",
            /* RTC init (polls SPI/MMU ROM functions) */
            "rtc_init",
            "mspi_init",
            "sys_rtc_init",
            "esp_rtc_init",
            /* Startup (calls ROM spin loops) */
            "system_early_init",
            "startup_resume_other_cores",
            /* Abort/panic (avoid ROM spin loops) */
            "abort",
            "esp_system_abort",
            /* Init framework functions (all return ESP_OK) */
            "esp_system_init_fn",
            /* ESP-IDF system init functions */
            "__esp_system_init_fn_init_efuse_check",
            "__esp_system_init_fn_init_show_cpu_freq",
            "__esp_system_init_fn_init_show_app_info",
            "__esp_system_init_fn_init_efuse_show_app_info",
            "__esp_system_init_fn_init_psram_new",
            "__esp_system_init_fn_add_psram_to_heap",
            "__esp_system_init_fn_init_libc",
            "__esp_system_init_fn_init_newlib_time",
            "__esp_system_init_fn_esp_timer_init_os",
            "__esp_system_init_fn_esp_timer_init_nonos",
            "__esp_system_init_fn_esp_security_init",
            "__esp_system_init_fn_init_brownout",
            "__esp_system_init_fn_init_apb_dma",
            "__esp_system_init_fn_init_coexist",
            "__esp_system_init_fn_init_coredump",
            "__esp_system_init_fn_init_flash",
            "__esp_system_init_fn_init_efuse",
            "__esp_system_init_fn_mbedtls_psa_crypto_init_fn",
            "__esp_system_init_fn_init_disable_rtc_wdt",
            /* UART VFS、console 与 stdio 初始化必须真实执行，确保 owner E2E 经 UART0 到达 production stdin。 */
            "__esp_system_init_fn_init_vfs_nullfs",
            "__esp_system_init_fn_init_vfs_usj_sec",
            "__esp_system_init_fn_usb_serial_jtag_conn_status_init",
            /* Newlib locks are backed by FreeRTOS mutexes on ESP-IDF.
             * The simulator does not run a real scheduler, so lock paths
             * must not execute queue/mutex code before the first frame. */
            "_lock_init",
            "_lock_init_recursive",
            "_lock_close",
            "_lock_close_recursive",
            "_lock_acquire",
            "_lock_acquire_recursive",
            "_lock_release",
            "_lock_release_recursive",
            "__retarget_lock_init",
            "__retarget_lock_init_recursive",
            "__retarget_lock_close",
            "__retarget_lock_close_recursive",
            "__retarget_lock_acquire",
            "__retarget_lock_acquire_recursive",
            "__retarget_lock_release",
            "__retarget_lock_release_recursive",
            /* FreeRTOS: RETW-patch queue/task functions so they
             * return immediately without doing real work.  The
             * scheduler is bypassed via vTaskStartScheduler below. */
            "xQueueGenericCreate",
            "xQueueGenericReset",
            "xQueueCreateMutex",
            /* ESP-IDF WithCaps wrapper 會先配置 queue backing storage，再呼叫
             * static FreeRTOS API。模擬器改由 opaque queue handle 持有生命週期，
             * 因此在 wrapper 邊界攔截，並讓成對 delete 保持不配置記憶體。 */
            "xQueueCreateWithCaps",
            "xSemaphoreCreateGenericWithCaps",
            "vQueueDeleteWithCaps",
            "vSemaphoreDeleteWithCaps",
            "xQueueGenericSend",
            "xQueueSemaphoreTake",
            "xQueueTakeMutexRecursive",
            "xQueueGiveMutexRecursive",
            "xQueueReceive",
            "xQueuePeek",
            "xQueueGenericSendFromISR",
            "xQueueGiveFromISR",
            "xQueueReceiveFromISR",
            "uxQueueMessagesWaiting",
            "uxQueueSpacesAvailable",
            "vQueueDelete",
            "xQueueGenericGetStaticBuffers",
            "xQueueGenericCreateStatic",
            "xQueueCreateCountingSemaphore",
            "xQueueCreateCountingSemaphoreStatic",
            "xQueueCreateMutexStatic",
            "pvPortMalloc",
            "vPortFree",
            "heap_caps_malloc",
            "heap_caps_malloc_base",
            "heap_caps_malloc_default",
            "heap_caps_malloc_prefer",
            "heap_caps_aligned_alloc",
            "heap_caps_aligned_alloc_base",
            "heap_caps_free",
            "heap_caps_calloc",
            "heap_caps_calloc_base",
            "heap_caps_calloc_prefer",
            "heap_caps_realloc",
            "heap_caps_realloc_base",
            "heap_caps_realloc_default",
            "heap_caps_realloc_prefer",
            "multi_heap_malloc",
            "multi_heap_malloc_impl",
            "multi_heap_free",
            "multi_heap_free_impl",
            "tlsf_malloc",
            "tlsf_free",
            "tlsf_memalign_offs",
            "tlsf_realloc",
            "vTaskStartScheduler",
            "xPortStartScheduler",
            "xTaskCreatePinnedToCore",
            "xTaskCreateStaticPinnedToCore",
            "xTaskGetSchedulerState",
            "vTaskDelete",
            "vTaskDelay",
            "vTaskSuspendAll",
            "xTaskResumeAll",
            "vTaskSwitchContext",
            "vTaskPrioritySet",
            "vTaskPlaceOnEventList",
            "vTaskPlaceOnEventListRestricted",
            "xTaskRemoveFromEventList",
            "xTaskPriorityInherit",
            "xTaskPriorityDisinherit",
            /* Atomics (s32c1i throws EXCCAUSE=3 on some RAM regions in QEMU) */
            "__atomic_fetch_add_4",
            "__atomic_s32c1i_fetch_add_4",
            "xTaskCheckForTimeOut",
            "xTaskIncrementTick",
            "xTaskGenericNotify",
            "xTaskGenericNotifyWait",
            "ulTaskGenericNotifyValueClear",
            "ulTaskGenericNotifyTake",
            "prvInitialiseNewTask$constprop$0",
            "prvAddNewTaskToReadyList",
            "prvAddCurrentTaskToDelayedList",
            "prvCheckForValidListAndQueue",
            "vApplicationGetIdleTaskMemory",
            "vApplicationGetTimerTaskMemory",
            "xEventGroupCreate",
            "xEventGroupSetBits",
            "xEventGroupWaitBits",
            "esp_log",
            "esp_log_va",
            "esp_log_write",
            "esp_log_writev",
            "esp_log_impl_lock",
            "esp_log_impl_unlock",
            "esp_log_level_set",
            "esp_log_set_default_level",
            "esp_log_is_tag_loggable",
            "esp_log_linked_list_set_level",
            "esp_log_linked_list_get_level",
            "esp_log_linked_list_clean",
            "__assert_func",
            "malloc",
            "calloc",
            "free",
            "realloc",
            "_malloc_r",
            "_calloc_r",
            "_free_r",
            "_realloc_r",
            "_ZdlPv",
            "_ZdlPvj",
            "_ZdaPv",
            "_ZdaPvj",
        };
        int n_retw_names = sizeof(retw_names) / sizeof(retw_names[0]);
        fprintf(stderr, "[QEMU-DBG] RETW: %d names to resolve\n", n_retw_names);

        /* Short firmware wrappers/accessors that still execute real C++
         * code need the pre-RETW underflow/illegal checks bypassed.
         * Returning through the safe stub helper can read stale env->regs[0]
         * instead of the live translated A0, so register the exact RETW PC
         * and keep the normal translated-register RETW path. */
        static const char* retw_normal_precheck_names[] = {
            "_ZN9HalSystem17isRebootFromPanicEv",
            "_ZN15RuntimePlatform7backendEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend17beginPowerManagerEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend19bootTimestampMillisEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend16loadI18nSettingsEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend10loadStoresEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend8beginBleEb",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend22beginBackgroundWorkersEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend24beginFrontlightAndBuzzerEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend14syncFrontlightERK10MPSettings",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend18handleUsbPowerWakeER7HalGPIO",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend19bindBleMainLoopTaskEP19tskTaskControlBlock",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend23shouldStartInputWatcherEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend21hasConnectedAppClientEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend17shouldRunAutoLockEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend18shouldRunAutoSleepEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend29shouldHandleDashboardBootLockEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend30shouldTrackActivityPerformanceEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend29shouldRunSleepButtonDeepSleepEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend26shouldUseBlockingLoopDelayEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend30shouldUseReaderStorageTtfFontsEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend28shouldSkipTxtAdjacentPreloadEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend37shouldRunFileBrowserCoverWorkerInlineEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend47shouldSkipFileBrowserCoverWorkerSramPreflightEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend32shouldShowVirtualPetDebugActionsEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend20hasNetworkConnectionEv",
            "_ZN12_GLOBAL__N_131SimulatorRuntimePlatformBackend15beforeLoopDelayEb",
            "_ZN16BackgroundWorker5beginEv",
            "_ZN16ReaderOpenWorker5beginEv",
        };
        int n_retw_normal_precheck_names = sizeof(retw_normal_precheck_names) / sizeof(retw_normal_precheck_names[0]);

        /* Parse ELF section headers to find .symtab + .strtab */
        if (elf_len >= 52 && n_retw_names > 0) {
          uint16_t e_shnum = *(uint16_t*)(elf_data + 48);
          uint32_t e_shoff = *(uint32_t*)(elf_data + 32);
          uint16_t e_shentsize = *(uint16_t*)(elf_data + 46);
          /* uint16_t e_shstrndx = *(uint16_t *)(elf_data + 50); */

          uint32_t symtab_off = 0, symtab_size = 0, strtab_off = 0;
          for (int si = 0; si < e_shnum; si++) {
            uint32_t sh_off = e_shoff + si * e_shentsize;
            if (sh_off + 40 > elf_len) break;
            uint32_t* sh = (uint32_t*)(elf_data + sh_off);
            uint32_t sh_type = sh[1];
            if (sh_type == 2) { /* SHT_SYMTAB */
              symtab_off = sh[4];
              symtab_size = sh[5];
              /* sh_link = index of associated strtab */
              uint32_t str_idx = sh[6];
              uint32_t str_sh_off = e_shoff + str_idx * e_shentsize;
              if (str_sh_off + 40 <= elf_len) {
                uint32_t* str_sh = (uint32_t*)(elf_data + str_sh_off);
                strtab_off = str_sh[4];
              }
            }
          }

          if (symtab_off && strtab_off) {
            /* Iterate symtab entries (Elf32_Sym = 16 bytes) */
            int n_syms = symtab_size / 16;
            for (int ni = 0; ni < n_retw_names && n_retw < MAX_RETW_PATCHES; ni++) {
              const char* want = retw_names[ni];
              int want_len = strlen(want);
              if (strcmp(want, "xQueueGenericCreate") == 0 || strcmp(want, "pvPortMalloc") == 0 ||
                  strcmp(want, "heap_caps_malloc") == 0) {
                fprintf(stderr, "[QEMU-DBG] RETW-DBG: searching for '%s' want_len=%d symtab_size=%d strtab=%d\n", want,
                        want_len, symtab_size, strtab_off != 0);
              }
              /* Check if this function needs to return success (pdPASS=1) */
              int is_success = 0;
              for (int si2 = 0; si2 < n_retw_success; si2++) {
                if (strcmp(want, retw_success_names[si2]) == 0) {
                  is_success = 1;
                  break;
                }
              }
              for (int si = 0; si < n_syms; si++) {
                uint32_t sym_off = symtab_off + si * 16;
                if (sym_off + 16 > elf_len) break;
                uint32_t* s = (uint32_t*)(elf_data + sym_off);
                uint32_t st_name = s[0];
                uint32_t st_value = s[1];
                /* uint32_t st_size = s[2]; */
                if (st_name == 0 || st_value == 0) continue;
                if (st_name + want_len >= elf_len) continue;
                if (memcmp(elf_data + strtab_off + st_name, want, want_len + 1) == 0) {
                  retw_dyn[n_retw].name = want;
                  retw_dyn[n_retw].addr = st_value;
                  retw_dyn[n_retw].found = 1;
                  retw_dyn[n_retw].return_success = is_success;
                  n_retw++;
                  break;
                }
              }
            }
            fprintf(stderr, "[QEMU-DBG] Resolved %d/%d RETW patches from ELF symtab\n", n_retw, n_retw_names);

            for (int ni = 0; ni < n_retw_normal_precheck_names; ni++) {
              const char* want = retw_normal_precheck_names[ni];
              int want_len = strlen(want);
              for (int si = 0; si < n_syms; si++) {
                uint32_t sym_off = symtab_off + si * 16;
                if (sym_off + 16 > elf_len) break;
                uint32_t* s = (uint32_t*)(elf_data + sym_off);
                uint32_t st_name = s[0];
                uint32_t st_value = s[1];
                uint32_t st_size = s[2];
                if (st_name == 0 || st_value == 0) continue;
                if (st_name + want_len >= elf_len) continue;
                if (memcmp(elf_data + strtab_off + st_name, want, want_len + 1) == 0) {
                  uint32_t fn_end =
                      st_size > 0 ? st_value + st_size : mofei_resolve_elf_function_end(elf_data, elf_len, want);
                  if (fn_end <= st_value) fn_end = st_value + 64;
                  uint32_t retw_pc = mofei_find_last_retw_in_elf_span(elf_data, elf_len, st_value, fn_end, 256);
                  if (!retw_pc) {
                    fprintf(stderr, "[QEMU-DBG] WARNING: could not find normal-precheck RETW for %s @ 0x%08x\n", want,
                            st_value);
                    break;
                  }
                  mofei_add_retw_normal_precheck(&mofei_sim_addrs, retw_pc, want);
                  break;
                }
              }
            }

            /* Auto-detect allocator-family functions in retw_dyn and add
             * them to alloc_patch_addrs so the typed bump allocator fires
             * for them.  These functions have different signatures, so the
             * patch table stores argument roles instead of treating every A2
             * value as a size. */
            {
              MofeiSimAddrs* ma = &mofei_sim_addrs;
              for (int ri = 0; ri < n_retw; ri++) {
                if (!retw_dyn[ri].found) continue;
                const char* nm = retw_dyn[ri].name;
                MofeiAllocPatchSpec spec = mofei_alloc_patch_spec_for_name(nm);
                if (spec.size_kind != 0) {
                  mofei_add_alloc_patch(ma, retw_dyn[ri].addr, spec, nm);
                }
                if (strcmp(nm, "uxQueueMessagesWaiting") == 0 && ma->call_null_count < MOFEI_CALL_NULL_MAX) {
                  ma->call_null_addrs[ma->call_null_count++] = retw_dyn[ri].addr;
                }
              }
            }

            /* Resolve memory-set patch addresses from symtab */
            for (int mi = 0; mi < n_memset; mi++) {
              const char* want = memset_patches[mi].name;
              int want_len2 = strlen(want);
              for (int si = 0; si < n_syms; si++) {
                uint32_t sym_off2 = symtab_off + si * 16;
                if (sym_off2 + 16 > elf_len) break;
                uint32_t* s = (uint32_t*)(elf_data + sym_off2);
                uint32_t st_name = s[0];
                uint32_t st_value = s[1];
                if (st_name == 0 || st_value == 0) continue;
                if (st_name + want_len2 >= elf_len) continue;
                if (memcmp(elf_data + strtab_off + st_name, want, want_len2 + 1) == 0) {
                  memset_patches[mi].found = 1;
                  memset_patches[mi].addr = st_value;
                  fprintf(stderr, "[QEMU-DBG] MEMSET resolved: %s @ 0x%08x\n", want, st_value);
                  break;
                }
              }
            }
          } else {
            if (mofei_firmware_symbols) {
              for (int ni = 0; ni < n_retw_names && n_retw < MAX_RETW_PATCHES; ni++) {
                const char* want = retw_names[ni];
                int is_success = 0;
                for (int si2 = 0; si2 < n_retw_success; si2++) {
                  if (strcmp(want, retw_success_names[si2]) == 0) {
                    is_success = 1;
                    break;
                  }
                }

                uint32_t st_value = mofei_resolve_elf_symbol(elf_data, elf_len, want);
                if (st_value) {
                  retw_dyn[n_retw].name = want;
                  retw_dyn[n_retw].addr = st_value;
                  retw_dyn[n_retw].found = 1;
                  retw_dyn[n_retw].return_success = is_success;
                  n_retw++;
                }
              }
              fprintf(stderr, "[QEMU-DBG] Resolved %d/%d RETW patches from firmware symbol sidecar\n", n_retw,
                      n_retw_names);

              for (int ni = 0; ni < n_retw_normal_precheck_names; ni++) {
                const char* want = retw_normal_precheck_names[ni];
                uint32_t st_value = mofei_resolve_elf_symbol(elf_data, elf_len, want);
                if (!st_value) {
                  continue;
                }

                uint32_t fn_end = mofei_resolve_elf_function_end(elf_data, elf_len, want);
                if (fn_end <= st_value) fn_end = st_value + 64;
                uint32_t retw_pc = mofei_find_last_retw_in_elf_span(elf_data, elf_len, st_value, fn_end, 256);
                if (!retw_pc) {
                  fprintf(stderr, "[QEMU-DBG] WARNING: could not find normal-precheck RETW for %s @ 0x%08x\n", want,
                          st_value);
                  continue;
                }
                mofei_add_retw_normal_precheck(&mofei_sim_addrs, retw_pc, want);
              }

              {
                MofeiSimAddrs* ma = &mofei_sim_addrs;
                for (int ri = 0; ri < n_retw; ri++) {
                  if (!retw_dyn[ri].found) continue;
                  const char* nm = retw_dyn[ri].name;
                  MofeiAllocPatchSpec spec = mofei_alloc_patch_spec_for_name(nm);
                  if (spec.size_kind != 0) {
                    mofei_add_alloc_patch(ma, retw_dyn[ri].addr, spec, nm);
                  }
                  if (strcmp(nm, "uxQueueMessagesWaiting") == 0 && ma->call_null_count < MOFEI_CALL_NULL_MAX) {
                    ma->call_null_addrs[ma->call_null_count++] = retw_dyn[ri].addr;
                  }
                }
              }

              for (int mi = 0; mi < n_memset; mi++) {
                const char* want = memset_patches[mi].name;
                uint32_t st_value = mofei_resolve_elf_symbol(elf_data, elf_len, want);
                if (st_value) {
                  memset_patches[mi].found = 1;
                  memset_patches[mi].addr = st_value;
                  fprintf(stderr, "[QEMU-DBG] MEMSET resolved: %s @ 0x%08x\n", want, st_value);
                }
              }
            } else {
              fprintf(stderr, "[QEMU-DBG] WARNING: no .symtab found in ELF, skipping dynamic patches\n");
            }
          }
        }

        /* Apply RETW patches: overwrite entry+3 with the appropriate
         * return sequence.
         * - Plain RETW (3 bytes: 90 00 00) for functions returning void/0.
         * - NOP.N + RETW.N (4 bytes: 3D F0 1D F0) for functions that
         *   preserve a caller-provided return candidate or receive a
         *   TCG-level success override.
         * The Xtensa 'entry' instruction is 3 bytes; the second
         * instruction starts at offset +3. */
        {
          uint16_t phnum = *(uint16_t*)(elf_data + 44);
          uint32_t phoff = *(uint32_t*)(elf_data + 28);
          static const uint8_t retw3[] = {0x90, 0x00, 0x00};          /* retw (wide, 3 bytes) */
          static const uint8_t nop_retw[] = {0x3D, 0xF0, 0x1D, 0xF0}; /* nop.n; retw.n (4 bytes) */
          for (int pi = 0; pi < n_retw; pi++) {
            uint32_t patch_addr = retw_dyn[pi].addr + 3;
            uint32_t retw_pc = retw_dyn[pi].return_success ? (retw_dyn[pi].addr + 5) : patch_addr;
            const uint8_t* patch_bytes = retw_dyn[pi].return_success ? nop_retw : retw3;
            int patch_len = retw_dyn[pi].return_success ? 4 : 3;
            for (int i = 0; i < phnum && phoff + (i + 1) * 32 <= elf_len; i++) {
              uint32_t* ph = (uint32_t*)(elf_data + phoff + i * 32);
              if (ph[0] != 1) continue;
              uint32_t p_offset = ph[1];
              uint32_t p_vaddr = ph[3];
              uint32_t p_filesz = ph[4];
              if (p_vaddr <= patch_addr && patch_addr + patch_len <= p_vaddr + p_filesz) {
                uint32_t file_off = p_offset - p_vaddr + patch_addr;
                if (file_off + patch_len <= elf_len) {
                  memcpy(elf_data + file_off, patch_bytes, patch_len);
                  mofei_add_retw_patch(&mofei_sim_addrs, retw_pc, retw_dyn[pi].name);
                  fprintf(stderr, "[QEMU-DBG] RETW%s %s @ 0x%08x\n", retw_dyn[pi].return_success ? "+SUCCESS" : "",
                          retw_dyn[pi].name, retw_pc);
                }
                break;
              }
            }
          }
        }
      }

      /* Patch ROM functions that crash during call_start_cpu0.
       *
       * The ROM bootloader functions (memset, rom_config_instruction_cache_mode,
       * Cache_Resume_DCache, etc.) access SPI flash tables or hardware registers
       * that aren't initialized in QEMU's -kernel mode (no second-stage bootloader).
       *
       * We patch the ROM binary so these functions return immediately.
       * Two patch types:
       *   A) "Entry + RETW": for functions with an 'entry' instruction,
       *      overwrite entry+3 with RETW (the entry already set up the window).
       *   B) "Entry + RETW.N": for jump-table thunks that have no 'entry',
       *      write 'entry a1,16; retw.n' at the start.
       *
       * ROM function addresses are fixed (from ESP32-S3 ROM binary), so
       * this works across all firmware builds. */
      {
        static const uint8_t retw3[] = {0x90, 0x00, 0x00}; /* retw */
        static const uint8_t entry_retw[] = {
            0x36, 0x21, 0x00, /* entry a1, 16 */
            0x1D, 0xF0,       /* retw.n */
        };

        /* Type A: functions with their own 'entry' instruction.
         * Patch entry+3 with RETW. */
        struct {
          uint32_t addr;
          const char* name;
        } rom_entry_retw[] = {
            /* spi_flash_set_rom_required_regs — called from rom_config_instruction_cache_mode thunk */
            {0x4004bae4, "spi_flash_set_rom_required_regs"},
            /* spi_flash_rom_init */
            {0x4004e290, "spi_flash_rom_init"},
            /* spi_flash_read_ops — the one that dereferences NULL at 0x3fceffc4 */
            {0x4004eab0, "spi_flash_read_ops"},
            /* cache_resume_dcache_impl */
            {0x4004bb38, "cache_resume_dcache_impl"},
            /* cache_resume_impl */
            {0x4004f480, "cache_resume_impl"},
            /* cache_resume_after_config */
            {0x4004bb68, "cache_resume_after_config"},
            /* cache_set_idrom_mmu */
            {0x4004f308, "cache_set_idrom_mmu"},
        };
        for (int ri = 0; ri < (int)(sizeof(rom_entry_retw) / sizeof(rom_entry_retw[0])); ri++) {
          uint32_t addr = rom_entry_retw[ri].addr;
          uint32_t patch_off = (addr - 0x40000000) + 3; /* skip entry */
          if (patch_off + 3 <= rom_len) {
            memcpy(rom_data + patch_off, retw3, 3);
            mofei_add_retw_patch(&mofei_sim_addrs, addr + 3, rom_entry_retw[ri].name);
            fprintf(stderr, "[QEMU-DBG] ROM-RETW %s @ 0x%08x\n", rom_entry_retw[ri].name, addr);
          }
        }

        /* Type B: jump-table thunks (l32r + jx, no 'entry').
         * Overwrite with 'entry a1,16; retw.n'. */
        struct {
          uint32_t addr;
          const char* name;
        } rom_thunk_retw[] = {
            /* memset thunk — LEFT UNPATCHED so the firmware can
             * use memset normally. The ROM memset at 0x400011e8
             * is a jump-table thunk (l32r a9, real_memset; jx a9)
             * that jumps to the real implementation. The real
             * memset writes to DRAM using standard store
             * instructions, which works in QEMU. */
            /* { 0x400011e8, "memset(thunk)" }, */
            /* Cache_Resume_DCache thunk */
            {0x400018c0, "Cache_Resume_DCache(thunk)"},
            /* rom_config_instruction_cache_mode thunk */
            {0x40001a1c, "rom_config_instruction_cache_mode(thunk)"},
            /* rom_config_data_cache_mode thunk */
            {0x40001a28, "rom_config_data_cache_mode(thunk)"},
            /* Cache_Set_IDROM_MMU_Size thunk */
            {0x40001914, "Cache_Set_IDROM_MMU_Size(thunk)"},
            /* esp_rom_get_reset_reason thunk (called early in call_start_cpu0) */
            {0x4000057c, "esp_rom_get_reset_reason(thunk)"},
        };
        for (int ri = 0; ri < (int)(sizeof(rom_thunk_retw) / sizeof(rom_thunk_retw[0])); ri++) {
          uint32_t addr = rom_thunk_retw[ri].addr;
          uint32_t patch_off = addr - 0x40000000;
          if (patch_off + sizeof(entry_retw) <= rom_len) {
            memcpy(rom_data + patch_off, entry_retw, sizeof(entry_retw));
            mofei_add_retw_patch(&mofei_sim_addrs, addr + 3, rom_thunk_retw[ri].name);
            fprintf(stderr, "[QEMU-DBG] ROM-ENTRY-RETW %s @ 0x%08x\n", rom_thunk_retw[ri].name, addr);
          }
        }
      }

      /* Patch ADC calibration functions to return 0.
       * These functions read eFuse calibration data which is not emulated
       * in QEMU, causing infinite loops.  read_cal_channel is the key
       * function that loops on eFuse reads.
       * We patch AFTER load_elf and manual segment loading, writing
       * directly to the system memory address space (overlay RAM).
       * Patch: entry a1, 0; movi.n a2, 0; retw.n
       * (36 21 00 02 0c 1d f0 = 7 bytes) */
      {
        static const uint8_t ret0_patch[] = {0x36, 0x21, 0x00, 0x02, 0x0c, 0x1d, 0xf0};
        for (int ti = 0; ti < mofei_cpu1_sync.adc_patch_count; ti++) {
          uint32_t target = mofei_cpu1_sync.adc_patch_fns[ti];
          if (target == 0) continue;
          address_space_write(&address_space_memory, target, MEMTXATTRS_UNSPECIFIED, ret0_patch, sizeof(ret0_patch));
          /* Also verify by reading back */
          uint8_t check[sizeof(ret0_patch)];
          address_space_read(&address_space_memory, target, MEMTXATTRS_UNSPECIFIED, check, sizeof(check));
          bool ok = (memcmp(check, ret0_patch, sizeof(ret0_patch)) == 0);
          if (ok) {
            mofei_add_retw_patch(&mofei_sim_addrs, target + 5, "adc calibration ret0");
          }
          fprintf(stderr, "[QEMU-DBG] ADC patch @ 0x%08x: %s (readback: %s)\n", target, ok ? "OK" : "MISMATCH",
                  ok ? "matches" : "does NOT match patch!");
        }
      }

      uint64_t manual_image_entry = 0;
      if (elf_len >= 28 && memcmp(elf_data, ELFMAG, 4) == 0) {
        uint32_t entry32 = 0;
        memcpy(&entry32, elf_data + 24, sizeof(entry32));
        manual_image_entry = entry32;
      }

      gchar* tmpfile = g_build_filename(g_get_tmp_dir(), "panda-patched.elf", NULL);
      g_file_set_contents(tmpfile, elf_data, elf_len, NULL);
      g_free(elf_data);

      image_size = load_elf(tmpfile, NULL, translate_phys_addr, &ss->cpu[0], &image_entry, &image_lowaddr, NULL, NULL,
                            0, EM_XTENSA, 0, 0);
      fprintf(stderr, "[QEMU-DBG] ELF load result: size=%d entry=0x%lx lowaddr=0x%lx\n", image_size,
              (unsigned long)image_entry, (unsigned long)image_lowaddr);
      /* load_elf may not write ICACHE/DCACHE segments correctly due to
       * virtual-to-physical translation issues.  Manually write all PT_LOAD
       * segments to the system address space to ensure patched code/data
       * are in RAM (especially the 0x420xxxxx ICACHE overlay). */
      int manual_load_count = 0;
      uint64_t manual_lowaddr = (uint64_t)-1;
      uint64_t manual_highaddr = 0;
      ssize_t manual_image_size = 0;
      {
        uint8_t* elf2 = NULL;
        gsize elf2_len = 0;
        if (g_file_get_contents(tmpfile, (gchar**)&elf2, &elf2_len, NULL) && elf2 != NULL && elf2_len > 0) {
          uint16_t ph2 = *(uint16_t*)(elf2 + 44);
          uint32_t ph2off = *(uint32_t*)(elf2 + 28);
          for (int j = 0; j < ph2 && ph2off + (j + 1) * 32 <= elf2_len; j++) {
            uint32_t* php = (uint32_t*)(elf2 + ph2off + j * 32);
            if (php[0] != 1) continue; /* PT_LOAD */
            uint32_t seg_vaddr = php[3];
            uint32_t seg_filesz = php[4];
            uint32_t seg_offset = php[1];
            if (seg_filesz == 0) continue;
            if (seg_offset + seg_filesz > elf2_len) continue;
            MemTxResult write_result = address_space_write(&address_space_memory, seg_vaddr, MEMTXATTRS_UNSPECIFIED,
                                                           elf2 + seg_offset, seg_filesz);
            if (write_result != MEMTX_OK) {
              fprintf(stderr, "[QEMU-DBG] Manual load seg %d failed: vaddr=0x%08x filesz=0x%x result=%d\n", j,
                      seg_vaddr, seg_filesz, write_result);
              continue;
            }
            manual_load_count++;
            manual_image_size += seg_filesz;
            if (seg_vaddr < manual_lowaddr) {
              manual_lowaddr = seg_vaddr;
            }
            if ((uint64_t)seg_vaddr + seg_filesz > manual_highaddr) {
              manual_highaddr = (uint64_t)seg_vaddr + seg_filesz;
            }
            fprintf(stderr,
                    "[QEMU-DBG] Manual load seg %d: "
                    "vaddr=0x%08x filesz=0x%x\n",
                    j, seg_vaddr, seg_filesz);
          }
          if (mofei_sim_addrs.murphyDeviceShellRunSimulatorLoopWithContext_addr) {
            uint32_t loop_resume = mofei_sim_addrs.murphyDeviceShellRunSimulatorLoopWithContext_addr + 0x5cu;
            uint8_t loop_resume_bytes[6] = {0};
            address_space_read(&address_space_memory, loop_resume, MEMTXATTRS_UNSPECIFIED, loop_resume_bytes,
                               sizeof(loop_resume_bytes));
            fprintf(stderr, "[QEMU-DBG] Murphy loop resume readback @ 0x%08x: %02x %02x %02x %02x %02x %02x\n",
                    loop_resume, loop_resume_bytes[0], loop_resume_bytes[1], loop_resume_bytes[2], loop_resume_bytes[3],
                    loop_resume_bytes[4], loop_resume_bytes[5]);
          }
          g_free(elf2);
        }
      }
      if (image_size < 0 && manual_load_count > 0 && manual_image_entry != 0) {
        fprintf(stderr,
                "[QEMU-DBG] load_elf failed (%s); continuing with %d manually loaded PT_LOAD segments "
                "(entry=0x%lx lowaddr=0x%lx highaddr=0x%lx)\n",
                load_elf_strerror(image_size), manual_load_count, (unsigned long)manual_image_entry,
                (unsigned long)manual_lowaddr, (unsigned long)manual_highaddr);
        image_entry = manual_image_entry;
        image_lowaddr = manual_lowaddr;
        image_size = manual_image_size > 0 ? manual_image_size : 1;
      }
      /* Apply memory-set patches (write runtime values to DRAM/BSS) */
      {
        for (int mi = 0; mi < (int)(sizeof(memset_patches) / sizeof(memset_patches[0])); mi++) {
          if (memset_patches[mi].found) {
            uint8_t val[4];
            memcpy(val, &memset_patches[mi].value, 4);
            address_space_write(&address_space_memory, memset_patches[mi].addr, MEMTXATTRS_UNSPECIFIED, val, 4);
            fprintf(stderr, "[QEMU-DBG] MEMSET %s @ 0x%08x = %u\n", memset_patches[mi].name, memset_patches[mi].addr,
                    memset_patches[mi].value);
          }
        }
      }
      g_free(tmpfile);
      /* Verify init function table loaded correctly */
      {
        uint32_t init_start = 0x3c5a8260;
        uint32_t init_end = 0x3c5a8328;
        fprintf(stderr, "[QEMU-DBG] Init fn table @ 0x%08x-0x%08x:\n", init_start, init_end);
        for (uint32_t a = init_start; a < init_end; a += 8) {
          uint32_t fn_ptr = 0, flags = 0;
          address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED, &fn_ptr, 4);
          address_space_read(&address_space_memory, a + 4, MEMTXATTRS_UNSPECIFIED, &flags, 4);
          if (fn_ptr != 0) {
            fprintf(stderr, "  [0x%08x] fn=0x%08x flags=0x%x\n", a, fn_ptr, flags);
          }
        }
      }
      if (image_size < 0) {
        error_report("Error: could not load ELF file '%s'", load_elf_filename);
        exit(1);
      }
      ss->kernel_entry = image_entry;
    }

    fprintf(stderr, "[QEMU-DBG] XCHAL_RESET_VECTOR_PADDR=0x%lx\n", (unsigned long)XCHAL_RESET_VECTOR_PADDR);
    if (image_entry != XCHAL_RESET_VECTOR_PADDR) {
      /* Patch trampoline into ROM data at the reset vector offset.
       * Trampoline sets SP and jumps to ELF entry.
       *
       * Layout (26 bytes):
       *   +0x00: j _code            -- 00 02 06
       *   +0x03: padding            -- 00
       *   +0x04: .word image_entry    -- literal
       *   +0x08: .word MOFEI_SIM_FAKE_APP_STACK_TOP
       *   +0x0c: l32r a1, [sp]      -- ff ff 11
       *   +0x0f: rsync              -- 00 20 10
       *   +0x12: l32r a0, [entry]   -- ff fb 01
       *   +0x15: jx a0              -- 00 00 a0
       */
      size_t tramp_offset = XCHAL_RESET_VECTOR_PADDR - esp32s3_memmap[ESP32S3_MEMREGION_IROM].base;
      uint8_t p[4];
      memcpy(p, &image_entry, 4);
      const uint32_t boot_stack_top = MOFEI_SIM_FAKE_APP_STACK_TOP;
      uint8_t sp[4];
      memcpy(sp, &boot_stack_top, 4);
      uint8_t boot[] = {
          0x00,  0x02,  0x06,         /* +0x00: j _code */
          0x00,                       /* +0x03: padding */
          p[0],  p[1],  p[2],  p[3],  /* +0x04: .word image_entry */
          sp[0], sp[1], sp[2], sp[3], /* +0x08: .word fake app stack top */
          0xFF,  0xFF,  0x11,         /* +0x0c: l32r a1, [SP literal] */
          0x00,  0x20,  0x10,         /* +0x0f: rsync */
          0xFF,  0xFB,  0x01,         /* +0x12: l32r a0, [entry literal] */
          0x00,  0x00,  0xA0,         /* +0x15: jx a0 */
      };
      if (rom_data && tramp_offset + sizeof(boot) <= rom_len) {
        memcpy(rom_data + tramp_offset, boot, sizeof(boot));
        fprintf(stderr, "[QEMU-DBG] Trampoline patched at ROM offset 0x%zx, entry=0x%lx\n", tramp_offset,
                (unsigned long)image_entry);
      }
    }

    /* ROM memset patch removed — we NOP the call sites instead.
     * The ROM memset at offset 0x11e8 is left as-is because
     * NOP'ing the calls is safer (no window frame issues). */

    /* ROM cache function trampoline patches removed — we NOP the call
     * sites instead because retw.n in trampolines causes window issues
     * (the trampolines don't have 'entry', so window state is ambiguous). */

    /* Patch ROM functions that loop endlessly in QEMU TCG.
     * Keep 'entry' intact so window state is managed correctly.
     * Patch bytes after entry with: movi.n a2, value; retw.n */
    if (rom_data && rom_len > 0x5544c + 2) {
      /* Patch ROM strcmp at 0x55448: keep entry (36 21 00),
       * patch bytes at +3 with movi.n a2, 1; retw.n
       * Original at +3: 82 02 00 (l8ui a8, a2, 0)
       * Replace:  0c 12    = movi.n a2, 1  (not equal)
       *           1d f0    = retw.n */
      if (rom_data[0x55448] == 0x36 && rom_data[0x55449] == 0x21 && rom_data[0x5544a] == 0x00) {
        rom_data[0x5544b] = 0x0c;  // movi.n a2, 1
        rom_data[0x5544c] = 0x12;
        rom_data[0x5544d] = 0x1d;  // retw.n
        rom_data[0x5544e] = 0xf0;
        mofei_add_retw_patch(&mofei_sim_addrs, 0x4005544du, "ROM strcmp");
        fprintf(stderr, "[QEMU-DBG] Patched ROM strcmp → entry; movi a2,1; retw\n");
      }
      /* Patch ROM strlen at 0x55698: keep entry (36 21 00),
       * patch bytes at +3 with movi.n a2, 0; retw.n
       * Original at +3: 32 c2 fc (addi a3, a2, -4)
       * Replace:  0c 02    = movi.n a2, 0  (length 0)
       *           1d f0    = retw.n */
      if (rom_data[0x55698] == 0x36 && rom_data[0x55699] == 0x21 && rom_data[0x5569a] == 0x00) {
        rom_data[0x5569b] = 0x0c;  // movi.n a2, 0
        rom_data[0x5569c] = 0x02;
        rom_data[0x5569d] = 0x1d;  // retw.n
        rom_data[0x5569e] = 0xf0;
        mofei_add_retw_patch(&mofei_sim_addrs, 0x4005569du, "ROM strlen");
        fprintf(stderr, "[QEMU-DBG] Patched ROM strlen → entry; movi a2,0; retw\n");
      }
    }

    /* Register ROM as one blob (includes patched trampoline) */
    if (rom_data) {
      rom_add_blob_fixed_as("esp32s3.rom", rom_data, rom_len, esp32s3_memmap[ESP32S3_MEMREGION_IROM].base,
                            CPU(&ss->cpu[0])->as);
    }
    g_free(rom_data);

    ss->cpu[0].env.kernel_entry = ss->kernel_entry;
    ss->cpu[0].env.pc = ss->kernel_entry;
    ss->cpu[0].env.sregs[WINDOW_START] = 1;
    ss->cpu[0].env.regs[1] = MOFEI_SIM_FAKE_APP_STACK_TOP;
    ss->cpu[0].env.sregs[PS] = PS_WOE;
    ss->suppress_reset_count = 3;

    /* Initialize ROM data pointers that the bootloader normally sets up.
     * In the ELF boot path, the 2nd-stage bootloader never runs, so these
     * pointers in SRAM (0x3fceffc0–0x3fcefffc) are all zero.  ROM functions
     * dereference them during early init; a NULL pointer causes an immediate
     * crash at PC=0.
     *
     * Layout (in SRAM, below the ROM data area):
     *   0x3fce0100: retw.n instruction (0x1df0) — safe stub for vtable calls
     *   0x3fce0200: vtable (512 entries = 2KB, all → retw) — covers offsets up to 0x7FC
     *   0x3fce0A00: struct (4 bytes → vtable) — used by 0x3fceffc4 chain
     *   0x3fce0B00: data area (256 bytes, zeroed) — safe for data reads
     *   0x3fce0C00: safe struct (1KB, each word → vtable) — for nested dereferences
     *   0x3fce1000: safe func-ptr (256 bytes, each word → retw) — for indirect calls
     *
     * ROM data pointer map (from firmware .iram0 symbols):
     *   0x3fceffc0: ets_ecc_table_ptr
     *   0x3fceffc4: (vtable struct chain — already initialized above)
     *   0x3fceffd0: _global_impure_ptr
     *   0x3fceffd4: syscall_table_ptr
     *   0x3fceffe4: rom_spiflash_legacy_data
     *   0x3fceffe8: rom_spiflash_legacy_funcs
     *   0x3fcefff4: rom_opiflash_cmd_def
     *   0x3fcefffc: ets_ops_table_ptr
     *
     * Additional ROM pointers discovered via disassembly:
     *   0x3fceff64: used by ROM function at 0x40006f6b (nested struct deref)
     *   Many other pointers in 0x3fceff00–0x3fceffc0 range
     *
     * For function-table pointers we use the vtable (→ retw stub).
     * For data pointers we use the zeroed area.
     * For safety, we also fill the entire 0x3fceff00–0x3fceffff range. */
    {
      const uint32_t retw_addr = 0x3fce0100;
      const uint32_t vtable_addr = 0x3fce0200;
      const uint32_t struct_addr = 0x3fce0A00;
      const uint32_t data_addr = 0x3fce0B00;
      const uint32_t safe_struct_addr = 0x3fce0C00;
      const uint32_t safe_func_addr = 0x3fce1000;
      const uint16_t retw_insn = 0x1df0;
      uint32_t vtable[512];
      for (int i = 0; i < 512; i++) {
        vtable[i] = retw_addr;
      }
      address_space_write(&address_space_memory, retw_addr, MEMTXATTRS_UNSPECIFIED, &retw_insn, 2);
      address_space_write(&address_space_memory, vtable_addr, MEMTXATTRS_UNSPECIFIED, vtable, sizeof(vtable));
      address_space_write(&address_space_memory, struct_addr, MEMTXATTRS_UNSPECIFIED, &vtable_addr, 4);

      /* Zeroed data area */
      uint8_t zeros[256] = {0};
      address_space_write(&address_space_memory, data_addr, MEMTXATTRS_UNSPECIFIED, zeros, sizeof(zeros));

      /* Safe struct area: each word is a pointer to the vtable.
       * Any struct dereference will find the vtable → retw stub.
       * ROM functions may read at large offsets (e.g., 0x278),
       * so allocate enough space to cover the maximum offset. */
      uint32_t safe_struct[256];
      for (int i = 0; i < 256; i++) {
        safe_struct[i] = vtable_addr;
      }
      address_space_write(&address_space_memory, safe_struct_addr, MEMTXATTRS_UNSPECIFIED, safe_struct,
                          sizeof(safe_struct));

      /* Safe func-ptr area: each word is a pointer to retw stub.
       * Any indirect call through these will just return. */
      uint32_t safe_func[64];
      for (int i = 0; i < 64; i++) {
        safe_func[i] = retw_addr;
      }
      address_space_write(&address_space_memory, safe_func_addr, MEMTXATTRS_UNSPECIFIED, safe_func, sizeof(safe_func));

      /* Fill the entire 0x3fceff00–0x3fceffff range with safe_struct_addr.
       * This covers all ROM data pointers including unknown ones.
       * When the ROM dereferences any of these:
       *   *(0x3fceffXX) = safe_struct_addr → *(safe_struct + offset) = vtable_addr
       *   → vtable[N] = retw_addr → call executes retw (returns immediately)
       */
      uint32_t safe_fill[64];
      for (int i = 0; i < 64; i++) {
        safe_fill[i] = safe_struct_addr;
      }
      address_space_write(&address_space_memory, 0x3fceff00, MEMTXATTRS_UNSPECIFIED, safe_fill, sizeof(safe_fill));

      /* Also fill 0x3fcefe00–0x3fcefeff for good measure */
      address_space_write(&address_space_memory, 0x3fcefe00, MEMTXATTRS_UNSPECIFIED, safe_fill, sizeof(safe_fill));

      address_space_write(&address_space_memory, 0x3fceffc4, MEMTXATTRS_UNSPECIFIED, &struct_addr, 4);

      const uint32_t rom_ptrs[] = {
          0x3fceffc0, 0x3fceffd0, 0x3fceffd4, 0x3fceffe4, 0x3fceffe8, 0x3fcefff4, 0x3fcefffc,
      };
      for (int i = 0; i < (int)(sizeof(rom_ptrs) / sizeof(rom_ptrs[0])); i++) {
        address_space_write(&address_space_memory, rom_ptrs[i], MEMTXATTRS_UNSPECIFIED, &data_addr, 4);
      }
      fprintf(stderr, "[QEMU-DBG] ROM data ptrs initialized: vtable@0x%x data@0x%x safe_struct@0x%x\n", vtable_addr,
              data_addr, safe_struct_addr);
    }
    /* Set CCOMPARE0 to maximum to prevent an immediate timer interrupt.
     * CCOUNT starts at 0 on reset; if CCOMPARE0 is also 0, the timer
     * match fires on the first tick, causing an interrupt storm that
     * traps the CPU in an endless interrupt loop.  Setting it to
     * 0xFFFFFFFF gives us ~4 billion ticks before the first match.
     * CCOMPARE is special register 240; CCOMPARE0/1/2 are at 240/241/242. */
    ss->cpu[0].env.sregs[240] = 0xFFFFFFFF; /* CCOMPARE0 */
    ss->cpu[0].env.sregs[241] = 0xFFFFFFFF; /* CCOMPARE1 */
    ss->cpu[0].env.sregs[242] = 0xFFFFFFFF; /* CCOMPARE2 */
    CPU(&ss->cpu[0])->exception_index = -1;
    CPU(&ss->cpu[0])->halted = 0;
    CPU(&ss->cpu[0])->stop = false;
    CPU(&ss->cpu[0])->stopped = false;
    ss->cpu[1].env.kernel_entry = 0;
    ss->cpu[1].env.runstall = true;
    CPU(&ss->cpu[1])->start_powered_off = true;
    CPU(&ss->cpu[1])->halted = true;

    fprintf(stderr, "[QEMU] kernel_entry=0x%lx pc=0x%x\n", (unsigned long)ss->kernel_entry, ss->cpu[0].env.pc);
  } else {
    /* Flash boot: load ROM binary.  If MOFEI_ROM_BINARY is set, use it
     * as the ROM path (allows the headless harness to pass a patched ROM).
     * Otherwise fall back to the standard pc-bios ROM. */
    char* rom_binary = mofei_resolve_rom_binary();
    if (rom_binary == NULL) {
      error_report("Error: ROM binary not found (set MOFEI_ROM_BINARY or ensure pc-bios/esp32s3_rev0_rom.bin exists)");
      exit(1);
    }

    int size = load_image_targphys_as(rom_binary, esp32s3_memmap[ESP32S3_MEMREGION_IROM].base,
                                      esp32s3_memmap[ESP32S3_MEMREGION_IROM].size, CPU(&ss->cpu[0])->as);
    if (size < 0) {
      error_report("Error: could not load ROM binary '%s'", rom_binary);
      exit(1);
    }
    fprintf(stderr, "[QEMU-DBG] Flash boot ROM: %d bytes from %s\n", size, rom_binary);
    g_free(rom_binary);

    if (ESP32S3_CPU_COUNT > 1) {
      rom_binary = mofei_resolve_rom_binary();
      if (rom_binary == NULL) {
        error_report("Error: ROM binary not found for CPU1");
        exit(1);
      }

      size = load_image_targphys_as(rom_binary, esp32s3_memmap[ESP32S3_MEMREGION_IROM].base,
                                    esp32s3_memmap[ESP32S3_MEMREGION_IROM].size, CPU(&ss->cpu[1])->as);
      if (size < 0) {
        error_report("Error: could not load ROM binary '%s' for CPU1", rom_binary);
        exit(1);
      }
      g_free(rom_binary);
    }

    /* Start CPU0 from ROM reset vector.  Leave kernel_entry clear here:
     * cpu.c uses it for the direct -kernel path and forces a synthetic
     * window state that is not valid for ROM flash boot. */
    ss->cpu[0].env.kernel_entry = 0;
    ss->cpu[0].env.pc = 0x40000400;
    ss->cpu[0].env.sregs[240] = 0xFFFFFFFF;
    ss->cpu[0].env.sregs[241] = 0xFFFFFFFF;
    ss->cpu[0].env.sregs[242] = 0xFFFFFFFF;
    CPU(&ss->cpu[0])->exception_index = -1;
    CPU(&ss->cpu[0])->halted = 0;
    CPU(&ss->cpu[0])->stop = false;
    CPU(&ss->cpu[0])->stopped = false;
    ss->cpu[1].env.kernel_entry = 0;
    ss->cpu[1].env.runstall = true;
    CPU(&ss->cpu[1])->start_powered_off = true;
    CPU(&ss->cpu[1])->halted = true;
  }
  /* Pre-write CPU1 sync flags immediately (instead of timer-based).
   * In the ELF boot path, the firmware enters tight spin loops waiting
   * for s_cpu_up[1] and s_cpu_inited[1]. QEMU timers can't fire while
   * the CPU is in a tight TB loop, so we must set the flags before
   * the CPU starts executing. */
  if (mofei_cpu1_sync.s_cpu_up_1 || mofei_cpu1_sync.s_cpu_inited_1 || mofei_cpu1_sync.s_system_inited_1 ||
      mofei_cpu1_sync.s_system_full_inited) {
    uint8_t one = 1;
    if (mofei_cpu1_sync.s_cpu_up_1) {
      uint32_t s_cpu_up_0 = mofei_cpu1_sync.s_cpu_up_1 - 1;
      address_space_write(&address_space_memory, s_cpu_up_0, MEMTXATTRS_UNSPECIFIED, &one, 1);
      address_space_write(&address_space_memory, mofei_cpu1_sync.s_cpu_up_1, MEMTXATTRS_UNSPECIFIED, &one, 1);
    }
    if (mofei_cpu1_sync.s_cpu_inited_1) {
      uint32_t s_cpu_inited_0 = mofei_cpu1_sync.s_cpu_inited_1 - 1;
      address_space_write(&address_space_memory, s_cpu_inited_0, MEMTXATTRS_UNSPECIFIED, &one, 1);
      address_space_write(&address_space_memory, mofei_cpu1_sync.s_cpu_inited_1, MEMTXATTRS_UNSPECIFIED, &one, 1);
    }
    if (mofei_cpu1_sync.s_system_inited_1) {
      uint32_t s_system_inited_0 = mofei_cpu1_sync.s_system_inited_1 - 1;
      address_space_write(&address_space_memory, s_system_inited_0, MEMTXATTRS_UNSPECIFIED, &one, 1);
      address_space_write(&address_space_memory, mofei_cpu1_sync.s_system_inited_1, MEMTXATTRS_UNSPECIFIED, &one, 1);
    }
    if (mofei_cpu1_sync.s_system_full_inited) {
      address_space_write(&address_space_memory, mofei_cpu1_sync.s_system_full_inited, MEMTXATTRS_UNSPECIFIED, &one, 1);
    }
    fprintf(stderr, "[QEMU] CPU1 sync flags pre-written\n");
  }

  /* Mofei: Start a fast debug timer to trace early boot progress.
   * DISABLED for crash debugging */
#if 0
    {
        static int early_pc_count = 0;
        static QEMUTimer *early_pc_timer;
        if (!early_pc_timer) {
            early_pc_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                           mofei_debug_pc_timer_cb, NULL);
        }
        early_pc_count = 0;
        mofei_debug_pc_dumps = 0;
        timer_mod(early_pc_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 5000);
    }
#endif

  {
    static QEMUTimer* cpu_resume_timer;
    if (!cpu_resume_timer) {
      cpu_resume_timer = timer_new_ms(QEMU_CLOCK_REALTIME, mofei_cpu_resume_timer_cb, ss);
    }
    timer_mod(cpu_resume_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 10000);
  }

  /* Start the CPU tick timer to keep the simulated CPU alive.
   * Without periodic interrupts the firmware halts on `waiti` and
   * never resumes.  The timer injects software interrupts every 100 ms
   * of virtual time to simulate a ticking clock. */
  {
    if (!mofei_cpu_tick_timer) {
      mofei_cpu_tick_timer = timer_new_ms(QEMU_CLOCK_REALTIME, mofei_cpu_tick_timer_cb, ss);
    }
    mofei_cpu_tick_count = 0;
    timer_mod(mofei_cpu_tick_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100); /* 100 ms */
  }

  fprintf(stderr, "[QEMU] esp32s3_machine_init done\n");
}

static ram_addr_t esp32s3_fixup_ram_size(ram_addr_t requested_size) {
  ram_addr_t size;
  if (requested_size == 0) {
    size = 0;
  } else if (requested_size <= 2 * MiB) {
    size = 2 * MiB;
  } else if (requested_size <= 4 * MiB) {
    size = 4 * MiB;
  } else if (requested_size <= 8 * MiB) {
    size = 8 * MiB;
  } else if (requested_size <= 16 * MiB) {
    size = 16 * MiB;
  } else if (requested_size <= 32 * MiB) {
    size = 32 * MiB;
  } else {
    qemu_log("RAM size larger than 32 MB not supported\n");
    size = 32 * MiB;
  }
  return size;
}

/* Initialize machine type */
static void esp32s3_machine_class_init(ObjectClass* oc, void* data) {
  MachineClass* mc = MACHINE_CLASS(oc);
  mc->desc = "Espressif ESP32S3 machine";
  mc->init = esp32s3_machine_init;
  mc->max_cpus = 2;
  mc->default_cpus = 2;
  mc->default_ram_size = 0;
  mc->fixup_ram_size = esp32s3_fixup_ram_size;
}

static const TypeInfo esp32s3_info = {
    .name = TYPE_ESP32S3_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Esp32s3MachineState),
    .class_init = esp32s3_machine_class_init,
};

static void esp32s3_machine_type_init(void) { type_register_static(&esp32s3_info); }

type_init(esp32s3_machine_type_init);
