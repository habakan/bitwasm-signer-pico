cmake_minimum_required(VERSION 3.13)
set(PICO_PLATFORM rp2350-riscv)
set(PICO_BOARD pico2)
set(ROOT ${CMAKE_CURRENT_LIST_DIR})
set(SRC ${ROOT}/src)
set(BRINGUP ${ROOT}/bringup)

# Only a build that asks for it can select the test seed
set(TEST_SEED 0 CACHE STRING "allow selecting the BIP39 test vector seed")
set(TESTNET 0 CACHE STRING "testnet / signet addresses and derivation")
include(${ROOT}/third_party/pico-sdk/external/pico_sdk_import.cmake)
project(signer_rp2350 C CXX ASM)
pico_sdk_init()

set(WAMR_ROOT_DIR ${ROOT}/third_party/wasm-micro-runtime)
set(SHARED_PLATFORM_CONFIG ${SRC}/runtime/wamr-platform/shared_platform.cmake)
set(WAMR_BUILD_PLATFORM rp2350)
set(WAMR_BUILD_TARGET RISCV32_ILP32)
if(NOT DEFINED WAMR_BUILD_AOT)
  set(WAMR_BUILD_AOT 0)
endif()
if(WAMR_BUILD_AOT)
  set(WAMR_BUILD_INTERP 0)
else()
  set(WAMR_BUILD_INTERP 1)
