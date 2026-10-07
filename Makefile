# Builds OPM86 Player and copies it to the repository root.
#   make          build "./OPM86 Player.app" (macOS)
#   make linux    build and test on Linux, in a Docker container (build-linux)
#   make clean    remove "./OPM86 Player.app" (the build folders are kept)

BUILD_DIR ?= build
CONFIG ?= Release
PLAYER_APP = OPM86 Player.app

.PHONY: player linux clean

player: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target opm86_player --config $(CONFIG) --parallel
	rm -rf "./$(PLAYER_APP)"
	cp -R "$(BUILD_DIR)/opm86_player_artefacts/$(CONFIG)/$(PLAYER_APP)" "./$(PLAYER_APP)"

# The container builds in build-linux, with the compiler cache in it too, so a
# second run recompiles only what changed.
linux:
	docker build -q -t opm86-linux -f scripts/linux.Dockerfile . >/dev/null
	docker run --rm -v "$(CURDIR)":/src -w /src -e CCACHE_DIR=/src/build-linux/.ccache opm86-linux sh -c '\
		cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=$(CONFIG) -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache >/dev/null && \
		cmake --build build-linux && \
		ctest --test-dir build-linux --output-on-failure'

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

clean:
	rm -rf "./$(PLAYER_APP)"
