# Builds OPM86 Player and copies it to the repository root.
#   make          build for the system it runs on: make mac, linux or windows
#   make mac      build "./OPM86 Player.app", signed as SIGN_ID if the keychain has it (macOS)
#   make linux    on Linux, build ./opm86-player; on a Mac, build and test in a
#                 Docker container with Ubuntu (build-linux)
#   make windows  build "./OPM86 Player.exe" (Windows, in Git Bash with make)
#   make release VERSION=0.2.0
#                 set the version, tag it v0.2.0 and push: GitHub publishes the release
#   make clean    remove the app from the repository root (the build folders are kept)

BUILD_DIR ?= build
CONFIG ?= Release
# Compiles at once: one per core. JUCE's large files need memory, so a
# machine short of it, a Raspberry Pi 3 with 1 GB, builds with JOBS=1.
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
APPS = $(BUILD_DIR)/opm86_player_artefacts/$(CONFIG)
# A local certificate that keeps the app's identity across builds, so macOS
# remembers its folder permissions; without it the app keeps the ad hoc
# signature, and macOS asks again after every build.
SIGN_ID ?= OPM86 Dev

ifeq ($(OS),Windows_NT)
SYSTEM := windows
else ifeq ($(shell uname -s),Darwin)
SYSTEM := mac
else
SYSTEM := linux
endif

.DEFAULT_GOAL := $(SYSTEM)
.PHONY: mac player linux windows release clean configure

# The build folder, configured the first time.
configure:
	[ -f $(BUILD_DIR)/CMakeCache.txt ] || cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

ifeq ($(SYSTEM),mac)
mac: configure
	cmake --build $(BUILD_DIR) --target opm86_player --config $(CONFIG) --parallel $(JOBS)
	rm -rf "./OPM86 Player.app"
	cp -R "$(APPS)/OPM86 Player.app" "./OPM86 Player.app"
	if security find-identity -p codesigning | grep -q '"$(SIGN_ID)"'; then \
		codesign --force --deep -s "$(SIGN_ID)" "./OPM86 Player.app"; fi

# The container builds in build-linux, with the compiler cache in it too, so a
# second run recompiles only what changed.
linux:
	docker build -q -t opm86-linux -f scripts/linux.Dockerfile . >/dev/null
	docker run --rm -v "$(CURDIR)":/src -w /src -e CCACHE_DIR=/src/build-linux/.ccache opm86-linux sh -c '\
		cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=$(CONFIG) -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache >/dev/null && \
		cmake --build build-linux && \
		ctest --test-dir build-linux --output-on-failure'
else
mac:
	@echo "make mac: the macOS app builds on a Mac only" >&2; exit 1
endif

ifeq ($(SYSTEM),linux)
linux: configure
	cmake --build $(BUILD_DIR) --target opm86_player --config $(CONFIG) --parallel $(JOBS)
	cp "$(APPS)/opm86-player" ./opm86-player
endif

ifeq ($(SYSTEM),windows)
windows: configure
	cmake --build $(BUILD_DIR) --target opm86_player --config $(CONFIG) --parallel $(JOBS)
	cp "$(APPS)/OPM86 Player.exe" "./OPM86 Player.exe"

linux:
	@echo "make linux: build on Linux, or on a Mac with Docker" >&2; exit 1
else
windows:
	@echo "make windows: the Windows app builds on Windows only; a pull request's checks build it too" >&2; exit 1
endif

# The old name of make mac.
player: mac

release:
	scripts/release.sh "$(VERSION)"

clean:
	rm -rf "./OPM86 Player.app" ./opm86-player "./OPM86 Player.exe"
