// GT911 touch controller over I2C0. Registers are 16-bit, big-endian addressed.
#include <stddef.h>
#include "gt911.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define TOUCH_I2C   i2c0
#define PIN_SDA     8
#define PIN_SCL     9
#define PIN_RST     10
#define PIN_INT     11
#define GT911_ADDR  0x5D    // selected by the reset sequence below

// GT911 register map (subset)
#define REG_PID     0x8140  // 4-byte product id ("911\0")
#define REG_STATUS  0x814E  // bit7 = buffer ready, bits3:0 = touch count
#define REG_POINT1  0x8150  // 8 bytes/point: id, xL,xH, yL,yH, sizeL,sizeH, rsv

static int rd(uint16_t reg, uint8_t *buf, size_t len) {
    uint8_t a[2] = { reg >> 8, reg & 0xFF };
    if (i2c_write_blocking(TOUCH_I2C, GT911_ADDR, a, 2, true) < 0) return -1;
    return i2c_read_blocking(TOUCH_I2C, GT911_ADDR, buf, len, false);
}
static int wr(uint16_t reg, uint8_t val) {
    uint8_t a[3] = { reg >> 8, reg & 0xFF, val };
    return i2c_write_blocking(TOUCH_I2C, GT911_ADDR, a, 3, false);
}

int gt911_init(void) {
    i2c_init(TOUCH_I2C, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);

    // Reset with INT held low -> the controller comes up at I2C address 0x5D.
    gpio_init(PIN_INT); gpio_set_dir(PIN_INT, GPIO_OUT); gpio_put(PIN_INT, 0);
    gpio_init(PIN_RST); gpio_set_dir(PIN_RST, GPIO_OUT); gpio_put(PIN_RST, 0);
    sleep_ms(10);
    gpio_put(PIN_INT, 0);
    sleep_us(120);
    gpio_put(PIN_RST, 1);     // release reset; INT level latched as the address bit
    sleep_ms(10);
    gpio_put(PIN_INT, 0);
    sleep_ms(50);
    gpio_set_dir(PIN_INT, GPIO_IN);   // hand INT back to the controller
    sleep_ms(50);

    uint8_t pid[4] = {0};
    if (rd(REG_PID, pid, 4) < 0) return 0;
    return 1;
}

int gt911_status(uint8_t *status)        { return rd(REG_STATUS, status, 1); }
int gt911_point_raw(int i, uint8_t o[8]) { return rd(REG_POINT1 + i * 8, o, 8); }
void gt911_clear(void)                   { wr(REG_STATUS, 0); }
int gt911_product_id(uint8_t out[4])     { return rd(REG_PID, out, 4); }

int gt911_read(gt911_point_t *pts, int max) {
    uint8_t st;
    if (rd(REG_STATUS, &st, 1) < 0) return -1;
    // The ready bit (0x80) is set only when a NEW sample exists. If it's clear
    // there is simply no fresh data this poll — that is NOT "finger lifted", so
    // return -1 and let the caller keep the finger state unchanged. A genuine
    // lift arrives as a fresh sample (ready bit set) whose count is 0.
    if (!(st & 0x80)) return -1;
    int n = st & 0x0F;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        uint8_t d[8];
        if (rd(REG_POINT1 + i * 8, d, 8) < 0) { n = i; break; }
        // Layout (verified on the EP-0172 panel): X@0:1, Y@2:3, size@4:5 (all
        // little-endian), track id @7. Range matches the display: 0..319 / 0..479.
        pts[i].x    = (uint16_t)(d[0] | (d[1] << 8));
        pts[i].y    = (uint16_t)(d[2] | (d[3] << 8));
        pts[i].size = (uint16_t)(d[4] | (d[5] << 8));
        pts[i].id   = d[7];
    }
    wr(REG_STATUS, 0);                     // tell the controller we consumed the buffer
    return n;
}
