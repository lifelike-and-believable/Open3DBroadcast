# Packaging Structure Update

## Summary

Updated the release packaging to organize plugin files by Unreal Engine version, making installation easier for end users.

## New Structure

Release zip files now contain:

```
UE_5.4/
  Plugins/
    Open3DStream/
      Open3DStream.uplugin
      Source/
      Binaries/
      lib/
      ...

UE_5.5/
  Plugins/
    Open3DStream/
      Open3DStream.uplugin
      Source/
      Binaries/
      lib/
      ...

lib/
  (shared libraries)

include/
  (C++ headers)
```

### Benefits

1. **Easy Installation**: Users can extract the entire `UE_X.X` folder to their project root, and it automatically merges with their project's `Plugins` directory
2. **Multi-Version Support**: Single zip contains builds for multiple Unreal Engine versions
3. **Clear Organization**: Version-specific builds are clearly separated
4. **Standard Layout**: Follows Unreal Engine plugin conventions

## Changes Made

### 1. package.py
- Modified `unreal_dst_base` to point to `out_dir` instead of nested path
- Changed destination to create `UE_X.X/Plugins/Open3DStream` structure
- Updated library and include paths to be relative to plugin directory
- Fixed `remove_directory()` to actually remove directories using `shutil.rmtree()`
- Fixed regex warning by using raw string
- Added comprehensive docstring explaining the new structure

### 2. GitHub Actions Workflow (unreal-plugin-release.yml)
- Added "Restructure for archive" step that:
  - Detects UE version from `UE_ROOT` environment variable
  - Creates `UE_X.X/Plugins/PluginName` directory structure
  - Copies built plugin into new structure
- Updated archive creation to use restructured directory
- Updated release notes with new installation instructions

### 3. README.md
- Added "Packaging for Release" section explaining the structure
- Updated "Using in Unreal Engine" with installation instructions for both release packages and source
- Included example of the directory structure

### 4. Test Script (scripts/test_package_layout.py)
- Created test harness to verify zip structure
- Simulates the packaging process
- Creates sample zip and displays contents

## Testing

### Test Results

```
Adding lib/dummy.lib
Adding UE_5.4/Plugins/Open3DStream/Open3DStream.uplugin
Adding UE_5.5/Plugins/Open3DStream/Open3DStream.uplugin
Adding include/dummy.h

Zip contains:
  lib/dummy.lib
  UE_5.4/Plugins/Open3DStream/Open3DStream.uplugin
  UE_5.5/Plugins/Open3DStream/Open3DStream.uplugin
  include/dummy.h

Test zip created at /workspaces/Open3DStream/Open3DStream_test.zip
```

### Validation

- ✅ Python syntax check passed (package.py)
- ✅ YAML validation passed (unreal-plugin-release.yml)
- ✅ Test packaging script successfully creates expected structure
- ✅ Zip contains `UE_X.X/Plugins/Open3DStream/` layout

## Installation Instructions for Users

### From Release Package

1. Download the latest release zip from GitHub Releases
2. Extract the zip file
3. Choose one of these methods:
   - **Method A**: Copy the entire `UE_X.X` folder (matching your engine version) to your project root
   - **Method B**: Copy the `Plugins/Open3DStream` folder from inside `UE_X.X` to your project's `Plugins` directory
4. Restart Unreal Editor
5. Enable the plugin in **Edit → Plugins**

### Example

For Unreal Engine 5.5:
```bash
# Method A - Extract version folder to project root
unzip Open3DStream-1.0.0-Win64.zip
cp -r UE_5.5 /path/to/MyProject/

# Method B - Copy plugin folder directly
unzip Open3DStream-1.0.0-Win64.zip
cp -r UE_5.5/Plugins/Open3DStream /path/to/MyProject/Plugins/
```

## Migration Notes

### For Developers

If you have existing build scripts or workflows that depend on the old flat structure, you'll need to update them to:
1. Navigate into the appropriate `UE_X.X` folder
2. Access `Plugins/Open3DStream` within that folder

### For CI/CD

The GitHub Actions workflow automatically handles the restructuring, so no changes are needed to the build process. The restructuring happens after the plugin is built and before archiving.

## Date

October 16, 2025
