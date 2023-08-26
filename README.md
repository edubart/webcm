# Cartesi WASM Term

This project allows testing the Cartesi Machine in interactive mode in the browser,
it uses the [Cartesi Machine Emulator SDK 0.16.2](https://github.com/cartesi/machine-emulator-sdk) kernel and rootfs.

## How it works?

The Cartesi Machine was compiled to WASM using Emscripten toolchain.
To have a terminal in the browser the following projects were used:

- https://github.com/mame/xterm-pty
- https://xtermjs.org
