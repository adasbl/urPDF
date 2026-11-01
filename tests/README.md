# Tests

The `urPDF.Tests` project contains unit tests for urPDF. It uses the Microsoft
Native Unit Test Framework included with Visual Studio's Desktop development
with C++ workload.

The current tests run without starting the WinUI application or loading MuPDF.
They require Visual Studio 2026's C++ toolset `v145` and Windows SDK
`10.0.26100.0`, and use C++20, matching the application project. vcpkg integration
is explicitly disabled for this project, so local vcpkg installations do not
affect the tests.

The separate `MuPdfRuntime.Tests` project loads the same MuPDF installation as
the application. It checks shared resources and locks, runtime lifetime, and
concurrent context cloning, transferring prepared contexts to workers, and
cleanup after MuPDF errors. Restore the application's vcpkg dependencies before
building this project; the original `urPDF.Tests` remains independent of MuPDF.

## Visual Studio

1. Open `urPDF.slnx`.
2. Select `Debug | x64`.
3. Right-click `urPDF.Tests` in Solution Explorer and choose **Build**.
4. Open **Test > Test Explorer** and choose **Run All Tests**.

## Command line

From a Developer Command Prompt for Visual Studio, in the repository root:

```bat
msbuild tests\urPDF.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64
vstest.console out\tests\x64\Debug\urPDF.Tests.dll /Platform:x64

msbuild tests\MuPdfRuntime.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:VcpkgManifestInstall=false
vstest.console out\runtime-tests\x64\Debug\MuPdfRuntime.Tests.dll /Platform:x64
```

Build outputs go to the ignored `out` directory. Commit the test sources,
project, and this README; do not commit the generated DLL, PDB, or object files.
