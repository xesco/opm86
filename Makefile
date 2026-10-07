# Builds OPM86 Player and copies it to the repository root.
#   make          build "./OPM86 Player.app" (macOS)
#   make clean    remove "./OPM86 Player.app" (the build folder is kept)

BUILD_DIR ?= build
CONFIG ?= Release
PLAYER_APP = OPM86 Player.app

.PHONY: player clean

player: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target opm86_player --config $(CONFIG) --parallel
	rm -rf "./$(PLAYER_APP)"
	cp -R "$(BUILD_DIR)/opm86_player_artefacts/$(CONFIG)/$(PLAYER_APP)" "./$(PLAYER_APP)"

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

clean:
	rm -rf "./$(PLAYER_APP)"
