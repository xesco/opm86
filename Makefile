# Builds the opm86 command-line program and copies it to the repository root.
#   make          build ./opm86
#   make clean    remove ./opm86 (the build folder is kept)

BUILD_DIR ?= build
CONFIG ?= Release

.PHONY: opm86 clean

opm86: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target opm86_cli --config $(CONFIG) --parallel
	cp $(BUILD_DIR)/opm86_cli_artefacts/$(CONFIG)/opm86 ./opm86

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

clean:
	rm -f ./opm86
