all:
	ninja-build -C build

install:
	ninja-build -C build install

clean:
	ninja-build -C build clean

run:
	PIPEWIRE_MODULE_DIR=build \
	PIPEWIRE_CONFIG_FILE=build/pipewire/daemon/pipewire.conf \
	build/pipewire/daemon/pipewire
