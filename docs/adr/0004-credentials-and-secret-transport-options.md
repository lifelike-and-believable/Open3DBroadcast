# 0004: Credentials and secret transport options

- **Status:** Accepted (maintainer sign-off 2026-09-29)
- **Accepted with defaults:** the open choices below (Q3 to Q5) were accepted as this ADR proposes: no OS credential store in v1.x (R4 deferred), fixed env var names with an optional profile suffix, and plain `http://` token endpoints refused except on localhost. Needs-verification items stay open for the implementing WPs.
- **Date:** 2026-09-29
- **Plan decision:** D6 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0001](0001-platform-scope-first-fab-release.md) (Win64 only), [ADR 0002](0002-webrtc-and-moq-in-first-fab-release.md) (WebRTC becomes the `Open3DBroadcastWebRTC` add-on, WP-F11), D4 (transport abstraction, not yet decided); feeds WP-S9, WP-A1, WP-F11, WP-T2

**Recommendation in one line:** each transport customization declares which of its option keys are secret. A secret never goes into `TransportOptions`, `AdvancedParams`, an asset, an ini file or a LiveLink connection string. At runtime it is resolved from a transient in-process store, an environment variable, or (editor only, opt-in) the per-user `EditorPerProjectUserSettings` file under `Saved/`. Logs redact by key pattern and strip URL query values. The token server must authenticate the caller and decide grants itself.

## Context

**Where secrets exist today.** The only secret in the plugin is the LiveKit JWT used by WebRTC (`webrtc.token`, `Plugin/Source/Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:32`). A grep of every non-ThirdParty `.h`/`.cpp` outside WebRTC for `token|secret|password|auth|credential` finds only the generic `FO3DTransportConfig::Token` field and its reset in the sender. MoQ has no authentication: `moq_connect` takes only a URL (`Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/include/moq_ffi.h:185-190`). A relay URL could still carry a credential in its query string, and MoQ logs the full relay URL at Log level (`Plugin/Source/Open3DTransportMoQ/Private/Sender/MoQSender.cpp:82`, `:253`; `Receiver/MoQReceiver.cpp:62`, `:254`). NNG, Sockets and Loopback have no secrets.

**How the token leaks.**
- **Sender asset.** `TransportOptions` is a non-transient `UPROPERTY(VisibleAnywhere)` (`Plugin/Source/Open3DSender/Public/O3DSenderComponent.h:159-161`), so it is saved with the level or Blueprint. The WebRTC panel writes the token into it through `SetTransportOption` (`Open3DTransportWebRTCModule.cpp:221-230`), which calls `Modify()` and stores the value (`Open3DSender/Private/O3DSenderComponent.cpp:667-687`).
- **Runtime config.** `BuildTransportConfig` resets `Config.Token`, sets `bPersistToken = false`, then copies every option into `AdvancedParams` (`O3DSenderComponent.cpp:329-335`). The WebRTC customization then sets `Config.Token` and also adds the token to `AdvancedParams` (`Open3DTransportWebRTCModule.cpp:638`, `:647`; receiver `:678`, `:688`). The transports read only `Config.Token` (`Private/Sender/WebRTCSender.cpp:1061`, `Private/Receiver/WebRTCReceiver.cpp:814`), so the `AdvancedParams` copy serves no purpose.
- **Receiver ini and connection string.** The receiver panel writes the token into `Settings.TransportOptions` (`Open3DTransportWebRTCModule.cpp:507-510`). `OnCreateClicked` exports the whole struct into the LiveLink connection string (`Plugin/Source/Open3DReceiver/Private/O3DReceiverSourceFactory.cpp:191-192`, passed on at `:199`) and saves it to `UO3DReceiverSettingsObject`, which is `Config = GameUserSettings` with `GlobalConfig` (`:194-196`; `Open3DReceiver/Public/O3DReceiverSourceSettings.h:33-40`). `CreateSource` imports it back (`O3DReceiverSourceFactory.cpp:548-554`).
- **`bPersistToken`** says "Default false so callers explicitly opt-in to storing credentials" (`Open3DShared/Public/O3DTransportTypes.h:70-74`) and is read nowhere.
- **`ToDebugString`** claims "secrets redacted" but prints every `AdvancedParams` pair and the `Uri` verbatim (`O3DTransportTypes.h:107-151`). It has no callers today (grep), so the leak is latent.
- **Logs.** The receiver logs the first 20 characters of the JWT at Warning (`WebRTCReceiver.cpp:1059-1061`). The token fetcher logs the full error response body at Warning (`Private/Shared/WebRTCTokenFetcher.cpp:96`) and the success response body, which contains the token, at Verbose (`:344`).
- **Token endpoint.** The request has only a `Content-Type` header (`WebRTCTokenFetcher.cpp:48-50`) and the client picks its own grants (`:226-265`). The mock server honours client grants (`Tests/mock-token-server.py:83-85`) and defaults its signing secret to `test-secret` (`:31`), inside the shipped module tree (LIC-1).

