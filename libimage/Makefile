CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude -std=c11

SRC_DIR = src
BIN_DIR = bin
BUILD_DIR = build
PIC_DIR   = $(BUILD_DIR)/pic

LIB_NAME    = libimage.a
SHARED_NAME = libimage.so
LIBRARY     = $(BIN_DIR)/$(LIB_NAME)
SHARED_LIB  = $(BIN_DIR)/$(SHARED_NAME)

SRCS     = $(wildcard $(SRC_DIR)/*.c)
LIB_SRCS = $(filter-out $(SRC_DIR)/test_validation.c, $(SRCS))
LIB_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))
PIC_OBJS = $(patsubst $(SRC_DIR)/%.c,$(PIC_DIR)/%.o,$(LIB_SRCS))

TEST_SRC = $(SRC_DIR)/test_validation.c
TEST_BIN = $(BIN_DIR)/test_validation

# -- Build rules --------------------------------------------------------
all: $(LIBRARY) $(SHARED_LIB)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(PIC_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(LIBRARY): $(LIB_OBJS)
	@mkdir -p $(BIN_DIR)
	ar rcs $@ $(LIB_OBJS)
	@echo "  Static library: $@"

$(SHARED_LIB): $(PIC_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) -shared -o $@ $(PIC_OBJS) -lm
	@echo "  Shared library: $@"

shared: $(SHARED_LIB)

test: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(LIBRARY)
	$(CC) $(CFLAGS) $< $(LIBRARY) -lm -o $@
	@echo "  Test compiled: $@"

clean:
	rm -rf $(BUILD_DIR) $(TEST_BIN) $(LIBRARY) $(SHARED_LIB)

.PHONY: all clean test shared
