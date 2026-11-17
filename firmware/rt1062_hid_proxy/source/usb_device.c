/*
 * usb_device.c - USB1 HID mouse (device role) on the i.MX RT1062.
 *
 * Controller : USB_OTG1 -> kUSB_ControllerEhci0, IRQ USB_OTG1_IRQn
 * Speed      : High-Speed (480 Mbit/s), Full-Speed fallback
 * Endpoint   : EP1 interrupt IN, wMaxPacketSize 8, bInterval 1
 *
 * 8 kHz polling
 * -------------
 * For HS interrupt endpoints the polling period is 2^(bInterval-1) micro-
 * frames. bInterval = 1 -> every microframe -> 125 us -> 8000 polls/s.
 *
 * The EHCI device controller answers the PC's IN token *in hardware* from a
 * primed dTD; if nothing is primed it NAKs, which for HID means "no change"
 * and is exactly what a real mouse does. The CPU's job is only to re-prime
 * after each completion: USB_OTG1 ISR -> USB_DeviceEhciIsrFunction() ->
 * HID class -> USB_DeviceHidMouseCallback(kUSB_DeviceHidEventSendResponse) ->
 * hid_merge_on_tx_complete() -> USB_DeviceHidSend(). On a 600 MHz M7 this
 * path is a few microseconds, well inside the 125 us budget, provided:
 *   - USB_OTG1 has the highest FreeRTOS-safe NVIC priority (app_config.h),
 *   - no code masks interrupts for long (hid_merge locks are < 1 us),
 *   - optionally the ISR path runs from ITCM (see APP_HOT in app_config.h).
 *
 * Required usb_device_config.h settings (copy the SDK template and set):
 *   USB_DEVICE_CONFIG_EHCI            1U
 *   USB_DEVICE_CONFIG_HID             1U
 *   USB_DEVICE_CONFIG_ENDPOINTS       4U
 *   USB_DEVICE_CONFIG_USE_TASK        0U   (callbacks run in the ISR)
 *   USB_DEVICE_CONFIG_MAX_MESSAGES    8U
 *   USB_DEVICE_CONFIG_SELF_POWER      0U
 *   USB_DEVICE_CONFIG_REMOTE_WAKEUP   0U
 */
#include "usb_device_app.h"
#include "app_config.h"
#include "app_status.h"
#include "status_led.h"

#include <string.h>

#include "fsl_device_registers.h"
#include "board.h" /* BOARD_XTAL0_CLK_HZ, BOARD_USB_PHY_* calibration values */
#include "usb_device_config.h"
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"
#include "usb_device_hid.h"
#include "usb_device_ch9.h"
#include "usb_phy.h"

_Static_assert((USB_SPEED_FULL == APP_USB_SPEED_FULL) && (USB_SPEED_LOW == APP_USB_SPEED_LOW) &&
                   (USB_SPEED_HIGH == APP_USB_SPEED_HIGH),
               "app_status speed codes must match the SDK");

#define CONTROLLER_ID       kUSB_ControllerEhci0
#define USB_DEVICE_IRQn     USB_OTG1_IRQn

#ifndef BOARD_USB_PHY_D_CAL
#define BOARD_USB_PHY_D_CAL     (0x0CU)
#define BOARD_USB_PHY_TXCAL45DP (0x06U)
#define BOARD_USB_PHY_TXCAL45DM (0x06U)
#endif

/* ========================================================================= */
/* Descriptors                                                               */
/* ========================================================================= */

#define USB_HID_MOUSE_INTERFACE_INDEX  0U
#define USB_HID_MOUSE_CONFIGURE_INDEX  1U
#define USB_HID_MOUSE_INTERFACE_COUNT  1U
#define USB_HID_MOUSE_ENDPOINT_COUNT   1U
#define USB_DEVICE_CONFIGURATION_COUNT 1U
#define USB_DEVICE_STRING_COUNT        4U
#define USB_DEVICE_LANGUAGE_COUNT      1U

