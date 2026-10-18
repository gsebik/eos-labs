# L1 · Exercise 03 — Find the issue: the monitor is silent

**Type:** diagnose + fix + verify · **Code:** `main.c` · **Build target:** `l1_find_starvation`

Read **Exercise 03** in the course-docs Lab 1 guide before changing the code. The guide contains the complete problem statement, evidence requirements, questions, and collapsed hints. The starter is intentionally faulty; reference diagnosis and repair are kept in the separate solutions repository.

```sh
cmake -S lab1-kernel/03-find-starvation -B build/03-find-starvation -G Ninja
cmake --build build/03-find-starvation
```

Capture serial output as `session.log`, then check it:

```sh
python3 tools/check.py lab1-kernel/03-find-starvation/check.toml session.log
```
