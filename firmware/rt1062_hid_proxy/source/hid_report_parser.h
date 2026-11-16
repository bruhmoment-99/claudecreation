/*
 * hid_report_parser.h - Minimal HID report descriptor parser that locates the
 * mouse fields (buttons, X, Y, wheel) of an arbitrary mouse, so the report
 * format is never hardcoded.
 *
 * Supported: short items, long items (skipped), Push/Pop, Report ID (incl.
 * interleaved IDs), Usage / Usage Min/Max (16- and 32-bit), arbitrary bit
 * sizes and alignments (e.g. 12-bit packed X/Y), constant padding, nested
 * collections. Only fields inside a Generic Desktop Mouse/Pointer application
 * collection are considered, so composite descriptors (mouse + keyboard +
 * vendor reports) work.
 */
#ifndef HID_REPORT_PARSER_H_
#define HID_REPORT_PARSER_H_

#include <stdbool.h>
#include <stdint.h>

#define HID_MAX_MAPPED_BUTTONS 8U

typedef struct
{
    bool present;
    bool isSigned;
    uint8_t reportId;   /* 0 when the descriptor uses no report IDs       */
    uint8_t bitSize;    /* 1..32                                         */
    uint16_t bitOffset; /* from start of report payload (after ID byte)  */
} hid_field_t;

typedef struct
{
    uint8_t mask;       /* bit n set -> button n+1 found                  */
    uint8_t reportId;
    uint16_t bitOffset[HID_MAX_MAPPED_BUTTONS];
} hid_button_map_t;

typedef struct
{
    bool valid;          /* X and Y found                                 */
    bool usesReportId;   /* every report is prefixed with a 1-byte ID     */
    hid_button_map_t buttons;
    hid_field_t x;
    hid_field_t y;
    hid_field_t wheel;
} hid_mouse_layout_t;

typedef struct
{
    bool buttonsValid;   /* this report carried the button field          */
    uint8_t buttons;
    int32_t x;
    int32_t y;
    int32_t wheel;
} hid_mouse_sample_t;

/* Parse a report descriptor. Returns layout->valid. Not re-entrant. */
bool hid_parse_mouse_layout(const uint8_t *desc, uint32_t len, hid_mouse_layout_t *layout);

/* Boot-protocol layout (HID 1.11 App. B.2) - fallback if parsing fails and
 * the interface supports the boot subclass. */
void hid_mouse_layout_boot(hid_mouse_layout_t *layout);

/* Decode one input report. Returns false if the report contains none of the
 * mouse fields (e.g. a report ID belonging to a keyboard collection). */
bool hid_extract_mouse(const hid_mouse_layout_t *layout, const uint8_t *report, uint32_t len,
                       hid_mouse_sample_t *out);

#endif /* HID_REPORT_PARSER_H_ */
