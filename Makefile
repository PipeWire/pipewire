all:
	ninja-build -C build

install:
	ninja-build -C build install

clean:
	ninja-build -C build clean

run:
	PINOS_MODULE_DIR=build \
	PINOS_CONFIG_FILE=build/pinos/daemon/pinos.conf \
	build/pinos/daemon/pinos
