/*
 * hid_report_parser.c - see hid_report_parser.h
 *
 * Reference: Device Class Definition for HID 1.11, section 6.2.2.
 */
#include "hid_report_parser.h"
#include "app_config.h"

#include <string.h>

/* Item types */
#define ITEM_MAIN   0U
#define ITEM_GLOBAL 1U
#define ITEM_LOCAL  2U

/* Main item tags */
#define MAIN_INPUT          0x8U
#define MAIN_OUTPUT         0x9U
#define MAIN_COLLECTION     0xAU
#define MAIN_FEATURE        0xBU
#define MAIN_END_COLLECTION 0xCU

/* Global item tags */
#define GLOBAL_USAGE_PAGE   0x0U
#define GLOBAL_LOGICAL_MIN  0x1U
#define GLOBAL_LOGICAL_MAX  0x2U
#define GLOBAL_REPORT_SIZE  0x7U
#define GLOBAL_REPORT_ID    0x8U
#define GLOBAL_REPORT_COUNT 0x9U
#define GLOBAL_PUSH         0xAU
#define GLOBAL_POP          0xBU

/* Local item tags */
#define LOCAL_USAGE         0x0U
#define LOCAL_USAGE_MIN     0x1U
#define LOCAL_USAGE_MAX     0x2U

/* Input item flag bits */
#define INPUT_CONSTANT      0x01U
#define INPUT_VARIABLE      0x02U
#define INPUT_RELATIVE      0x04U

/* Usages (page << 16 | id) */
#define USAGE_GD_POINTER    0x00010001UL
#define USAGE_GD_MOUSE      0x00010002UL
#define USAGE_GD_X          0x00010030UL
#define USAGE_GD_Y          0x00010031UL
#define USAGE_GD_WHEEL      0x00010038UL
#define PAGE_BUTTON         0x0009U

#define COLLECTION_APPLICATION 0x01U

#define MAX_USAGES          32U
#define GLOBAL_STACK_DEPTH  4U

typedef struct
{
    uint32_t usagePage;
    int32_t logicalMin;
    uint32_t reportSize;
    uint32_t reportCount;
    uint8_t reportId;
} hid_globals_t;

typedef struct
{
    uint32_t usages[MAX_USAGES];
    uint8_t usageCount;
    uint32_t usageMin;
    uint32_t usageMax;
    bool haveMin;
    bool haveMax;
} hid_locals_t;

/* Running bit position per report ID (input reports only). Static to keep
 * the host task stack small; the parser runs once per enumeration. */
static uint16_t s_bitPos[256];

static uint32_t full_usage(const hid_globals_t *g, uint32_t raw, uint8_t size)
{
    /* 4-byte usages carry their own page (HID 1.11, 6.2.2.8). */
    return (size == 4U) ? raw : ((g->usagePage << 16) | (raw & 0xFFFFU));
}

static uint32_t usage_for_index(const hid_locals_t *l, uint32_t idx)
{
    if (l->haveMin && l->haveMax)
    {
        uint32_t u = l->usageMin + idx;
        return (u > l->usageMax) ? l->usageMax : u;
    }
    if (l->usageCount > 0U)
    {
        return l->usages[(idx < l->usageCount) ? idx : (l->usageCount - 1U)];
    }
    return 0U;
}

static void set_field(hid_field_t *f, const hid_globals_t *g, uint16_t bitOffset, bool relative)
{
    if (f->present)
    {
        return; /* first occurrence inside the mouse collection wins */
    }
    f->present   = true;
    f->reportId  = g->reportId;
    f->bitOffset = bitOffset;
    f->bitSize   = (uint8_t)((g->reportSize > 32U) ? 32U : g->reportSize);
    /* Relative axes are signed by definition; some descriptors still declare
     * Logical Minimum 0, so trust the Relative flag as well. */
    f->isSigned  = (g->logicalMin < 0) || relative;
}

