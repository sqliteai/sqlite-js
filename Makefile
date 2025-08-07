# Makefile for SQLite JavaScript Extension
# Supports compilation for Linux, macOS, Windows, Android and iOS

# Set default platform if not specified
ifeq ($(OS),Windows_NT)
	PLATFORM := windows
	HOST := windows
	CPUS := $(shell powershell -Command "[Environment]::ProcessorCount")
else
	HOST = $(shell uname -s | tr '[:upper:]' '[:lower:]')
	ifeq ($(HOST),darwin)
		PLATFORM := macos
		CPUS := $(shell sysctl -n hw.ncpu)
	else
		PLATFORM := $(HOST)
		CPUS := $(shell nproc)
	endif
endif

# Speed up builds by using all available CPU cores
MAKEFLAGS += -j$(CPUS)

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
	STRIP = strip --strip-unneeded $@
else ifeq ($(PLATFORM),macos)
	TARGET := $(DIST_DIR)/js.dylib
	LDFLAGS := -arch x86_64 -arch arm64 -dynamiclib -undefined dynamic_lookup
	# macOS-specific flags
	CFLAGS += -arch x86_64 -arch arm64
	STRIP = strip -x -S $@
else ifeq ($(PLATFORM),android)
	ifndef ARCH # Set ARCH to find Android NDK's Clang compiler, the user should set the ARCH
		$(error "Android ARCH must be set to ARCH=x86_64 or ARCH=arm64-v8a")
	endif
	ifndef ANDROID_NDK # Set ANDROID_NDK path to find android build tools; e.g. on MacOS: export ANDROID_NDK=/Users/username/Library/Android/sdk/ndk/25.2.9519653
		$(error "Android NDK must be set")
	endif
	BIN = $(ANDROID_NDK)/toolchains/llvm/prebuilt/$(HOST)-x86_64/bin
	ifneq (,$(filter $(ARCH),arm64 arm64-v8a))
		override ARCH := aarch64
	endif
	CC = $(BIN)/$(ARCH)-linux-android26-clang
	TARGET := $(DIST_DIR)/js.so
	LDFLAGS := -lm -shared
	STRIP = $(BIN)/llvm-strip --strip-unneeded $@
else ifeq ($(PLATFORM),ios)
	TARGET := $(DIST_DIR)/js.dylib
	SDK := -isysroot $(shell xcrun --sdk iphoneos --show-sdk-path) -miphoneos-version-min=11.0
	LDFLAGS := -dynamiclib $(SDK)
	# iOS-specific flags
	CFLAGS += -arch arm64 $(SDK)
	STRIP = strip -x -S $@
else ifeq ($(PLATFORM),ios-sim)
	TARGET := $(DIST_DIR)/js.dylib
	SDK := -isysroot $(shell xcrun --sdk iphonesimulator --show-sdk-path) -miphonesimulator-version-min=11.0
	LDFLAGS := -arch x86_64 -arch arm64 -dynamiclib $(SDK)
	# iphonesimulator-specific flags
	CFLAGS += -arch x86_64 -arch arm64 $(SDK)
	STRIP = strip -x -S $@
else # linux
	TARGET := $(DIST_DIR)/js.so
	LDFLAGS := -lm -shared
	STRIP = strip --strip-unneeded $@
endif

# Object files
OBJ_FILES := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(SRC_FILES)))

# Make sure the build and dist directories exist
$(shell mkdir -p $(BUILD_DIR) $(DIST_DIR))

# Main target
all: $(TARGET)
extension: $(TARGET)

# Link the final target
$(TARGET): $(OBJ_FILES) $(DEF_FILE)
	$(CC) -o $@ $^ $(LDFLAGS)
ifeq ($(PLATFORM),windows)
	# Generate import library for Windows
	dlltool -D $@ -d $(DEF_FILE) -l $(DIST_DIR)/js.lib
