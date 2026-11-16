/*
 * uart_parser.h - LPUART3 (CH343P, 6 Mbaud) command channel.
 *
 * Grammar (one command per line, terminated by "\r\n"; a bare "\n" is also
 * accepted):
 *     cmd.move(dx,dy)      add relative motion
 *     cmd.left(0|1)        set injected left button   (query: cmd.left())
 *     cmd.right(0|1)       set injected right button  (query: cmd.right())
 *     cmd.middle(0|1)      set injected middle button (query: cmd.middle())
 *     cmd.wheel(delta)     add wheel motion
 *
 * Responses:
 *     success : "<echo>\r\n>>> "
 *     query   : "<echo>\r\n<0|1>\r\n>>> "   (logical state = physical | injected)
 *     error   : "<echo>\r\nERR\r\n>>> "
 */
#ifndef UART_PARSER_H_
#define UART_PARSER_H_

#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

typedef void (*cmd_write_fn_t)(const char *data, size_t len);

typedef struct
{
    char line[APP_CMD_MAX_LINE + 1U];
    uint32_t len;
    uint8_t overflow; /* discard until end of line */
    cmd_write_fn_t write;
} cmd_parser_t;

/* ---- Pure command layer (unit-testable) -------------------------------- */
void cmd_parser_init(cmd_parser_t *p, cmd_write_fn_t write);
void cmd_parser_feed(cmd_parser_t *p, const uint8_t *data, size_t len);
void cmd_execute_line(const char *line, size_t len, cmd_write_fn_t write);

/* ---- Hardware layer (LPUART3 + eDMA ring buffer + FreeRTOS task) -------- */
void uart_parser_hw_init(void);   /* call before the scheduler starts tasks */
void uart_parser_task(void *arg); /* medium priority FreeRTOS task          */

#endif /* UART_PARSER_H_ */
