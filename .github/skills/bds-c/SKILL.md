---
name: bds-c
description: Write, edit, and review BDS C 1.6 code for CP/M/Altair-style projects. Use when working on .C, .H, .SUB, or assembly-adjacent code intended for the BD Software C compiler, especially when the task mentions BDS C, CP/M, Altair 8800, VT100 terminal apps, ANSI terminal control, TFT VT100 color rendering, emulator ports, SDK helpers, or compiler quirks such as 7-character symbols, no casts, K&R declarations, and no block-local declarations.
---

# BDS C

## Core Rules

Treat BDS C as its own language, not as old ANSI C.

- Keep every identifier at 7 characters or less: globals, locals, parameters, functions, labels, macros, struct tags, members, and preprocessor symbols.
- Also ensure no two identifiers collide in their first 7 characters.
- Name BDS C source/header files in all caps, including the extension: `FOO.C`, `FOO.H`.
- Do not use casts. Rewrite with temporary variables, helper functions, or compatible types.
- Use K&R function definitions:

```c
int fn(a, b)
int a;
char *b;
{
    return 0;
}
```

- Declare formal parameters immediately after the function parameter list and before `{`.
- Declare function-local variables only at the start of the function body. BDS C has no block-local variable scope.
- Explicitly type all external/global data definitions.
- Do not use ANSI prototypes, `void`, `typedef`, `const`, `enum`, `signed`, `long long`, `static inline`, `//` comments, or mixed declarations and statements.
- Do not rely on C library headers unless the project already does. Prefer existing project patterns.
- Keep function calls parenthesized, including no-argument calls: `fn()`, never `fn`.
- Avoid side effects in function-call arguments. BDS C evaluates arguments in reverse order.
- Parenthesize constant arithmetic when used inside larger expressions.

## Workflow

1. Read nearby working code first. Match the repo's successful BDS C style over modern preferences.
2. Use the shared libraries in `Apps/SDK/` where applicable — terminal I/O, timers, system/sensor ports, environment variables, string functions, and 32-bit arithmetic are already implemented. Read the `.H` and `.C` files for the API.
3. Check included headers and linked modules for 7-character hazards. If a public helper name exceeds 7 characters, either use an already-working project pattern or write a tiny local helper with a short name.
4. Implement with short, distinct names from the start. Renaming later is where breakage sneaks in.
5. Avoid clever expressions. Prefer simple statements and small helpers.
6. Update `.SUB` files when dependencies change.
7. Run the identifier checker on each BDS C source/header file you changed before finalizing:

```bash
python3 .github/skills/bds-c/scripts/check_bds_c.py path/to/file.c
```

For multiple changed files, pass them all in one command.

8. If host-compiling for syntax, treat modern compiler K&R warnings as expected, but fix real syntax errors. Host compilers do not enforce all BDS C rules.

## Altair CP/M Build Workflow

When the user asks to build, test-build, compile, or smoke-test a CP/M app in this repo, use the `altair-cpm-build` MCP server instead of manually driving CP/M command-by-command.

- Prefer the MCP tool `build_app` for normal app builds. Pass the app folder/base name in lowercase, for example `{"app":"breakout"}` or `{"app":"snake"}`. It returns the full CP/M transcript by default; set `verbose=false` only when a compact summary is enough.
- `build_app` restores fresh disks by default, switches to `B:`, fetches `<app>/<app>.sub`, runs `submit <app>`, advances SuperSUB internally, and stops at the `MCP-TOOL-COMPLETED <APP>` marker.
- Use `run_submit` for arbitrary submit files such as `BUILDALL.SUB`. Example: `{"submit":"buildall","timeout_seconds":10}`. If `fetch` is omitted it tries `<submit>.sub`, then `<submit>/<submit>.sub`.
- Do not inspect the app files first just to discover the build command when the request is simply "test build <app>". Call `build_app` directly.
- Use `run_cpm` only for manual CP/M inspection or debugging: `dir`, checking disk contents, running one-off commands, or recovering from an unusual failed build.
- Use `reset` when the user wants a clean CP/M disk state without running a build.
- Successful submit files should end with `mcpdone <app>`, which prints `MCP-TOOL-COMPLETED <APP>`.
- `build_app` and `run_submit` include elapsed time in milliseconds in their final result line.
- If `build_app` or `run_submit` reports `FAIL` or `INCOMPLETE`, summarize the failing command/output and then use `run_cpm` only if more diagnosis is needed.

## Terminal And Emulator Code

The TFT VT100 display is 80 columns by 30 rows. CP/M apps should treat rows 1-30 as the drawable terminal area. Cursor positions are 1-based.

## Common Hazards

- Struct member names share a constrained namespace. Avoid reusing member/tag names unless matching the compiler's exact allowed case.
- There is no `extern` keyword behavior like modern C. Multi-file external data layout must match exactly and all external variables used by a program must be declared in the source file containing `main`.
- Do not place `#include` inside conditional compilation blocks; BDS C processes includes before conditionals.

## Validation

Run `.github/skills/bds-c/scripts/check_bds_c.py` on each changed BDS C source/header file. The script strips comments and literals, then reports:

- identifiers longer than 7 characters
- first-7-character collisions
- lowercase or mixed-case `.c`/`.h` filenames
- likely casts
- declarations after statements inside functions
- `//` comments and common unsupported keywords

Use it as a guardrail, not a substitute for compiling with BDS C.

## SDK Libraries

The `Apps/SDK/` folder contains shared BDS C libraries for Altair 8800 apps. All public SDK symbols are 7 characters or fewer. Use these SDKs where applicable instead of reimplementing common functionality. Read the corresponding `.H` and `.C` files for the full API.

To download and compile the SDK on CP/M, run `SDK.SUB`. For the string library, run `STRING.SUB`.

- **DXTERM** (`DXTERM.H` / `DXTERM.C`) — Terminal I/O: screen control, cursor movement, keyboard input, key-code tests, and ANSI color/attribute helpers. Include `"dxterm.h"`, link `DXTERM`.
- **DXTIMER** (`DXTIMER.H` / `DXTIMER.C`) — Three hardware timers (0-2) for blocking delays and non-blocking timing (game loops, animations). Include `"dxtimer.h"`, link `DXTIMER`.
- **DXSYS** (`DXSYS.H` / `DXSYS.C`) — System and sensor data via Altair emulator I/O ports: random numbers, version info, uptime, UTC/local time, weather, location, pollution, and Sense HAT sensors. Include `"dxsys.h"`, link `DXSYS`.
- **DXENV** (`DXENV.C`) — Persistent key-value environment variable storage in `A:ALTAIR.ENV`. No header; declare needed functions with forward declarations. Link `DXENV`.
- **STRING** (`STRING.H` / `STRING.C`) — Standard C string and memory functions (`memcpy`, `strlen`, `strcmp`, etc.) implemented for BDS C. Include `"string.h"`, link `STRING`.
- **LONG** (`LONG.C`) — 32-bit signed integer arithmetic (add, subtract, multiply, divide, compare, ASCII conversion). Longs are 4-byte char arrays. Link `LONG`.
