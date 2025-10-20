# O3DS Core ThirdParty Module

This directory contains prebuilt O3DS core static libraries and headers for the Unreal plugin.

## Directory Structure

```
o3ds/
├── include/
│   ├── o3ds/          # Public O3DS headers
│   └── o3ds_generated.h  # FlatBuffers generated header
└── lib/
    ├── Win64/Release/o3ds.lib
    ├── Linux/Release/libo3ds.a
    └── Mac/Release/libo3ds.a
```

## Building Libraries

The O3DS core libraries are built using the CI workflow `.github/workflows/build-o3ds-core.yml`.

To update the libraries:

1. Go to the Actions tab in GitHub
2. Select "Build O3DS core" workflow
3. Click "Run workflow"
4. Download the artifacts for each platform
5. Extract and commit the libraries to this directory

## Dependencies

The O3DS core library includes:
- FlatBuffers (header-only)
- NNG (statically linked)
- CRCpp (header-only, included in build)

These dependencies are built into the static library and don't need to be distributed separately.

## Version

Current version: 1.0.4
Built from: (commit SHA will be in BUILD_INFO.txt after build)
