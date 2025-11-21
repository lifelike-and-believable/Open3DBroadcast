# MsQuic Third-Party Artifacts

This directory vendors the Microsoft Native MsQuic library that powers the QUIC transport module.

## Version & Source
- **Version:** v2.5.5 (SChannel flavor)
- **Package:** [`Microsoft.Native.Quic.MsQuic.Schannel`](https://www.nuget.org/packages/Microsoft.Native.Quic.MsQuic.Schannel/2.5.5)
- **License:** MIT (see `LICENSE` in this folder)
- **Retrieved On:** 2025-11-21
- **Acquisition Command:**
  ```powershell
  Invoke-WebRequest -Uri 'https://www.nuget.org/api/v2/package/Microsoft.Native.Quic.MsQuic.Schannel/2.5.5' -OutFile msquic_schannel_2.5.5.nupkg
  Expand-Archive msquic_schannel_2.5.5.nupkg -DestinationPath msquic_schannel_2.5.5
  ```

## Layout
```
msquic/
  include/    -> Public headers (msquic.h, msquic.hpp, msquicp.h, msquic_winuser.h)
  lib/Win64/  -> Import library (msquic.lib)
  bin/Win64/  -> Runtime binaries (msquic.dll, msquic.pdb)
  LICENSE     -> Upstream MIT license
```

## Notes
- The Win64 build uses the Windows SChannel TLS backend to avoid bundling OpenSSL on Windows.
- The DLL is delay-loaded and also registered as a runtime dependency so the packager copies it automatically.
- Set the environment variable `O3D_WITH_TRANSPORT_QUIC=0` before generating project files to exclude the module entirely.
- For other platforms (Linux/Mac), obtain the matching MsQuic OpenSSL packages and mirror the same directory layout under the appropriate architecture subfolders before enabling those targets.