**What does not leak.** No secret is Blueprint-readable: `TransportOptions` has no Blueprint specifier, and `Get/SetTransportOption` are not `UFUNCTION`s (`O3DSenderComponent.h:328-334`). `FO3DReceiverSourceConfig` is `BlueprintType`, but its `TransportOptions` has no Blueprint specifier (`O3DReceiverSourceSettings.h:28`). `ProjectSandbox/.gitignore:9` ignores `Saved/`, so the `GameUserSettings.ini` written in the editor is not committed by default; it is still plain text on disk. The main committed carriers are therefore level and Blueprint assets, and LiveLink presets that store the connection string (**needs-verification**: that UE 5.7 LiveLink presets persist `ConnectionString` verbatim, Q1).

**Constraints from accepted decisions.** WebRTC moves to a separate plugin (ADR 0002, WP-F11), so the secret mechanism must be a public, exported API in the main plugin that another plugin can call. ADR 0002 also says D6 must not hard-wire LiveKit fields into shared types (SHR-36). D4 will later decide the typed config; this ADR must work before and after it.

## Decision drivers

1. No secret reaches any file that is committed or shared: assets, `Config/*.ini`, LiveLink presets, logs.
2. Works from a separate plugin through public API (WP-F11).
3. Works in a packaged game with no editor.
4. Small enough for WP-S9 (size M) and compatible with whatever D4 decides.
5. Honest UI: the user can tell whether a secret is set, where it came from, and whether it will survive a restart.

## Options considered

### Option A: pattern-strip at save time
Strip any `TransportOptions` key matching `token|secret|key` before `ExportText`/`SaveConfig`, keep everything else as is.
- Pros: tiny change.
- Cons: the pattern also matches non-secret keys (`webrtc.useAutoTokenFetch`, `webrtc.tokenEndpointUrl`, `webrtc.tokenRefreshLeadTimeSec`, `Open3DTransportWebRTCModule.cpp:33-35`), which would then be lost on save. It doesn't fix the sender asset (a `UPROPERTY` saves on its own) and gives a separate plugin no way to declare its keys.
- Cost: S. Risk: medium (silent loss of settings).

### Option B: declared secret keys plus a transient secret store (chosen)
Customizations declare their secret keys. Setters route those keys to a store in `Open3DShared`. Persisted options never contain them.
- Pros: exact, per transport, works for add-on plugins, no pattern guessing on the persistence path.
- Cons: one new API and store; existing assets need a migration step.
- Cost: M. Risk: low.

### Option C: typed per-transport config with `Transient` UPROPERTYs
Replace the string map with typed structs whose secret fields are `UPROPERTY(Transient)`.
- Pros: the cleanest end state; UE handles non-persistence.
- Cons: this is D4 and WP-A1 (size L). It blocks a P0 fix on an undecided architecture.
- Cost: L. Risk: medium.

### Where a secret lives at runtime
- **R1. Transient in-process store:** always available; lost on restart. Chosen as the primary store.
- **R2. Environment variable:** works for CI, dedicated machines and packaged games; nothing on disk in the project. Chosen as a fallback.
- **R3. `EditorPerProjectUserSettings`:** written under the project's `Saved/Config/` (per user, not committed; external sources in References, **needs-verification** for 5.7, Q2). Editor only. Chosen as the opt-in "remember on this machine" store, which is what `bPersistToken` becomes.
- **R4. OS credential store (Windows Credential Manager):** best at-rest protection, but needs Win64-only native code and has no UE wrapper that could be found (**needs-verification**, Q3). Deferred.

## Decision

**Option B, with R1 and R2 at runtime and R3 as the opt-in persistence.** Option C remains the D4/WP-A1 end state; when it lands it must keep the semantics below.

