#include "qr_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include "camera.h"
#include "core.h"
#include "hardware/watchdog.h"
#include "wipe.h"

/* Without an FPU a frame that nearly reads takes seconds at the library's full budget, while the
 * camera is already holding the next one. 15 keeps the worst frame near 2.3s */
#define READ_BUDGET 15
#define FRAME_SIZE ((size_t)CAMERA_W * CAMERA_H)

size_t qr_reader_state_size(void);
size_t qr_reader_output_cap(void);
void qr_reader_state_init(void *state);
int32_t qr_reader_decode(void *state, const uint8_t *px, size_t w, size_t h, bool seedqr, bool animated, uint8_t *out,
                         uint32_t budget);

static uint8_t *memory, *output;
static size_t memory_size;

bool qr_reader_open(void) {
    /* The frame first: malloc's alignment is what the camera's DMA and the state both need */
    memory_size = FRAME_SIZE + qr_reader_output_cap() + qr_reader_state_size() + 8;
    if (!(memory = malloc(memory_size))) return false;
    output = memory + FRAME_SIZE;
    qr_reader_state_init((void *)(((uintptr_t)output + qr_reader_output_cap() + 7) & ~(uintptr_t)7));
    return true;
}

uint8_t *qr_reader_frame(void) {
    return memory;
}

int32_t qr_reader_read(qr_reader_kind_t kind) {
    void *state = (void *)(((uintptr_t)output + qr_reader_output_cap() + 7) & ~(uintptr_t)7);
    return qr_reader_decode(state, memory, CAMERA_W, CAMERA_H, kind == QR_READER_SEEDQR, kind == QR_READER_ANIMATED,
                            output, READ_BUDGET);
}

const uint8_t *qr_reader_output(void) {
    return output;
}

void qr_reader_close(void) {
    if (!memory) return;
    wipe(memory, memory_size);
    free(memory);
    memory = output = NULL;
}

/* Called from Rust's panic handler. A reset keeps RAM, so the frame and the key are wiped first */
void qr_reader_panic(void) {
    qr_reader_close();
    core_unload();
    printf("QR reader panicked, restarting\n");
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}
