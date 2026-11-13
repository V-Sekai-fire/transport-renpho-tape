import Lake
open Lake DSL System

package «renpho-tape» where

lean_lib RenphoTape where

/-! Per-platform BLE backend, all behind the same C ABI (`lean_ble_*`):
- macOS: `ffi/ble_ffi.m`, CoreBluetooth.
- Linux: `ffi/ble_ffi_linux.c`, BlueZ over D-Bus with GIO. Pass the GLib build flags
  with `-Kcflags=...` and absolute library paths with `-Klinklibs=...` (see
  `scripts/linux-flags.sh`): Lean's bundled lld searches no distro directories.
- Windows: `ffi/ble_ffi_win.c` loads `renpho_ble_win.dll` (WinRT, built with MSVC by
  `scripts/build-win-dll.bat`) from beside the executable. -/

def words (s : String) : Array String :=
  ((s.splitOn " ").filter (· != "")).toArray

def extraCflags : Array String := words ((get_config? cflags).getD "")
def extraLinkLibs : Array String := words ((get_config? linklibs).getD "")

/-- macOS SDK root: Lean's bundled linker does not search the system framework
directories. Override with `-Ksdk=/path/to/MacOSX.sdk`. -/
def sdkRoot : String :=
  (get_config? sdk).getD
    "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"

def bleSource : String :=
  if Platform.isOSX then "ble_ffi.m"
  else if Platform.isWindows then "ble_ffi_win.c"
  else "ble_ffi_linux.c"

/-- The FFI is compiled with the system compiler: Lean's own clang has no system
headers (gcc from MinGW on Windows, to match Lean's ABI). -/
target ble_ffi.o pkg : FilePath := do
  let oFile := pkg.buildDir / "ffi" / "ble_ffi.o"
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / bleSource
  let compiler := if Platform.isWindows then "gcc" else "cc"
  let flags : Array String :=
    if Platform.isOSX then #["-fPIC", "-fobjc-arc", "-O2", "-Wall"]
    else if Platform.isWindows then #["-O2", "-Wall", "-I", (pkg.dir / "ffi").toString]
    else #["-fPIC", "-O2", "-Wall"] ++ extraCflags
  buildO oFile srcJob #["-I", (← getLeanIncludeDir).toString] flags compiler getLeanTrace

extern_lib libble_ffi pkg := do
  let ffiO ← ble_ffi.o.fetch
  buildStaticLib (pkg.buildDir / "lib" / nameToStaticLib "ble_ffi") #[ffiO]

def linkArgs : Array String :=
  if Platform.isOSX then
    #[s!"-F{sdkRoot}/System/Library/Frameworks", s!"-L{sdkRoot}/usr/lib",
      "-framework", "CoreBluetooth", "-framework", "Foundation",
      "-framework", "CoreFoundation", "-lobjc"]
  else if Platform.isWindows then #[]
  else extraLinkLibs

@[default_target]
lean_exe «renpho-tape» where
  root := `Main
  moreLinkArgs := linkArgs
