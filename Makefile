# Makefile for SQLite JavaScript Extension
# Supports compilation for Linux, macOS, and Windows

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
CFLAGS := -Wall -Wextra -fPIC -g -O2 $(INCLUDES)

# Platform-specific settings
ifeq ($(PLATFORM),windows)
    TARGET := $(DIST_DIR)/js.dll
    LDFLAGS := -shared
    CC := gcc
    # Windows-specific flags
    CFLAGS += -D_WIN32
    # Create .def file for Windows
    DEF_FILE := $(BUILD_DIR)/js.def
else ifeq ($(PLATFORM),macos)
    TARGET := $(DIST_DIR)/js.dylib
    LDFLAGS := -dynamiclib -undefined dynamic_lookup
    # macOS-specific flags
    CFLAGS += -arch x86_64 -arch arm64
else # linux
    TARGET := $(DIST_DIR)/js.so
    LDFLAGS := -shared
    # Linux-specific flags
    CFLAGS += -fvisibility=hidden
endif

# Object files
OBJ_FILES := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(SRC_FILES)))

# Make sure the build and dist directories exist
$(shell mkdir -p $(BUILD_DIR) $(DIST_DIR))

# Main target
all: $(TARGET)

# Link the final target
$(TARGET): $(OBJ_FILES)
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

# Testing the extension
test: $(TARGET)
	sqlite3 ":memory:" -cmd ".load ./$(TARGET)" "SELECT 1;"

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
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build the extension (default)"
	@echo "  clean     - Remove built files"
	@echo "  install   - Install the extension"
	@echo "  test      - Test the extension"
	@echo "  help      - Display this help message"

.PHONY: all clean install test help