1. **Declaring secret keys.** Add to both `FO3DSenderTransportCustomization` (`Open3DSender/Public/O3DSenderTransportCustomization.h:12-24`) and `FO3DReceiverTransportCustomization` (`Open3DReceiver/Public/O3DReceiverTransportCustomization.h:13-20`), outside `WITH_EDITOR`:
   ```cpp
   /** Option keys whose values are credentials. Never persisted, never logged. */
   TArray<FString> SecretOptionKeys;
   /** Optional env var name per secret key, e.g. {"webrtc.token", "O3DB_WEBRTC_TOKEN"}. */
   TMap<FString, FString> SecretEnvVars;
   ```
   WebRTC declares `webrtc.token` and the new `webrtc.tokenEndpointAuth` (item 6). Keys that are not declared are treated as non-secret for persistence. When D4 introduces a typed config, the typed secret fields replace this list; the store in item 2 stays.

2. **The secret store** lives in `Open3DShared` so that Sender, Receiver and the add-on can all use it:
   - New `Open3DShared/Public/O3DSecretStore.h`, `OPEN3DSHARED_API`, thread-safe (a mutex; reads happen when a transport starts, not per frame).
   - Secrets are keyed by `(TransportName, Profile, OptionKey)`. `Profile` is a new **non-secret** persisted option (`<transport>.credentialProfile`, default `"default"`). An asset or connection string stores only the profile name, which names the secret without containing it.
   - `Set(Transport, Profile, Key, Value, EO3DSecretPersistence)`, `Clear(...)`, `Resolve(...) -> TOptional<FO3DResolvedSecret>` (value plus source: Session, Environment or UserSettings). No API returns a list of values.
   - Resolution order: session store (R1), then environment variable (R2, from `SecretEnvVars`), then per-user settings (R3, editor builds only).
   - R3 is a `UCLASS(Config = EditorPerProjectUserSettings)` object in `#if WITH_EDITOR` code (moved to the D9 editor module when that exists). It stores values only when the user ticks "Remember on this machine".
3. **`bPersistToken`** becomes the R3 opt-in. `FO3DTransportConfig::bPersistToken` is deprecated and no longer set by callers; the persistence choice is the `EO3DSecretPersistence` argument to `Set`. Its doc comment is changed to say so, and WP-A1 removes it with the other LiveKit fields (SHR-36). No option exists to persist a secret into an asset or project ini.
4. **Routing.**
   - `UO3DSenderComponent::SetTransportOption` checks the active customization's `SecretOptionKeys`. A secret key goes to the store with no `Modify()`; `TransportOptions` never receives it. `GetTransportOption` never returns a secret.
   - `BuildTransportConfig` resolves declared secrets from the store into a new `FO3DTransportConfig::Secrets` map (not `AdvancedParams`). The WebRTC customization reads `Config.Secrets` into `Config.Token` and stops adding the token to `AdvancedParams` (delete `Open3DTransportWebRTCModule.cpp:647`, `:688`).
   - The receiver factory removes declared secret keys from a copy of `Settings` before `ExportText` and before `SaveConfig` (`O3DReceiverSourceFactory.cpp:192`, `:196`).
   - **Migration:** `UO3DSenderComponent::PostLoad` and `CreateSource` move any declared secret key found in loaded `TransportOptions` or an imported connection string into the session store, remove it from the map, and log one Warning naming the asset (never the value) and asking the user to resave. No automatic save.
5. **UI.** Secret fields are password boxes that open **empty**. Next to each is a status line: "Not set", "Set for this session", "Remembered on this machine", or "From environment variable O3DB_WEBRTC_TOKEN", plus a **Clear** button and a "Remember on this machine" checkbox (editor only). A warning line appears when auto-fetch is off and no token resolves.
6. **Token endpoint (WebRTC, WP-S9 and WP-S7).**
   - The fetcher sends `Authorization: Bearer <secret>` when the declared secret `webrtc.tokenEndpointAuth` resolves.
   - The client stops sending `grants` (`WebRTCTokenFetcher.cpp:226-265`). It sends room, identity and role as a request only; the server decides grants from the authenticated identity.
   - `http://` is refused unless the host is `localhost`, `127.0.0.1` or `::1`; the error names the URL without its query.
   - Docs (moving with the add-on) say the endpoint is a reference contract, that it must authenticate callers, and that it holds the LiveKit API secret.
   - The mock server requires `API_SECRET` from the environment and exits if it is unset, ignores client grants, and moves out of the shipped tree with HYG-1 (also fix TRF-35 there).
