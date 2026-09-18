/*
 * Mofei Simulator Dynamic Addresses
 *
 * Resolved from the firmware ELF symbol table at load time.
 * Used by translate.c and exc_helper.c to intercept firmware functions
 * without hardcoding PC addresses that change on every rebuild.
 *
 * Defined in hw/xtensa/esp32s3.c
 */

#ifndef MOFEI_SIM_ADDRS_H
#define MOFEI_SIM_ADDRS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum MofeiSimBoardKind {
  MOFEI_SIM_BOARD_MOFEI = 0,
  MOFEI_SIM_BOARD_S37UC = 1,
  MOFEI_SIM_BOARD_LILYGO_T5S3_PRO = 2,
  MOFEI_SIM_BOARD_M5PAPERS3 = 3,
} MofeiSimBoardKind;

extern MofeiSimBoardKind mofei_sim_active_board;

static inline bool mofei_sim_board_is_s37uc(void) { return mofei_sim_active_board == MOFEI_SIM_BOARD_S37UC; }
static inline bool mofei_sim_board_is_lilygo_t5s3_pro(void) {
  return mofei_sim_active_board == MOFEI_SIM_BOARD_LILYGO_T5S3_PRO;
}
static inline bool mofei_sim_board_is_m5papers3(void) { return mofei_sim_active_board == MOFEI_SIM_BOARD_M5PAPERS3; }

#define MOFEI_LILYGO_FRAMEBUFFER_WIDTH 540u
#define MOFEI_LILYGO_FRAMEBUFFER_HEIGHT 960u
#define MOFEI_LILYGO_GRAY16_PIXELS_PER_BYTE 2u
#define MOFEI_LILYGO_FRAMEBUFFER_BYTES \
  ((MOFEI_LILYGO_FRAMEBUFFER_WIDTH * MOFEI_LILYGO_FRAMEBUFFER_HEIGHT) / MOFEI_LILYGO_GRAY16_PIXELS_PER_BYTE)
#define MOFEI_PAPERS3_FRAMEBUFFER_WIDTH MOFEI_LILYGO_FRAMEBUFFER_WIDTH
#define MOFEI_PAPERS3_FRAMEBUFFER_HEIGHT MOFEI_LILYGO_FRAMEBUFFER_HEIGHT
#define MOFEI_PAPERS3_FRAMEBUFFER_BYTES MOFEI_LILYGO_FRAMEBUFFER_BYTES

/* ESP32-S3 simulator guest memory ranges used by QEMU helper paths. */
#define MOFEI_SIM_EXTMEM_REGION_SIZE 0x02000000u
#define MOFEI_SIM_DCACHE_BASE 0x3c000000u
#define MOFEI_SIM_DCACHE_LIMIT (MOFEI_SIM_DCACHE_BASE + MOFEI_SIM_EXTMEM_REGION_SIZE)
#define MOFEI_SIM_DCACHE_FIRMWARE_RESERVE_BYTES 0x00800000u
#define MOFEI_SIM_INTERNAL_DRAM_BASE 0x3fc80000u
#define MOFEI_SIM_INTERNAL_DRAM_LIMIT 0x3fdf0000u
#define MOFEI_SIM_EXTERNAL_RAM_BASE 0x3fd00000u
#define MOFEI_SIM_EXTERNAL_RAM_LIMIT 0x3fe00000u
#define MOFEI_SIM_ICACHE_BASE 0x42000000u
#define MOFEI_SIM_ICACHE_LIMIT (MOFEI_SIM_ICACHE_BASE + MOFEI_SIM_EXTMEM_REGION_SIZE)
#define MOFEI_SIM_FAKE_APP_STACK_TOP 0x3fd88000u
#define MOFEI_SIM_FAKE_LOOP_TASK_STACK_TOP 0x3fd86000u
#define MOFEI_SIM_FAKE_TASK_STACK_BYTES 0x4000u
#define MOFEI_SIM_FAKE_APP_STACK_BASE (MOFEI_SIM_FAKE_APP_STACK_TOP - MOFEI_SIM_FAKE_TASK_STACK_BYTES)
#define MOFEI_SIM_FAKE_LOOP_TASK_STACK_BASE (MOFEI_SIM_FAKE_LOOP_TASK_STACK_TOP - MOFEI_SIM_FAKE_TASK_STACK_BYTES)
#define MOFEI_SIM_ALLOC_HEAP_BASE (MOFEI_SIM_DCACHE_BASE + MOFEI_SIM_DCACHE_FIRMWARE_RESERVE_BYTES)
#define MOFEI_SIM_ALLOC_HEAP_LIMIT MOFEI_SIM_DCACHE_LIMIT
#define MOFEI_SIM_PSRAM_HEAP_BYTES 0x00800000u
#define MOFEI_SIM_PSRAM_HEAP_LIMIT (MOFEI_SIM_ALLOC_HEAP_BASE + MOFEI_SIM_PSRAM_HEAP_BYTES)
#define MOFEI_SIM_INTERNAL_HEAP_DEVICE_BYTES 0x00040000u
#define MOFEI_SIM_INTERNAL_HEAP_BYTES MOFEI_SIM_INTERNAL_HEAP_DEVICE_BYTES
#define MOFEI_SIM_INTERNAL_HEAP_ALIGNMENT 64u
#define MOFEI_SIM_ROM_SAFE_STUB_BASE 0x3fce0000u
#define MOFEI_SIM_ROM_SAFE_STUB_LIMIT 0x3fcf0000u
#define MOFEI_SIM_INTERNAL_HEAP_FALLBACK_BASE MOFEI_SIM_ROM_SAFE_STUB_LIMIT
#define MOFEI_SIM_INTERNAL_HEAP_TEST_LIMIT MOFEI_SIM_FAKE_LOOP_TASK_STACK_BASE

