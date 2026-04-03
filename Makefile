BUILD_DIR ?= build
DEMO_BIN := $(BUILD_DIR)/core/po32_kick_sequence_demo
WAV ?= demo_kick_160bpm.wav

.PHONY: all configure build render play run

all: run

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release

build: configure
	cmake --build $(BUILD_DIR) --target po32_kick_sequence_demo

render: build
	$(DEMO_BIN) $(WAV)

play: render
	@command -v play >/dev/null 2>&1 || { echo "missing 'play' command (install SoX)"; exit 1; }
	play $(WAV)

run: play
