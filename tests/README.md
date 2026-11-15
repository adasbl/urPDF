# Tests

The `urPDF.UnitTests` project contains unit tests for urPDF. It uses the Microsoft
Native Unit Test Framework included with Visual Studio's Desktop development
with C++ workload.

The unit tests run without starting the WinUI application or loading MuPDF.
They require Visual Studio 2026's C++ toolset `v145` and Windows SDK
`10.0.26100.0`, and use C++20, matching the application project. vcpkg integration
is explicitly disabled for this project, so local vcpkg installations do not
affect the tests.

`PageLayoutGeometryTests.cpp` uses the same pure layout calculation as the
WinUI `PdfPageLayout` adapter. It checks page scaling, alignment and row spacing,
then captures and restores view anchors across column changes on that generated
geometry. Cases cover mixed page sizes, swapped dimensions after rotation,
right-hand and unpaired pages, zoom, horizontal scrolling, centered content,
and the available scroll range at document boundaries.

The separate `urPDF.RuntimeTests` project loads the same MuPDF installation as
the application. It checks shared resources and locks, runtime lifetime, and
concurrent context cloning, transferring prepared contexts to workers, and
cleanup after MuPDF errors. Restore the application's vcpkg dependencies before
building this project; `urPDF.UnitTests` remains independent of MuPDF.
The runtime tests also open a one-page PDF and verify document/context cleanup
after window ownership is released and after a MuPDF error, without executing
the queued UI callback. Document loading tests open a PDF from a path with Polish
characters in tracked background work, as a tab does, and check open errors and
shutdown while a document is loading.

## Visual Studio

1. Open `urPDF.slnx`.
2. Select `Debug | x64`.
3. Build `urPDF.UnitTests` and `urPDF.RuntimeTests` in Solution Explorer.
4. Open **Test > Test Explorer** and choose **Run All Tests**.

## Command line

From a Developer Command Prompt for Visual Studio, in the repository root:

```bat
msbuild tests\urPDF.UnitTests.vcxproj /p:Configuration=Debug /p:Platform=x64
vstest.console out\unit-tests\x64\Debug\urPDF.UnitTests.dll /Platform:x64

msbuild tests\urPDF.RuntimeTests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:VcpkgManifestInstall=false
vstest.console out\runtime-tests\x64\Debug\urPDF.RuntimeTests.dll /Platform:x64
```

## Smoke tests

The `smoke` folder contains PowerShell scripts that start the unpackaged (MSI)
Debug build, `out\msi\x64\Debug\bin\urPDF.exe`, and check the result through UI
Automation. Pass `-Executable` to test another build. Close every running urPDF
instance first, because a running instance receives the activations. The scripts
restore the user settings they change and print a PASS/FAIL table.

- `ActivationSmoke.ps1` starts urPDF the way File Explorer does: settings
  commands, the *Open files from File Explorer in an existing window* setting,
  redirection to the running instance, busy windows and Unicode paths. It also
  simulates a MuPDF initialization failure through the
  `URPDF_SIMULATE_MUPDF_INIT_FAILURE` environment variable, which only Debug
  builds read.
- `DragSmoke.ps1` drags tabs with the real mouse: reordering, tear-out of PDF
  and empty tabs, and moving tabs between windows. It checks that tabs stay in
  their source window until the mouse is released and that Esc cancels the drag
  without opening a window.
  Tab widths are checked for short and long filenames, empty and selected tabs,
  and tabs moved to a new window, accounting for Windows DPI scaling.
  With `-CheckPresentation`, it also samples the desktop output when torn-out
  windows first appear, checking for black startup frames and verifying that
  tearing out the last tab replaces and closes its source window.

```powershell
powershell -ExecutionPolicy Bypass -File tests\smoke\ActivationSmoke.ps1
powershell -ExecutionPolicy Bypass -File tests\smoke\DragSmoke.ps1
powershell -ExecutionPolicy Bypass -File tests\smoke\DragSmoke.ps1 -CheckPresentation
```

The drag tests take over the mouse and keyboard and need an interactive desktop
session, so neither script runs in CI.

Build outputs go to the ignored `out` directory. Commit the test sources,
project, and this README; do not commit the generated DLL, PDB, or object files.