7. **Redaction rules (logs and `ToDebugString`).**
   - Values of declared secret keys are never logged at any verbosity. `ToDebugString` prints `Secrets={webrtc.token=<set>}` (key and presence only).
   - Defence in depth: `ToDebugString` and a shared `O3DRedact::Value(Key, Value)` helper print `<redacted>` for any key matching, case-insensitively, `token|secret|password|passwd|auth|credential|jwt|apikey|api_key` or ending in `.key`/`_key`. Over-redacting a non-secret value in a log is acceptable; this pattern is never used to decide persistence.
   - URLs (`Uri`, relay URLs, token endpoints) are logged through `O3DRedact::Url`, which keeps scheme, host, port and path and replaces query values and user-info with `<redacted>`.
   - Delete the token-prefix log (`WebRTCReceiver.cpp:1059-1061`) and the response-body logs (`WebRTCTokenFetcher.cpp:96`, `:344`); log status code and length instead.
8. **Packaged games (no editor).** A shipped game gets a secret in one of three ways, all without writing it to disk:
   - **Auto-fetch** from the configured token endpoint (the URL is not secret and persists normally). The game authenticates to its own endpoint through `webrtc.tokenEndpointAuth`, set at runtime.
   - **Blueprint/C++ setter:** a `UBlueprintFunctionLibrary` in `Open3DShared`, `UO3DCredentialLibrary::SetTransportSecret(TransportName, Profile, Key, Value)` and `ClearTransportSecret(...)`. Write-only: there is no getter. Open3DShared already depends on `CoreUObject` and `Engine` (`Open3DShared/Open3DShared.Build.cs:55-60`) but has no `UCLASS` today, so this adds UHT to the module.
   - **Environment variable** for kiosk, render-node or CI deployments.

   R3 is unavailable in packaged games by design.
9. **Add-on plugin.** `Open3DBroadcastWebRTC` uses only `SecretOptionKeys`, `SecretEnvVars`, `FO3DSecretStore` and `O3DRedact`, all exported from the main plugin. The main plugin contains no LiveKit key names. Removing the leftover LiveKit fields from `FO3DTransportConfig` stays with WP-A1 (SHR-36).

## Consequences

- **Easier:** WP-S9 has a precise contract. Any future transport with credentials (MoQ relay auth, a TLS client key for NNG) declares its keys and gets the same handling for free. Assets and LiveLink presets become safe to commit.
- **Harder:** a secret entered in the editor is gone after restart unless the user opts into R3 or sets an env var. That is deliberate and the UI says so. Existing assets holding a token need one resave after migration.
- **Constrains D4/WP-A1:** the typed config must keep "declared secret, resolved from the store, never serialized". The deprecated `bPersistToken` and `Token` fields are removed there.
- **Constrains D9:** the R3 settings class and the secret widgets move to the editor module.
- **Out of scope:** encryption at rest (R4), rotating LiveKit API keys, and MoQ relay authentication, which the FFI does not support today.

## Implementation outline

1. **WP-S9a (Shared):** `O3DSecretStore.h/.cpp`, `O3DRedact.h/.cpp`, `FO3DTransportConfig::Secrets`, `ToDebugString` redaction, deprecation comment on `bPersistToken`. Files: `Open3DShared/Public/O3DTransportTypes.h`, new files in `Open3DShared/Public` and `Private`.
2. **WP-S9b (customization API):** `SecretOptionKeys` and `SecretEnvVars` on both customization structs; routing in `UO3DSenderComponent::Set/GetTransportOption`, `BuildTransportConfig` and `PostLoad`; stripping and migration in `O3DReceiverSourceFactory.cpp` (`OnCreateClicked`, `CreateSource`). Files: `O3DSenderTransportCustomization.h`, `O3DReceiverTransportCustomization.h`, `O3DSenderComponent.h/.cpp`, `O3DReceiverSourceFactory.cpp`.
3. **WP-S9c (WebRTC):** declare the keys, read `Config.Secrets`, remove the `AdvancedParams` copies, update both panels to the status-line UI, add the auth header, remove client grants, enforce HTTPS, delete the token and body logs. Files: `Open3DTransportWebRTCModule.cpp`, `Private/Shared/WebRTCTokenFetcher.cpp`, `Private/Shared/WebRTCTokenManager.cpp`, `WebRTCSender.cpp`, `WebRTCReceiver.cpp`. Done before or after the WP-F11 move; the API is the same.
4. **WP-S9d (logs elsewhere):** route MoQ and NNG URL logs and `O3DReceiverSource.cpp:381` through `O3DRedact::Url`.
5. **WP-S9e (runtime API):** `UO3DCredentialLibrary`; the R3 settings class (editor, later D9).
6. **WP-S9f (mock server and docs):** require `API_SECRET`, drop client grants, fix TRF-35, move the file out of the plugin with HYG-1; token-endpoint guidance in the WebRTC docs (WP-D2 for the add-on).

