CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude

SRC_DIR = src
BIN_DIR = bin
BUILD_DIR = build

LIB_NAME = libimage.a
LIBRARY = $(BIN_DIR)/$(LIB_NAME)

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

TEST_SRC = $(SRC_DIR)/test_validation.c
TEST_BIN = $(BIN_DIR)/test_validation

all: $(LIBRARY)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBRARY): $(OBJS)
	@mkdir -p $(BIN_DIR)
	ar rcs $(LIBRARY) $(OBJS)
	@echo "📦 Biblioteca construída: $(LIBRARY)"

test: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(LIBRARY)
	$(CC) $(CFLAGS) $< -L$(BIN_DIR) -limage -lm -o $@
	@echo "🧪 Teste compilado: $(TEST_BIN)"

clean:
	rm -rf $(BUILD_DIR) $(TEST_BIN) $(LIBRARY)

.PHONY: all clean test
