#include <stdio.h>

#include "pico/stdlib.h"

/* EP-0172 LED used throughout EOS. */
enum { LED = 16 };

int main(void)
{
    stdio_init_all();
    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);

    printf("EOS L1 hello: station check\n");
    for (uint32_t count = 0;; count++) {
        gpio_xor_mask(1u << LED);
        sleep_ms(500);
        printf("tick %lu\n", (unsigned long)count);
    }
}
