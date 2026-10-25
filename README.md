# EOS Labs — lab source

Source for the Embedded and Operating Systems labs. One directory per lab; the
course sequence and the student instructions live in the course documentation.

| Lab | Directory | Guide | Build targets |
|---|---|---|---|
| L1 | [`lab01/`](lab01/) | Kernel and timing | `lab01_calibration`, `lab01_mode1`…`lab01_mode4` |
| L2 | [`lab02/`](lab02/) | Communication and sharing | `lab02_mode1`…`lab02_mode4` |
| L3 | [`lab03/`](lab03/) | Spectrum instrument | `lab03_instrument_rtos`, `lab03_instrument_foreground`, `lab03_priority_inheritance` |
| L4 | [`lab04/`](lab04/) | Multicore | `lab04_mode1`…`lab04_mode3` |
| L5 | — | Networking | built from the guide |

L5 has no starter directory: that lab builds its project from code in the guide,
because the radio and lwIP configuration are part of what it teaches.

The primary station is a Pico 2 W on the EP-0172 carrier with a PicoGremlin.
L1–L4 build against **FreeRTOS V11.3.1** with its RP2350 Arm port; set
`FREERTOS_KERNEL_PATH` to that checkout (the lab manual shows how to get it).

## Getting the source

```sh
git clone https://github.com/gsebik/eos-labs.git
cd eos-labs
```

Students work from the clone. Each guide names the directory to build and the
target names above; builds need no board, and building is not hardware
verification.

## Report images

L3 also ships prebuilt images for its reported faults. They are release
downloads, not clone contents: the repository holds the source, the release holds
the binaries. Each image has a SHA-256 in the sheet that accompanies it.

## What is not here

No solutions, no private marking notes and no credentials. Reference firmware and
answer keys are supplied separately by the lecturer.