endif()
set(WAMR_BUILD_FAST_INTERP 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
# runtime_lib.cmake adds -std=gnu99, but pico-sdk needs C11 for static_assert
string(REPLACE "-std=gnu99" "-std=gnu11" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

# The bring-up application (make build/rp2350/app.elf), running parser.wasm on the interpreter.
# app_nolcd sends the screens' text to the UART instead of the panel, for checking before soldering
set(SECP ${ROOT}/third_party/secp256k1)
if(NOT DEFINED TESTNET)
  set(TESTNET 0)
endif()
# The QR reader, in Rust. Cargo decides for itself whether anything changed, so it always runs.
# zba, zbb and zbs are what the rest of the firmware is built with; the remaps keep paths out of the UF2
set(QR_READER_DIR ${SRC}/runtime/qr_reader)
set(QR_READER_LIB ${CMAKE_CURRENT_BINARY_DIR}/qr_reader/riscv32imac-unknown-none-elf/release/libqr_reader.a)
if(DEFINED ENV{CARGO_HOME})
  set(CARGO_HOME $ENV{CARGO_HOME})
else()
  set(CARGO_HOME $ENV{HOME}/.cargo)
endif()
add_custom_target(qr_reader_lib
  COMMAND ${CMAKE_COMMAND} -E env
    "RUSTFLAGS=-C target-feature=+zba,+zbb,+zbs -C relocation-model=static --remap-path-prefix=${ROOT}=/pico --remap-path-prefix=${CARGO_HOME}=/cargo"
    cargo build --release --locked --target-dir ${CMAKE_CURRENT_BINARY_DIR}/qr_reader
  WORKING_DIRECTORY ${QR_READER_DIR}
  BYPRODUCTS ${QR_READER_LIB}
  USES_TERMINAL)
set(APP_SRC ${SRC}/main.c ${SRC}/runtime/qr_reader.c ${SRC}/drivers/st7789.c
  ${SRC}/drivers/buttons.c ${SRC}/runtime/parser_host.c ${SRC}/ui/ui.c ${ROOT}/third_party/QR-Code-generator/c/qrcodegen.c
  ${ROOT}/components/parts/signer/core.c ${ROOT}/components/parts/signer/address.c ${ROOT}/components/parts/signer/bip32.c ${ROOT}/components/parts/signer/sighash.c ${ROOT}/components/parts/parser/c/src/tx.c
  ${ROOT}/components/parts/parser/c/src/sha256.c ${ROOT}/components/parts/signer/ripemd160.c ${ROOT}/components/parts/signer/sha512.c ${ROOT}/components/parts/signer/secp_callbacks.c ${ROOT}/components/parts/signer/seedqr.c
  ${ROOT}/components/parts/signer/bip85.c ${ROOT}/components/parts/signer/multisig.c ${ROOT}/components/parts/signer/cbor.c ${ROOT}/components/parts/signer/multisig_cbor.c
  ${ROOT}/components/parts/signer/secp256k1_unity.c ${SRC}/drivers/camera.c ${SRC}/drivers/camera_ov7670.c
  ${WAMR_RUNTIME_LIB_SOURCE})
foreach(t app app_nolcd)
  add_executable(${t} ${APP_SRC})
  pico_generate_pio_header(${t} ${SRC}/drivers/camera.pio)
  target_compile_definitions(${t} PRIVATE PICO_BUILD=1 ENABLE_MODULE_EXTRAKEYS=1 ENABLE_MODULE_SCHNORRSIG=1 ENABLE_MODULE_RECOVERY=1
    ECMULT_WINDOW_SIZE=2 USE_EXTERNAL_DEFAULT_CALLBACKS=1 COMB_BLOCKS=2 COMB_TEETH=5 PICO_STACK_SIZE=0x8000
    NO_LCD=$<STREQUAL:${t},app_nolcd> TEST_SEED=${TEST_SEED} TESTNET=${TESTNET}
    # __DATE__ in the binary info would make every UF2 differ from the released one
    PICO_NO_BI_PROGRAM_BUILD_DATE=1)
  target_compile_options(${t} PRIVATE -Wno-unused-function)
  target_include_directories(${t} PRIVATE ${SIGNER_WASM_H_DIR} ${SRC} ${SRC}/drivers ${ROOT}/components/parts/signer
    ${SRC}/runtime ${ROOT}/components/parts/parser/c/include ${SRC}/ui ${ROOT}/third_party/QR-Code-generator/c ${SECP}/include ${SECP}/src
    )
  add_dependencies(${t} qr_reader_lib)
  target_link_libraries(${t} pico_stdlib pico_rand hardware_watchdog hardware_spi hardware_pio hardware_dma hardware_i2c
    hardware_clocks ${QR_READER_LIB})
  pico_enable_stdio_uart(${t} 1)
  pico_enable_stdio_usb(${t} 0)
  target_link_options(${t} PRIVATE -Wl,--print-memory-usage)
  pico_add_extra_outputs(${t})
endforeach()

# Times the QR reader on the hardware (make build/rp2350/qr_bench.elf), from built-in images rather
# than the camera
add_executable(qr_bench ${ROOT}/tests/host/qr_bench.c ${SRC}/runtime/qr_reader.c)
target_compile_definitions(qr_bench PRIVATE PICO_BUILD=1 PICO_STACK_SIZE=0x8000)
target_include_directories(qr_bench PRIVATE ${SIGNER_WASM_H_DIR} ${SRC}/drivers ${SRC}/runtime ${ROOT}/components/parts/signer)
add_dependencies(qr_bench qr_reader_lib)
target_link_libraries(qr_bench pico_stdlib hardware_watchdog ${QR_READER_LIB})
pico_enable_stdio_uart(qr_bench 1)
pico_enable_stdio_usb(qr_bench 0)
target_link_options(qr_bench PRIVATE -Wl,--print-memory-usage)
pico_add_extra_outputs(qr_bench)

# Checks camera.pio's capture with no camera attached (make build/rp2350/pio_loopback_test.elf)
add_executable(pio_loopback_test ${BRINGUP}/pio_loopback_test.c)
pico_generate_pio_header(pio_loopback_test ${SRC}/drivers/camera.pio)
pico_generate_pio_header(pio_loopback_test ${BRINGUP}/dvp_gen.pio)
target_include_directories(pio_loopback_test PRIVATE ${SRC} ${SRC}/drivers)
target_link_libraries(pio_loopback_test pico_stdlib hardware_pio hardware_dma hardware_clocks)
pico_enable_stdio_uart(pio_loopback_test 1)
pico_enable_stdio_usb(pio_loopback_test 0)
pico_add_extra_outputs(pio_loopback_test)

# Checks the wiring (make build/rp2350/button_test.elf)
add_executable(button_test ${BRINGUP}/button_test.c)
target_link_libraries(button_test pico_stdlib)
pico_enable_stdio_uart(button_test 1)
pico_enable_stdio_usb(button_test 0)
pico_add_extra_outputs(button_test)

# Times a full PSBT round on the hardware (make build/rp2350/psbt_bench.elf), with no screen or buttons
add_executable(psbt_bench ${BRINGUP}/psbt_bench.c ${SRC}/runtime/parser_host.c ${SRC}/ui/ui.c
  ${ROOT}/third_party/QR-Code-generator/c/qrcodegen.c ${ROOT}/components/parts/signer/core.c ${ROOT}/components/parts/signer/address.c ${ROOT}/components/parts/signer/bip32.c
  ${ROOT}/components/parts/signer/sighash.c ${ROOT}/components/parts/parser/c/src/tx.c ${ROOT}/components/parts/parser/c/src/sha256.c ${ROOT}/components/parts/signer/ripemd160.c
  ${ROOT}/components/parts/signer/sha512.c ${ROOT}/components/parts/signer/secp_callbacks.c ${ROOT}/components/parts/signer/secp256k1_unity.c
  ${ROOT}/components/parts/signer/seedqr.c ${ROOT}/components/parts/signer/bip85.c ${ROOT}/components/parts/signer/multisig.c
  ${ROOT}/components/parts/signer/cbor.c ${ROOT}/components/parts/signer/multisig_cbor.c ${SRC}/runtime/qr_reader.c
  ${WAMR_RUNTIME_LIB_SOURCE})
if(NOT DEFINED PARSER_POOL_KB)
  set(PARSER_POOL_KB 256)
endif()
target_compile_definitions(psbt_bench PRIVATE PICO_BUILD=1 POOL_KB=${PARSER_POOL_KB} PICO_STACK_SIZE=0x8000
  ENABLE_MODULE_EXTRAKEYS=1 ENABLE_MODULE_SCHNORRSIG=1 ENABLE_MODULE_RECOVERY=1 ECMULT_WINDOW_SIZE=2 USE_EXTERNAL_DEFAULT_CALLBACKS=1
  COMB_BLOCKS=2 COMB_TEETH=5)
target_compile_options(psbt_bench PRIVATE -Wno-unused-function)
add_dependencies(psbt_bench qr_reader_lib)
target_include_directories(psbt_bench PRIVATE ${SIGNER_WASM_H_DIR} ${SRC} ${SRC}/drivers ${ROOT}/components/parts/signer
  ${SRC}/runtime ${ROOT}/components/parts/parser/c/include ${SRC}/ui ${ROOT}/third_party/QR-Code-generator/c ${SECP}/include ${SECP}/src)
target_link_libraries(psbt_bench ${QR_READER_LIB} hardware_watchdog pico_stdlib)
pico_enable_stdio_uart(psbt_bench 1)
pico_enable_stdio_usb(psbt_bench 0)
target_link_options(psbt_bench PRIVATE -Wl,--print-memory-usage)
pico_add_extra_outputs(psbt_bench)

# Camera bring-up (make build/rp2350/camera_test.elf): PIO picks out the Y and the QR reader reads it
add_executable(camera_test ${BRINGUP}/camera_test.c ${SRC}/drivers/camera.c
  ${SRC}/drivers/camera_ov7670.c ${SRC}/drivers/st7789.c
  ${SRC}/runtime/qr_reader.c)
pico_generate_pio_header(camera_test ${SRC}/drivers/camera.pio)
target_compile_definitions(camera_test PRIVATE PICO_STACK_SIZE=0x8000)
target_include_directories(camera_test PRIVATE ${SRC} ${SRC}/drivers ${SRC}/runtime ${ROOT}/components/parts/signer)
add_dependencies(camera_test qr_reader_lib)
target_link_libraries(camera_test ${QR_READER_LIB} hardware_watchdog pico_stdlib hardware_pio hardware_dma hardware_i2c hardware_spi hardware_clocks)
pico_enable_stdio_uart(camera_test 1)
pico_enable_stdio_usb(camera_test 0)
target_link_options(camera_test PRIVATE -Wl,--print-memory-usage)
pico_add_extra_outputs(camera_test)
