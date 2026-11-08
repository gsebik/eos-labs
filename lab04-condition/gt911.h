// Minimal GT911 capacitive-touch driver for the 52Pi EP-0172.
// I2C0: SDA=GP8 SCL=GP9, RST=GP10, INT=GP11 (confirmed on the 52Pi wiki).
#pragma once
#include <stdint.h>

typedef struct {
    uint16_t x, y, size;
    uint8_t  id;
} gt911_point_t;

// Reset + bring up the controller. Returns 1 if it answers on I2C, else 0.
int gt911_init(void);

// Read currently-touched points into pts[0..max-1].
//   returns  0..max  = a FRESH sample with this many fingers (0 = just lifted)
//   returns  -1       = no fresh sample this read (controller hasn't updated yet)
// Most polls return -1 because you poll far faster than the controller reports;
// treat -1 as "nothing new", NOT as "finger up", or strokes break into dots.
int gt911_read(gt911_point_t *pts, int max);

// --- low-level primitives (used for diagnostics / calibration) ---
int  gt911_status(uint8_t *status);          // read 0x814E (bit7=ready, bits0:3=count)
int  gt911_point_raw(int i, uint8_t out[8]); // raw 8 bytes of touch point i
void gt911_clear(void);                       // clear the status flag (write 0 to 0x814E)
int  gt911_product_id(uint8_t out[4]);        // 4-byte product id (should read "911")
