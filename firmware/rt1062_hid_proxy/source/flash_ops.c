/*
 * flash_ops.c - see flash_ops.h.
 */
#include "flash_ops.h"
#include "app_config.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "fsl_common.h"
#include "fsl_cache.h"
#include "fsl_romapi.h"

#define FLEXSPI_INSTANCE 0U /* FlexSPI (the boot flash) */

/* The ROM's FlexSPI NOR API table (layout from fsl_romapi.c). It is used
 * directly by the install routine, which must not call anything in flash. */
typedef struct
{
    uint32_t version;
    status_t (*init)(uint32_t instance, flexspi_nor_config_t *config);
    status_t (*program)(uint32_t instance, flexspi_nor_config_t *config, uint32_t dst_addr, const uint32_t *src);
    status_t (*erase_all)(uint32_t instance, flexspi_nor_config_t *config);
    status_t (*erase)(uint32_t instance, flexspi_nor_config_t *config, uint32_t start, uint32_t lengthInBytes);
    status_t (*read)(uint32_t instance, flexspi_nor_config_t *config, uint32_t *dst, uint32_t addr,
                     uint32_t lengthInBytes);
    void (*clear_cache)(uint32_t instance);
} rom_nor_api_t;

typedef struct
{
    const uint32_t version;
    const char *copyright;
    void (*runBootloader)(void *arg);
    const uint32_t *reserved0;
    const rom_nor_api_t *flexSpiNorDriver;
} rom_api_tree_t;

#define ROM_API_TREE ((const rom_api_tree_t *)*(const uint32_t *)0x0020001cU)

static flexspi_nor_config_t s_norConfig;
static SemaphoreHandle_t s_lock;
static bool s_ready;

/* Page buffer for the ROM (word aligned). Also the copy buffer of install. */
static uint32_t s_page[APP_FLASH_PAGE_SIZE / 4U];
static uint32_t s_sector[APP_FLASH_SECTOR_SIZE / 4U];

/* ---- ROM call bracket --------------------------------------------------- */

static uint32_t rom_enter(void)
{
    uint32_t primask = DisableGlobalIRQ();
    /* NXP's ROM API examples turn the I-cache off so branch prediction
     * cannot prefetch from the flash while it is busy. */
    SCB_DisableICache();
    return primask;
}

static void rom_exit(uint32_t primask, uint32_t off, uint32_t len)
{
    ROM_API_TREE->flexSpiNorDriver->clear_cache(FLEXSPI_INSTANCE);
    SCB_EnableICache();
    /* Drop stale cached copies of what changed. */
    if (len != 0U)
    {
        DCACHE_InvalidateByRange(APP_FLASH_BASE + off, len);
    }
    EnableGlobalIRQ(primask);
}

static void lock(void)
{
    if ((s_lock != NULL) && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING))
    {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if ((s_lock != NULL) && (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING))
    {
        (void)xSemaphoreGive(s_lock);
    }
}

bool flash_ops_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }
    /* The FCB at the start of the flash is exactly what the ROM booted
     * with, including the erase and program command sequences. */
    memcpy(&s_norConfig, (const void *)APP_FLASH_BASE, sizeof(s_norConfig));

    lock();
    uint32_t m = rom_enter();
    status_t st = ROM_API_TREE->flexSpiNorDriver->init(FLEXSPI_INSTANCE, &s_norConfig);
    rom_exit(m, 0U, 0U);
    unlock();

    s_ready = (st == kStatus_Success) && (s_norConfig.pageSize == APP_FLASH_PAGE_SIZE) &&
              (s_norConfig.sectorSize == APP_FLASH_SECTOR_SIZE);
    return s_ready;
}

/* ---- flash_if_t ------------------------------------------------------------ */

static bool op_read(uint32_t off, void *dst, uint32_t len)
{
    if (!s_ready)
    {
        return false;
    }
    lock();
    DCACHE_InvalidateByRange(APP_FLASH_BASE + off, len);
    memcpy(dst, (const void *)(APP_FLASH_BASE + off), len);
    unlock();
    return true;
}

