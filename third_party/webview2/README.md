# Microsoft Edge WebView2 SDK

The parts of the WebView2 SDK that TEKITO builds against, taken unmodified
from the NuGet package `Microsoft.Web.WebView2` version 1.0.3240.44:

- `include/WebView2.h`, `include/WebView2EnvironmentOptions.h` from
  `build/native/include`
- `lib/x64/WebView2LoaderStatic.lib` from `build/native/x64`

The SDK is licensed under `LICENSE.txt`; `NOTICE.txt` lists its third-party
notices. The WebView2 Runtime itself is not included; Windows 11 ships it.