/* These normally live in the SDK example's usb_device_descriptor.h. */
#define USB_DEVICE_SPECIFIC_BCD_VERSION 0x0200U /* USB 2.0 */
#define USB_DESCRIPTOR_LENGTH_HID       9U
#define USB_HID_CLASS                   USB_DEVICE_CONFIG_HID_CLASS_CODE /* 0x03 */

/*
 * Report descriptor: 8 buttons, 16-bit relative X/Y, 8-bit relative wheel.
 * Must match hid_mouse_report_t (6 bytes, no report ID).
 */
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_hidReportDescriptor[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop)            */
    0x09, 0x02,       /* Usage (Mouse)                           */
    0xA1, 0x01,       /* Collection (Application)                */
    0x09, 0x01,       /*   Usage (Pointer)                       */
    0xA1, 0x00,       /*   Collection (Physical)                 */
    0x05, 0x09,       /*     Usage Page (Button)                 */
    0x19, 0x01,       /*     Usage Minimum (1)                   */
    0x29, 0x08,       /*     Usage Maximum (8)                   */
    0x15, 0x00,       /*     Logical Minimum (0)                 */
    0x25, 0x01,       /*     Logical Maximum (1)                 */
    0x95, 0x08,       /*     Report Count (8)                    */
    0x75, 0x01,       /*     Report Size (1)                     */
    0x81, 0x02,       /*     Input (Data,Var,Abs)   -> byte 0    */
    0x05, 0x01,       /*     Usage Page (Generic Desktop)        */
    0x09, 0x30,       /*     Usage (X)                           */
    0x09, 0x31,       /*     Usage (Y)                           */
    0x16, 0x01, 0x80, /*     Logical Minimum (-32767)            */
    0x26, 0xFF, 0x7F, /*     Logical Maximum (32767)             */
    0x75, 0x10,       /*     Report Size (16)                    */
    0x95, 0x02,       /*     Report Count (2)                    */
    0x81, 0x06,       /*     Input (Data,Var,Rel)   -> bytes 1-4 */
    0x09, 0x38,       /*     Usage (Wheel)                       */
    0x15, 0x81,       /*     Logical Minimum (-127)              */
    0x25, 0x7F,       /*     Logical Maximum (127)               */
    0x75, 0x08,       /*     Report Size (8)                     */
    0x95, 0x01,       /*     Report Count (1)                    */
    0x81, 0x06,       /*     Input (Data,Var,Rel)   -> byte 5    */
    0xC0,             /*   End Collection                        */
    0xC0,             /* End Collection                          */
};
#define HID_REPORT_DESC_LEN (sizeof(s_hidReportDescriptor))
_Static_assert(sizeof(hid_mouse_report_t) == 6U, "report struct must be 6 bytes");
_Static_assert(sizeof(hid_mouse_report_t) <= APP_HID_IN_MPS, "report exceeds wMaxPacketSize");

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_deviceDescriptor[] = {
    USB_DESCRIPTOR_LENGTH_DEVICE,
    USB_DESCRIPTOR_TYPE_DEVICE,
    USB_SHORT_GET_LOW(USB_DEVICE_SPECIFIC_BCD_VERSION), /* bcdUSB 2.00 */
    USB_SHORT_GET_HIGH(USB_DEVICE_SPECIFIC_BCD_VERSION),
    0x00, /* bDeviceClass: per interface */
    0x00,
    0x00,
    USB_CONTROL_MAX_PACKET_SIZE, /* bMaxPacketSize0 = 64 */
    USB_SHORT_GET_LOW(APP_USB_VID),
    USB_SHORT_GET_HIGH(APP_USB_VID),
    USB_SHORT_GET_LOW(APP_USB_PID),
    USB_SHORT_GET_HIGH(APP_USB_PID),
    USB_SHORT_GET_LOW(APP_USB_BCD_DEVICE),
    USB_SHORT_GET_HIGH(APP_USB_BCD_DEVICE),
    0x01, /* iManufacturer */
    0x02, /* iProduct */
    0x03, /* iSerialNumber */
    USB_DEVICE_CONFIGURATION_COUNT,
};

