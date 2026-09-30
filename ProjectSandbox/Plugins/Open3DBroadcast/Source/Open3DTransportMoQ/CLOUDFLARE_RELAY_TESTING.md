# MoQ Relay Testing

Relay tests need the internet or a relay you run, so they are not part of the default test run (ADR 0006 §6, UX-4). They live in the editor-only `Open3DBroadcastTests` module, `Private/Network/MoQ/MoQRelayNetworkTests.cpp`, under `Open3DBroadcast.Network.MoQ.*`.

The tests register only when `O3DB_NETWORK_TESTS=1`. The relay comes only from `O3D_MOQ_RELAY_URL`; there is no built-in default relay. With the flag set and no URL, every network test fails.

| Test | Checks |
|------|--------|
| `Open3DBroadcast.Network.MoQ.SenderPublishes` | A sender created through the `MoQ` registry entry connects and publishes at least two frames. |
| `Open3DBroadcast.Network.MoQ.SenderToReceiver` | A frame travels sender, relay, receiver and arrives byte-exact. |
| `Open3DBroadcast.Network.MoQ.TwoSendersSeparateNamespaces` | Two senders on different namespaces each reach only their own receiver. |

## Running

```powershell
$env:O3DB_NETWORK_TESTS = "1"
$env:O3D_MOQ_RELAY_URL  = "https://127.0.0.1:4443"   # a local moq-relay-ietf, or another relay you may use
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DBroadcast.Network.MoQ"
```

Each test waits up to 30 s for the relay by polling (no fixed sleeps). Namespaces include a GUID, so parallel runs do not collide.

## Offline coverage

The session wrapper, reconnect and backoff, subscribe retries, callback lifetime and the send queue are tested without a relay through the fake moq-ffi table (`Open3DBroadcast.Transport.MoQ.*` and `Open3DBroadcast.Conformance.MoQ.*`). Those tests run in every default run.
