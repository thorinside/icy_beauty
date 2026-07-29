PLUGIN_NAME := icy_beauty
SOURCE := icy_beauty.cpp
API_DIR := distingNT_API
INCLUDE_FLAGS := -I$(API_DIR)/include
COMMON_FLAGS := -std=c++11 -fno-rtti -fno-exceptions -Wall -Wextra -Werror

ARM_CXX ?= arm-none-eabi-c++
ARM_NM ?= arm-none-eabi-nm
ARM_READELF ?= arm-none-eabi-readelf
NATIVE_CXX ?= c++

PLUGIN := plugins/$(PLUGIN_NAME).o
NATIVE_TEST := build/$(PLUGIN_NAME)_test

ARM_FLAGS := $(COMMON_FLAGS) -mcpu=cortex-m7 -mfpu=fpv5-d16 \
	-mfloat-abi=hard -mthumb -fPIC -Os -ffunction-sections -fdata-sections
NATIVE_FLAGS := $(COMMON_FLAGS) -O2

all: hardware

hardware: $(PLUGIN)

$(PLUGIN): $(SOURCE) $(API_DIR)/include/distingnt/api.h
	@mkdir -p $(@D)
	$(ARM_CXX) $(ARM_FLAGS) $(INCLUDE_FLAGS) -c -o $@ $(SOURCE)
	@echo "Built disting NT plugin: $@"

$(NATIVE_TEST): $(SOURCE) tests/icy_beauty_test.cpp $(API_DIR)/include/distingnt/api.h
	@mkdir -p $(@D)
	$(NATIVE_CXX) $(NATIVE_FLAGS) $(INCLUDE_FLAGS) -o $@ \
		tests/icy_beauty_test.cpp

test: $(NATIVE_TEST)
	./$(NATIVE_TEST)

endurance: $(NATIVE_TEST)
	./$(NATIVE_TEST) --endurance

inspect: $(PLUGIN)
	@$(ARM_READELF) -h $(PLUGIN) | grep -Eq 'Type:[[:space:]]+REL' \
		|| (echo "Plugin is not a relocatable object" >&2; exit 1)
	@$(ARM_READELF) -h $(PLUGIN) | grep -Eq 'Machine:[[:space:]]+ARM' \
		|| (echo "Plugin is not built for ARM" >&2; exit 1)
	@$(ARM_NM) --defined-only $(PLUGIN) | grep -Eq '[[:space:]]pluginEntry$$' \
		|| (echo "pluginEntry export is missing" >&2; exit 1)
	@unexpected="$$( $(ARM_NM) -u $(PLUGIN) | awk '{print $$2}' | \
		grep -Ev '^(NT_globals|_GLOBAL_OFFSET_TABLE_|exp2f)$$' || true )"; \
		test -z "$$unexpected" \
		|| (echo "Unexpected undefined symbols: $$unexpected" >&2; exit 1)
	@echo "PASS: $(PLUGIN) is a relocatable ARM plugin with pluginEntry"
	@$(ARM_NM) -u $(PLUGIN)

verify: clean hardware test inspect

clean:
	rm -rf build plugins

.PHONY: all clean endurance hardware inspect test verify
