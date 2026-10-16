# Browser demo

[Open the live demo](https://habakan.github.io/bitwasm-signer-pico/).

The page runs the Pico firmware's `main.c`, UI, signing core, and `parser.wasm` in a browser. Emscripten builds them into a single HTML file. The adapter in `hal.c` sends LCD frames to a canvas, maps keyboard and button input to firmware keys, and supplies camera frames when scanning a PSBT.

## Try it

- Use the arrow keys or on-screen pad to navigate; press Enter or Space to confirm.
- Press Escape or the Close button to go back or reject.
- Choose **Use test seed**, then **Sign test PSBT** to try a signing round.
- **Scan PSBT** requests camera access to read an animated QR. Camera access requires a secure page such as the hosted HTTPS demo or localhost.

## Build locally

With the repository dependencies and Emscripten available, run:

```sh
make web
```

The output is `build/web/signer.html`. Serve it from localhost or HTTPS when testing camera access.

## Safety

The demo is built with `TEST_SEED=1` and is intended for public test seeds. Manual mnemonic/passphrase entry is disabled in the browser build. It is not a signing device and does not provide hardware isolation; never scan a real SeedQR or use real funds with it.

## Source files

- `CMakeLists.txt` builds the firmware and WAMR runtime with Emscripten.
- `shell.html` provides the browser controls and canvas around the generated module.
- `hal.c` adapts the Pico display, buttons, camera, and timing calls to browser APIs.
- `../mk/web.mk` defines the `make web` target.
