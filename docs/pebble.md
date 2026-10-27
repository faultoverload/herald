# Pebble companion: plan

**Goal:** talk to Hermes from a Pebble (first target: **Pebble Time 2**) through Herald on an Android phone. It should work the way the Wear OS companion does wherever the hardware allows, and do something sensible where it doesn't.

**Status:** this is a plan. No code exists yet. Written 2026-10-27 against Herald `master` @ `687d80e3` and Pebble SDK 4.33.1.

**Evidence labels.** ✅ means verified from source or docs (the link is given). ❓ means inferred or unverified, and the plan says which phase settles it.

---

## 1. What Herald does for Wear OS today

The Wear companion is a thin watch UI over a phone-side relay. The phone holds the gateway connection and the sign-in. The watch never sees either.

| Piece | Where | What it does |
|---|---|---|
| Protocol | [`shared/wearlink/WearLink.kt`](../shared/wearlink/src/commonMain/kotlin/dev/hermeskotlin/wearlink/WearLink.kt) | Defines the message paths (`/herald/ask`, `/reply`, `/chats`, `/status`, `/ring`, `/call/*`, …) and their JSON bodies (`WatchAsk`, `ReplyUpdate`, `ChatList`, `PhoneStatus`, `WatchRing`, …) |
| Relay | [`shared/core/wear/WatchRelay.kt`](../shared/core/src/commonMain/kotlin/dev/hermeskotlin/core/wear/WatchRelay.kt) | Platform-agnostic. Runs the watch's ask as its own side `ChatSession`, sends `ReplyUpdate`s every ≤400 ms, and voices the reply in clips. It reaches the watch **only through the `WatchOutbox` interface** (`send(node, path, bytes)` and `stream(...)`) |
| Phone transport | [`androidApp/wear/`](../androidApp/src/main/kotlin/dev/hermeskotlin/android/wear/) | `WearBridgeService` (incoming messages and channels) and `PhoneOutbox` (the Data Layer). `WatchRinger` rings the watch, `WatchAccentSync` pushes colours, and `WearRequests` posts the watch chat's approvals as phone notifications |
| Live calls | [`shared/ui/voice/WatchCalls.kt`](../shared/ui/src/commonMain/kotlin/dev/hermeskotlin/ui/voice/WatchCalls.kt) | The watch holds a WebRTC call. The phone brokers the SDP and runs requests as turns |
| Watch app | [`wearApp/`](../wearApp/src/main/kotlin/dev/hermeskotlin/wear/) | Talk screen (orb, caption, reply), chats list, settings, ring screen, call screen, tile, complication, on-watch speech recognizer or recorder, and on-watch TTS fallback |

**The key seam is `WatchOutbox`.** Most of the Pebble work is a second transport behind it, plus a new watch app. The relay's turn logic, side-chat handling and status logic are reused unchanged.

Gaps in that seam (✅ from reading the code):

- **DI binds one `WatchOutbox`** (`WearModule.kt`), which is the Data Layer one. Pebble needs a **routing outbox** keyed by node id: for example, `pebble:<watchId>` goes to Pebble and everything else goes to the Data Layer.
- **`WatchRinger` and `WatchAccentSync` call Wear APIs directly** (`CapabilityClient`), and `WearBridgeService` *is* a `WearableListenerService`. Each needs a Pebble counterpart, or a small "watch transports" abstraction that both implement.
- **`WatchRinger.ring` only rings when the gateway talks live**, because answering a ring on Wear means starting a WebRTC call. Pebble can't do that (see §3), so ringing needs its own rule.
- **No "replay current turn" message.** If the watch app is closed mid-turn, it misses updates, and the Wear protocol has no way to ask for the current state again.

---

## 2. What the Pebble platform gives us

### Hardware: Pebble Time 2 = platform `emery` ✅