static bool op_erase(uint32_t off)
{
    if (!s_ready || ((off % APP_FLASH_SECTOR_SIZE) != 0U) || (off < APP_SLOT_SIZE))
    {
        return false; /* never erase the running image through this path */
    }
    lock();
    uint32_t m  = rom_enter();
    status_t st = ROM_API_TREE->flexSpiNorDriver->erase(FLEXSPI_INSTANCE, &s_norConfig, off, APP_FLASH_SECTOR_SIZE);
    rom_exit(m, off, APP_FLASH_SECTOR_SIZE);
    unlock();
    return st == kStatus_Success;
}

static bool op_program(uint32_t off, const void *src, uint32_t len)
{
    uint32_t page = off & ~(APP_FLASH_PAGE_SIZE - 1U);
    if (!s_ready || (len == 0U) || (off < APP_SLOT_SIZE) || ((off + len) > (page + APP_FLASH_PAGE_SIZE)))
    {
        return false;
    }
    lock();
    /* Bytes outside [off, off+len) stay 0xFF, which leaves them unchanged. */
    memset(s_page, 0xFF, sizeof(s_page));
    memcpy((uint8_t *)s_page + (off - page), src, len);
    uint32_t m  = rom_enter();
    status_t st = ROM_API_TREE->flexSpiNorDriver->program(FLEXSPI_INSTANCE, &s_norConfig, page, s_page);
    rom_exit(m, page, APP_FLASH_PAGE_SIZE);
    unlock();
    return st == kStatus_Success;
}

static const flash_if_t s_if = {op_read, op_erase, op_program};

const flash_if_t *flash_ops_if(void)
{
    return &s_if;
}

/* ---- install ---------------------------------------------------------------- */

/*
 * Runs entirely from ITCM: the image it overwrites is the one the rest of
 * the firmware executes from. No calls into flash (only the ROM table and
 * inlined CMSIS helpers), and loop-to-memcpy conversion is disabled so the
 * compiler cannot insert one.
 */
__attribute__((section("CodeQuickAccess"), noinline, noreturn, optimize("no-tree-loop-distribute-patterns")))
void flash_ops_install(uint32_t size)
{
    const rom_nor_api_t *rom = ROM_API_TREE->flexSpiNorDriver;

    __disable_irq();
    SCB_DisableICache();

    for (uint32_t off = 0U; off < size; off += APP_FLASH_SECTOR_SIZE)
    {
        /* Keep the watchdog quiet: one sector takes ~50 ms. */
        WDOG1->WSR = 0x5555U;
        WDOG1->WSR = 0xAAAAU;

        /* Staging -> RAM (the staging area is not modified, so the cached
         * XIP view of it is valid). */
        const volatile uint32_t *src = (const volatile uint32_t *)(APP_FLASH_BASE + APP_STAGING_OFFSET + off);
        for (uint32_t i = 0U; i < (APP_FLASH_SECTOR_SIZE / 4U); i++)
        {
            s_sector[i] = src[i];
        }

        (void)rom->erase(FLEXSPI_INSTANCE, &s_norConfig, off, APP_FLASH_SECTOR_SIZE);
        for (uint32_t p = 0U; p < APP_FLASH_SECTOR_SIZE; p += APP_FLASH_PAGE_SIZE)
        {
            (void)rom->program(FLEXSPI_INSTANCE, &s_norConfig, off + p, &s_sector[p / 4U]);
        }
    }
    rom->clear_cache(FLEXSPI_INSTANCE);

    /* Full chip reset through WDOG1 (WCR.SRS = 0): the ROM boots the new
     * image from scratch, peripherals included. */
    WDOG1->WCR = (uint16_t)(WDOG1->WCR & ~WDOG_WCR_SRS_MASK);
    for (;;)
    {
    }
}
