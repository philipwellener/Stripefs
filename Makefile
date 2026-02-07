CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
FUSE_CFLAGS = $(shell pkg-config --cflags fuse3)
FUSE_LDFLAGS = $(shell pkg-config --libs fuse3)
LDFLAGS = $(FUSE_LDFLAGS) -lpthread

SRC_DIR = src
TEST_DIR = tests

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/stripefs_ops.c \
       $(SRC_DIR)/stripe.c \
       $(SRC_DIR)/cache.c \
       $(SRC_DIR)/stats.c

OBJS = $(SRCS:.c=.o)

# Default target
.PHONY: all
all: stripefs

# Main binary
stripefs: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files with FUSE flags
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c -o $@ $<

# Debug build with AddressSanitizer
.PHONY: debug
debug: CFLAGS += -g -O0 -DDEBUG -fsanitize=address -fno-omit-frame-pointer
debug: LDFLAGS += -fsanitize=address
debug: stripefs

# Release build
.PHONY: release
release: CFLAGS += -O2 -DNDEBUG
release: stripefs

# Unit tests — these don't need FUSE
test_cache: $(TEST_DIR)/test_cache.c $(SRC_DIR)/cache.c $(SRC_DIR)/stats.c
	$(CC) $(CFLAGS) -g -I$(SRC_DIR) -o $@ $^ -lpthread

test_stripe: $(TEST_DIR)/test_stripe.c $(SRC_DIR)/stripe.c $(SRC_DIR)/stats.c
	$(CC) $(CFLAGS) -g -I$(SRC_DIR) -o $@ $^ -lpthread

.PHONY: test
test: test_cache test_stripe
	@echo "=== Running cache unit tests ==="
	./test_cache
	@echo ""
	@echo "=== Running stripe unit tests ==="
	./test_stripe
	@echo ""
	@echo "All unit tests passed."

.PHONY: integration
integration: release
	./tests/test_integration.sh

.PHONY: stress
stress: release
	./tests/test_stress.sh

.PHONY: benchmark
benchmark: release
	./scripts/benchmark.sh

.PHONY: clean
clean:
	rm -f stripefs test_cache test_stripe $(SRC_DIR)/*.o

.PHONY: format
format:
	clang-format -i $(SRC_DIR)/*.c $(SRC_DIR)/*.h $(TEST_DIR)/*.c
