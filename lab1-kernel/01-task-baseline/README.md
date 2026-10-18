# L1 · Exercise 01 — Two tasks, one scheduler

**Type:** guided build · **Code:** `main.c` · **Build target:** `l1_task_baseline`

Read the full student procedure in the course docs under **Labs → Lab 1 — Build, Predict, Diagnose → Exercise 01**. Complete Task B there, record your prediction and evidence, then use the commands below from the `eos-labs` repository root.

```sh
cmake -S lab1-kernel/01-task-baseline -B build/01-task-baseline -G Ninja
cmake --build build/01-task-baseline
```

After capturing the serial output as `session.log`:

```sh
python3 tools/check.py lab1-kernel/01-task-baseline/check.toml session.log
```