[Hardware information](https://developer.repebble.com/guides/tools-and-resources/hardware-information/):

- **Display:** 200×228, rectangular, 64 colours.
- **Limits:** 128 KB for code plus heap, 256 KB for resources.
- **Hardware:** 4 buttons, touchscreen (watchapps only), heart-rate monitor, **microphone**, **speaker** (flint and emery only), LRA vibration motor.

### Languages

- **C SDK:** full API, runs on every platform. ✅
- **Alloy** (on-watch JavaScript, Moddable/XS): runs on emery and gabbro only. ✅
  - It is unverified whether an Alloy `Message` reaches a PebbleKit Android 2 companion, or only PebbleKit JS. ❓
- **Recommendation: C.** It has the full API (speaker, AppGlance, quick launch), runs on every model, and the PebbleKit Android 2 sample watchapp is in C.

### Phone ↔ watch: PebbleKit Android 2 ✅

The docs say it is the [currently recommended](https://developer.repebble.com/guides/communication/using-pebblekit-android/) way to reach the watch. Library: [`io.rebble.pebblekit2:client`](https://github.com/pebble-dev/PebbleKitAndroid2) (1.3.2), Apache-2.0, Kotlin.

- **Supported phone apps:** the Core Pebble app ≥ 1.0.7.7, and microPebble.
- **Sending:** `DefaultPebbleSender().sendDataToPebble(uuid, dict, watches?)` is a suspend function that returns a `TransmissionResult`:
  - `Success`
  - `FailedWatchNotConnected`
  - `FailedWatchNacked`
  - `FailedTimeout`
  - `FailedDifferentAppOpen`
  - `FailedNoPermissions`
- **Receiving:** subclass `BasePebbleListenerService`. It has `onMessageReceived(uuid, dict, watch)` (returns Ack or Nack), `onAppOpened` and `onAppClosed`. The Pebble app binds it while our watchapp runs.
- **Also available:** `startAppOnTheWatch()`, `stopAppOnTheWatch()`, timeline pins from the phone, and `PebbleInfoRetriever`.
- **The watchapp must name its companion.** Its `package.json` needs a `companionApp.android.apps[].package` entry, or sends fail with `FailedNoPermissions`. List **both** `dev.herald.android` and `dev.herald.android.dev` (the debug suffix).
- **Turn off auto-select.** By default the library trusts any app that claims to be the Pebble app. Set `DefaultPebbleAndroidAppPicker.enableAutoSelect = false` and use the `client-ui` picker. Herald sends private agent output.
- **Avoid classic PebbleKit** (`com.getpebble:pebblekit`). It uses broadcasts that any app can read and that can't wake Herald, and the Gravel fork turns it off.

### AppMessage limits ✅ ([AppMessage](https://developer.repebble.com/docs/c/Foundation/AppMessage/))

- **Buffers:** the guaranteed minimum is 124 B inbox and 636 B outbox. The watch can ask for `app_message_inbox_size_maximum()`, documented as up to about 8 KB. Both buffers come out of the 128 KB heap.
- **Foreground only:** `FailedDifferentAppOpen` means our watchapp must be the one open. Anything sent while it's closed is lost.

### Voice

- **Input: dictation only.** `dictation_session_create` works on emery ✅. The Pebble phone app transcribes the speech and the watchapp gets **text only**.
  - There is no raw-microphone API ✅ (no capture symbol in the [API index](https://developer.repebble.com/api-index.json)).
  - The phone app's default speech-to-text backend on Android is unverified ❓. The options are WisprFlow cloud, on-device Parakeet or Whisper, Rebble ASR, and ML Kit. Phase 0 settles it.
- **Output: speaker** ([guide](https://developer.repebble.com/guides/events-and-services/speaker/)) ✅. It can play tones, note sequences, and **PCM streaming** at 8/16 kHz, 8/16-bit.
  - That covers a ringtone.
  - It might cover gateway TTS, but even 8 kHz/8-bit is about 8 KB/s, which is likely more than AppMessage over BLE can sustain. This is a spike, not a commitment.

### Glanceables and other APIs ✅

| Feature | What it gives us |
|---|---|
| [AppGlance](https://developer.repebble.com/guides/user-interfaces/appglance-c/) | A subtitle in the launcher; up to 8 slices |
| [Quick launch](https://developer.repebble.com/guides/design-and-interaction/one-click-actions/) | Hold a button to open the app straight into dictation |
| [Custom vibration patterns](https://developer.repebble.com/docs/c/User_Interface/Vibes/) | Our own ring and notification buzzes |
| [Persistent storage](https://developer.repebble.com/guides/events-and-services/persistent-storage/) | 1 MB per app on emery; 256 B per value |
| [Timeline local pins](https://developer.repebble.com/guides/pebble-timeline/timeline-local-pins/) | Pins pushed from the phone through PebbleKit 2. The web timeline API is gone |

### Notification mirroring (from the Core app's source, `libpebble3/.../notification/`)

What the Core app's source shows ✅:

- It mirrors Android notifications.
- It turns `RemoteInput` into a Reply action, with canned replies and dictation.
- It turns other actions into buttons that fire their `PendingIntent`.
- It **prefers `WearableExtender` actions** when a notification has them.
- It **ignores ongoing notifications.** Herald's ringing "call" notification is ongoing, so it won't show.
- It skips `setLocalOnly` notifications.
- By default it sends nothing while the phone is unlocked and its screen is on.

**Never tested on a watch** ❓: whether Herald's existing reply and approval notifications already work on a Pebble with no code.

---

## 3. Feature parity: Wear OS vs Pebble

| Wear OS feature | Pebble equivalent | Feasibility |
|---|---|---|
| Ask by voice (watch recognizer, or a recording the gateway transcribes) | Dictation API, giving text that goes into `WatchAsk.text` | ✅ Text only. **The gateway-transcription path isn't possible** (no raw audio). The transcription quality is whatever the Pebble app's speech-to-text gives |
| Ask by typing | Canned prompts on the watch, plus dictation | ✅ (no keyboard) |
| Live progress: phase, activity line, streaming reply | A talk window with a status line and the reply scrolling below | ✅ Needs a byte budget and throttling (§4.2) |
| Read the reply aloud (gateway voice, else watch TTS) | Text only by default. Gateway voice through speaker PCM streaming is a **spike** | ❓ The watch-TTS fallback **isn't possible** (no on-watch TTS) |
| Stop the running turn | Back or Select, sending `WatchStop` | ✅ |
| Recent chats: pick, new chat, continue | A `MenuLayer` of up to 15 chats, with "New chat" first | ✅ |
| Phone status dot (signed in, connected, failed) | A status icon in the header | ✅ |
| Accent colour follows the phone | Map ARGB to the nearest of the 64 `GColor`s; black-and-white on older models | ✅ |
| Approvals and questions in the watch's chat: "answer on your phone" | Same phase (`WaitingOnPhone`), plus the existing phone notification, which may mirror | ✅ Answering on the wrist is a **decision** (§7) |
| Ring for approvals and questions; Answer starts a live call | Vibration and a speaker ringtone, then a ring window. **Answer can't start a live call** | ❓ Needs a new answer behaviour (§7) |
| Live WebRTC voice call | — | ❌ **Not possible** (no WebRTC, no internet on the watch, no raw mic) |
| "Ask Hermes" tile | Quick launch (hold a button, go straight into dictation) | ✅ |
| Complication | AppGlance ("Last: …" or "Working…") | ✅ |
| Works while the app is closed | Rings and finished replies need a fallback: auto-launch, a timeline pin, or a mirrored notification (§4.4) | ❓ Phase 3 |

---

## 4. Architecture

```
 Pebble Time 2 (C watchapp, pebbleApp/)                    Android phone (Herald)
 ┌───────────────────────────────┐    AppMessage dicts    ┌──────────────────────────────────────────┐
 │ talk_window   chats_window    │◄──────────────────────►│ PebbleBridgeService : BasePebbleListener │
 │ ring_window   status/glance   │   (via Pebble app,     │        │ decode → WatchAsk/Stop/…          │
 │ link.c  (keys from package.json)│   PebbleKit Android 2)│        ▼                                  │
 └───────────────────────────────┘                        │  WatchRelay  (unchanged, shared/core)    │
                                                          │        │ outbox.send(node, path, json)    │
                                                          │        ▼                                  │
                                                          │  RoutingOutbox ── "pebble:*" ──► PebbleOutbox
                                                          │               └── else ──────► PhoneOutbox (Wear)
                                                          └──────────────────────────────────────────┘
```

### 4.1 Where the code goes

```
shared/wearlink/…/PebbleLink.kt       Pebble message keys, types and the codec between WearLink models and int-keyed
                                      dictionaries; UTF-8 byte clipping; markdown-to-plain text. Pure Kotlin, in commonTest.
androidApp/…/pebble/
  PebbleBridgeService.kt              BasePebbleListenerService: decodes asks, stops, chat and status requests; onAppOpened
                                      replays the last reply.
  PebbleOutbox.kt                     WatchOutbox over DefaultPebbleSender: one message in flight, latest wins, ~1/s.
  RoutingOutbox.kt                    Picks PebbleOutbox or PhoneOutbox by node id.
  PebbleRinger.kt                     Rings Pebbles (rules in §4.4), reports back to ChatNotifications.
  PebbleModule.kt                     Koin wiring; a Settings → Watch "Pebble" switch and the companion-app picker.
pebbleApp/                            The C watchapp (pebble-tool project; not part of Gradle).
  package.json                        uuid, messageKeys (the source of truth), companionApp, targetPlatforms.
  src/c/{main,link,talk,chats,ring,glance,colors}.c
```

### 4.2 Wire protocol (new, compact)

The Wear JSON protocol stays untouched. `PebbleOutbox` **decodes the JSON for each path** and projects it into a small dictionary. `PebbleBridgeService` does the reverse for incoming messages. JSON is never parsed on the watch.

- **One key, `TYPE`, says what the message is:**

  | TYPE | Direction | Carries |
  |---|---|---|
  | `ASK` | Watch → phone | `ASK_ID`, `TEXT`, `CHAT_ID?`, `speak=false` |
  | `STOP` | Watch → phone | `ASK_ID` |
  | `CHATS_REQ` | Watch → phone | — |
  | `STATUS_REQ` | Watch → phone | `ACCENT_REV` |
  | `RING_ON` | Watch → phone | `REQUEST_ID` |
  | `RING_END` | Watch → phone | `REQUEST_ID`, `ANSWERED` |
  | `REPLY` | Phone → watch | `ASK_ID`, `SEQ`, `PHASE`, `ACTIVITY`, `TITLE`, `TEXT`, `TEXT_OFFSET`, `ERROR` |
  | `CHAT_ITEM` | Phone → watch | `INDEX`, `COUNT`, `CHAT_ID`, `TITLE`, `AGO` |
  | `STATUS` | Phone → watch | flags and the 4 accent colours |
  | `RING` | Phone → watch | `REQUEST_ID`, `CHAT_ID`, `TITLE`, `APPROVAL`, `AGENT` |
  | `RING_END` | Phone → watch | `REQUEST_ID` |

- **Keys come from `pebbleApp/package.json` `messageKeys`**, which the build turns into C `MESSAGE_KEY_*`. A JVM test reads that file and fails if `PebbleLink`'s numeric keys drift from it. It lives in a new `shared/wearlink/src/androidHostTest`, because `commonTest` has no file I/O.
- **Budget for each message:** the watch asks for about 2 KB inbox and 512 B outbox (to be tuned, since both come out of the heap).
  - `PebbleOutbox` clips `TEXT` by **UTF-8 bytes** at a code-point boundary. Wear's `MAX_WATCH_TEXT` is 24,000 *characters*, which is useless here.
  - The whole reply kept on the watch is capped at about 6 KB, with an ellipsis.
- **Streaming:** `WatchRelay` resends the whole reply every 400 ms. `PebbleOutbox` turns that into **append-only deltas**:
  - Each delta is computed against the last **acked** offset for that watch, never against the last update built. A dropped or NACKed tail is then simply resent as part of the next delta.
  - The watch NACKs any `TEXT_OFFSET` that isn't its current text length. The phone then falls back to a replace.
  - A replace is a run of chunks from offset 0, each fitting the inbox (about 6 KB of reply won't fit a 2 KB inbox in one go).
  - One message is in flight at a time, at about 1 per second. Only *unsent* updates are dropped, the latest `SEQ` winning. Nothing already sent is skipped, so the text can't silently corrupt.
- **Plain text:** strip markdown (code fences, headings, links) before sending. The watch draws plain text in a `ScrollLayer`.
- **Speech:** the watch always sends `speak=false` for now, so the relay never voices. `PebbleOutbox.stream()` returns `false`.
- **Replay:** `PebbleOutbox` keeps the last `REPLY` for each watch. `onAppOpened` resends it, and so does a `STATUS_REQ`. Reopening the app mid-turn then shows where things stand.

### 4.3 Node identity

PebbleKit 2 gives a `WatchIdentifier` on every message. Node ids are `pebble:<id>`. The relay already keys everything by node string, so nothing changes there.

### 4.4 Rings and the closed-app problem

**Rings.** A Pebble rings whatever the gateway's live voice, because answering doesn't start a call (see the decision in §7). The ring goes like this:

1. `startAppOnTheWatch()` opens the watchapp.
2. The phone sends `RING`.
3. The watch shows a ring window, with a custom vibration pattern and a speaker ringtone.
4. The watch answers with `RING_ON`, so the existing "phone's call shows on the watch" fallback logic is reused.

**Finished replies while the app is closed.** When a `Done` reply gets `FailedDifferentAppOpen`, pick one of these (chosen in Phase 3 after testing on a device):

- (a) Post a normal reply notification for that chat, which the Pebble app mirrors. This is simplest, and it means releasing the side-chat hold.
- (b) Push a timeline pin.

Either way, the AppGlance is updated the next time the app runs.

### 4.5 Background wake (a risk, spiked first)

On Wear, `keepUp()` starts `ChatService` as a foreground service from inside the Data Layer callback, because Play Services grants a temporary allowlist to start one. It is **unverified** ❓ whether a PebbleKit 2 bound-service callback lets Herald start a foreground service on Android 14+ with Herald fully closed. If it doesn't, the relay stalls.

Fallbacks, in order:

1. The Pebble app's binding keeps us alive enough to run the turn without a foreground service.
2. Ask the user to exempt Herald from battery optimisation, which lifts the background-start ban. A foreground service *type* such as `connectedDevice` doesn't lift it on its own.

---

## 5. Phases

Each phase ends in something testable on the user's phone and Time 2.

### Phase 0: groundwork and baseline (no Herald code)

1. **Fork and branch.** Fork `Giton22/herald` to `faultoverload/herald`. Set the local git identity to the faultoverload noreply address. Then open a feature-request issue upstream proposing the plan, since CONTRIBUTING asks for an issue before anything big.
2. **Toolchain.** Arch ships Python 3.14, which pebble-tool doesn't support:

   ```
   uv tool install --python 3.13 pebble-tool
   pebble sdk install latest
   ```

   Build and run the PebbleKit 2 sample ([`sample/`](https://github.com/pebble-dev/PebbleKitAndroid2/tree/main/sample)) on the Time 2 with the Core app. That proves a message round trip.
3. **Developer Connection.** Turn it on in the Pebble app. Then `pebble install --phone <ip>` and `pebble logs --phone <ip>` work.
4. **Zero-code baseline.** Install Herald's current release and record what the Time 2 shows for each case below. Test each one with the phone screen on and with it off.
   - A finished-reply notification, replying by dictation and by canned reply.
   - An approval notification, using Approve/Deny.
   - A question notification.
   - The ringing "Hermes is calling" notification. It is expected **not** to mirror (ongoing).
   - The watch chat's quiet requests (`setLocalOnly`). These are expected not to mirror.
   - Which speech-to-text backend the Pebble app uses, and whether it needs internet or a subscription.
5. **Security check.** Herald sets `setAuthenticationRequired(true)` on approve and reply actions so a locked phone can't approve. `NotificationActionReceiver` doesn't check the lock itself (no `KeyguardManager` use), so the system shade is the only gate. **Find out whether firing them from the Pebble skips the unlock.**
   - It isn't specific to Pebble: any notification listener (Gadgetbridge, other bridges) can fire the `PendingIntent` directly. If the unlock is skipped, it may deserve an upstream issue on its own.
   - It also decides the wrist-approval policy below.

*Exit:* a short findings note appended to this doc, and the decisions in §7 made.

#### Phase 0 findings so far (2026-10-27)

**Setup used:** Pixel 9a on Android 17, Core Pebble app 1.15.0.1, Pebble Time 2, pebble-tool 5.0.40 with SDK 4.33.1.

- **Toolchain** ✅
  - Installed with `uv tool install --python 3.13 pebble-tool`, then `pebble sdk install latest`.
  - The PebbleKit 2 sample watchapp builds for every platform.
  - The sample's Android app (compileSdk 37, AGP 9.4.1) builds against a user-owned SDK in `~/Android/Sdk`. The AUR has no API 37 platform package. Install `android-sdk-cmdline-tools-latest`, accept the licenses with `sdkmanager --sdk_root=$HOME/Android/Sdk --licenses`, and the Gradle build downloads the platform and build-tools itself.
- **Developer Connection** ✅
  - It defaults to the **CloudPebble proxy**, so `pebble install --phone <ip>` is refused until *Settings → Phone → Connectivity → Use LAN developer connection* is on and the Dev Connection toggle is cycled.
  - After that, `pebble install --phone`, `pebble logs --phone` and `pebble screenshot --phone` all work.
  - LAN mode has no sign-in, so turn it off off-network.
- **PebbleKit 2 round trip** ✅
  - Watch → phone: a button press reached `BasePebbleListenerService.onMessageReceived` as `{1=UInt32(2)}`, with `WatchIdentifier(C111131100QK)`.
  - Phone → watch: `startAppOnTheWatch` returned `Success`, followed by `onAppOpened`. `sendDataToPebble` with a text item returned `Success`, and the text showed on the watch, confirmed by screenshot.
  - `onAppClosed` fires when the app is left.
  - No permission prompt appeared for the sample (its `companionApp` entry was enough).
- **Still to do:** the zero-code notification baseline and the approval-unlock security check (item 4 and item 5 above). Both need Herald signed in to a test gateway.

### Phase 1: phone transport (Herald only; tested against the PebbleKit 2 sample or a stub watchapp)

1. Add `io.rebble.pebblekit2:client` and `client-ui` to `libs.versions.toml` and `androidApp`.
2. `PebbleLink` codec in `shared/wearlink`, with tests:
   - Key table versus `package.json`.
   - UTF-8 clipping, including emoji at the cut.
   - Delta and replace.
   - Markdown stripping.
   - Round-trip of every type.
3. `RoutingOutbox`, `PebbleOutbox` (in-flight queue, latest wins, replay cache) and `PebbleBridgeService` (manifest entry with the `RECEIVE_DATA_FROM_WATCH` action).
4. **Spike: background wake (§4.5)** with Herald swiped away, on Android 14+. **Gate:** don't build the watchapp until a turn completes from a closed phone app.
5. Settings → Watch: a "Pebble" switch and the companion-app picker (`enableAutoSelect = false`).

*Exit:* a stub watchapp (the sample with our keys) sends `ASK "hello"` and logs streamed `REPLY` deltas through to `Done`.

### Phase 2: watchapp MVP (talk)

1. `pebbleApp/` scaffold: `targetPlatforms: ["emery"]` first, a `companionApp` entry, `messageKeys`, and the inbox/outbox sizes.
2. Talk window:
   - Select opens dictation, which sends `ASK`.
   - A header shows the chat title and the phone status.
   - A status line shows the phase and activity.
   - The reply streams into a `ScrollLayer`, with Up/Down to scroll.
   - Back during a turn sends `STOP`, after a confirm.
3. Phone status and error states: "Sign in to Herald on your phone first", unreachable gateway, "I didn't catch that".
4. The picked chat persists in storage, so the next ask continues it.
5. A fake-phone mode for the emulator (`pebble build && pebble install --emulator emery`), modelled on `wearApp/FakePhone.kt`, so the UI can be built without hardware.

*Exit:* on the Time 2, ask by voice, watch the reply stream in, and stop it. The phone's own chat isn't moved.

### Phase 3: chats, rings, closed-app delivery

1. Chats window: "New chat" plus up to 15 recent chats with how long ago each was active (`CHATS_REQ` and `CHAT_ITEM` paging), and the picked chat marked.
2. `WaitingOnPhone`: the watch says to answer on the phone, and a short vibration flags it.
3. `PebbleRinger`: rings as in §4.4, with the answer behaviour chosen in §7. Ring end goes both ways (decline on the watch stops the phone, and the reverse).
4. A finished reply while the app is closed uses the fallback chosen in §4.4.

*Exit:* the full talk → approval → reply loop on the device, with the app open and with it closed.

### Phase 4: glanceables and polish

1. Quick launch straight into dictation (the tile equivalent).
2. AppGlance with the last reply's first line, or "Working…" (the complication equivalent).
3. The accent colour mapped to `GColor`; the status sends only when the accent revision changes.
4. Canned prompts menu (for example "Summarize my day", "What's running?"), configurable later.
5. Touch: tap to dictate, swipe to scroll (emery only).

### Phase 5: other models, CI and release

1. Add `basalt`, `chalk`, `diorite`, `flint` and `gabbro` to `targetPlatforms`. Black-and-white and round layouts use `PBL_COLOR` and `PBL_ROUND`. The ringtone is speaker-only, vibration everywhere else.
2. CI: a job that installs pebble-tool (Python 3.13) and runs `pebble build`, uploading `herald-pebble.pbw`. The release workflow attaches `herald-pebble-$v.pbw` next to `herald-wear-$v.apk`. `UpdateChecker` must skip it the way it skips the watch APK (see commit `cb98f888`).
3. Docs: a README section ("Pebble watch: open the `.pbw` on your phone") and ticked ROADMAP items.
4. Optionally, publish to the Pebble app store through the [developer dashboard](https://developer.repebble.com/dashboard).

### Phase 6: stretch (each a spike first)

- **Gateway voice on the speaker:** measure AppMessage throughput on the Time 2, transcode the TTS to 8 kHz/8-bit PCM on the phone, and stream it. Keep it only if it plays without stutter.
- **Heart rate or health as context** for prompts.

---

## 6. Testing

| Layer | How |
|---|---|
| Codec and protocol | Kotlin `commonTest` in `shared/wearlink`, run in CI with the existing `:shared:wearlink:testAndroidHostTest` |
| Phone transport | Unit tests with a fake `PebbleSender`. The relay already has a fake-outbox pattern (`WatchTurnTest`) |
| Watch UI | The emulator (`emery`) with fake-phone mode. **The emulator can't reach a real PebbleKit 2 companion on the phone**, so it is for UI only |
| End to end | Real hardware only: the user's Android phone and Time 2, Developer Connection, `pebble logs --phone`, and `adb logcat` for Herald. Test against a test gateway, never real chats in screenshots (CONTRIBUTING) |

Device test script for each phase:

1. Ask with Herald in the foreground.
2. Ask with Herald in the background.
3. Ask with Herald swiped away.
4. Ask with the phone locked.
5. Ask with Bluetooth dropped mid-turn, then reconnected.
6. Change the accent colour.
7. Sign out of Herald.

---

## 7. Decisions for you

1. **Fork only, or upstream too?** The suggestion is to fork to `faultoverload/herald`, open an upstream issue first, and later submit as separate PRs:
   - (1) phone transport and protocol;
   - (2) watchapp;
   - (3) CI and release `.pbw`.

   Each PR ticks ROADMAP.
2. **Approving from the wrist.** Wear deliberately never sends the question to the watch, and approvals need a phone unlock. The options:
   - (a) Keep that rule: Pebble says "answer on your phone". This is the safest choice and the default.
   - (b) Allow Approve/Deny and dictated answers on the Pebble, behind a setting.

   This depends on the Phase 0 security check.
3. **What "Answer" does on a Pebble ring**, since a live call is impossible:
   - (a) Open the request on the phone (the phone's call screen).
   - (b) Show the question and answer it by dictation, which needs (2b).
   - (c) Ring the watch only to say "look at your phone".
4. **C or Alloy.** The suggestion is C: it has the full API, runs on every model, and the reference sample is in C.
5. **Which Pebble models** count as done: Time 2 only, or every model in Phase 5.
6. **Which phone Pebble app** to support: the Core app (≥ 1.0.7.7), plus microPebble if it's easy (PebbleKit 2 supports both).
