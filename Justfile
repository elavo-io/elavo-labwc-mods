# Build labwc with the mods layer. The mirror checkout is build/labwc.
build:
    ./scripts/assemble.sh

version: build
    ./build/labwc/build/labwc --version
