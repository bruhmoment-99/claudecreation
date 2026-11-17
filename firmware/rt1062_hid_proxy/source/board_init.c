/*
 * board_init.c - Board bring-up for the MIMXRT1062DVJ6B HID proxy.
 *
 * Pin assignments below are EXAMPLES - change them to match your schematic.
 * If you use the MCUXpresso Config Tools, generate pin_mux.c/clock_config.c
 * instead and keep only board_vbus_*() and the UART clock-root lines here.
 *
 *   Function            Pad             ALT   Signal
 *   ------------------  --------------  ----  ---------------------
 *   CH343P RXD  <-      GPIO_AD_B1_06   ALT2  LPUART3_TX
 *   CH343P TXD  ->      GPIO_AD_B1_07   ALT2  LPUART3_RX
 *   AP2553 EN           GPIO_AD_B1_10   ALT5  GPIO1_IO26 (output)
 *   AP2553 FLG          GPIO_AD_B1_11   ALT5  GPIO1_IO27 (input, pull-up)
 *   Status LED          GPIO_AD_B0_09   ALT5  GPIO1_IO09 (output; the user
 *                                             LED on the i.MX RT1060 EVK,
 *                                             active low there)
 *   USB1 D+/D-/VBUS     dedicated USB_OTG1 pins (no mux needed)
 *   USB2 D+/D-          dedicated USB_OTG2 pins (no mux needed)
 */
#include "board_init.h"

#include "board.h"        /* BOARD_ConfigMPU(), BOARD_XTAL0_CLK_HZ          */
#include "clock_config.h" /* BOARD_InitBootClocks() -> 600 MHz core          */
#include "pin_mux.h"      /* BOARD_InitBootPins() (Config Tools output)      */
#include "fsl_clock.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"

#define VBUS_EN_GPIO   GPIO1
#define VBUS_EN_PIN    26U
#define VBUS_FLG_GPIO  GPIO1
#define VBUS_FLG_PIN   27U
#define LED_GPIO       GPIO1
#define LED_PIN        9U
/* 1 if the LED lights when the pin is high, 0 if it lights when low. */
#ifndef BOARD_LED_ACTIVE_HIGH
#define BOARD_LED_ACTIVE_HIGH 0
#endif

/* Pad control helpers (IOMUXC_SW_PAD_CTL_PAD_*). */
#define PAD_UART_FAST                                                                     \
    (IOMUXC_SW_PAD_CTL_PAD_SRE(1U) |   /* fast slew - 6 Mbaud edges            */          \
     IOMUXC_SW_PAD_CTL_PAD_DSE(6U) |   /* R0/6 drive strength                  */          \
     IOMUXC_SW_PAD_CTL_PAD_SPEED(3U) | /* 200 MHz                              */          \
     IOMUXC_SW_PAD_CTL_PAD_PKE(1U) | IOMUXC_SW_PAD_CTL_PAD_PUE(1U) |                       \
     IOMUXC_SW_PAD_CTL_PAD_PUS(2U))    /* 100k pull-up (idle-high UART line)   */

#define PAD_GPIO_OUT_PULLDOWN                                                             \
    (IOMUXC_SW_PAD_CTL_PAD_DSE(6U) | IOMUXC_SW_PAD_CTL_PAD_SPEED(1U) |                     \
     IOMUXC_SW_PAD_CTL_PAD_PKE(1U) | IOMUXC_SW_PAD_CTL_PAD_PUE(1U) |                       \
     IOMUXC_SW_PAD_CTL_PAD_PUS(0U))    /* 100k pull-down keeps EN low          */

#define PAD_GPIO_IN_PULLUP                                                                \
    (IOMUXC_SW_PAD_CTL_PAD_HYS(1U) | IOMUXC_SW_PAD_CTL_PAD_PKE(1U) |                       \
     IOMUXC_SW_PAD_CTL_PAD_PUE(1U) | IOMUXC_SW_PAD_CTL_PAD_PUS(2U))

#define PAD_LED (IOMUXC_SW_PAD_CTL_PAD_DSE(6U) | IOMUXC_SW_PAD_CTL_PAD_SPEED(1U))

