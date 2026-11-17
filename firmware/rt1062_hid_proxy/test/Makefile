SRC_DIR := ../source
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -DUNIT_TEST -I$(SRC_DIR)
SRCS    := test_main.c $(SRC_DIR)/hid_merge.c $(SRC_DIR)/hid_report_parser.c $(SRC_DIR)/uart_parser.c $(SRC_DIR)/smooth_move.c $(SRC_DIR)/app_status.c $(SRC_DIR)/crc32.c $(SRC_DIR)/settings.c $(SRC_DIR)/fw_update.c $(SRC_DIR)/watch.c

all: run

test_runner: $(SRCS) $(wildcard $(SRC_DIR)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: test_runner
	./test_runner

# Board command channel on a pseudo-terminal (for testing PC tools).
SIM_SRCS := sim_board.c $(SRC_DIR)/uart_parser.c $(SRC_DIR)/hid_merge.c $(SRC_DIR)/smooth_move.c \
            $(SRC_DIR)/app_status.c $(SRC_DIR)/hid_report_parser.c $(SRC_DIR)/fw_update.c $(SRC_DIR)/crc32.c
sim: $(SIM_SRCS) $(wildcard $(SRC_DIR)/*.h)
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -DUNIT_TEST -I$(SRC_DIR) -o sim_board $(SIM_SRCS)

clean:
	rm -f test_runner sim_board

.PHONY: all run clean sim
