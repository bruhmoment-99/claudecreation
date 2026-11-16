/*
 * uart_parser.h - LPUART3 (CH343P, 6 Mbaud) command channel.
 *
 * One command per line, terminated by "\r\n" (a bare "\n" also works).
 *
 *   Buttons (val = 1 press / 0 release; empty parentheses = query)
 *     cmd.left(val)    cmd.right(val)   cmd.middle(val)
 *     cmd.side1(val)   button 4 (Back)
 *     cmd.side2(val)   button 5 (Forward)
 *     cmd.buttons(mask) set the whole injected button byte (0..255)
 *     cmd.release()    clear every injected button
 *   Motion
 *     cmd.move(dx,dy)  cmd.wheel(delta)
 *   Utility
 *     cmd.sleep(ms)    prompt is sent first, then the parser task sleeps
 *     cmd.version()    cmd.info()
 *
 * Every button change goes through hid_merge, so the button locking rule
 * applies: a report is sent only if (physical | injected) actually changes.
 *
 * Responses
 *     command : "<echo>\r\n>>> "
 *     query   : "<echo>\r\n<value>\r\n>>> "   (button queries report the
 *               logical state = physical | injected; version/info a string)
 *     invalid : nothing at all (unknown name, bad arguments, overlong line)
 */
#ifndef UART_PARSER_H_
#define UART_PARSER_H_

#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

typedef struct
{
    void (*write)(const char *data, size_t len); /* response output          */
    void (*sleep_ms)(uint32_t ms);               /* blocks the calling task  */
} cmd_io_t;

typedef struct
{
    char line[APP_CMD_MAX_LINE + 1U];
    uint32_t len;
    uint8_t overflow; /* discard until end of line */
    const cmd_io_t *io;
} cmd_parser_t;

/* ---- Pure command layer (unit-testable) -------------------------------- */
void cmd_parser_init(cmd_parser_t *p, const cmd_io_t *io);
void cmd_parser_feed(cmd_parser_t *p, const uint8_t *data, size_t len);
/* Returns 1 if the line was a valid command (and was answered), else 0. */
int cmd_execute_line(const char *line, size_t len, const cmd_io_t *io);

/* ---- Hardware layer (LPUART3 + eDMA ring buffer + FreeRTOS task) -------- */
void uart_parser_hw_init(void);   /* call before the scheduler starts tasks */
void uart_parser_task(void *arg); /* medium priority FreeRTOS task          */

#endif /* UART_PARSER_H_ */
