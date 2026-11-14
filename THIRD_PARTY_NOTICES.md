# Third-party notices

The Android APK of this project includes, as native code, the following software:

## Ryujinx shader translator (Ryujinx.Graphics.Shader, Spv.Generator, Ryujinx.Common), from Ryubing

Built into `libspirvharness.so` from `tools/maxwell/spirv_harness` by `tools/maxwell/build_p4g_translator.sh`.

```
MIT License

Copyright (c) Ryujinx Team and Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## .NET runtime (NativeAOT), .NET Foundation and Contributors

The runtime NativeAOT compiles into `libspirvharness.so`.

```
The MIT License (MIT)

Copyright (c) .NET Foundation and Contributors

All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```


## libadrenotools and linkernsbypass

The custom Vulkan loader is adapted from [P5R Android v0.5.3](https://github.com/Brandon04Gonzalez/p5r-android/tree/v0.5.3). The vendored source and original license notices are under [third_party/libadrenotools](third_party/libadrenotools).


## Chakeyy P5R NVN optimizations

Selected command allocation/replay, program-layout lookup, dynamic storage descriptor, and driver-constant cache changes
are adapted from [Chakeyy/p5r-android, commit 5f17826cab38eaeca12162abc4337ca0831a3a65](https://github.com/Chakeyy/p5r-android/commit/5f17826cab38eaeca12162abc4337ca0831a3a65)
(`perf/universal-nvn-opt`), authored by Chakeyy. The source is under the MIT license with the BelmanteGu and Brandon
copyright notices retained in this project's [LICENSE](LICENSE). See [the port and test notes](docs/chakeyy-nvn-port.md)
for the scope and P4G-specific adaptations.
