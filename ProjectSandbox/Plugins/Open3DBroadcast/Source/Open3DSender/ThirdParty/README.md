# Open3DSender Module Third-Party Assets

The sender module has no third-party dependencies of its own. The libraries it uses come from the
plugin-level `Source/ThirdParty/` folder (the Open3DStream core and Opus) through the modules it
depends on.

If the sender ever needs a library of its own, put it in a folder named after the library here
(for example `ThirdParty/<library>/lib/Win64/`), add it to `Open3DSender.Build.cs`, and record it
in `THIRD_PARTY_LICENSES.md`.
