# install cjson-devel lmdb-devel
CC := gcc

BUILD := build

# === Разделяем output для разных конфигураций ===
OBJDIR_DEBUG   := $(BUILD)/debug/obj
BINDIR_DEBUG   := $(BUILD)/debug/bin
TESTDIR_DEBUG  := $(BUILD)/debug/tests

OBJDIR_RELEASE := $(BUILD)/release/obj
BINDIR_RELEASE := $(BUILD)/release/bin

TARGET_DEBUG   := $(BINDIR_DEBUG)/evolution_core
TARGET_RELEASE := $(BINDIR_RELEASE)/evolution_core

# ==========================================================
# Compiler Flags
# ==========================================================

CFLAGS_DEBUG := \
	-std=gnu11 \
	-Wall -Wextra -Wpedantic \
	-Wshadow \
	-Wformat=2 \
	-Wconversion \
	-Wnull-dereference \
	-g \
	-O0 \
	-fsanitize=address,undefined \
	-march=native \
	-mtune=native \
	-fPIC \
	-Iinclude \
	-Isrc \
	-MMD -MP

CFLAGS_RELEASE := \
	-std=gnu11 \
	-Wall -Wextra -Wpedantic \
	-Wshadow \
	-O3 \
	-flto \
	-march=native \
	-mtune=native \
	-fomit-frame-pointer \
	-fPIC \
	-Iinclude \
	-Isrc \
	-MMD -MP

LDFLAGS_DEBUG := \
	-llmdb \
	-lcurl \
	-lcjson \
	-lm \
	-lpthread \
	-ldl \
	-Wl,--export-dynamic \
	-fsanitize=address,undefined

LDFLAGS_RELEASE := \
	-llmdb \
	-lcurl \
	-lcjson \
	-lm \
	-lpthread \
	-ldl \
	-Wl,--export-dynamic \
	-flto

# ==========================================================
# Sources
# ==========================================================
# SRC_MAIN выделен отдельно, чтобы его можно было исключить из общей библиотеки объектов
SRC_MAIN := src/main.c
SRC_LIB := $(filter-out $(SRC_MAIN),$(shell find src -name '*.c'))
OBJ_LIB_DEBUG   := $(patsubst src/%.c,$(OBJDIR_DEBUG)/%.o,$(SRC_LIB))
OBJ_LIB_RELEASE := $(patsubst src/%.c,$(OBJDIR_RELEASE)/%.o,$(SRC_LIB))
OBJ_MAIN_DEBUG   := $(patsubst src/%.c,$(OBJDIR_DEBUG)/%.o,$(SRC_MAIN))

TEST_SRC := $(shell find tests -name '*_test.c')
TEST_OBJ_DEBUG := $(patsubst tests/%.c,$(OBJDIR_DEBUG)/tests/%.o,$(TEST_SRC))
TEST_BIN_DEBUG := $(patsubst tests/%_test.c,$(TESTDIR_DEBUG)/%_test,$(TEST_SRC))

# ==========================================================
# Build Modes
# ==========================================================

.PHONY: all debug release clean run test

all: release

release: $(TARGET_RELEASE)

debug: $(TARGET_DEBUG)

# ==========================================================
# Main executable
# ==========================================================
# Цель debug
$(TARGET_DEBUG): $(OBJ_LIB_DEBUG) $(OBJ_MAIN_DEBUG)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS_DEBUG)
	@echo "[OK] Built $(TARGET_DEBUG)"

# Цель release
$(TARGET_RELEASE): $(OBJ_LIB_RELEASE)
	@mkdir -p $(dir $@)
# Для main.c используем флаги release, но собираем его отдельно, чтобы не дергать лишнего
	$(CC) $(CFLAGS_RELEASE) -c $(SRC_MAIN) -o $(OBJDIR_RELEASE)/main.o
	$(CC) $^ $(OBJDIR_RELEASE)/main.o -o $@ $(LDFLAGS_RELEASE)
	@echo "[OK] Built $(TARGET_RELEASE)"

# ==========================================================
# Object compilation
# ==========================================================

# Паттерн для всех .c -> .o. Переменные окружения цели задаются в основных целях debug/release/test.
# Важно: мы используем здесь OBJDIR и CFLAGS, которые будут определены в зависимостях.
# Чтобы это работало, нам нужно передавать их через target-specific variables.

# --- Debug objects ---
# Говорим make: "для всех файлов, НУЖНЫХ для TARGET_DEBUG, используем OBJDIR_DEBUG и CFLAGS_DEBUG"
$(OBJDIR_DEBUG)/%.o: OBJDIR := $(OBJDIR_DEBUG)
$(OBJDIR_DEBUG)/%.o: CFLAGS := $(CFLAGS_DEBUG)
$(OBJDIR_DEBUG)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR_DEBUG)/tests/%.o: OBJDIR := $(OBJDIR_DEBUG)
$(OBJDIR_DEBUG)/tests/%.o: CFLAGS := $(CFLAGS_DEBUG)
$(OBJDIR_DEBUG)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# --- Release objects ---
$(OBJDIR_RELEASE)/%.o: OBJDIR := $(OBJDIR_RELEASE)
$(OBJDIR_RELEASE)/%.o: CFLAGS := $(CFLAGS_RELEASE)
$(OBJDIR_RELEASE)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ==========================================================
# Test executables (используем debug-флаги и папки)
# ==========================================================

$(TESTDIR_DEBUG)/%_test: $(OBJDIR_DEBUG)/tests/%_test.o $(OBJ_LIB_DEBUG)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS_DEBUG)

# Отдельная переменная для линковки тестов
$(OBJDIR_DEBUG)/tests/%_test.o: CFLAGS := $(CFLAGS_DEBUG)

test: $(TEST_BIN_DEBUG)
	@echo ""
	@echo "==============================="
	@echo "Running C tests..."
	@echo "==============================="
	@for t in $(TEST_BIN_DEBUG); do \
		echo ""; \
		echo ">>> $$t"; \
		$$t || exit $$?; \
	done
	@echo ""
	@echo "[OK] All C tests passed."
	@echo ""
	@echo "==============================="
	@echo "Running Python end-to-end test..."
	@echo "==============================="
	python3 tests/end_to_end_test.py || exit $$?
	@echo ""
	@echo "[OK] All tests passed."

# ==========================================================
# Run
# ==========================================================

run: debug
	$(TARGET_DEBUG)

# ==========================================================
# Clean
# ==========================================================

clean:
	rm -rf $(BUILD)
	rm -rf ./test_*_db
	@echo "[CLEAN]"

# ==========================================================
# === ВОЛШЕБСТВО С АВТОЗАВИСИМОСТЯМИ ===
# ==========================================================
# Мы должны подключать .d-файлы для каждой конфигурации отдельно.
# Поскольку OBJDIR_DEBUG и OBJDIR_RELEASE теперь разные, мы можем просто
# найти их все. Ключ '-' заставляет make игнорировать ошибку, если .d файлов еще нет.

# Собираем все возможные .d-файлы из всех OBJDIR
ALL_D_FILES := $(shell find $(BUILD) -name '*.d' 2>/dev/null)
-include $(ALL_D_FILES)
