# References

Primary documentation the implementation follows. Links point to vendor documentation and
specifications; behavior that differs from the documentation in practice is noted in DECISIONS.md.

## Windows App SDK and WinUI 3

- Single instancing: https://learn.microsoft.com/windows/apps/windows-app-sdk/applifecycle/applifecycle-single-instance
- Rich activation (file activation, encoded launch arguments): https://learn.microsoft.com/windows/apps/windows-app-sdk/applifecycle/applifecycle-rich-activation
- `ActivationRegistrationManager` (unpackaged file associations): https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.windows.applifecycle.activationregistrationmanager
- `AppInstance.RedirectActivationToAsync`: https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.windows.applifecycle.appinstance.redirectactivationtoasync
- Title bar customization: https://learn.microsoft.com/windows/apps/develop/title-bar
- `SwapChainPanel`: https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.controls.swapchainpanel
- XAML theme resources (brushes per theme dictionary): https://learn.microsoft.com/windows/apps/design/style/xaml-theme-resources
- XAML styles and lightweight styling: https://learn.microsoft.com/windows/apps/design/style/xaml-styles
- Storage pickers for desktop apps (`Microsoft.Windows.Storage.Pickers`): https://learn.microsoft.com/windows/windows-app-sdk/api/winrt/microsoft.windows.storage.pickers
- Self-contained deployment: https://learn.microsoft.com/windows/apps/package-and-deploy/self-contained-deploy/deploy-self-contained-apps

## Windows shell, installer, default apps

- Launching the Default Apps settings page: https://learn.microsoft.com/windows/uwp/launch-resume/launch-default-apps-settings
- `ms-settings:` URIs: https://learn.microsoft.com/windows/apps/develop/launch/launch-settings-app
- WiX Toolset: https://docs.firegiant.com/wix/ (and the Open Source Maintenance Fee: https://docs.firegiant.com/wix/osmf/)
- Single package authoring (per-user or per-machine, `MSIINSTALLPERUSER`): https://learn.microsoft.com/windows/win32/msi/single-package-authoring
- SignTool: https://learn.microsoft.com/windows/win32/seccrypto/signtool
- `SHFileOperationW` (`FOF_ALLOWUNDO`, Recycle Bin): https://learn.microsoft.com/windows/win32/api/shellapi/nf-shellapi-shfileoperationw

## Direct3D, DXGI, WIC, PDH

- DXGI flip model: https://learn.microsoft.com/windows/win32/direct3ddxgi/dxgi-flip-model
- Frame latency waitable object: https://learn.microsoft.com/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
- Handling device-lost scenarios: https://learn.microsoft.com/windows/uwp/gaming/handling-device-lost-scenarios
- Windows Imaging Component: https://learn.microsoft.com/windows/win32/wic/-wic-lh
- `PdhAddEnglishCounterW` (locale-independent GPU engine counters): https://learn.microsoft.com/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw

## Audio

- WASAPI: https://learn.microsoft.com/windows/win32/coreaudio/wasapi
- `IAudioClock`: https://learn.microsoft.com/windows/win32/api/audioclient/nn-audioclient-iaudioclock
- Stream flags (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`): https://learn.microsoft.com/windows/win32/coreaudio/audclnt-streamflags-xxx-constants
- `IMMNotificationClient`: https://learn.microsoft.com/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient
- MMCSS (`AvSetMmThreadCharacteristicsW`): https://learn.microsoft.com/windows/win32/api/avrt/nf-avrt-avsetmmthreadcharacteristicsw

## FFmpeg

- Documentation: https://ffmpeg.org/documentation.html and API reference https://ffmpeg.org/doxygen/trunk/
- D3D11VA hardware context (`hwcontext_d3d11va.h`): https://ffmpeg.org/doxygen/trunk/hwcontext__d3d11va_8h.html
- Legal and licensing: https://ffmpeg.org/legal.html
- Platform notes for Windows builds: https://ffmpeg.org/platform.html
- Toolchain for Syna's build ([FFMPEG.md](FFMPEG.md)): llvm-mingw, https://github.com/mstorsjo/llvm-mingw
- Ubuntu archive snapshots (pinned build tools): https://snapshot.ubuntu.com/
- Development used BtbN's builds before Syna's own: https://github.com/BtbN/FFmpeg-Builds

## Media specifications

- ITU-T H.264 (SPS/PPS/slice header syntax, IDR, POC): https://www.itu.int/rec/T-REC-H.264
- ITU-T H.265: https://www.itu.int/rec/T-REC-H.265
- ISO/IEC 14496-12 (ISO base media file format: `stts`, `ctts`, edit lists) and ISO/IEC 14496-15
  (`avcC`/`hvcC` decoder configuration records)
- Matroska elements: https://www.matroska.org/technical/elements.html
- WebM container guidelines: https://www.webmproject.org/docs/container/

## .NET

- P/Invoke source generation (`LibraryImport`): https://learn.microsoft.com/dotnet/standard/native-interop/pinvoke-source-generation
- System.Text.Json source generation: https://learn.microsoft.com/dotnet/standard/serialization/system-text-json/source-generation
- Microsoft.Data.Sqlite: https://learn.microsoft.com/dotnet/standard/data/sqlite/
- Microsoft.Testing.Platform: https://learn.microsoft.com/dotnet/core/testing/unit-testing-platform-intro
