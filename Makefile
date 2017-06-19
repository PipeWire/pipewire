all:
	ninja-build -C build

install:
	ninja-build -C build install

clean:
	ninja-build -C build clean

run:
	SPA_PLUGIN_DIR=build/spa/plugins \
	PIPEWIRE_MODULE_DIR=build \
	PIPEWIRE_CONFIG_FILE=build/pipewire/daemon/pipewire.conf \
	build/pipewire/daemon/pipewire

monitor:
	SPA_PLUGIN_DIR=build/spa/plugins \
	build/pipewire/tools/pipewire-monitor

dist:
	git archive --prefix=pipewire-0.1.0/ -o pipewire-0.1.0.tar.gz HEAD

rpm: dist
	rpmbuild -ta pipewire-0.1.0.tar.gz
