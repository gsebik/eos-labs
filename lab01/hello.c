#include <stdio.h>

#include "pico/stdlib.h"

/* EP-0172 LED used throughout EOS. */
enum { LED = 16 };

/* Startup pause so a monitor attached after flashing still sees the banner. */
enum { BANNER_DELAY_MS = 2000 };

/* One toggle per half period: the LED is high for 500 ms, low for 500 ms. */
enum { TOGGLE_MS = 500 };

int main(void)
{
    stdio_init_all();
    sleep_ms(BANNER_DELAY_MS);

    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);

    printf("EOS L1 hello: station check\n");

    for (uint32_t count = 0;; count++) {
        gpio_xor_mask(1u << LED);
        sleep_ms(TOGGLE_MS);

        /* Printed after the wait, so the first line reports a completed
         * interval rather than the time spent starting up. */
        printf("tick %lu\n", (unsigned long)count);
    }
}
