CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude

SRC_DIR = src
BIN_DIR = bin
BUILD_DIR = build
PIC_DIR = $(BUILD_DIR)/pic

LIB_NAME = libimage.a
LIBRARY = $(BIN_DIR)/$(LIB_NAME)

SHARED_NAME = libimage.so
SHARED_LIB = $(BIN_DIR)/$(SHARED_NAME)

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

# Library sources (exclude test_validation.c which has main())
LIB_SRCS = $(filter-out $(SRC_DIR)/test_validation.c, $(SRCS))
LIB_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))
PIC_OBJS = $(patsubst $(SRC_DIR)/%.c,$(PIC_DIR)/%.o,$(LIB_SRCS))

TEST_SRC = $(SRC_DIR)/test_validation.c
TEST_BIN = $(BIN_DIR)/test_validation

all: $(LIBRARY) $(SHARED_LIB)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(PIC_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(PIC_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(LIBRARY): $(LIB_OBJS)
	@mkdir -p $(BIN_DIR)
	ar rcs $(LIBRARY) $(LIB_OBJS)
	@echo "📦 Biblioteca estática: $(LIBRARY)"

$(SHARED_LIB): $(PIC_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) -shared -o $@ $(PIC_OBJS) -lm
	@echo "🔗 Biblioteca compartilhada: $(SHARED_LIB)"

shared: $(SHARED_LIB)

test: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(LIBRARY)
	$(CC) $(CFLAGS) $< -L$(BIN_DIR) -limage -lm -o $@
	@echo "🧪 Teste compilado: $(TEST_BIN)"

clean:
	rm -rf $(BUILD_DIR) $(TEST_BIN) $(LIBRARY) $(SHARED_LIB)

.PHONY: all clean test shared
