# Feasibility and performance measurements

This page combines the RP2350 memory and performance studies for the current device, QR decoding,
and earlier experiments with running the complete signer in WebAssembly. The signer WASM results are
historical: the current firmware runs the signing core natively and uses WASM for transaction parsing.

## Current device

The current application uses about 346KB of the RP2350's 520KB SRAM. QR decoding and PSBT processing
share the heap: the QR reader is released after scanning, before the PSBT buffers are allocated.

| Item | Size | Basis |
|---|---:|---|
| WAMR pool, including parser.wasm memory and UR assembly | 160KB | 152,792 bytes maximum measured use |
| parser.wasm copy in RAM | 16KB | Rewritten by the interpreter while loading |
| Shared heap | 108KB | 107,872 bytes peak measured with `mallinfo` |
| Stack | 32KB reserved | 23KB measured; the default 2KB is insufficient |
| UI, qrcodegen, core, secp256k1, pico-sdk and other use | 30KB | Remainder of the application |
| **Total** | **about 346KB / 520KB** | |

The shared heap holds about 106KB for the QR reader while scanning, 77KB of it the frame, then about
68KB for PSBT buffers. Without sharing, the peak would be about 173KB. About 174KB remains;
double-buffering the camera would use another 77KB.

## QR decoding on the RP2350

The firmware links the QR reader from `bitwasm-signer`, in Rust, with its `small` feature, a read
budget of 15 and `opt-level="s"`. The measurements are from a Pico 2 H at 150MHz.

### Synthetic frames

`tools/generate/gen_qr_frames.py` puts one UR QR code into a 320×240 grayscale image, with a 5°
rotation, Gaussian blur, noise and uneven lighting. Run them with
`make run ELF=build/rp2350/qr_bench.elf`.

| QR | Read | Time when it reads |
|---|---|---|
| v5 | 6 of 6 | 0.29-0.31s |
| v8 | 5 of 6 | 0.39-0.40s |
| v11 | 3 of 6 | 0.42-0.43s |

A frame that does not read takes at most about 2s. The next part of an animated v8 code, read through
the previous frame's position, takes about 0.07s, and the same frame seen again about 0.04s.

### The real camera

An animated PSBT at v6, shown at 4 frames a second on a display, completed in all three runs, including
one where the code left the frame and came back. It took 9-16 seconds from the first read. 69 of 108
frames read, at about 240ms, with frames arriving every 384ms. A frame with nothing readable took a
median of 88-484ms and at most 1.9s. A Standard SeedQR read to the expected fingerprint. Other wallets'
codes and larger versions have not been tried.

A key is only checked between frames and needs three checks in a row, so a short press can be missed
during a scan. The scan screen asks for a hold.

### What this rests on

- **No FPU:** the RISC-V cores have none, so the reader keeps floats out of its hot loops: pattern
  search and sampling work in integers.
- **Flash cache:** `opt-level=3` was slower than `"s"`, with about 214,000 cache misses against 8,700
  on one v5 read.
- **WASM:** `qr.wasm` on the interpreter took 14-27s per frame and about 160KB, around 30 times slower
  than native, so decoding stays native and in the TCB.
- **Reader limits:** a larger budget reads more on hard frames, at the cost of seconds per frame. The
  QVGA limit makes the reader refuse codes above v15.

QR generation is not the bottleneck: drawing a v11 QR on the 240×240 LCD took under 14 million
instructions. For animated output, 100-byte fragments keep frames at v8. Larger PSBTs, especially
those carrying full previous transactions, can require many frames.

## Earlier experiments: signer and key derivation in WASM

The following measurements use the former `bitcoin-signer.wasm`, removed from bitwasm-signer v0.1.0. They
are retained as historical evidence for the performance and memory trade-offs. They do not describe
the current firmware, whose signing core is native.

### QEMU instruction counts (RV32)

| Operation | RV32 native | WAMR classic | WAMR fast | Classic + host SHA-512 |
|---|---:|---:|---:|---:|
| PBKDF2 (BIP39 seed) | 66.4M | 1,693M | 1,030M | 300M |
| BIP32 `m/84'/0'/0'/0` | 2.2M | 161M | 95M | 154M |
| BIP32 `m/84'/0'/0'/0/0` | 3.2M | 239M | 142M | 231M |
| ECDSA signing | 1.0M | 82M | 49M | 82M |

For the standalone signer study, classic interpreter configurations measured 82.2M instructions for
ECDSA and 153.5M for Schnorr with a 2KB `ecmult_gen` table. A 22KB table reduced those counts to
63.5M and 116.0M at a 20KB RAM cost. Fast interpreter measured 48.5M and 90.7M.

PBKDF2's 64-bit SHA-512 arithmetic made the interpreter especially slow. Importing only the
SHA-512 compression function reduced PBKDF2 from 1,693M to 300M instructions; BIP32 improved little
because public-key calculation dominated its cost. At an estimated CPI of 1–1.5, classic plus host
SHA-512 put seed generation at 2–3 seconds and signing one input at 2.1–3.1 seconds. Caching the
account key would reduce repeated derivation work.

### Pico 2 H measurements (150MHz, 2026-09-25)

