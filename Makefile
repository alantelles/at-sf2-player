CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fPIC -fvisibility=hidden -MMD -MP -DRELEASE=1 \
            -Isrc -Ithird_party -Ithird_party/pluginterfaces \
            $(shell pkg-config --cflags x11 cairo pangocairo)
LDFLAGS := -shared -Wl,--no-undefined \
           $(shell pkg-config --libs x11 cairo pangocairo) -lpthread -lm

BUNDLE_DIR := build/ATSF2Player.vst3
SO_DIR := $(BUNDLE_DIR)/Contents/x86_64-linux
TARGET := $(SO_DIR)/ATSF2Player.so

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

.PHONY: all clean test install

all: $(TARGET)

$(TARGET): $(PLUGIN_OBJS)
	@mkdir -p $(SO_DIR)
	$(CXX) $(PLUGIN_OBJS) -o $@ $(LDFLAGS)

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

clean:
	rm -rf build
