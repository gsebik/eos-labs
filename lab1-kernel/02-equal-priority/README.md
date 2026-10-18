# L1 · Exercise 02 — Equal-priority CPU sharing

**Type:** predict + measure · **Code:** `main.c` · **Build target:** `l1_equal_priority`

Follow **Exercise 02** in the course-docs Lab 1 guide. Compare the default tick-sharing build with `EOS_EXPLICIT_YIELD=ON`; capture GP18/GP19 for each variant.

```sh
cmake -S lab1-kernel/02-equal-priority -B build/02-equal-priority-tick -G Ninja
cmake --build build/02-equal-priority-tick
cmake -S lab1-kernel/02-equal-priority -B build/02-equal-priority-yield -G Ninja -DEOS_EXPLICIT_YIELD=ON
cmake --build build/02-equal-priority-yield
```

Optional pico-sim image: configure with `-DEOS_SIMULATOR_IMAGE=ON` in a separate build directory. Simulator time is not board timing.
