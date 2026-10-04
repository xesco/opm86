# Builds the opm86 programs and copies them to the repository root.
#   make          build ./opm86
#   make tracker  build "./OPM86 Tracker.app" (macOS)
#   make all      build both
#   make clean    remove ./opm86 and "./OPM86 Tracker.app" (the build folder is kept)

BUILD_DIR ?= build
CONFIG ?= Release
TRACKER_APP = OPM86 Tracker.app

.PHONY: opm86 tracker all clean

opm86: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target opm86_cli --config $(CONFIG) --parallel
	cp $(BUILD_DIR)/opm86_cli_artefacts/$(CONFIG)/opm86 ./opm86

tracker: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target opm86_tracker --config $(CONFIG) --parallel
	rm -rf "./$(TRACKER_APP)"
	cp -R "$(BUILD_DIR)/opm86_tracker_artefacts/$(CONFIG)/$(TRACKER_APP)" "./$(TRACKER_APP)"

all: opm86 tracker

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

clean:
	rm -f ./opm86
	rm -rf "./$(TRACKER_APP)"
