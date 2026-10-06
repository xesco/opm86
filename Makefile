# Builds the opm86 programs and copies them to the repository root.
#   make          build ./opm86
#   make player   build "./OPM86 Player.app" (macOS)
#   make all      build both
#   make clean    remove ./opm86 and "./OPM86 Player.app" (the build folder is kept)

BUILD_DIR ?= build
CONFIG ?= Release
PLAYER_APP = OPM86 Player.app

.PHONY: opm86 player all clean

opm86: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target opm86_cli --config $(CONFIG) --parallel
	cp $(BUILD_DIR)/opm86_cli_artefacts/$(CONFIG)/opm86 ./opm86

player: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target opm86_player --config $(CONFIG) --parallel
	rm -rf "./$(PLAYER_APP)"
	cp -R "$(BUILD_DIR)/opm86_player_artefacts/$(CONFIG)/$(PLAYER_APP)" "./$(PLAYER_APP)"

all: opm86 player

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

clean:
	rm -f ./opm86
	rm -rf "./$(PLAYER_APP)"
