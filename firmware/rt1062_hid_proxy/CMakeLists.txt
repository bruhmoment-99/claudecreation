# i.MX RT1062 USB HID proxy - firmware image.
#
#   tools/fetch_sdk.sh                       # once: MCUXpresso SDK 2.16 -> ./sdk
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#   cmake --build build
#
# Produces build/hid_proxy.elf (debug), .bin (raw image for 0x60000000) and
# .hex, plus a .map file. The image boots by XIP from FlexSPI NOR: the boot
# header (FCB + IVT) is at the start of the image, so flash the .bin at the
# start of the NOR (0x60000000), or the .hex/.elf at their own addresses.
#
# Options
#   -DMCUX_SDK_DIR=<path>        SDK root laid out as tools/fetch_sdk.sh does
#   -DCMAKE_BUILD_TYPE=Debug     -Og -g3 (default: Release, -O2 -g)
#   -DHID_PROXY_ITCM=ON          put the real-time USB path in ITCM (APP_HOT)
#   -DBOARD_FLASH_SIZE=0x1000000 QSPI NOR size in bytes (default 8 MB)
cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/arm-none-eabi.cmake" CACHE FILEPATH "")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

project(hid_proxy C ASM)

# Optimisation is set per configuration below; drop CMake's defaults
# (Release would otherwise add -O3).
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_C_FLAGS_${cfg} "")
    set(CMAKE_ASM_FLAGS_${cfg} "")
endforeach()