/* ESP32-S3 ROM/ABI functions used when the firmware ELF does not expose symbols. */
#define MOFEI_ESP32S3_ROM_MEMSET_ADDR 0x400011e8u
#define MOFEI_ESP32S3_ROM_MEMCPY_ADDR 0x400011f4u
#define MOFEI_ESP32S3_ROM_STRCMP_ADDR 0x40001230u
#define MOFEI_ESP32S3_ROM_STRLEN_ADDR 0x40001248u
#define MOFEI_ESP32S3_ROM_DIVDI3_STUB_ADDR 0x4000225cu
#define MOFEI_ESP32S3_ROM_MODDI3_STUB_ADDR 0x400023f4u
#define MOFEI_ESP32S3_ROM_UDIVDI3_STUB_ADDR 0x40002544u
#define MOFEI_ESP32S3_ROM_UMODDI3_STUB_ADDR 0x40002574u
#define MOFEI_ESP32S3_ROM_DIVDI3_ADDR 0x400563a0u
#define MOFEI_ESP32S3_ROM_MODDI3_ADDR 0x4005664cu
#define MOFEI_ESP32S3_ROM_UDIVDI3_ADDR 0x400568fcu
#define MOFEI_ESP32S3_ROM_UMODDI3_ADDR 0x40056b6cu

/* GfxRenderer::frameBuffer offset, verified by GfxRenderer::getFrameBuffer disassembly. */
#define MOFEI_GFX_RENDERER_FRAMEBUFFER_PTR_OFFSET 16u

/* Xtensa call-window markers used in stored CALL8 return PCs. */
#define MOFEI_XTENSA_CALL8_RETURN_MARKER 0x80000000u
#define MOFEI_GFX_SIMULATOR_PUBLISH_RETURN_OFFSET 6u
#define MOFEI_SIMULATOR_PUBLISH_RET_OFFSET 6u

