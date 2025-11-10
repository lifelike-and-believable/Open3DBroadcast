# Open3DSender Module Third-Party Assets

Module-specific static libraries that are only consumed by `Open3DSender`
should live in this folder. At present the sender module does not require
any dedicated third-party binaries; keep this directory reserved for future
sender-only dependencies.

If additional platforms are introduced, create the corresponding subfolder
under `Lib/` (for example `Lib/Linux`), and update `Open3DSender.Build.cs`
accordingly.