static void board_init_uart_clock(void)
{
    /*
     * UART_CLK_ROOT = OSC (24 MHz) / 1. 24 MHz / (OSR 4 * SBR 1) = 6.000 Mbaud
     * exactly. This root is shared by all LPUARTs; the SDK debug console (if
     * used on LPUART1) recomputes its divider from the new frequency.
     * Mux values (CSCDR1[UART_CLK_SEL]): 0 = PLL3/6 (80 MHz), 1 = OSC.
     */
    CLOCK_SetMux(kCLOCK_UartMux, 1U);
    CLOCK_SetDiv(kCLOCK_UartDiv, 0U);
}

uint32_t board_uart_clock_hz(void)
{
    uint32_t div = CLOCK_GetDiv(kCLOCK_UartDiv) + 1U;
    if (CLOCK_GetMux(kCLOCK_UartMux) == 0U)
    {
        return (CLOCK_GetPllFreq(kCLOCK_PllUsb1) / 6U) / div;
    }
    return CLOCK_GetOscFreq() / div;
}

static void board_init_pins(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);

    /* LPUART3 <-> CH343P */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_06_LPUART3_TX, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_07_LPUART3_RX, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_06_LPUART3_TX, PAD_UART_FAST);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_07_LPUART3_RX, PAD_UART_FAST);

    /* AP2553 EN / FLG. Configure the pad pull-down BEFORE switching the mux to
     * GPIO so EN never floats high. Fit an external 100k pull-down on EN too:
     * the pad is high-Z during reset and boot ROM execution. */
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_10_GPIO1_IO26, PAD_GPIO_OUT_PULLDOWN);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_10_GPIO1_IO26, 0U);

    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_11_GPIO1_IO27, PAD_GPIO_IN_PULLUP);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_11_GPIO1_IO27, 0U);

    /* Status LED */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_09_GPIO1_IO09, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_09_GPIO1_IO09, PAD_LED);
}

static void board_init_vbus_gpio(void)
{
    /* GPIO_PinInit() writes DR (= 0) before setting GDIR, so no glitch. */
    gpio_pin_config_t out = {kGPIO_DigitalOutput, 0U, kGPIO_NoIntmode};
    GPIO_PinInit(VBUS_EN_GPIO, VBUS_EN_PIN, &out);

    gpio_pin_config_t in = {kGPIO_DigitalInput, 0U, kGPIO_NoIntmode};
    GPIO_PinInit(VBUS_FLG_GPIO, VBUS_FLG_PIN, &in);

    /* LED lit from reset until the status task takes over: shows that the
     * image booted and reached board_init(). */
    gpio_pin_config_t led = {kGPIO_DigitalOutput, BOARD_LED_ACTIVE_HIGH ? 1U : 0U, kGPIO_NoIntmode};
    GPIO_PinInit(LED_GPIO, LED_PIN, &led);
}

void board_init(void)
{
    /*
     * BOARD_ConfigMPU() (SDK board.c) sets up cache attributes, including the
     * "NonCacheable" region used by AT_NONCACHEABLE_SECTION* and by the USB
     * stack's USB_DMA_*_DATA macros. Both USB controllers and the UART eDMA
     * write into those buffers, so this must run first.
     */
    BOARD_ConfigMPU();
    BOARD_InitBootPins();
    BOARD_InitBootClocks(); /* ARM core 600 MHz, PLL3 (USB1 PLL) enabled */

    board_init_uart_clock();
    board_init_pins();
    board_init_vbus_gpio(); /* VBUS to USB-A port OFF until host init; LED on */
}

void board_vbus_enable(bool on)
{
    GPIO_PinWrite(VBUS_EN_GPIO, VBUS_EN_PIN, on ? 1U : 0U);
}

bool board_vbus_fault(void)
{
    return GPIO_PinRead(VBUS_FLG_GPIO, VBUS_FLG_PIN) == 0U;
}

void board_led_set(bool on)
{
    GPIO_PinWrite(LED_GPIO, LED_PIN, (on == (BOARD_LED_ACTIVE_HIGH != 0)) ? 1U : 0U);
}
