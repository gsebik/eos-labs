#ifndef EOS_LABS_CHECK_H
#define EOS_LABS_CHECK_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

/* Machine-readable records; values remain visible to students in the console. */
#define EOS_CHECK_U32(name, pass, value, unit) \
    printf("@@CHECK %s %s %" PRIu32 " %s\n", (name), (pass) ? "PASS" : "FAIL", \
           (uint32_t)(value), (unit))

#define EOS_CHECK_I32(name, pass, value, unit) \
    printf("@@CHECK %s %s %" PRId32 " %s\n", (name), (pass) ? "PASS" : "FAIL", \
           (int32_t)(value), (unit))

#endif