typedef struct MofeiSimAddrs {
  /* Core firmware functions */
  uint32_t setup_addr;                                        /* setup() entry point */
  uint32_t setupDisplayAndFonts_addr;                         /* setupDisplayAndFonts() entry point */
  uint32_t setupDisplayAndFonts_retw;                         /* RETW instruction inside setupDisplayAndFonts */
  uint32_t loop_addr;                                         /* loop() entry point */
  uint32_t app_main_addr;                                     /* app_main() entry point */
  uint32_t loopTask_addr;                                     /* loopTask() entry point */
  uint32_t murphySimulatorMain_addr;                          /* Murphy OS simulator synchronous entry point */
  uint32_t murphySimulatorEnterRunLoop_addr;                  /* Murphy OS simulator run-loop marker */
  uint32_t murphyRunDeviceMain_retw;                          /* RETW after DeviceShell::startSimulatorLoop() */
  uint32_t murphyDeviceShellStartSimulatorLoop_addr;          /* DeviceShell::startSimulatorLoop() */
  uint32_t murphyDeviceShellRunSimulatorLoopContext_addr;     /* DeviceShell::runSimulatorLoop(Context) */
  uint32_t murphyDeviceShellRunSimulatorLoopMethod_addr;      /* DeviceShell::runSimulatorLoop() */
  uint32_t murphyDeviceShellRunSimulatorLoopWithContext_addr; /* DeviceShell::runSimulatorLoopWithContext(Context) */
  uint32_t murphyDeviceShellRunSimulatorLoop_addr;            /* DeviceShell::runSimulatorLoop() */
  uint32_t start_cpu0_addr;                                   /* ESP-IDF start_cpu0() entry point */
  uint32_t xTaskCreateUniversal_addr;                         /* xTaskCreateUniversal() entry point */
  uint32_t esp_cache_msync_addr;                              /* esp_cache_msync() entry point */
  uint32_t memset_addr;                                       /* ROM/ELF memset entry point */
  uint32_t memcpy_addr;                                       /* ROM/ELF memcpy entry point */
  uint32_t strcmp_addr;                                       /* ROM strcmp entry point */
  uint32_t strlen_addr;                                       /* ROM strlen entry point */
  uint32_t setjmp_addr;                                       /* libc/ROM setjmp entry point */
  uint32_t strdup_addr;                                       /* libc strdup entry point */
  uint32_t cxa_guard_acquire_addr;                            /* __cxa_guard_acquire entry point */
  uint32_t cxa_guard_release_addr;                            /* __cxa_guard_release entry point */
  uint32_t gfxSimulatorPublishFrameBuffer_addr;               /* GfxRenderer publish wrapper hook */
  uint32_t gfxSimulatorPublishFrameBuffer_return;             /* RETW instruction inside the publish wrapper */
  uint32_t mofeiSimulatorPublishFramebuffer_addr;             /* simulator framebuffer publish hook */
  uint32_t mofeiSimulatorPublishFramebuffer_retw;             /* RETW instruction inside simulator publish hook */
  uint32_t murphySimulatorBootFirstFrame_addr;                /* Murphy OS simulator first-frame entry point */
  uint32_t murphySimulatorPublishFramebuffer_addr;            /* Murphy OS simulator framebuffer publish hook */
  uint32_t murphySimulatorPublishFramebuffer_retw;            /* RETW instruction inside Murphy OS publish hook */
  uint32_t murphySimulatorTraceActivity_addr;                 /* Murphy OS simulator activity trace marker */
  uint32_t murphySimulatorHooksEnabled_addr;                  /* Murphy OS simulator hook enable flag */
  /* ActivityManager symbols */
  uint32_t goToBoot_addr;                    /* ActivityManager::goToBoot() */
  uint32_t goHome_addr;                      /* ActivityManager::goHome() */
  uint32_t activityManager_begin_addr;       /* ActivityManager::begin() */
  uint32_t activityManager_addr;             /* activityManager global (BSS) */
  uint32_t murphyDeviceShell_addr;           /* runMurphyDeviceMain static DeviceShell */
  uint32_t murphyDeviceRunner_addr;          /* runMurphyDeviceMain static AppRunner */
  uint32_t murphyDeviceFramebuffer_addr;     /* runMurphyDeviceMain static Framebuffer */
  uint32_t murphyDeviceDisplay_addr;         /* runMurphyDeviceMain static DisplayEsp */
  uint32_t murphyDeviceSleepInput_addr;      /* runMurphyDeviceMain static SleepHoldInput */
  uint32_t murphyDevicePower_addr;           /* runMurphyDeviceMain static PowerEsp */
  uint32_t murphyDeviceFrontlight_addr;      /* runMurphyDeviceMain static FrontlightEsp */
  uint32_t murphyDeviceStorage_addr;         /* runMurphyDeviceMain static StorageEsp */
  uint32_t murphyDeviceCastStore_addr;       /* runMurphyDeviceMain static CastFrameStore */
  uint32_t murphyDeviceInput_addr;           /* runMurphyDeviceMain static InputEsp */
  uint32_t murphyDeviceMappedInput_addr;     /* runMurphyDeviceMain static PortraitInput */
  uint32_t murphyDeviceDebugInput_addr;      /* runMurphyDeviceMain static SyntheticInput */
  uint32_t murphyDeviceRemoteInput_addr;     /* runMurphyDeviceMain static RemoteInput */
  uint32_t murphyDeviceWindow_addr;          /* runMurphyDeviceMain static Window */
  uint32_t murphyDeviceNowMs_addr;           /* runMurphyDeviceMain nowMs function */
  uint32_t murphyInputEspVtable_addr;        /* vtable for InputEsp */
  uint32_t murphyPortraitInputVtable_addr;   /* vtable for PortraitInput */
  uint32_t murphySleepHoldInputVtable_addr;  /* vtable for SleepHoldInput */
  uint32_t murphyRemoteInputVtable_addr;     /* vtable for RemoteInput */
  uint32_t murphySyntheticInputVtable_addr;  /* vtable for SyntheticInput */
  uint32_t murphySimulatorShellContext_addr; /* Murphy OS simulator shell context */
  uint32_t murphySimulatorRunner_addr;
  uint32_t murphySimulatorFramebufferObject_addr;
  uint32_t murphySimulatorDisplay_addr;
  uint32_t murphySimulatorSleepInput_addr;
  uint32_t murphySimulatorPower_addr;
  uint32_t murphySimulatorFrontlight_addr;
  uint32_t murphySimulatorStorage_addr;
  uint32_t murphySimulatorUiFont_addr;
  uint32_t murphySimulatorBodyFont_addr;
  uint32_t murphySimulatorCastStore_addr;
  uint32_t murphySimulatorRunnerInput_addr;
  uint32_t murphySimulatorRunnerWindow_addr;
  uint32_t murphySimulatorRunnerNowMs_addr;
  /* Display/rendering globals */
  uint32_t display_addr;                           /* display global (BSS, MofeiDisplay object) */
  uint32_t renderer_addr;                          /* renderer global (BSS, GfxRenderer object) */
  uint32_t gfxSimulatorFrameBuffer_addr;           /* simulator static framebuffer hook */
  uint32_t murphySimulatorFramebuffer_addr;        /* Murphy OS exported framebuffer data pointer */
  uint32_t murphySimulatorFramebufferStorage_addr; /* Murphy OS simulator framebuffer storage */
  /* Intercepted SPI/SKIP functions */
  uint32_t spi_beginTransaction_addr;     /* SPIClass::beginTransaction */
  uint32_t spi_endTransaction_addr;       /* SPIClass::endTransaction */
  uint32_t spi_transfer_addr;             /* SPIClass::transfer */
  uint32_t spi_writeBytes_addr;           /* SPIClass::writeBytes */
  uint32_t sendCommand_addr;              /* MofeiDisplay::sendCommand */
  uint32_t sendData_byte_addr;            /* MofeiDisplay::sendData(uint8_t) */
  uint32_t sendData_buf_addr;             /* MofeiDisplay::sendData(const uint8_t*, size_t) */
  uint32_t epdBusWriteCommand_addr;       /* murphyos::EpdBusEsp::writeCommand */
  uint32_t epdBusWriteData_addr;          /* murphyos::EpdBusEsp::writeData */
  uint32_t epdBusWaitBusy_addr;           /* murphyos::EpdBusEsp::waitBusy */
  uint32_t addApbChangeCallback_addr;     /* addApbChangeCallback */
  uint32_t removeApbChangeCallback_addr;  /* removeApbChangeCallback */
  uint32_t getApbFrequency_addr;          /* getApbFrequency */
  uint32_t gpio_config_addr;              /* ESP-IDF gpio_config */
  uint32_t gpio_set_level_addr;           /* ESP-IDF gpio_set_level */
  uint32_t gpio_get_level_addr;           /* ESP-IDF gpio_get_level */
  uint32_t pinMode_addr;                  /* __pinMode / pinMode */
  uint32_t digitalWrite_addr;             /* __digitalWrite / digitalWrite */
  uint32_t digitalRead_addr;              /* __digitalRead / digitalRead */
  uint32_t pvPortMalloc_addr;             /* pvPortMalloc */
  uint32_t heap_caps_malloc_addr;         /* heap_caps_malloc */
  uint32_t heap_caps_malloc_base_addr;    /* heap_caps_malloc_base */
  uint32_t heap_caps_malloc_default_addr; /* heap_caps_malloc_default */
  uint32_t heap_caps_malloc_prefer_addr;  /* heap_caps_malloc_prefer */
  uint32_t malloc_addr;                   /* malloc */
  uint32_t calloc_addr;                   /* calloc */
  uint32_t realloc_addr;                  /* realloc */
  uint32_t malloc_r_addr;                 /* _malloc_r */
  uint32_t calloc_r_addr;                 /* _calloc_r */
  uint32_t realloc_r_addr;                /* _realloc_r */
  uint32_t operator_new_addr;             /* operator new(unsigned int) */
  uint32_t operator_new_array_addr;       /* operator new[](unsigned int) */
  /* ROM/RTOS functions that get RETW-patched */
  uint32_t vTaskStartScheduler_addr;
  uint32_t __assert_func_addr;
  /* Functions intercepted at call-target level (callN target) */
  uint32_t esp_partition_find_addr;
  uint32_t esp_partition_next_addr;
  uint32_t esp_partition_find_first_addr;
  uint32_t esp_partition_verify_addr;
  uint32_t esp_timer_get_time_addr;
  uint32_t systimer_hal_get_counter_value_addr;
  uint32_t esp_ota_get_running_partition_addr;
  uint32_t esp_ota_get_next_update_partition_addr;
  uint32_t xTaskCreatePinnedToCore_addr;
  uint32_t atomic_s32c1i_exchange_1_addr;         /* __atomic_s32c1i_exchange_1 */
  uint32_t atomic_fetch_add_2_addr;               /* __atomic_fetch_add_2 */
  uint32_t atomic_fetch_add_4_addr;               /* __atomic_fetch_add_4 */
  uint32_t atomic_s32c1i_compare_exchange_1_addr; /* __atomic_s32c1i_compare_exchange_1 */
  uint32_t atomic_s32c1i_compare_exchange_4_addr; /* __atomic_s32c1i_compare_exchange_4 */
  uint32_t atomic_compare_exchange_4_addr;        /* __atomic_compare_exchange_4 */
  /* NVS functions (intercepted at callN and RETW level) */
  uint32_t nvs_flash_init_addr;
  uint32_t nvs_flash_init_partition_addr;
  uint32_t nvs_open_addr;
  uint32_t nvs_open_from_partition_addr;
  uint32_t nvs_get_blob_addr;
  uint32_t nvs_get_i8_addr;
  uint32_t nvs_get_u8_addr;
  uint32_t nvs_get_u16_addr;
  uint32_t nvs_get_u32_addr;
  uint32_t nvs_set_blob_addr;
  uint32_t nvs_set_i8_addr;
  uint32_t nvs_set_u8_addr;
  uint32_t nvs_set_u16_addr;
  uint32_t nvs_set_u32_addr;
  uint32_t esp_ota_get_app_partition_count_addr;
  uint32_t esp_ota_begin_addr;
  uint32_t esp_ota_end_addr;
  uint32_t esp_ota_write_addr;
  /* Pre-computed RETW PCs for all firmware functions whose bodies QEMU
   * patched into immediate-return stubs.  These are simulator stubs, not real
   * C++ returns, so translate.c keeps the safe RETW helper for them while
   * leaving ordinary firmware RETW on the native QEMU path. */
#define MOFEI_RETW_PATCH_MAX 512
  uint32_t retw_patch_addrs[MOFEI_RETW_PATCH_MAX];
  int retw_patch_count;
  /* Exact RETW PCs for real firmware wrapper frames that need to bypass
   * QEMU's pre-RETW underflow/illegal checks but must still return through the
   * normal translated-register RETW path.  Do not put QEMU-stubbed functions
   * here; those belong in retw_patch_addrs and use the safe RETW helper. */
#define MOFEI_RETW_NORMAL_PRECHECK_MAX 16
  uint32_t retw_normal_precheck_addrs[MOFEI_RETW_NORMAL_PRECHECK_MAX];
  int retw_normal_precheck_count;
  /* Pre-computed RETW PC addresses for allocator-family functions.  The
   * patched functions all return through function_addr + 3, but their size,
   * old-pointer, and capability parameters live in different Xtensa argument
   * registers. */
#define MOFEI_ALLOC_PATCH_MAX 96
#define MOFEI_ALLOC_SIZE_A2 1
#define MOFEI_ALLOC_SIZE_A3 2
#define MOFEI_ALLOC_SIZE_A4 3
#define MOFEI_ALLOC_SIZE_MUL_A2_A3 4
#define MOFEI_ALLOC_SIZE_MUL_A3_A4 5
#define MOFEI_ALLOC_SIZE_FREE 6
#define MOFEI_ALLOC_ARG_NONE 0
#define MOFEI_ALLOC_ARG_A2 2
#define MOFEI_ALLOC_ARG_A3 3
#define MOFEI_ALLOC_ARG_A4 4
#define MOFEI_ALLOC_ARG_A5 5
#define MOFEI_ALLOC_ARG_DEFAULT_POLICY 6
#define MOFEI_I2C_TRANSFER_TRANSMIT 1
#define MOFEI_I2C_TRANSFER_RECEIVE 2
#define MOFEI_I2C_TRANSFER_TRANSMIT_RECEIVE 3
#define MOFEI_I2C_TRANSFER_MULTI_TRANSMIT 4
  struct {
    uint32_t addr;
    uint8_t size_kind;
    uint8_t old_ptr_arg;
    uint8_t caps_arg;
  } alloc_patch_addrs[MOFEI_ALLOC_PATCH_MAX];
  int alloc_patch_count;
  /* Pre-computed: callN target addresses that return NULL */
#define MOFEI_CALL_NULL_MAX 16
  uint32_t call_null_addrs[MOFEI_CALL_NULL_MAX];
  int call_null_count;
  /* Pre-computed: callN target addresses that return ESP_FAIL (-1) */
#define MOFEI_CALL_FAIL_MAX 24
  uint32_t call_fail_addrs[MOFEI_CALL_FAIL_MAX];
  int call_fail_count;
  /* ADC calibration functions to skip at translate time */
  uint32_t read_cal_channel_addr;
  uint32_t adc_hal_set_controller_addr;
  uint32_t adc_hal_self_calibration_addr;
  uint32_t adc_hal_calibration_init_addr;
  uint32_t adc_hal_set_calibration_param_addr;
  uint32_t esp_efuse_read_field_blob_addr;
  uint32_t esp_efuse_read_field_blob_part_addr;
  /* Pre-computed: callN target addresses that get special handling */
  uint32_t xQueueGenericCreate_addr;
  uint32_t xQueueCreateMutex_addr;
  uint32_t xQueueCreateWithCaps_addr;
  uint32_t xSemaphoreCreateGenericWithCaps_addr;
  uint32_t vPortYield_addr;
  uint32_t i2c_master_transmit_addr;
  uint32_t i2c_master_receive_addr;
  uint32_t i2c_master_transmit_receive_addr;
  uint32_t i2c_master_multi_buffer_transmit_addr;
  uint32_t heap_caps_calloc_addr;
  uint32_t heap_caps_calloc_base_addr;
  uint32_t heap_caps_calloc_prefer_addr;
  uint32_t heap_caps_realloc_addr;
  uint32_t heap_caps_realloc_base_addr;
  uint32_t heap_caps_realloc_default_addr;
  uint32_t heap_caps_realloc_prefer_addr;
  uint32_t heap_caps_get_free_size_addr;
  uint32_t heap_caps_get_largest_free_block_addr;
  uint32_t heap_caps_get_total_size_addr;
  uint32_t heap_caps_get_allocated_size_addr;
  uint32_t esp_get_psram_size_addr;      /* ESP.getPsramSize() */
  uint32_t esp_get_free_psram_addr;      /* ESP.getFreePsram() */
  uint32_t esp_get_max_alloc_psram_addr; /* ESP.getMaxAllocPsram() */
  uint32_t xQueueGenericSend_addr;
  uint32_t xQueueSemaphoreTake_addr;
  uint32_t xQueueGiveMutexRecursive_addr;
  uint32_t xQueueReceive_addr;
  uint32_t xQueueTakeMutexRecursive_addr;
  uint32_t esp_intr_alloc_addr;
  uint32_t esp_intr_alloc_intrstatus_addr;
  uint32_t esp_intr_enable_addr;
  uint32_t esp_intr_disable_addr;
  uint32_t esp_intr_free_addr;
  uint32_t spi_device_polling_transmit_addr;
  uint32_t spi_device_transmit_addr;
  uint32_t sdmmc_host_wait_for_event_addr;
  uint32_t logPrintf_addr;
  uint32_t wrap_log_printf_addr;
  uint32_t log_printf_addr;
  uint32_t log_printfv_addr;
  uint32_t esp_log_addr;
  uint32_t esp_log_va_addr;
  uint32_t esp_log_write_addr;
  uint32_t esp_log_writev_addr;
  uint32_t esp_log_impl_lock_addr;
  uint32_t esp_log_impl_unlock_addr;
  uint32_t __env_lock_addr;
  uint32_t __env_unlock_addr;
  uint32_t pthread_key_create_addr;
  uint32_t pthread_getspecific_addr;
  uint32_t pthread_setspecific_addr;
  uint32_t esp_system_init_mbedtls_psa_crypto_addr; /* __esp_system_init_fn_mbedtls_psa_crypto_init_fn */
  /* Sleep functions to skip in simulator (GPIO not emulated) */
  uint32_t enterDeepSleep_addr;           /* enterDeepSleep() */
  uint32_t enterAutoLightSleep_addr;      /* enterAutoLightSleep() */
  uint32_t mofeiSimulatorTouchRead_addr;  /* simulator firmware touch hook */
  uint32_t mofeiSimulatorButtonRead_addr; /* simulator firmware button hook */
  uint32_t mofeiSimulatorTraceFileBrowserDirectory_addr;
  uint32_t mofeiSimulatorTraceRpipe_addr;
  uint32_t mofeiSimulatorTraceOpdsFetch_addr;
  uint32_t mofeiSimulatorTraceOpdsSearch_addr;
  uint32_t g_mofeiSimButtonBits_addr; /* simulator firmware button bits global */
  uint32_t g_mofeiSimButtonPressedEvents_addr;
  uint32_t g_mofeiSimButtonReleasedEvents_addr;
  uint32_t g_mofeiSimTouchEventPending_addr;
  uint32_t g_mofeiSimTouchEventType_addr;
  uint32_t g_mofeiSimTouchEventX_addr;
  uint32_t g_mofeiSimTouchEventY_addr;
  uint32_t g_mofeiSimConsolePending_addr;
  uint32_t g_mofeiSimConsoleLength_addr;
  uint32_t g_mofeiSimConsoleBuffer_addr;
  uint32_t g_mofeiSimPandaDebugRequestPending_addr;
  uint32_t g_mofeiSimPandaDebugRequestLength_addr;
  uint32_t g_mofeiSimPandaDebugRequestBuffer_addr;
  uint32_t g_mofeiSimPandaDebugResponsePending_addr;
  uint32_t g_mofeiSimPandaDebugResponseLength_addr;
  uint32_t g_mofeiSimPandaDebugResponseBuffer_addr;
  uint32_t mofeiSimulatorPandaDebugMailboxCapacity_addr;
  uint32_t mofeiSimulatorPandaDebugReset_addr;
  uint32_t mofeiSimulatorPandaDebugPump_addr;
  /* Render/activity trace points for simulator diagnosis */
  uint32_t dashboardRender_addr;                 /* DashboardActivity::render(RenderLock&&) */
  uint32_t settingsRender_addr;                  /* SettingsActivity::render(RenderLock&&) */
  uint32_t buttonRemapRender_addr;               /* ButtonRemapActivity::render(RenderLock&&) */
  uint32_t deviceDiagnosticsRender_addr;         /* DeviceDiagnosticsActivity::render(RenderLock&&) */
  uint32_t statusBarSettingsRender_addr;         /* StatusBarSettingsActivity::render(RenderLock&&) */
  uint32_t calendarRender_addr;                  /* CalendarActivity::render(RenderLock&&) */
  uint32_t weatherClockRender_addr;              /* WeatherClockActivity::render(RenderLock&&) */
  uint32_t arcadeHubRender_addr;                 /* ArcadeHubActivity::render(RenderLock&&) */
  uint32_t game2048Render_addr;                  /* Game2048Activity::render(RenderLock&&) */
  uint32_t recentBooksRender_addr;               /* RecentBooksActivity::render(RenderLock&&) */
  uint32_t fileBrowserRender_addr;               /* FileBrowserActivity::render(RenderLock&&) */
  uint32_t appletsRender_addr;                   /* AppletsActivity::render(RenderLock&&) */
  uint32_t luaAppRender_addr;                    /* LuaAppActivity::render(RenderLock&&) */
  uint32_t readingHubRender_addr;                /* ReadingHubActivity::render(RenderLock&&) */
  uint32_t studyHubRender_addr;                  /* StudyHubActivity::render(RenderLock&&) */
  uint32_t studyCardsTodayRender_addr;           /* StudyCardsTodayActivity::render(RenderLock&&) */
  uint32_t opdsServerListRender_addr;            /* OpdsServerListActivity::render(RenderLock&&) */
  uint32_t opdsBookBrowserRender_addr;           /* OpdsBookBrowserActivity::render(RenderLock&&) */
  uint32_t opdsSettingsRender_addr;              /* OpdsSettingsActivity::render(RenderLock&&) */
  uint32_t dictionaryRender_addr;                /* DictionaryActivity::render(RenderLock&&) */
  uint32_t keyboardEntryRender_addr;             /* KeyboardEntryActivity::render(RenderLock&&) */
  uint32_t epubChapterSelectRender_addr;         /* EpubReaderChapterSelectionActivity::render(RenderLock&&) */
  uint32_t epubPercentSelectionRender_addr;      /* EpubReaderPercentSelectionActivity::render(RenderLock&&) */
  uint32_t epubSearchResultsRender_addr;         /* EpubSearchResultsActivity::render(RenderLock&&) */
  uint32_t txtSearchResultsRender_addr;          /* TxtSearchResultsActivity::render(RenderLock&&) */
  uint32_t txtBookmarksRender_addr;              /* TxtBookmarksActivity::render(RenderLock&&) */
  uint32_t epubBookmarksRender_addr;             /* EpubBookmarksActivity::render(RenderLock&&) */
  uint32_t epubReaderFootnotesRender_addr;       /* EpubReaderFootnotesActivity::render(RenderLock&&) */
  uint32_t ttfFontSelectRender_addr;             /* TtfFontSelectActivity::render(RenderLock&&) */
  uint32_t timeZoneSelectRender_addr;            /* TimeZoneSelectActivity::render(RenderLock&&) */
  uint32_t traditionalChineseFontsRender_addr;   /* TraditionalChineseFontsActivity::render(RenderLock&&) */
  uint32_t languageSelectRender_addr;            /* LanguageSelectActivity::render(RenderLock&&) */
  uint32_t sleepWallpaperRender_addr;            /* SleepWallpaperActivity::render(RenderLock&&) */
  uint32_t readerFrontlightSelectionRender_addr; /* ReaderFrontlightSelectionActivity::render(RenderLock&&) */
  uint32_t readerRender_addr;                    /* ReaderActivity::render(RenderLock&&) */
  uint32_t epubReaderRender_addr;                /* EpubReaderActivity::render(RenderLock&&) */
  uint32_t txtReaderRender_addr;                 /* TxtReaderActivity::render(RenderLock&&) */
  uint32_t xtcReaderRender_addr;                 /* XtcReaderActivity::render(RenderLock&&) */
  uint32_t murphyVtableHomeScene_addr;           /* vtable for murphy::app::HomeScene */
  uint32_t murphyVtableSettingsScene_addr;       /* vtable for murphy::app::SettingsScene */
  uint32_t murphyVtableWifiSelectionScene_addr;  /* vtable for murphy::app::WifiSelectionScene */
  uint32_t murphyVtableFileBrowserScene_addr;    /* vtable for murphy::app::FileBrowserScene */
  uint32_t murphyVtableLibraryScene_addr;        /* vtable for murphy::app::LibraryScene */
  uint32_t murphyVtableRecentBooksScene_addr;    /* vtable for murphy::app::RecentBooksScene */
  uint32_t murphyVtableArcadeHubScene_addr;      /* vtable for murphy::app::ArcadeHubScene */
  uint32_t murphyVtableGame2048Scene_addr;       /* vtable for murphy::app::Game2048Scene */
  uint32_t murphyVtableWeatherScene_addr;        /* vtable for Murphy WeatherScene */
  uint32_t murphyVtableCalendarScene_addr;       /* vtable for Murphy CalendarScene */
  uint32_t murphyVtableStudyHubScene_addr;       /* vtable for murphy::app::study::StudyHubScene */
  uint32_t murphyVtableCardsTodayScene_addr;     /* vtable for Murphy CardsTodayScene */
  uint32_t murphyVtableStudyQueueScene_addr;
  uint32_t murphyVtableStudyReviewQueueScene_addr;
  uint32_t murphyVtableStudyImportStatusScene_addr;
  uint32_t murphyVtableStudyQuizScene_addr;
  uint32_t murphyVtableStudyReportScene_addr;
  uint32_t murphyVtableStudyRecoveryScene_addr;
  uint32_t murphyVtableKeyboardScene_addr;      /* vtable for murphy::ui::KeyboardScene */
  uint32_t murphyVtableConfirmationScene_addr;  /* vtable for panda::ui::ConfirmationScene */
  uint32_t murphyVtableReaderScene_addr;        /* vtable for murphy::app::ReaderScene */
  uint32_t murphyVtableSleepScene_addr;         /* vtable for murphy::app::SleepScene */
  uint32_t murphyVtablePandaHubScene_addr;      /* vtable for murphy::app::PandaHubScene */
  uint32_t murphyVtablePandaLuaAppScene_addr;   /* vtable for murphy::app::PandaLuaAppScene */
  uint32_t murphyVtableButtonRemapScene_addr;   /* vtable for murphy::app::ButtonRemapScene */
  uint32_t murphyVtableDiagnosticsScene_addr;   /* vtable for murphy::app::DiagnosticsScene */
  uint32_t murphyVtableFontPickerScene_addr;    /* vtable for murphy::app::FontPickerScene */
  uint32_t murphyVtableEnumEditorScene_addr;    /* vtable for murphy::app::EnumEditorScene */
  uint32_t murphyVtableToggleGroupScene_addr;   /* vtable for murphy::app::ToggleGroupScene */
  uint32_t murphyVtableSearchResultsScene_addr; /* vtable for murphy::app::SearchResultsScene */
  uint32_t murphyVtableBookmarkListScene_addr;  /* vtable for murphy::app::BookmarkListScene */
  uint32_t murphyVtableChapterListScene_addr;   /* vtable for murphy::app::ChapterListScene */
  uint32_t murphyVtablePercentJumpScene_addr;   /* vtable for murphy::app::PercentJumpScene */
  uint32_t murphyVtableDictionaryScene_addr;    /* vtable for murphy::app::ReaderDictionaryResultScene */
  uint32_t murphyVtableSudokuScene_addr;        /* vtable for murphy::app::SudokuScene */
  uint32_t murphyVtableVirtualPetScene_addr;    /* vtable for murphy::app::VirtualPetScene */
  uint32_t fillRect_addr;                       /* GfxRenderer::fillRect(int,int,int,int,bool) const */
  uint32_t displayBuffer_addr;                  /* GfxRenderer::displayBuffer(HalDisplay::RefreshMode) const */
  uint32_t renderActivitySync_addr;             /* renderActivitySynchronously(Activity*) */
  uint32_t drawLine_addr;                       /* GfxRenderer::drawLine(int,int,int,int,bool) const */
  uint32_t fillPhysicalRect_addr;               /* fillPhysicalRect(uint8_t*,int,int,int,int,bool) */
  /* EPUB open pipeline trace points for simulator E2E diagnosis */
  uint32_t epubLoad_addr;                 /* Epub::load(bool,bool) */
  uint32_t epubSetupCacheDir_addr;        /* Epub::setupCacheDir() const */
  uint32_t zipFileOpen_addr;              /* ZipFile::open() */
  uint32_t bmcBeginWrite_addr;            /* BookMetadataCache::beginWrite() */
  uint32_t bmcBeginContentOpfPass_addr;   /* BookMetadataCache::beginContentOpfPass() */
  uint32_t epubParseContentOpf_addr;      /* Epub::parseContentOpf(...) */
  uint32_t epubFindContentOpfFile_addr;   /* Epub::findContentOpfFile(...) const */
  uint32_t epubReadItemStream_addr;       /* Epub::readItemContentsToStream(...) const */
  uint32_t epubReadItemUtf8Stream_addr;   /* Epub::readItemContentsToUtf8Stream(...) const */
  uint32_t zipReadFileToStream_addr;      /* ZipFile::readFileToStream(...) */
  uint32_t contentOpfWrite_addr;          /* ContentOpfParser::write(uint8_t const*, size_t) */
  uint32_t contentOpfFlush_addr;          /* ContentOpfParser::flush() */
  uint32_t bmcCreateSpineEntry_addr;      /* BookMetadataCache::createSpineEntry(...) */
  uint32_t bmcEndContentOpfPass_addr;     /* BookMetadataCache::endContentOpfPass() */
  uint32_t expatPsramRealloc_addr;        /* epub::expat_psram::xmlRealloc(...) */
  uint32_t bmcBeginTocPass_addr;          /* BookMetadataCache::beginTocPass() */
  uint32_t bmcEndWrite_addr;              /* BookMetadataCache::endWrite() */
  uint32_t bmcBuildBookBin_addr;          /* BookMetadataCache::buildBookBin(...) */
  uint32_t bmcCleanupBuildArtifacts_addr; /* BookMetadataCache::cleanupBuildArtifacts() const */
  /* ESP-IDF startup flags used to unblock CPU1 handshakes in the single-core simulator. */
  uint32_t s_cpu_up_1_addr;
  uint32_t s_cpu_inited_1_addr;
  uint32_t s_system_inited_1_addr;
  uint32_t s_system_full_inited_addr;
  uint32_t heap_start_addr; /* linker _heap_start for simulator-safe internal heap base */
  bool resolved;            /* true after successful symbol resolution */
} MofeiSimAddrs;

extern MofeiSimAddrs mofei_sim_addrs;

#endif /* MOFEI_SIM_ADDRS_H */