| Operation | Classic interpreter | Classic + host SHA-512 | AOT XIP | AOT expanded into RAM |
|---|---:|---:|---:|---:|
| ECDSA signature | 0.696s | 0.696s | 0.319s | **0.041s** |
| Schnorr signature | 1.297s | 1.298s | 0.622s | **0.078s** |
| BIP39 seed (PBKDF2, 2048 rounds) | 14.81s | 3.053s | 0.979s | **0.832s** |
| BIP32 `m/84'/0'/0'/0/0` | 2.029s | 1.960s | 0.937s | **0.122s** |
| Runtime initialization | 16.9ms | 16.9ms | 2.5ms | 1.4ms |
| Flash | 125.4KB | 124.2KB | 228.6KB | 228.1KB |
| RAM | 135.3KB | 133.0KB | **68.9KB** | 347.5KB |
| Maximum WAMR pool | 36.7KB | 36.0KB | 34.9KB | 277.3KB |

All four configurations produced signatures, seeds, and public keys matching QEMU and Mac native
results. The interpreter's measured CPI was 1.27 (1.35 for PBKDF2). AOT expanded into RAM measured
CPI 1.08 and followed QEMU instruction counts. AOT XIP measured CPI 7.5 for secp256k1 paths because
the 150KB generated code exceeded the 16KB XIP cache; PBKDF2 stayed near CPI 1.1 because its SHA-512
compression loop fit in cache. Thus QEMU instruction counts alone did not predict XIP performance.

The AOT QEMU instruction counts were:

| Operation | Native | Classic interpreter | AOT, not XIP | AOT XIP (default) | AOT XIP, more runtime calls |
|---|---:|---:|---:|---:|---:|
| ECDSA signature | 1.05M | 82.2M | 5.68M | 6.42M | 12.2M |
| Schnorr signature | 1.89M | 153M | 10.9M | 12.2M | 23.3M |
| PBKDF2 (BIP39) | 66.4M | 1,693M | 119M | 136M | 177M |
| BIP32 `m/84'/0'/0'/0/0` | 3.19M | 239M | 16.9M | 19.0M | 35.9M |
| Maximum WAMR pool | - | 36.7KB | 272KB | 35.1KB | 35.1KB |

Setting `--bounds-checks=0` reduced ECDSA to 4.33M, PBKDF2 to 97.0M, and BIP32 to 12.9M instructions,
but is not suitable for an MCU security boundary. The default `WAMRC_FLAGS` used `--xip` and limited
runtime calls to operations such as i64 division and remainder; wamrc's broader RISC-V defaults made
the workloads about twice as slow.

RAM-expanded AOT reduced the complete 1-input, 2-output P2WPKH round to about 0.2 seconds, while AOT
XIP took about 1.5 seconds. RAM-expanded code used 347.5KB overall, including a 277.3KB WAMR pool.
That left too little headroom for QR capture buffers in the former whole-signer design. This memory
trade-off, the TCB cost of the AOT loader and build-time LLVM, and the current parser-only WASM
boundary informed the current architecture.

### Runtime and memory constraints found

- The classic interpreter rewrites bytecode while loading, so the WASM image needs a RAM copy.
- WASM data segments, including libsecp256k1 constant tables, occupy linear memory. The study used
  `ECMULT_GEN_KB=2` (`COMB_BLOCKS=2`, `COMB_TEETH=5`) instead of the 86KB default.
- WAMR's shrunk-memory mode can avoid allocating whole 64KB pages when `memory.grow` is unused and
  `__heap_base` / `__data_end` are exported.
- On RV32, libsecp256k1's 10x26 field arithmetic uses many 64-bit operations, which WAMR handles in
  software.
- WAMR classic interpreter's `i64.store` implementation assumed 4-byte alignment; Hazard3 faults on
  some unaligned stores while QEMU does not. The fix from
  [WAMR PR #5123](https://github.com/wasm-micro-runtime/wasm-micro-runtime/pull/5123) is included in
  the pinned revision.
- The RP2350's default 2KB stack was too small; the firmware used a 16KB stack.
- WAMR links libm functions such as `ceil` and `sqrt` even when the signer itself does not use floating
  point, adding them to the trusted computing base.
- RISC-V AOT code written to RAM needs `fence.i`; the firmware added this through `os_icache_flush`.
  The measured QEMU AOT target used `generic-rv32` with `m,a,c,zba,zbb,zbs`, not Hazard3's
  `zbkb`, `zcb`, or `zcmp` extensions.

### Historical signer reproduction

These commands target the removed `bitcoin-signer.wasm`. Check out commit `f2f61c6` before running
them; they do not run against the current tree.

```sh
make check-host
make check-qemu-native
make check-qemu POOL_KB=64
make check-qemu FAST=1 POOL_KB=256
uv run tools/ref_kdf.py
make build/wamrc/wamrc
```

For the former AOT hardware build, the study used `AOT=1`, `RP2350_POOL_KB=320`, and
`--target=riscv32 --target-abi=ilp32 --cpu=generic-rv32 --cpu-features=+m,+a,+c,+zba,+zbb,+zbs
--bounds-checks=1`. Rebuilds required deleting generated `bitcoin-signer.wasm`, AOT, and header files
when switching configurations.