static void handle_input(hid_mouse_layout_t *out, const hid_globals_t *g, const hid_locals_t *l,
                         uint32_t flags, bool inMouse)
{
    uint16_t base  = s_bitPos[g->reportId];
    uint32_t total = g->reportSize * g->reportCount;

    if (inMouse && ((flags & INPUT_CONSTANT) == 0U) && ((flags & INPUT_VARIABLE) != 0U) &&
        (g->reportSize > 0U))
    {
        bool relative = (flags & INPUT_RELATIVE) != 0U;

        for (uint32_t i = 0U; i < g->reportCount; i++)
        {
            uint32_t usage = usage_for_index(l, i);
            uint16_t off   = (uint16_t)(base + i * g->reportSize);
            uint16_t page  = (uint16_t)(usage >> 16);
            uint16_t id    = (uint16_t)(usage & 0xFFFFU);

            if ((page == PAGE_BUTTON) && (id >= 1U) && (id <= HID_MAX_MAPPED_BUTTONS) && (g->reportSize == 1U))
            {
                uint8_t bit = (uint8_t)(1U << (id - 1U));
                if (out->buttons.mask == 0U)
                {
                    out->buttons.reportId = g->reportId;
                }
                if ((out->buttons.reportId == g->reportId) && ((out->buttons.mask & bit) == 0U))
                {
                    out->buttons.mask |= bit;
                    out->buttons.bitOffset[id - 1U] = off;
                }
            }
            else if (usage == USAGE_GD_X)
            {
                set_field(&out->x, g, off, relative);
            }
            else if (usage == USAGE_GD_Y)
            {
                set_field(&out->y, g, off, relative);
            }
            else if (usage == USAGE_GD_WHEEL)
            {
                set_field(&out->wheel, g, off, relative);
            }
            /* Extend here: Consumer AC Pan (0x000C0238) for horizontal scroll,
             * array-style button fields, etc. */
        }
    }
    /* Constant padding and non-mouse fields still occupy bits. */
    s_bitPos[g->reportId] = (uint16_t)(base + total);
}

bool hid_parse_mouse_layout(const uint8_t *desc, uint32_t len, hid_mouse_layout_t *out)
{
    hid_globals_t g = {0};
    hid_globals_t stack[GLOBAL_STACK_DEPTH];
    uint32_t sp = 0U;
    hid_locals_t l;
    int32_t depth      = 0;
    int32_t mouseDepth = -1; /* collection depth of the mouse application collection */
    bool mouseSeen     = false;
    uint32_t i         = 0U;

    memset(out, 0, sizeof(*out));
    memset(&l, 0, sizeof(l));
    memset(s_bitPos, 0, sizeof(s_bitPos));

    while (i < len)
    {
        uint8_t prefix = desc[i++];

        if (prefix == 0xFEU) /* long item: bDataSize, bLongItemTag, data */
        {
            if (i >= len)
            {
                break;
            }
            i += 2U + desc[i];
            continue;
        }

        uint8_t size = (uint8_t)(prefix & 0x03U);
        size         = (size == 3U) ? 4U : size;
        uint8_t type = (uint8_t)((prefix >> 2) & 0x03U);
        uint8_t tag  = (uint8_t)((prefix >> 4) & 0x0FU);

        if ((i + size) > len)
        {
            break; /* truncated descriptor */
        }

        uint32_t u = 0U;
        for (uint8_t k = 0U; k < size; k++)
        {
            u |= (uint32_t)desc[i + k] << (8U * k);
        }
        int32_t s;
        if (size == 1U)
        {
            s = (int8_t)u;
        }
        else if (size == 2U)
        {
            s = (int16_t)u;
        }
        else
        {
            s = (int32_t)u;
        }
        i += size;

        bool inMouse = (mouseDepth >= 0) && (depth >= mouseDepth);

        switch (type)
        {
            case ITEM_MAIN:
                switch (tag)
                {
                    case MAIN_INPUT:
                        handle_input(out, &g, &l, u, inMouse);
                        break;
                    case MAIN_COLLECTION:
                        depth++;
                        if ((u == COLLECTION_APPLICATION) && (mouseDepth < 0) && !mouseSeen)
                        {
                            uint32_t usage = usage_for_index(&l, 0U);
                            if ((usage == USAGE_GD_MOUSE) || (usage == USAGE_GD_POINTER))
                            {
                                mouseDepth = depth;
                                mouseSeen  = true;
                            }
                        }
                        break;
                    case MAIN_END_COLLECTION:
                        if (depth == mouseDepth)
                        {
                            mouseDepth = -1;
                        }
                        if (depth > 0)
                        {
                            depth--;
                        }
                        break;
                    case MAIN_OUTPUT:
                    case MAIN_FEATURE:
                    default:
                        /* Output/Feature reports have their own bit spaces;
                         * we only track Input. */
                        break;
                }
                memset(&l, 0, sizeof(l)); /* locals reset after every main item */
                break;

            case ITEM_GLOBAL:
                switch (tag)
                {
                    case GLOBAL_USAGE_PAGE:
                        g.usagePage = u & 0xFFFFU;
                        break;
                    case GLOBAL_LOGICAL_MIN:
                        g.logicalMin = s;
                        break;
                    case GLOBAL_LOGICAL_MAX:
                        break; /* not needed for decoding */
                    case GLOBAL_REPORT_SIZE:
                        g.reportSize = u;
                        break;
                    case GLOBAL_REPORT_ID:
                        g.reportId        = (uint8_t)u;
                        out->usesReportId = true;
                        break;
                    case GLOBAL_REPORT_COUNT:
                        g.reportCount = u;
                        break;
                    case GLOBAL_PUSH:
                        if (sp < GLOBAL_STACK_DEPTH)
                        {
                            stack[sp++] = g;
                        }
                        break;
                    case GLOBAL_POP:
                        if (sp > 0U)
                        {
                            g = stack[--sp];
                        }
                        break;
                    default:
                        break; /* physical min/max, unit, unit exponent */
                }
                break;

            case ITEM_LOCAL:
                switch (tag)
                {
                    case LOCAL_USAGE:
                        if (l.usageCount < MAX_USAGES)
                        {
                            l.usages[l.usageCount++] = full_usage(&g, u, size);
                        }
                        break;
                    case LOCAL_USAGE_MIN:
                        l.usageMin = full_usage(&g, u, size);
                        l.haveMin  = true;
                        break;
                    case LOCAL_USAGE_MAX:
                        l.usageMax = full_usage(&g, u, size);
                        l.haveMax  = true;
                        break;
                    default:
                        break; /* designators, strings, delimiters */
                }
                break;

            default:
                break; /* reserved */
        }
    }

    out->valid = out->x.present && out->y.present;
    return out->valid;
}

