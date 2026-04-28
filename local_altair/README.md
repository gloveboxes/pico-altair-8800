# Altair Local Runner

`altair-local` is a cut-down host build of the Altair emulator for quick CP/M app testing in a local terminal. It uses the universal 88-DCDD disk controller, reads and writes the terminal through stdio, and routes the file transfer/time/utility ports through host-side port drivers.

Build:

```sh
cmake -S local_altair -B local_altair/build
cmake --build local_altair/build
```

Windows native MSVC build from a "Developer PowerShell for VS" prompt:

```powershell
cmake -S local_altair -B local_altair/build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build local_altair/build-msvc --config Release
```

For Windows on Arm64, use the Arm64 Visual Studio generator platform:

```powershell
cmake -S local_altair -B local_altair/build-msvc-arm64 -G "Visual Studio 17 2022" -A ARM64
cmake --build local_altair/build-msvc-arm64 --config Release
```

For the full Windows host-tool build flow, including `local_altair` and `mcp_app_build_server`, run the shared helper from the repo root or the `scripts` folder:

```powershell
.\scripts\build-host-tools-windows.cmd
```

That script builds both Windows host binaries and writes the `local_altair` output to the matching MSVC build folder.

Build the executable once with the commands above or with `scripts\build-host-tools-windows.cmd`. The VS Code task `start local altair - Windows` only launches the already-built executable and does not rebuild it.

Run:

```sh
./local_altair/build/altair-local
```

On Windows, run the generated executable from the matching build configuration, for example:

```powershell
.\local_altair\build-msvc\Release\altair-local.exe
```

On Windows on Arm64, run:

```powershell
.\local_altair\build-msvc-arm64\Release\altair-local.exe
```

The launch helper used by the VS Code task is:

```powershell
.\scripts\start-local-altair-windows.cmd
```

Windows amd64 requirements:

- Windows 10 version 1903 or newer, or Windows 11, with Windows Terminal recommended for ANSI output.
- Visual Studio 2022 Build Tools or Visual Studio 2022 with the "Desktop development with C++" workload.
- CMake, either from the Visual Studio installer or a separate CMake install on `PATH`.
- Git for Windows if cloning the repository on Windows.

Windows Arm64 requirements:

- Windows 11 on Arm64, with Windows Terminal recommended for ANSI output.
- Visual Studio 2022 17.4 or newer, or Build Tools for Visual Studio 2022, with the "Desktop development with C++" workload and MSVC Arm64 build tools installed.
- CMake, either from the Visual Studio installer or a separate Arm64 or x64 CMake install on `PATH`.
- Git for Windows Arm64, or regular Git for Windows under x64 emulation.

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