#define CONFIG_DESC_TOTAL_LEN                                                                     \
    (USB_DESCRIPTOR_LENGTH_CONFIGURE + USB_DESCRIPTOR_LENGTH_INTERFACE + USB_DESCRIPTOR_LENGTH_HID + \
     USB_DESCRIPTOR_LENGTH_ENDPOINT)

/* Offsets used by usb_device_set_speed() to patch the endpoint descriptor. */
#define EP_DESC_OFFSET \
    (USB_DESCRIPTOR_LENGTH_CONFIGURE + USB_DESCRIPTOR_LENGTH_INTERFACE + USB_DESCRIPTOR_LENGTH_HID)

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_configDescriptor[CONFIG_DESC_TOTAL_LEN] = {
    /* Configuration */
    USB_DESCRIPTOR_LENGTH_CONFIGURE,
    USB_DESCRIPTOR_TYPE_CONFIGURE,
    USB_SHORT_GET_LOW(CONFIG_DESC_TOTAL_LEN),
    USB_SHORT_GET_HIGH(CONFIG_DESC_TOTAL_LEN),
    USB_HID_MOUSE_INTERFACE_COUNT,
    USB_HID_MOUSE_CONFIGURE_INDEX,
    0x00,       /* iConfiguration */
    0x80,       /* bmAttributes: bus powered (D7 must be 1) */
    0xFA,       /* bMaxPower: 500 mA (board + downstream mouse) */

    /* Interface 0: HID. Subclass 0 / protocol 0 (no boot interface) because the
     * 16-bit report is not boot compatible. To support BIOS boot protocol use
     * subclass 1 / protocol 2 and send a 3-byte boot report while
     * SET_PROTOCOL(0) is in effect. */
    USB_DESCRIPTOR_LENGTH_INTERFACE,
    USB_DESCRIPTOR_TYPE_INTERFACE,
    USB_HID_MOUSE_INTERFACE_INDEX,
    0x00, /* bAlternateSetting */
    USB_HID_MOUSE_ENDPOINT_COUNT,
    USB_HID_CLASS,     /* 0x03 */
    0x00,              /* bInterfaceSubClass: none */
    0x00,              /* bInterfaceProtocol: none */
    0x00,

    /* HID descriptor */
    USB_DESCRIPTOR_LENGTH_HID,
    USB_DESCRIPTOR_TYPE_HID,
    0x11, 0x01, /* bcdHID 1.11 */
    0x00,       /* bCountryCode */
    0x01,       /* bNumDescriptors */
    USB_DESCRIPTOR_TYPE_HID_REPORT,
    USB_SHORT_GET_LOW(HID_REPORT_DESC_LEN),
    USB_SHORT_GET_HIGH(HID_REPORT_DESC_LEN),

    /* Endpoint 1 IN, interrupt */
    USB_DESCRIPTOR_LENGTH_ENDPOINT,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    APP_HID_IN_EP | (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
    USB_ENDPOINT_INTERRUPT,
    USB_SHORT_GET_LOW(APP_HID_IN_MPS),
    USB_SHORT_GET_HIGH(APP_HID_IN_MPS),
    APP_HID_IN_INTERVAL_HS, /* bInterval = 1 -> 125 us @ HS (8 kHz) */
};

/* Mandatory for HS-capable devices (USB 2.0 9.6.2). */
USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_deviceQualifierDescriptor[] = {
    USB_DESCRIPTOR_LENGTH_DEVICE_QUALITIER,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALITIER,
    USB_SHORT_GET_LOW(USB_DEVICE_SPECIFIC_BCD_VERSION),
    USB_SHORT_GET_HIGH(USB_DEVICE_SPECIFIC_BCD_VERSION),
    0x00, 0x00, 0x00,
    USB_CONTROL_MAX_PACKET_SIZE,
    USB_DEVICE_CONFIGURATION_COUNT,
    0x00, /* reserved */
};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_str0[] = {4U, USB_DESCRIPTOR_TYPE_STRING, 0x09U, 0x04U}; /* en-US */

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_str1[] = {2U + 2U * 7U, USB_DESCRIPTOR_TYPE_STRING,
                           'G', 0, 'e', 0, 'n', 0, 'e', 0, 'r', 0, 'i', 0, 'c', 0};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_str2[] = {2U + 2U * 9U, USB_DESCRIPTOR_TYPE_STRING,
                           'H', 0, 'I', 0, 'D', 0, ' ', 0, 'P', 0, 'r', 0, 'o', 0, 'x', 0, 'y', 0};

USB_DMA_INIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE)
static uint8_t s_str3[] = {2U + 2U * 4U, USB_DESCRIPTOR_TYPE_STRING, '0', 0, '0', 0, '0', 0, '1', 0};

