# Tests

The `urPDF.Tests` project contains unit tests for urPDF. It uses the Microsoft
Native Unit Test Framework included with Visual Studio's Desktop development
with C++ workload.

The current tests run without starting the WinUI application or loading MuPDF.

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
```

Build outputs go to the ignored `out` directory. Commit the test sources,
project, and this README; do not commit the generated DLL, PDB, or object files.
