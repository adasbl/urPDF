# Tests

The `urPDF.UnitTests` project contains unit tests for urPDF. It uses the Microsoft
Native Unit Test Framework included with Visual Studio's Desktop development
with C++ workload.

The unit tests run without starting the WinUI application or loading MuPDF.
They require Visual Studio 2026's C++ toolset `v145` and Windows SDK
`10.0.26100.0`, and use C++20, matching the application project. vcpkg integration
is explicitly disabled for this project, so local vcpkg installations do not
affect the tests.

The separate `urPDF.RuntimeTests` project loads the same MuPDF installation as
the application. It checks shared resources and locks, runtime lifetime, and
concurrent context cloning, transferring prepared contexts to workers, and
cleanup after MuPDF errors. Restore the application's vcpkg dependencies before
building this project; `urPDF.UnitTests` remains independent of MuPDF.
The runtime tests also open a one-page PDF and verify document/context cleanup
after window ownership is released and after a MuPDF error, without executing
the queued UI callback.

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

Build outputs go to the ignored `out` directory. Commit the test sources,
project, and this README; do not commit the generated DLL, PDB, or object files.
