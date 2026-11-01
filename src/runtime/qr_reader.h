#ifndef RP2350_QR_READER_H
#define RP2350_QR_READER_H
#include <stdbool.h>
#include <stdint.h>

/* What qr_reader_read gives besides a length */
#define QR_READER_NONE 0
#define QR_READER_UNREADABLE (-1)
#define QR_READER_WRONG_KIND (-3)

typedef enum { QR_READER_ANY, QR_READER_SEEDQR, QR_READER_ANIMATED } qr_reader_kind_t;

bool qr_reader_open(void);
/* Where the camera writes CAMERA_W x CAMERA_H bytes of greyscale */
uint8_t *qr_reader_frame(void);
int32_t qr_reader_read(qr_reader_kind_t kind);
const uint8_t *qr_reader_output(void);
/* Wipes the frame and everything the read left behind, then releases the memory */
void qr_reader_close(void);

#endif