set(MCUX_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/sdk" CACHE PATH "MCUXpresso SDK root (see tools/fetch_sdk.sh)")
option(HID_PROXY_ITCM "Run the latency-critical USB path from ITCM" OFF)
set(BOARD_FLASH_SIZE "0x800000" CACHE STRING "QSPI NOR size in bytes")

set(SDK     "${MCUX_SDK_DIR}/core")
set(USB     "${MCUX_SDK_DIR}/middleware/usb")
set(RTOS    "${MCUX_SDK_DIR}/rtos/freertos-kernel")
set(CMSIS   "${MCUX_SDK_DIR}/CMSIS")
set(DEVICE  "${SDK}/devices/MIMXRT1062")

if(NOT EXISTS "${DEVICE}/MIMXRT1062.h")
    message(FATAL_ERROR
        "MCUXpresso SDK not found in ${MCUX_SDK_DIR}.\n"
        "Run tools/fetch_sdk.sh, or pass -DMCUX_SDK_DIR=<path>.")
endif()

# --------------------------------------------------------------------------
# Sources
# --------------------------------------------------------------------------
set(APP_SOURCES
    source/main.c
    source/board_init.c
    source/hid_merge.c
    source/hid_report_parser.c
    source/smooth_move.c
    source/uart_parser.c
    source/usb_device.c
    source/usb_host.c
    source/app_status.c
    source/status_led.c
)

set(BOARD_SOURCES
    board/board.c
    board/clock_config.c
    board/pin_mux.c
    board/flexspi_nor_config.c
)

set(SDK_SOURCES
    ${DEVICE}/gcc/startup_MIMXRT1062.S
    ${DEVICE}/system_MIMXRT1062.c
    ${DEVICE}/xip/fsl_flexspi_nor_boot.c
    ${DEVICE}/drivers/fsl_clock.c
    ${SDK}/drivers/common/fsl_common.c
    ${SDK}/drivers/common/fsl_common_arm.c
    ${SDK}/drivers/igpio/fsl_gpio.c
    ${SDK}/drivers/lpuart/fsl_lpuart.c
    ${SDK}/drivers/edma/fsl_edma.c
    ${SDK}/drivers/dmamux/fsl_dmamux.c
    ${SDK}/drivers/pit/fsl_pit.c
    ${SDK}/drivers/cache/armv7-m7/fsl_cache.c
    ${SDK}/components/osa/fsl_os_abstraction_free_rtos.c
    ${SDK}/components/lists/fsl_component_generic_list.c
)

set(USB_SOURCES
    ${USB}/phy/usb_phy.c
    # device (USB1 -> PC)
    ${USB}/device/usb_device_dci.c
    ${USB}/device/usb_device_ehci.c
    ${USB}/output/source/device/usb_device_ch9.c
    ${USB}/output/source/device/class/usb_device_class.c
    ${USB}/output/source/device/class/usb_device_hid.c
    # host (USB2 <- mouse)
    ${USB}/host/usb_host_hci.c
    ${USB}/host/usb_host_devices.c
    ${USB}/host/usb_host_framework.c
    ${USB}/host/usb_host_ehci.c
    ${USB}/host/class/usb_host_hid.c
)

set(RTOS_SOURCES
    ${RTOS}/tasks.c
    ${RTOS}/queue.c
    ${RTOS}/list.c
    ${RTOS}/timers.c
    ${RTOS}/event_groups.c
    ${RTOS}/stream_buffer.c
    ${RTOS}/portable/GCC/ARM_CM4F/port.c
    ${RTOS}/portable/MemMang/heap_4.c
)

add_executable(hid_proxy.elf ${APP_SOURCES} ${BOARD_SOURCES} ${SDK_SOURCES} ${USB_SOURCES} ${RTOS_SOURCES})

target_include_directories(hid_proxy.elf PRIVATE
    source
    config
    board
)
# Vendor headers as SYSTEM so -Wconversion -Werror only applies to our code.
target_include_directories(hid_proxy.elf SYSTEM PRIVATE
    ${CMSIS}/Core/Include
    ${DEVICE}
    ${DEVICE}/drivers
    ${DEVICE}/xip
    ${SDK}/drivers/common
    ${SDK}/drivers/igpio
    ${SDK}/drivers/lpuart
    ${SDK}/drivers/edma
    ${SDK}/drivers/dmamux
    ${SDK}/drivers/pit
    ${SDK}/drivers/cache/armv7-m7
    ${SDK}/components/osa
    ${SDK}/components/lists
    ${USB}/include
    ${USB}/phy
    ${USB}/device
    ${USB}/host
    ${USB}/host/class
    ${USB}/output/source/device
    ${USB}/output/source/device/class
    ${RTOS}/include
    ${RTOS}/portable/GCC/ARM_CM4F
)

target_compile_definitions(hid_proxy.elf PRIVATE
    CPU_MIMXRT1062DVJ6B
    MCUXPRESSO_SDK
    SDK_OS_FREE_RTOS
    USE_RTOS=1
    FSL_RTOS_FREE_RTOS
    # Boot from FlexSPI NOR (XIP) with FCB + IVT, no DCD (no SDRAM used).
    XIP_EXTERNAL_FLASH=1
    XIP_BOOT_HEADER_ENABLE=1
    XIP_BOOT_HEADER_DCD_ENABLE=0
    BOARD_FLASH_SIZE=${BOARD_FLASH_SIZE}
    # startup_MIMXRT1062.S: zero .bss, copy CodeQuickAccess to ITCM and
    # initialise the NonCacheable sections.
    __STARTUP_CLEAR_BSS
    __STARTUP_INITIALIZE_RAMFUNCTION
    __STARTUP_INITIALIZE_NONCACHEDATA
    # Reset_Handler calls main() directly (no newlib crt0; see -nostartfiles).
    __START=main
    # No debug console: LPUART3 is the command channel. Silences usb_echo().
    SDK_DEBUGCONSOLE=2
    # The watchdog stays disabled at boot for now (SystemInit).
    DISABLE_WDOG=1
)
if(HID_PROXY_ITCM)
    target_compile_definitions(hid_proxy.elf PRIVATE
        "APP_HOT=__attribute__((section(\"CodeQuickAccess\"), noinline))")
endif()

set(CPU_FLAGS -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)

target_compile_options(hid_proxy.elf PRIVATE
    ${CPU_FLAGS}
    -ffunction-sections -fdata-sections -fno-common -ffreestanding -fno-builtin
    -fstack-usage
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
    $<$<CONFIG:Debug>:-Og -g3 -DDEBUG>
    $<$<CONFIG:Release>:-O2 -g -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g3>
    $<$<CONFIG:MinSizeRel>:-Os -g -DNDEBUG>
)

# Warnings: strict for our code, quiet for vendor code we do not maintain.
set_source_files_properties(${APP_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Werror")
set_source_files_properties(${BOARD_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall")

set(LINKER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/linker/MIMXRT1062xxxxx_flexspi_nor.ld")
set_target_properties(hid_proxy.elf PROPERTIES LINK_DEPENDS "${LINKER_SCRIPT}")

target_link_options(hid_proxy.elf PRIVATE
    ${CPU_FLAGS}
    -T${LINKER_SCRIPT}
    --specs=nano.specs --specs=nosys.specs
    # startup_MIMXRT1062.S already copies .data, zeroes .bss and calls main();
    # crt0 would only add exit()/stdio code this C-only firmware never uses.
    -nostartfiles
    -Wl,--gc-sections
    -Wl,--print-memory-usage
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/hid_proxy.map
    -Wl,--cref
    # Main stack (MSP): used by startup code and by all ISRs once the
    # scheduler runs. FreeRTOS task stacks come from configTOTAL_HEAP_SIZE.
    -Wl,--defsym=__stack_size__=0x1000
    -Wl,--defsym=__heap_size__=0x400
    -Wl,-z,max-page-size=0x1000
)
target_link_libraries(hid_proxy.elf PRIVATE -Wl,--start-group m c gcc nosys -Wl,--end-group)

add_custom_command(TARGET hid_proxy.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:hid_proxy.elf> ${CMAKE_CURRENT_BINARY_DIR}/hid_proxy.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:hid_proxy.elf> ${CMAKE_CURRENT_BINARY_DIR}/hid_proxy.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:hid_proxy.elf>
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/hid_proxy.bin ${CMAKE_CURRENT_BINARY_DIR}/hid_proxy.hex
    COMMENT "Creating hid_proxy.bin / hid_proxy.hex"
)