## Verification / acceptance

- The WP-S9 test (in the D10 test module): with a token set, save a level containing a sender component, create a receiver source, and save a LiveLink preset. The token string appears in none of the saved `.umap`/`.uasset` bytes, `Saved/Config/**/GameUserSettings.ini`, or the connection string.
- `ToDebugString` test: a config with a token in `Secrets`, a `?token=abc` URI and an `AdvancedParams` key `x.apikey` produces output containing none of the three values.
- Migration test: loading a component whose `TransportOptions` contains `webrtc.token` moves it to the session store, removes it from the map, and logs a Warning without the value.
- Resolution-order test: session beats environment beats per-user settings; a packaged-style run (no `WITH_EDITOR`) never reads R3.
- Grep check in CI (WP-F8): no `UE_LOG` argument in `Source/` includes `Token`, `Left(` of a token, or `GetContentAsString()`.
- Manual: the mock server refuses to start without `API_SECRET`, and a fetch without the bearer returns 401 when `API_KEY` is set.

## Open questions for the maintainer

1. **needs-verification:** do UE 5.7 LiveLink presets store the source `ConnectionString` verbatim in the preset asset? This ADR assumes yes.
2. **needs-verification:** in UE 5.7, does `Config = EditorPerProjectUserSettings` write to `<Project>/Saved/Config/<Platform>Editor/EditorPerProjectUserSettings.ini` and never to a `Default*.ini` unless `DefaultConfig` is specified? (Confirmed only by third-party sources.)
3. **Accepted default:** no OS credential store in v1.x (R4 deferred). **needs-verification** for later: is an OS credential store (R4) wanted? It is Win64-only (acceptable under ADR 0001) and needs native `CredWrite`/`CredRead` code; no UE wrapper was found.
4. **Accepted default:** fixed names plus an optional profile suffix. Original question: should the env var names be fixed per key (`O3DB_WEBRTC_TOKEN`) or include the profile (`O3DB_WEBRTC_TOKEN__<PROFILE>`)? This ADR proposes fixed names plus an optional profile suffix.
5. **Accepted default:** refuse, except localhost. Original question: is refusing plain `http://` token endpoints (except localhost) acceptable, or should it be a warning plus an explicit "allow insecure" option for lab networks?

## References

- Findings: SND-10, RCV-3, TRF-21, TRF-22, SHR-11, LIC-1, SHR-36; related TRF-35, HYG-1.
- Files: `Plugin/Source/Open3DShared/Public/O3DTransportTypes.h`; `Plugin/Source/Open3DSender/Public/O3DSenderComponent.h`, `Private/O3DSenderComponent.cpp`; `Plugin/Source/Open3DSender/Public/O3DSenderTransportCustomization.h`; `Plugin/Source/Open3DReceiver/Public/O3DReceiverSourceSettings.h`, `O3DReceiverTransportCustomization.h`, `Private/O3DReceiverSourceFactory.cpp`; `Plugin/Source/Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp`, `Private/Shared/WebRTCTokenFetcher.cpp`, `Tests/mock-token-server.py`; `Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/include/moq_ffi.h`; `.github/copilot-instructions.md:63`, `:114`.
- External (retrieved 2026-09-29, search snippets only; Epic's own config page was listed but not fetched): Tom Looman, "Adding 'Project Settings' to Unreal Engine (DeveloperSettings)", https://tomlooman.com/unreal-engine-developer-settings/ ; hzFishy, "Developer Settings", https://notes.hzfishy.fr/Unreal-Engine/Miscs/Types/Developer-Settings ; Epic, "Configuration Files in Unreal Engine", https://dev.epicgames.com/documentation/en-us/unreal-engine/configuration-files-in-unreal-engine .
