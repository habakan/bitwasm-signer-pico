#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "qr_frames.h"
#include "qr_reader.h"

int main(void) {
    int pass = 0, count = sizeof(frames) / sizeof(frames[0]);

    stdio_init_all();
    printf("\nqr_bench: %dx%d\n", FRAME_W, FRAME_H);
    if (!qr_reader_open()) return printf("QR reader failed\n"), 1;
    printf("reader_heap %d\n", mallinfo().uordblks);
    for (int i = 0; i < count; i++) {
        const struct frame *f = &frames[i];
        memcpy(qr_reader_frame(), f->pix, FRAME_W * FRAME_H); /* on the device the camera's DMA writes straight here */
        uint64_t t0 = time_us_64();
        int32_t read = qr_reader_read(QR_READER_ANY);
        uint64_t t = time_us_64() - t0;
        int ok = read > 0 && (size_t)read == strlen(f->text) && !memcmp(qr_reader_output(), f->text, (size_t)read);
        pass += ok;
        printf("%-14s v%-2d read=%d %s us=%llu\n", f->name, f->version, (int)read, ok ? "ok" : "NG",
               (unsigned long long)t);
    }
    printf("pass %d/%d\ndone\n", pass, count);
    while (1) tight_loop_contents();
}