endif
	# Strip debug symbols
	$(STRIP)

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
	$(CC) $(INCLUDES) $^ -o $@ libs/sqlite3.c -DSQLITE_CORE

# Testing the extension
test: $(TARGET) $(TEST_TARGET)
	sqlite3 ":memory:" -cmd ".bail on" ".load ./$<" "SELECT js_eval('console.log(\"hello, world\nToday is\", new Date().toLocaleDateString())');"
	./$(TEST_TARGET)

.NOTPARALLEL: %.dylib
%.dylib:
	rm -rf $(BUILD_DIR) && $(MAKE) PLATFORM=$*
	mv $(DIST_DIR)/js.dylib $(DIST_DIR)/$@

define PLIST
<?xml version=\"1.0\" encoding=\"UTF-8\"?>\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\
<plist version=\"1.0\">\
<dict>\
<key>CFBundleDevelopmentRegion</key>\
<string>en</string>\
<key>CFBundleExecutable</key>\
<string>js</string>\
<key>CFBundleIdentifier</key>\
<string>ai.sqlite.js</string>\
<key>CFBundleInfoDictionaryVersion</key>\
<string>6.0</string>\
<key>CFBundlePackageType</key>\
<string>FMWK</string>\
<key>CFBundleSignature</key>\
<string>????</string>\
<key>CFBundleVersion</key>\
<string>$(shell make version)</string>\
<key>CFBundleShortVersionString</key>\
<string>$(shell make version)</string>\
<key>MinimumOSVersion</key>\
<string>11.0</string>\
</dict>\
</plist>
endef

LIB_NAMES = ios.dylib ios-sim.dylib macos.dylib
FMWK_NAMES = ios-arm64 ios-arm64_x86_64-simulator macos-arm64_x86_64
$(DIST_DIR)/%.xcframework: $(LIB_NAMES)
	@$(foreach i,1 2 3,\
		lib=$(word $(i),$(LIB_NAMES)); \
		fmwk=$(word $(i),$(FMWK_NAMES)); \
		mkdir -p $(DIST_DIR)/$$fmwk/js.framework; \
		printf "$(PLIST)" > $(DIST_DIR)/$$fmwk/js.framework/Info.plist; \
		mv $(DIST_DIR)/$$lib $(DIST_DIR)/$$fmwk/js.framework/js; \
		install_name_tool -id "@rpath/js.framework/js" $(DIST_DIR)/$$fmwk/js.framework/js; \
	)
	xcodebuild -create-xcframework $(foreach fmwk,$(FMWK_NAMES),-framework $(DIST_DIR)/$(fmwk)/js.framework) -output $@
	rm -rf $(foreach fmwk,$(FMWK_NAMES),$(DIST_DIR)/$(fmwk))

xcframework: $(DIST_DIR)/js.xcframework

version:
	@echo $(shell sed -n 's/^#define SQLITE_JS_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' src/sqlitejs.h)

# Help message
help:
	@echo "SQLite JavaScript Extension Makefile"
	@echo "Usage:"
	@echo "  make  [PLATFORM=platform] [ARCH=arch] [ANDROID_NDK=\$$ANDROID_HOME/ndk/26.1.10909125] [target]"
	@echo ""
	@echo "Platforms:"
	@echo "  linux (default on Linux)"
	@echo "  macos (default on macOS)"
	@echo "  windows (default on Windows)"
	@echo "  android (needs ARCH to be set to x86_64 or arm64-v8a and ANDROID_NDK to be set)"
	@echo "  ios (only on macOS)"
	@echo "  ios-sim (only on macOS)"
	@echo ""
	@echo "Targets:"
	@echo "  all		- Build the extension (default)"
	@echo "  clean		- Remove built files"
	@echo "  install	- Install the extension"
	@echo "  test		- Test the extension"
	@echo "  help		- Display this help message"

.PHONY: all extension clean install test help version xcframework
