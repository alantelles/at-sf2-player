VERSION ?= $(shell cat VERSION 2>/dev/null || echo 1.0.0)

CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fPIC -fvisibility=hidden -MMD -MP -DRELEASE=1 \
            -DPLUGIN_VERSION=\"$(VERSION)\" \
            -Isrc -Ithird_party -Ithird_party/pluginterfaces \
            $(shell pkg-config --cflags x11 cairo pangocairo)
LDFLAGS := -shared -Wl,--no-undefined -static-libstdc++ -static-libgcc \
           $(shell pkg-config --libs x11 cairo pangocairo) -lpthread -lm

BUNDLE_DIR := build/ATSF2Player.vst3
SO_DIR := $(BUNDLE_DIR)/Contents/x86_64-linux
RES_DIR := $(BUNDLE_DIR)/Contents/Resources
TARGET := $(SO_DIR)/ATSF2Player.so
DIST_ZIP := build/ATSF2Player-linux-x86_64.zip

PLUGIN_SRCS := \
	src/synth_engine.cpp \
	src/processor.cpp \
	src/controller.cpp \
	src/view_x11.cpp \
	src/factory.cpp \
	src/vst_iids.cpp \
	third_party/pluginterfaces/base/funknown.cpp \
	third_party/pluginterfaces/base/ustring.cpp \
	third_party/pluginterfaces/base/conststringtable.cpp \
	third_party/pluginterfaces/base/coreiids.cpp

PLUGIN_OBJS := $(patsubst %.cpp,build/obj/%.o,$(PLUGIN_SRCS))
PLUGIN_DEPS := $(PLUGIN_OBJS:.o=.d)

TEST_BIN := build/test_host

.PHONY: all clean test install dist

all: $(TARGET)

$(TARGET): $(PLUGIN_OBJS)
	@mkdir -p $(SO_DIR) $(RES_DIR)
	$(CXX) $(PLUGIN_OBJS) -o $@ $(LDFLAGS)
	cp README.md LICENSE VERSION $(RES_DIR)/

build/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(PLUGIN_DEPS)

$(TEST_BIN): tests/test_host.cpp $(TARGET)
	@mkdir -p build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Isrc -Ithird_party -Ithird_party/pluginterfaces \
		tests/test_host.cpp src/vst_iids.cpp \
		third_party/pluginterfaces/base/funknown.cpp \
		third_party/pluginterfaces/base/ustring.cpp \
		third_party/pluginterfaces/base/conststringtable.cpp \
		third_party/pluginterfaces/base/coreiids.cpp \
		-o $@ -ldl -lpthread

test: $(TEST_BIN)
	./$(TEST_BIN)

install: $(TARGET)
	@mkdir -p $(HOME)/.vst3
	rm -rf $(HOME)/.vst3/ATSF2Player.vst3
	cp -r $(BUNDLE_DIR) $(HOME)/.vst3/

dist: $(TARGET)
	@rm -f $(DIST_ZIP)
	cd build && zip -r ATSF2Player-linux-x86_64.zip ATSF2Player.vst3
	@echo "Created $(DIST_ZIP)"

clean:
	rm -rf build