void hid_mouse_layout_boot(hid_mouse_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    /* Boot report: byte0 buttons (bits 0..2), byte1 X int8, byte2 Y int8. */
    out->buttons.mask = 0x07U;
    for (uint8_t b = 0U; b < 3U; b++)
    {
        out->buttons.bitOffset[b] = b;
    }
    out->x = (hid_field_t){.present = true, .isSigned = true, .reportId = 0U, .bitSize = 8U, .bitOffset = 8U};
    out->y = (hid_field_t){.present = true, .isSigned = true, .reportId = 0U, .bitSize = 8U, .bitOffset = 16U};
    out->valid = true;
}

static APP_HOT uint32_t get_bits(const uint8_t *buf, uint32_t lenBytes, uint32_t bitOff, uint8_t nBits)
{
    uint32_t v = 0U;
    for (uint8_t n = 0U; n < nBits; n++)
    {
        uint32_t b = bitOff + n;
        if ((b >> 3) >= lenBytes)
        {
            break; /* short report: missing bits read as 0 */
        }
        if ((buf[b >> 3] >> (b & 7U)) & 1U)
        {
            v |= (1UL << n);
        }
    }
    return v;
}

static APP_HOT int32_t get_field(const hid_field_t *f, const uint8_t *buf, uint32_t len)
{
    uint32_t v = get_bits(buf, len, f->bitOffset, f->bitSize);
    if (f->isSigned && (f->bitSize < 32U) && (v & (1UL << (f->bitSize - 1U))))
    {
        v |= ~((1UL << f->bitSize) - 1U); /* sign-extend */
    }
    return (int32_t)v;
}

APP_HOT bool hid_extract_mouse(const hid_mouse_layout_t *l, const uint8_t *report, uint32_t len,
                               hid_mouse_sample_t *out)
{
    uint8_t rid      = 0U;
    const uint8_t *p = report;
    bool any         = false;

    out->buttonsValid = false;
    out->buttons      = 0U;
    out->x = out->y = out->wheel = 0;

    if (!l->valid || (len == 0U))
    {
        return false;
    }
    if (l->usesReportId)
    {
        rid = report[0];
        p++;
        len--;
    }

    if ((l->buttons.mask != 0U) && (l->buttons.reportId == rid))
    {
        for (uint8_t b = 0U; b < HID_MAX_MAPPED_BUTTONS; b++)
        {
            if ((l->buttons.mask & (1U << b)) && get_bits(p, len, l->buttons.bitOffset[b], 1U))
            {
                out->buttons |= (uint8_t)(1U << b);
            }
        }
        out->buttonsValid = true;
        any               = true;
    }
    if (l->x.present && (l->x.reportId == rid))
    {
        out->x = get_field(&l->x, p, len);
        any    = true;
    }
    if (l->y.present && (l->y.reportId == rid))
    {
        out->y = get_field(&l->y, p, len);
        any    = true;
    }
    if (l->wheel.present && (l->wheel.reportId == rid))
    {
        out->wheel = get_field(&l->wheel, p, len);
        any        = true;
    }
    return any;
}
