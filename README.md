# urPDF

Minimalist, high-performance PDF reader for Windows 11 build on MuPDF engine.

> **Project status:** urPDF is under active development. Some features, including
> printing, may still be experimental.

## Requirements
- Windows 10 version 1809 (build 17763) or later.
- Visual Studio with the **Desktop development with C++** workload.
- A compatible Windows SDK and MSBuild installation.
- NuGet package restore enabled in Visual Studio.
- MuPDF development headers and libraries available to the native build
  environment.

The project currently targets `x64`, `Win32`, and `ARM64` configurations. The
recommended configuration for modern Windows PCs is `x64 | Debug` or
`x64 | Release`.

## Dependencies
- **MuPDF** — PDF parsing and rendering engine. See the
  [MuPDF documentation](https://mupdf.readthedocs.io/).
- **WinUI 3 / Windows App SDK** — native Windows user interface.
- **C++/WinRT** — language projection for Windows Runtime APIs.
- **Microsoft WebView2** — included in the project dependency set.

## Privacy
urPDF is designed to work offline. It does not collect, store, transmit, or
share personal data, telemetry, or user documents. All document processing
takes place locally on the device.

See [PRIVACY.md](PRIVACY.md) for the project's privacy statement.

## License
The urPDF source code is distributed under the
[GNU Affero General Public License v3.0](LICENSE).

MuPDF is a separate third-party dependency with its own licensing terms. See
the [MuPDF licensing information](https://mupdf.com/licensing/) before
redistributing urPDF or distributing a modified build.
