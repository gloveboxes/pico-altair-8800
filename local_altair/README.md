# Altair Local Runner

`altair-local` is a cut-down host build of the Altair emulator for quick CP/M app testing in a local terminal. It uses the universal 88-DCDD disk controller, reads and writes the terminal through stdio, and routes the file transfer/time/utility ports through host-side port drivers.

Build:

```sh
cmake -S local_altair -B local_altair/build
cmake --build local_altair/build
```

Run:

```sh
./local_altair/build/altair-local
```

The default disks are referenced from the repo `Disks` folder, not copied:

```text
A: Disks/cpm63k.dsk
B: Disks/bdsc-v1.60.dsk
C: Disks/blank.dsk
```

Because the disk images are opened read/write, CP/M writes update those files directly. You can point at alternate images with `--drive-a`, `--drive-b`, and `--drive-c`.

Press `Ctrl-]` to exit the runner and restore the terminal.

File transfer uses the repo `Apps` folder by default, so inside CP/M you can use `FT` in the same style as the MCP build server:

```text
B:
FT -G BREAKOUT/BREAKOUT.SUB
SUBMIT BREAKOUT
```
