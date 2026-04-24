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
2. Check included headers and linked modules for 7-character hazards. If a public helper name exceeds 7 characters, either use an already-working project pattern or write a tiny local helper with a short name.
3. Implement with short, distinct names from the start. Renaming later is where breakage sneaks in.
4. Avoid clever expressions. Prefer simple statements and small helpers.
5. Update `.SUB` files when dependencies change.
6. Run the identifier checker on each BDS C source/header file you changed before finalizing:

```bash
python3 .github/skills/bds-c/scripts/check_bds_c.py path/to/file.c
```

For multiple changed files, pass them all in one command.

7. If host-compiling for syntax, treat modern compiler K&R warnings as expected, but fix real syntax errors. Host compilers do not enforce all BDS C rules.

## Terminal And Emulator Code

For Altair/VT100 apps:

- Prefer simple BIOS/BDOS wrappers with names under 7 chars, such as `outch`, `pstr`, `cur`, `cls`, `hide`, `show`, `rst`.
- Use literal VT100 escape sequences when that avoids long SDK names. The repo's TFT VT100 emulator and xterm.js path both consume the same byte stream from CP/M apps.
- Keyboard and timer ports are often accessed directly in these apps; keep helper names short and document port choices briefly.
- If using SDK functions, verify the symbol names are safe for the compiler and linker in that build path.
- Cursor positions are 1-based for app helpers such as `x_curmv(row,col)` and for `ESC[row;colH`.

### VT100 Control Sequences

Prefer tiny local helpers when an SDK name is longer than 7 characters or when a file is not already using `DXTERM.H`.

Common output sequences:

- Clear screen and reset attributes: `"\033[2J\033[0m"`, then home with `"\033[1;1H"`.
- Move cursor: `"\033[%d;%dH"` using 1-based row and column.
- Hide/show cursor: `"\033[?25l"` and `"\033[?25h"`.
- Erase to end of line: `"\033[K"`.
- Reset attributes: `"\033[0m"`.
- Save/restore cursor: `"\033[s"` and `"\033[u"`.
- Move relative: `"\033[A"`, `"\033[B"`, `"\033[C"`, `"\033[D"` with an optional numeric prefix.

The TFT VT100 display is 80 columns by 30 rows. The bottom hardware status bar is outside the terminal grid, so CP/M apps should treat rows 1-30 as the drawable terminal area.

### ANSI Color

Use SGR color codes rather than direct TFT APIs from CP/M apps. The TFT VT100 renderer stores a 16-color foreground/background attribute per character cell, so color changes affect subsequent printed cells until reset or changed again.

The normal palette used by the TFT VT100 renderer:

- Palette `0`: black, RGB `0,0,0`, foreground `30`, background `40`
- Palette `1`: red, RGB `170,0,0`, foreground `31`, background `41`
- Palette `2`: green, RGB `0,170,0`, foreground `32`, background `42`
- Palette `3`: yellow/brown, RGB `170,85,0`, foreground `33`, background `43`
- Palette `4`: blue, RGB `0,0,170`, foreground `34`, background `44`
- Palette `5`: magenta, RGB `170,0,170`, foreground `35`, background `45`
- Palette `6`: cyan, RGB `0,170,170`, foreground `36`, background `46`
- Palette `7`: white/light grey, RGB `170,170,170`, foreground `37`, background `47`

The bright palette:

- Palette `8`: bright black/grey, RGB `85,85,85`, foreground `90`, background `100`
- Palette `9`: bright red, RGB `255,85,85`, foreground `91`, background `101`
- Palette `10`: bright green, RGB `85,255,85`, foreground `92`, background `102`
- Palette `11`: bright yellow, RGB `255,255,85`, foreground `93`, background `103`
- Palette `12`: bright blue, RGB `85,85,255`, foreground `94`, background `104`
- Palette `13`: bright magenta, RGB `255,85,255`, foreground `95`, background `105`
- Palette `14`: bright cyan, RGB `85,255,255`, foreground `96`, background `106`
- Palette `15`: bright white, RGB `255,255,255`, foreground `97`, background `107`

Useful attribute sequences:

- Reset all attributes: `ESC[0m`; this returns to foreground palette `7` on background palette `0`.
- Reset foreground only: `ESC[39m`; reset background only: `ESC[49m`.
- Bold/bright foreground: `ESC[1m`; normal intensity: `ESC[22m`.
- Reverse video: `ESC[7m`; this swaps foreground and background in the renderer.
- Multiple attributes can be combined, such as `ESC[1;33m` for bright yellow foreground or `ESC[37;44m` for light text on blue.

Repo helper conventions:

- `DXTERM.H` defines foreground constants such as `XC_RED` (`31`), `XC_CYN` (`36`), `XC_BYEL` (`93`), and `XC_RST` (`0`).
- `x_setc(code)` emits `ESC[code m`; it can emit any single SGR code, including background codes such as `44` or bright background codes such as `103`.
- `x_rstc()` emits `ESC[0m`.
- Local game helpers often use short names such as `setfg(c)`, `setbg(c)`, `col(c)`, `bg(c)`, `rst()`, `c256(c)`, and `b256(c)` to stay under BDS C symbol limits.

For xterm 256-color:

- Foreground: `ESC[38;5;Nm`
- Background: `ESC[48;5;Nm`
- The TFT renderer accepts these but approximates them to the nearest 16-color palette. For example, Breakout's orange `208` becomes a yellow/red-family palette color on the TFT, not a true 256-color orange.
- Use 16-color SGR codes when exact visual intent matters across xterm.js, serial terminals, and the TFT.

When drawing colored game blocks, print spaces with a background color, usually two spaces per logical cell for squarer pixels. Prefer high-contrast backgrounds such as `41` red, `42` green, `43` yellow, `44` blue, `45` magenta, `46` cyan, `47` white, and `100`-`107` bright backgrounds. Always reset attributes before normal text, after clearing colored regions, and before exiting.

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
