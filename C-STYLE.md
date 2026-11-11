# C style — the short version

Read this before adding a lab file. `clang-format` enforces the layout; the rest
is a habit. Run `clang-format -i labNN/*.c labNN/*.h` before you commit, and let
`.clang-format` in this repository decide the details.

## Layout

- **4 spaces**, never a tab, **90 columns**. If a line needs a third wrapped
  argument, extract a variable or a helper instead of nesting further.
- **Braces always.** Every `if`, `else`, `for`, `while` and `do` gets braces,
  even for one statement. `InsertBraces: true` adds them for you.
- A function's opening brace goes on its own line; a control statement's brace
  stays on the same line.
- One blank line between logical blocks inside a function, two between
  definitions. Group the steps of an experiment, don't line them up as a wall.
- One declaration per line, and declare it where it is used:

  ```c
  /* yes */
  uint32_t sent = 0;
  uint32_t dropped = 0;

  /* no */
  uint32_t sent = 0, dropped = 0;
  ```

## Names and numbers

- **Named constants, not magic numbers.** A period, a seed, a stack size and a
  pin belong in an `enum` at the top of the file with a one-line comment that
  says what the number means:

  ```c
  enum {
      PRODUCER_PERIOD_MS = 20,  /* one record per release */
      TASK_STACK_WORDS = 512,
  };
  ```

  `pdMS_TO_TICKS(PRODUCER_PERIOD_MS)` reads as an experiment;
  `pdMS_TO_TICKS(20)` reads as a guess.
- `snake_case` for variables and functions, `g_` for globals a debugger or a
  reporter reads, `UPPER_CASE` for constants and macros, `_t` on typedef names.
- Units in the name: `_ms`, `_us`, `_ticks`, `_words`, `_cycles`. A number
  without a unit is a bug waiting for a reviewer.

## Comments say why, not what

```c
/* no: sets the LED */
gpio_xor_mask(1u << LED_A);

/* yes: one masked write toggles the LED and its probe together, so the visible
 * and the captured evidence describe the same instant. */
gpio_xor_mask((1u << LED_A) | (1u << PROBE_A));
```

Explain the experiment, the boundary, the trap. If a comment repeats the code,
delete it. Keep the `TODO NAME:` marker text exactly as the lab guide quotes it —
students search for that string.

## Files and modes

- Include order: standard headers, SDK headers, then FreeRTOS headers, then
  local headers, each group separated by a blank line.
- The lab files carry several experiment modes behind `#if EOS_LAB_MODE == N`.
  Keep one blank line around each block and a banner comment naming the mode.
- Declare state at the top of its mode block, initialise what has a meaningful
  start value, and leave the rest zero.
- FreeRTOS calls that can fail are wrapped in `configASSERT(...)`. A failed
  allocation that silently returns NULL turns into a mystery later.
- `(void)arg;` at the top of every task function: the signature is fixed, the
  parameter is unused.
- Never block while holding a mutex except where the lab deliberately does it to
  widen an observation window — and say so in the comment.

## Python tools

The Python helpers follow the same spirit: 4-space indent, 90 columns, `snake_case`,
no bare numbers, and a docstring that says what question the script answers.
