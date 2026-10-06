# Open3DTransportNNG Third-Party Assets

Third-party code of the NNG transport lives here.

```
ThirdParty/
  README.md
  nng/
    LICENSE.txt
    README.md
    include/
      nng/
    lib/
      Win64/
        nng.lib
```

`Open3DTransportNNG.Build.cs` links `nng/lib/Win64/nng.lib` and adds `nng/include` to the
include path. `nng/README.md` records the library's version, provenance and hash, and how to
refresh it. Another platform would get its own folder next to `lib/Win64` and a matching change
in `Open3DTransportNNG.Build.cs`.