static uint8_t *const s_strings[USB_DEVICE_STRING_COUNT] = {s_str0, s_str1, s_str2, s_str3};
static const uint32_t s_stringLen[USB_DEVICE_STRING_COUNT] = {sizeof(s_str0), sizeof(s_str1), sizeof(s_str2),
                                                              sizeof(s_str3)};

/* ========================================================================= */
/* SDK class configuration structures                                        */
/* ========================================================================= */

static usb_device_endpoint_struct_t s_hidEndpoints[USB_HID_MOUSE_ENDPOINT_COUNT] = {
    {
        APP_HID_IN_EP | (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
        USB_ENDPOINT_INTERRUPT,
        APP_HID_IN_MPS,
        APP_HID_IN_INTERVAL_HS,
    },
};

static usb_device_interface_struct_t s_hidInterface[] = {
    {0U /* alternate setting */, {USB_HID_MOUSE_ENDPOINT_COUNT, s_hidEndpoints}, NULL},
};

static usb_device_interfaces_struct_t s_hidInterfaces[USB_HID_MOUSE_INTERFACE_COUNT] = {
    {USB_HID_CLASS, 0x00U, 0x00U, USB_HID_MOUSE_INTERFACE_INDEX, s_hidInterface,
     sizeof(s_hidInterface) / sizeof(usb_device_interface_struct_t)},
};

static usb_device_interface_list_t s_hidInterfaceList[USB_DEVICE_CONFIGURATION_COUNT] = {
    {USB_HID_MOUSE_INTERFACE_COUNT, s_hidInterfaces},
};

static usb_device_class_struct_t s_hidClass = {
    s_hidInterfaceList,
    kUSB_DeviceClassTypeHid,
    USB_DEVICE_CONFIGURATION_COUNT,
};

static usb_status_t usb_device_hid_callback(class_handle_t handle, uint32_t event, void *param);
static usb_status_t usb_device_callback(usb_device_handle handle, uint32_t event, void *param);

static usb_device_class_config_struct_t s_classConfig[1] = {
    {usb_device_hid_callback, (class_handle_t)NULL, &s_hidClass},
};

static usb_device_class_config_list_struct_t s_classConfigList = {
    s_classConfig,
    usb_device_callback,
    1U,
};

/* ========================================================================= */
/* State                                                                     */
/* ========================================================================= */

static usb_device_handle s_deviceHandle;
static class_handle_t s_hidHandle;
static volatile uint8_t s_currentConfiguration;
static uint8_t s_currentAltSetting;
static volatile uint8_t s_speed = USB_SPEED_FULL;

/* IN transfer buffer: the controller DMAs from here -> non-cacheable RAM. */
USB_DMA_NONINIT_DATA_ALIGN(USB_DATA_ALIGN_SIZE) static uint8_t s_txBuffer[APP_HID_IN_MPS];

/* ========================================================================= */
/* Report transmission (called by hid_merge)                                 */
/* ========================================================================= */

APP_HOT bool usb_device_send_report(const hid_mouse_report_t *report)
{
    if ((s_hidHandle == NULL) || (s_currentConfiguration == 0U))
    {
        return false;
    }
    /* hid_merge guarantees no transfer is pending when we get here, so the
     * buffer is not owned by the controller. */
    memcpy(s_txBuffer, report, sizeof(*report));
    if (USB_DeviceHidSend(s_hidHandle, APP_HID_IN_EP, s_txBuffer, sizeof(*report)) != kStatus_USB_Success)
    {
        return false;
    }
    app_status_pc_report_sent();
    return true;
}

/* ========================================================================= */
/* Speed-dependent descriptor patching                                       */
/* ========================================================================= */

static void usb_device_set_speed(uint8_t speed)
{
    uint8_t interval = (speed == USB_SPEED_HIGH) ? APP_HID_IN_INTERVAL_HS : APP_HID_IN_INTERVAL_FS;
    uint8_t *ep      = &s_configDescriptor[EP_DESC_OFFSET];

    /* Descriptor sent to the host ... */
    ep[4] = USB_SHORT_GET_LOW(APP_HID_IN_MPS);
    ep[5] = USB_SHORT_GET_HIGH(APP_HID_IN_MPS);
    ep[6] = interval;
    /* ... and the copy the class driver uses to initialise the endpoint. */
    s_hidEndpoints[0].maxPacketSize = APP_HID_IN_MPS;
    s_hidEndpoints[0].interval      = interval;
    s_speed                         = speed;
    app_status_pc_speed(speed);
}

/* ========================================================================= */
/* HID class callback (ISR context, USB_DEVICE_CONFIG_USE_TASK = 0)          */
/* ========================================================================= */

static APP_HOT usb_status_t usb_device_hid_callback(class_handle_t handle, uint32_t event, void *param)
{
    usb_status_t status = kStatus_USB_Success;
    (void)handle;

    switch (event)
    {
        case kUSB_DeviceHidEventSendResponse:
        {
            /* The IN transfer was ACKed by the PC (or cancelled by a reset). */
            usb_device_endpoint_callback_message_struct_t *msg =
                (usb_device_endpoint_callback_message_struct_t *)param;
            if ((msg != NULL) && (msg->length == USB_CANCELLED_TRANSFER_LENGTH))
            {
                break; /* bus reset / detach: hid_merge was reset separately */
            }
            hid_merge_on_tx_complete(); /* re-prime with pending data, if any */
            break;
        }
        case kUSB_DeviceHidEventGetReport:
        case kUSB_DeviceHidEventSetReport:
        case kUSB_DeviceHidEventRequestReportBuffer:
            status = kStatus_USB_InvalidRequest; /* STALL: not supported */
            break;
        case kUSB_DeviceHidEventGetIdle:
        case kUSB_DeviceHidEventGetProtocol:
        case kUSB_DeviceHidEventSetIdle:
        case kUSB_DeviceHidEventSetProtocol:
            break;
        default:
            status = kStatus_USB_InvalidRequest;
            break;
    }
    return status;
}

/* ========================================================================= */
/* Device (chapter 9) callback (ISR context)                                 */
/* ========================================================================= */

static usb_status_t usb_device_callback(usb_device_handle handle, uint32_t event, void *param)
{
    usb_status_t status = kStatus_USB_InvalidRequest;
    uint8_t *temp8      = (uint8_t *)param;
    uint16_t *temp16    = (uint16_t *)param;

    switch (event)
    {
        case kUSB_DeviceEventBusReset:
        {
            uint8_t speed           = USB_SPEED_FULL;
            s_currentConfiguration  = 0U;
            hid_merge_set_device_ready(false);
            app_status_pc_configured(false);
            /* After the HS chirp handshake the controller knows the speed. */
            if (USB_DeviceClassGetSpeed(CONTROLLER_ID, &speed) == kStatus_USB_Success)
            {
                usb_device_set_speed(speed);
            }
            status = kStatus_USB_Success;
            break;
        }

#if (defined(USB_DEVICE_CONFIG_DETACH_ENABLE) && (USB_DEVICE_CONFIG_DETACH_ENABLE > 0U))
        case kUSB_DeviceEventDetach:
            s_currentConfiguration = 0U;
            hid_merge_set_device_ready(false);
            app_status_pc_configured(false);
            status = kStatus_USB_Success;
            break;
#endif

        case kUSB_DeviceEventSetConfiguration:
            if (*temp8 == 0U)
            {
                s_currentConfiguration = 0U;
                hid_merge_set_device_ready(false);
                app_status_pc_configured(false);
                status = kStatus_USB_Success;
            }
            else if (*temp8 == USB_HID_MOUSE_CONFIGURE_INDEX)
            {
                /* The class driver has opened EP1 IN by now. */
                s_currentConfiguration = *temp8;
                hid_merge_set_device_ready(true);
                app_status_pc_configured(true);
                status = kStatus_USB_Success;
            }
            break;

        case kUSB_DeviceEventSetInterface:
            if (s_currentConfiguration != 0U)
            {
                uint8_t iface = (uint8_t)((*temp16 & 0xFF00U) >> 8U);
                uint8_t alt   = (uint8_t)(*temp16 & 0x00FFU);
                if ((iface == USB_HID_MOUSE_INTERFACE_INDEX) && (alt == 0U))
                {
                    s_currentAltSetting = alt;
                    status              = kStatus_USB_Success;
                }
            }
            break;

        case kUSB_DeviceEventGetConfiguration:
            *temp8 = s_currentConfiguration;
            status = kStatus_USB_Success;
            break;

        case kUSB_DeviceEventGetInterface:
            if (s_currentConfiguration != 0U)
            {
                uint8_t iface = (uint8_t)((*temp16 & 0xFF00U) >> 8U);
                if (iface == USB_HID_MOUSE_INTERFACE_INDEX)
                {
                    *temp16 = (uint16_t)((*temp16 & 0xFF00U) | s_currentAltSetting);
                    status  = kStatus_USB_Success;
                }
            }
            break;

        case kUSB_DeviceEventGetDeviceDescriptor:
        {
            usb_device_get_device_descriptor_struct_t *d = (usb_device_get_device_descriptor_struct_t *)param;
            d->buffer = s_deviceDescriptor;
            d->length = sizeof(s_deviceDescriptor);
            status    = kStatus_USB_Success;
            break;
        }

        case kUSB_DeviceEventGetConfigurationDescriptor:
        {
            usb_device_get_configuration_descriptor_struct_t *d =
                (usb_device_get_configuration_descriptor_struct_t *)param;
            if (d->configuration == 0U) /* index 0 -> our only configuration */
            {
                d->buffer = s_configDescriptor;
                d->length = sizeof(s_configDescriptor);
                status    = kStatus_USB_Success;
            }
            break;
        }

        case kUSB_DeviceEventGetStringDescriptor:
        {
            usb_device_get_string_descriptor_struct_t *d = (usb_device_get_string_descriptor_struct_t *)param;
            if (d->stringIndex == 0U)
            {
                d->buffer = s_strings[0];
                d->length = s_stringLen[0];
                status    = kStatus_USB_Success;
            }
            else if ((d->languageId == 0x0409U) && (d->stringIndex < USB_DEVICE_STRING_COUNT))
            {
                d->buffer = s_strings[d->stringIndex];
                d->length = s_stringLen[d->stringIndex];
                status    = kStatus_USB_Success;
            }
            break;
        }

        case kUSB_DeviceEventGetDeviceQualifierDescriptor:
        {
            usb_device_get_device_qualifier_descriptor_struct_t *d =
                (usb_device_get_device_qualifier_descriptor_struct_t *)param;
            d->buffer = s_deviceQualifierDescriptor;
            d->length = sizeof(s_deviceQualifierDescriptor);
            status    = kStatus_USB_Success;
            break;
        }

        case kUSB_DeviceEventGetHidDescriptor:
        {
            usb_device_get_hid_descriptor_struct_t *d = (usb_device_get_hid_descriptor_struct_t *)param;
            if (d->interfaceNumber == USB_HID_MOUSE_INTERFACE_INDEX)
            {
                d->buffer = &s_configDescriptor[USB_DESCRIPTOR_LENGTH_CONFIGURE + USB_DESCRIPTOR_LENGTH_INTERFACE];
                d->length = USB_DESCRIPTOR_LENGTH_HID;
                status    = kStatus_USB_Success;
            }
            break;
        }

        case kUSB_DeviceEventGetHidReportDescriptor:
        {
            usb_device_get_hid_report_descriptor_struct_t *d =
                (usb_device_get_hid_report_descriptor_struct_t *)param;
            if (d->interfaceNumber == USB_HID_MOUSE_INTERFACE_INDEX)
            {
                d->buffer = s_hidReportDescriptor;
                d->length = HID_REPORT_DESC_LEN;
                status    = kStatus_USB_Success;
            }
            break;
        }

        default:
            break; /* GetHidPhysicalDescriptor, vendor requests, etc. -> STALL */
    }
    (void)handle;
    return status;
}

/* ========================================================================= */
/* Interrupt handler                                                         */
/* ========================================================================= */

APP_HOT void USB_OTG1_IRQHandler(void)
{
    USB_DeviceEhciIsrFunction(s_deviceHandle);
    SDK_ISR_EXIT_BARRIER;
}

/* ========================================================================= */
/* Initialisation                                                            */
/* ========================================================================= */

static void usb_device_clock_init(void)
{
    usb_phy_config_struct_t phyConfig = {
        BOARD_USB_PHY_D_CAL,
        BOARD_USB_PHY_TXCAL45DP,
        BOARD_USB_PHY_TXCAL45DM,
    };

    /* USB1 PLL (PLL3, 480 MHz) + controller clock gate, then PHY power-up and
     * HS calibration. Identical to USB_DeviceClockInit() in the SDK examples. */
    CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, 480000000U);
    CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, 480000000U);
    USB_EhciPhyInit(CONTROLLER_ID, BOARD_XTAL0_CLK_HZ, &phyConfig);
}

void usb_device_app_init(void)
{
    usb_device_clock_init();

#if (defined(FSL_FEATURE_SOC_SYSMPU_COUNT) && (FSL_FEATURE_SOC_SYSMPU_COUNT > 0U))
    SYSMPU_Enable(SYSMPU, 0);
#endif

    s_currentConfiguration = 0U;
    usb_device_set_speed(USB_SPEED_HIGH);

    /* Registers the HID class + our ch9 callback with the device stack and
     * allocates the EHCI device controller (dQH/dTD in non-cacheable RAM). */
    if (USB_DeviceClassInit(CONTROLLER_ID, &s_classConfigList, &s_deviceHandle) != kStatus_USB_Success)
    {
        app_fatal(APP_FATAL_USB_DEVICE_INIT);
    }
    s_hidHandle = s_classConfigList.config->classHandle;

    NVIC_SetPriority(USB_DEVICE_IRQn, APP_NVIC_PRIO_USB_DEVICE);
    EnableIRQ(USB_DEVICE_IRQn);

    /* Pull up D+ -> PC starts enumeration (HS chirp handled by the controller). */
    USB_DeviceRun(s_deviceHandle);
}
