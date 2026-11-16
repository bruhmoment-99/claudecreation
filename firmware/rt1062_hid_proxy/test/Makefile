SRC_DIR := ../source
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -DUNIT_TEST -I$(SRC_DIR)
SRCS    := test_main.c $(SRC_DIR)/hid_merge.c $(SRC_DIR)/hid_report_parser.c $(SRC_DIR)/uart_parser.c $(SRC_DIR)/smooth_move.c

all: run

test_runner: $(SRCS) $(wildcard $(SRC_DIR)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: test_runner
	./test_runner

clean:
	rm -f test_runner

.PHONY: all run clean
