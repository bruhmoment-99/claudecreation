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
 *
 * Buttons 1..16 are mapped, as bit fields (Input Variable) or as arrays
 * (each field holds the number of a pressed button), and may be spread over
 * several reports with different report IDs. Horizontal scroll is the
 * Consumer "AC Pan" usage.
 */
#ifndef HID_REPORT_PARSER_H_
#define HID_REPORT_PARSER_H_

#include <stdbool.h>
#include <stdint.h>

#define HID_MAX_MAPPED_BUTTONS 16U
#define HID_MAX_BUTTON_ARRAYS  2U

typedef struct
{
    bool present;
    bool isSigned;
    uint8_t reportId;   /* 0 when the descriptor uses no report IDs       */
    uint8_t bitSize;    /* 1..32                                         */
    uint16_t bitOffset; /* from start of report payload (after ID byte)  */
} hid_field_t;

/* Buttons reported as one bit each (Input Variable). */
typedef struct
{
    uint16_t mask;      /* bit n set -> button n+1 found                  */
    uint8_t reportId[HID_MAX_MAPPED_BUTTONS];
    uint16_t bitOffset[HID_MAX_MAPPED_BUTTONS];
} hid_button_map_t;

/* Buttons reported as an array (Input Array): count fields of bitSize bits,
 * each holding a logical value; value v means button
 * usageMin + (v - logicalMin), and a value outside the range means none. */
typedef struct
{
    bool present;
    uint8_t reportId;
    uint8_t bitSize;
    uint8_t count;
    uint16_t bitOffset;
    uint16_t usageMin;  /* Button page usage IDs                          */
    uint16_t usageMax;
    int32_t logicalMin;
    int32_t logicalMax;
} hid_button_array_t;

typedef struct
{
    bool valid;          /* X and Y found                                 */
    bool usesReportId;   /* every report is prefixed with a 1-byte ID     */
    hid_button_map_t buttons;
    hid_button_array_t arrays[HID_MAX_BUTTON_ARRAYS];
    hid_field_t x;
    hid_field_t y;
    hid_field_t wheel;
    hid_field_t pan;     /* horizontal scroll (Consumer AC Pan)           */
} hid_mouse_layout_t;

typedef struct
{
    uint16_t buttonsMask; /* buttons this report carries (others unknown) */
    uint16_t buttons;     /* their state, bit n = button n+1              */
    int32_t x;
    int32_t y;
    int32_t wheel;
    int32_t pan;
} hid_mouse_sample_t;

/* Mask of the buttons an array can report (bit n = button n+1). */
uint16_t hid_button_array_mask(const hid_button_array_t *a);

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
