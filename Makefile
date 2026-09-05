CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -O2

SRC_DIR := src
BIN_DIR := bin

SRCS := $(SRC_DIR)/main.c \
        $(SRC_DIR)/diag.c \
        $(SRC_DIR)/types.c \
        $(SRC_DIR)/preprocess.c \
        $(SRC_DIR)/lexer.c \
        $(SRC_DIR)/ast.c \
        $(SRC_DIR)/parser.c \
        $(SRC_DIR)/typecheck.c \
        $(SRC_DIR)/optimize.c \
        $(SRC_DIR)/codegen.c \
        $(SRC_DIR)/vm.c \
        $(SRC_DIR)/builtins.c

OBJS := $(SRCS:.c=.o)

TARGET := $(BIN_DIR)/orgitoc

all: $(TARGET)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(TARGET): $(BIN_DIR) $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

test: $(TARGET)
	@./tests/run_tests.sh

.PHONY: all clean test
