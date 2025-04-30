# Makefile for SQLite JavaScript Extension
# Supports compilation for Linux, macOS, Windows, Android and iOS

# Set default platform if not specified
ifeq ($(OS),Windows_NT)
    PLATFORM := windows
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM := macos
    else
        PLATFORM := linux
    endif
endif

# Directories
SRC_DIR := src
LIB_DIR := libs
BUILD_DIR := build
DIST_DIR := dist

# Source files
SRC_FILES := $(SRC_DIR)/sqlitejs.c $(LIB_DIR)/quickjs.c

# Include directories
INCLUDES := -I$(SRC_DIR) -I$(LIB_DIR)

# Compiler and flags
CC := gcc
CFLAGS := -Wall -Wextra -fPIC -g -O2 -DQJS_BUILD_LIBC $(INCLUDES)

# Platform-specific settings
ifeq ($(PLATFORM),windows)
    TARGET := $(DIST_DIR)/js.dll
    LDFLAGS := -shared
    # Create .def file for Windows
    DEF_FILE := $(BUILD_DIR)/js.def
else ifeq ($(PLATFORM),macos)
    TARGET := $(DIST_DIR)/js.dylib
    LDFLAGS := -dynamiclib -undefined dynamic_lookup
    # macOS-specific flags
    CFLAGS += -arch x86_64 -arch arm64
else ifeq ($(PLATFORM),android)
    # Use Android NDK's Clang compiler, the user should set the CC
    # example CC=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang
    ifeq ($(filter %-clang,$(CC)),)
        $(error "CC must be set to the Android NDK's Clang compiler")
    endif
    TARGET := $(DIST_DIR)/js.so
    LDFLAGS := -shared -lm
    # Android-specific flags
    CFLAGS += -D__ANDROID__
else ifeq ($(PLATFORM),ios)
    TARGET := $(DIST_DIR)/js.dylib
    SDK := -isysroot $(shell xcrun --sdk iphoneos --show-sdk-path) -miphoneos-version-min=11.0
    LDFLAGS := -dynamiclib $(SDK)
    # iOS-specific flags
    CFLAGS += -arch arm64 $(SDK)
else ifeq ($(PLATFORM),isim)
    TARGET := $(DIST_DIR)/js.dylib
    SDK := -isysroot $(shell xcrun --sdk iphonesimulator --show-sdk-path) -miphonesimulator-version-min=11.0
    LDFLAGS := -dynamiclib $(SDK)
    # iphonesimulator-specific flags
    CFLAGS += -arch x86_64 -arch arm64 $(SDK)
else # linux
    TARGET := $(DIST_DIR)/js.so
    LDFLAGS := -shared
endif

# Object files
OBJ_FILES := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(SRC_FILES)))

# Make sure the build and dist directories exist
$(shell mkdir -p $(BUILD_DIR) $(DIST_DIR))

# Main target
all: $(TARGET)

# Link the final target
$(TARGET): $(OBJ_FILES) $(DEF_FILE)
	$(CC) $(LDFLAGS) -o $@ $^
ifeq ($(PLATFORM),windows)
	# Generate import library for Windows
	dlltool -D $@ -d $(DEF_FILE) -l $(DIST_DIR)/js.lib
endif

# Compile source files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(LIB_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Windows .def file generation
$(DEF_FILE):
ifeq ($(PLATFORM),windows)
	@echo "LIBRARY js.dll" > $@
	@echo "EXPORTS" >> $@
	@echo "    sqlite3_js_init" >> $@
	@echo "    sqlitejs_version" >> $@
	@echo "    quickjs_version" >> $@
endif

# Clean up
clean:
	rm -rf $(BUILD_DIR)/* $(DIST_DIR)/*

# Install the extension (adjust paths as needed)
install: $(TARGET)
ifeq ($(PLATFORM),windows)
	mkdir -p $(DESTDIR)/usr/local/lib/sqlite3
	cp $(TARGET) $(DESTDIR)/usr/local/lib/sqlite3/
	cp $(DIST_DIR)/js.lib $(DESTDIR)/usr/local/lib/
else ifeq ($(PLATFORM),macos)
	mkdir -p $(DESTDIR)/usr/local/lib/sqlite3
	cp $(TARGET) $(DESTDIR)/usr/local/lib/sqlite3/
else # linux
	mkdir -p $(DESTDIR)/usr/local/lib/sqlite3
	cp $(TARGET) $(DESTDIR)/usr/local/lib/sqlite3/
endif

# Test source files
TEST_FILES := test/main.c

# Test target files
ifeq ($(PLATFORM),windows)
	TEST_TARGET := $(patsubst %.c,$(DIST_DIR)/%.exe,$(notdir $(TEST_FILES)))
else
	TEST_TARGET := $(patsubst %.c,$(DIST_DIR)/%,$(notdir $(TEST_FILES)))
endif

# Compile test target
$(TEST_TARGET): $(TEST_FILES) $(TARGET)
	$(CC) $(INCLUDES) $^ -lm -o $@ libs/sqlite3.c -DSQLITE_CORE

# Testing the extension
test: $(TARGET) $(TEST_TARGET)
	sqlite3 ":memory:" -cmd ".bail on" ".load ./$<" "SELECT js_eval('console.log(\"hello, world\nToday is\", new Date().toLocaleDateString())');"
	./$(TEST_TARGET)

# Help message
help:
	@echo "SQLite JavaScript Extension Makefile"
	@echo "Usage:"
	@echo "  make [PLATFORM=platform] [target]"
	@echo ""
	@echo "Platforms:"
	@echo "  linux (default on Linux)"
	@echo "  macos (default on macOS)"
	@echo "  windows (default on Windows)"
	@echo "  android (needs CC to be set to Android NDK's Clang compiler)"
	@echo "  ios (only on macOS)"
	@echo "  isim (only on macOS)"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build the extension (default)"
	@echo "  clean     - Remove built files"
	@echo "  install   - Install the extension"
	@echo "  test      - Test the extension"
	@echo "  help      - Display this help message"

.PHONY: all clean install test help
