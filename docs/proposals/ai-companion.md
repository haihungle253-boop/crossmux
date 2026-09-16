# Proposal: AI Reading Companion

> Status: **Draft for discussion** — not an accepted feature. Filed under
> [SCOPE.md](../../SCOPE.md) ("Use CrossMux Issues to discuss a substantial
> addition before investing in it"). Every resource figure below is an
> **estimate** unless explicitly marked as measured.

## 1. Summary

Add an opt-in, on-demand **AI reading companion**: a question-answering layer
attached to the reader's existing word-selection pipeline, not a general chat
application.

The three claims that define the design:

1. It reuses the dictionary selection machinery
   ([`DictionaryWordSelectActivity`](../../src/activities/reader/DictionaryWordSelectActivity.h))
   rather than inventing a new text-selection UI.
2. It is **progress-aware and spoiler-guarded** — the model only ever receives
   text the reader has already passed, and is told where the reader is.
3. It treats the network as a scarce, explicit resource: requests are batched,
   answers are cached to SD, and an **offline question queue** is a first-class
   mode, not a fallback.

Without (2) the feature is a chatbot that happens to run on an e-reader. With
it, it is a companion.

## 2. Motivation

CrossMux already ships an offline StarDict lookup
([docs/dictionary.md](../dictionary.md)). It answers "what does this word mean"
and nothing else. Three gaps remain that an offline dictionary structurally
cannot close:

- **Sense disambiguation in context.** A dictionary returns all senses; it
  cannot tell you which one this sentence uses, and it misses idioms, classical
  Chinese usage, and proper nouns entirely. This is the single highest-value,
  lowest-cost gap.
- **Paragraph-level comprehension.** Non-native readers and readers of dense
  non-fiction want "what is this page saying", which no per-word tool provides.
- **Continuity across sessions.** An e-reader is picked up days apart. Nothing
  in the firmware helps a reader re-enter a book they left two weeks ago.

## 3. Non-goals

- A general-purpose chat assistant, or any always-on / background networking.
  [SCOPE.md](../../SCOPE.md) excludes "unbounded background networking" and
  "persistent workloads that prevent normal sleep".
- On-device inference. No model runs on the ESP32.
- Sending whole books anywhere. Context is hard-capped (§7.1).
- Text-to-speech. The Waveshare 3.97 does have an ES8311 codec and NS4150B
  amplifier driving 16 kHz mono PCM
  ([waveshare-epaper-397.md](../engineering/waveshare-epaper-397.md)), so
  spoken answers are physically possible — but streaming TTS decode is a
  separate project and is out of scope here.

## 4. Constraints that shape the design

| Constraint | Source | Consequence |
|---|---|---|
| **No touch panel; four buttons** (Back/Left/Function/Right) plus the AXP2101 power key | [waveshare-epaper-397.md](../engineering/waveshare-epaper-397.md) hardware contract; the `waveshare_epaper_397_hardware` block in [platformio.ini](../../platformio.ini) declares no touch controller, unlike `metalio_eink4_hardware` (CST816S) | Selecting text is expensive. The **primary path must require zero selection**; range selection is an enhancement, not the entry point. |
| **ESP32-C3 baseline: ~380 KB RAM, no PSRAM** | [AGENTS.md](../../AGENTS.md) Golden Rule 1; [hardware-constraints.md](../engineering/hardware-constraints.md) | Gate the feature behind a capability flag, enabled on S3 + PSRAM targets first (§9). |
| **E-ink refresh is 0.3–1 s** | [waveshare-epaper-397.md](../engineering/waveshare-epaper-397.md) refresh/LUT section | **Never render per token.** Accumulate the full answer, then paint once (§7.2). |
| **Connectivity is on-demand** | [SCOPE.md](../../SCOPE.md); the `startActivityForResultWith<WifiSelectionActivity>` pattern at [`OpdsBookBrowserActivity.cpp:629`](../../src/activities/browser/OpdsBookBrowserActivity.cpp) | Connect, ask, disconnect. Batch where possible (§7.3). |
| **Existing TLS does not verify certificates** | `setInsecure()` at [`HttpDownloader.cpp:79`](../../src/network/HttpDownloader.cpp), [`WeReadHttpClient.cpp:247`](../../lib/WeReadWebApi/src/WeReadHttpClient.cpp), [`KOReaderSyncClient.cpp:84,120,152,262`](../../lib/KOReaderSync/KOReaderSyncClient.cpp) | Acceptable for public dictionary downloads; **not** acceptable for a request carrying a bearer credential (§8.1). |

## 5. Architecture

### 5.1 Layering

Mirrors the split CrossMux already uses for WeRead — portable protocol logic in
`lib/`, UI in `src/activities/` — so the protocol layer is unit-testable on the
host, as `test/weread_webapi/` and `test/streaming_json_parser/` already are.

```
lib/AiCompanion/                 host-testable, no HAL dependency
  AiProvider.h                   provider abstraction (§7.4)
  AiHttpClient.{h,cpp}           POST + Server-Sent Events
  SseDecoder.{h,cpp}             frames "data: {...}\n\n" out of a byte stream
  AiChatParser.{h,cpp}           drives StreamingJsonParser; extracts delta content
  PromptBuilder.{h,cpp}          context assembly + hard byte caps (§7.1)
  AiNoteStore.{h,cpp}            per-book answer cache and note persistence

src/activities/reader/ai/
  AiRangeSelectActivity          sentence/paragraph selection (§5.3)
  AiAskActivity                  preset question list; custom entry via keyboard
  AiAnswerActivity               paginated answer view

src/activities/settings/
  AiSettingsActivity             provider, endpoint, model, credential, disclaimer

test/ai_companion/               gtest suite, registered in test/CMakeLists.txt
```

### 5.2 Request flow

```
  reader page
      |
      | (a) zero-selection: whole current Page
      | (b) range selection: AiRangeSelectActivity
      v
  PromptBuilder ---- reading position (spineIndex, visibleTextOffset)
      |         \--- book title / author / chapter title
      |         \--- spoiler-guard system instruction   (§7.1)
      v
  AiHttpClient  --POST--> provider (or self-hosted proxy, §7.4)
      |
      | SSE byte chunks arrive via HttpDownloader::DataCallback
      | (returning false aborts -> this is the Back-to-cancel implementation)
      v
  SseDecoder -> AiChatParser -> PSRAM accumulation buffer
      |
      | complete (or paragraph boundary)
      v
  AiAnswerActivity  -- single e-ink repaint --  [ optional: save as note ]
```

### 5.3 Reuse points

Three pieces of existing code do most of the work.

**Selection.** `DictionaryWordSelectActivity` already extracts a
`WordBox { x, y, width, row, text }` list from the laid-out `Page`'s
`TextBlock`s, and repaints a cursor move differentially by restoring saved
pixels under the old highlight instead of re-running the full two-pass page
render (`SNAPSHOT_CAPACITY`, `DictionaryWordSelectActivity.h:77`). Extending
`int selected` (`:58`) to a `selStart`/`selEnd` pair gives range selection with
the same repaint strategy: Left/Right extend by word, Up/Down by line,
Confirm asks, Back cancels.

**Streaming.** `HttpDownloader::DataCallback`
([`HttpDownloader.h:21`](../../src/network/HttpDownloader.h)) is a
`bool(const uint8_t*, size_t)` invoked per body chunk, where returning `false`
aborts the transfer. Paired with the existing SAX-style
[`StreamingJsonParser`](../../lib/JsonParser/StreamingJsonParser.h), an SSE
response can be decoded with no whole-body buffering — and cancellation comes
free.

`HttpDownloader` currently exposes GET only; this proposal adds a
`postJson(url, headers, body, DataCallback)` overload alongside the existing
`fetchUrl` family.

**Reading position.** `EpubReaderActivity` already carries the
`(spineIndex, visibleTextOffset)` pair for KOReader progress sync
([`EpubReaderActivity.cpp:975-991`](../../src/activities/reader/EpubReaderActivity.cpp)),
backed by `Page::visibleTextOffset`
([`Page.h:90`](../../lib/Epub/Epub/Page.h)), and resolves a chapter title via
`getStatsChapterTitle()` (`:165`). That is exactly the triple the spoiler guard
needs — no new bookkeeping.

### 5.4 Entry points

- **Reader menu.** One item beside the existing
  `{MenuAction::DICTIONARY, StrId::STR_LOOKUP}`
  ([`EpubReaderMenuActivity.cpp:85`](../../src/activities/reader/EpubReaderMenuActivity.cpp)).
- **Long-press Confirm.** `LONG_PRESS_MENU_FUNCTION`
  ([`CrossPointSettings.h:205`](../../src/CrossPointSettings.h)) already offers
  `LP_MENU_DICTIONARY`. Append a new value at the **end** of the enum and of the
  `SettingsList.h` array — the comment above the enum warns that inserting in
  the middle silently reinterprets stored indices.
- **Dictionary miss.** The `Popup::NotFound` branch in
  `DictionaryWordSelectActivity::performLookup()` (`.cpp:220-222`) offers
  "ask AI" instead of dead-ending (feature A2).

## 6. Feature plan

### Tier A — minimum viable

| ID | Feature | Behaviour | Why first |
|---|---|---|---|
| **A1** | **Ask about a selection** | Select sentence/paragraph → preset question list → paginated answer | The core interaction; three keypresses, no typing |
| **A2** | **Dictionary fallback** | Local StarDict miss → ask the model, passing the **whole sentence** as context | Smallest diff (one existing branch), smallest response, and the one thing an offline dictionary structurally cannot do |
| **A3** | **Explain this page** | Zero selection. Concatenate the current `Page`'s `TextBlock`s (~1–2 KB) → 2–3 sentence summary | Best possible UX on a four-button device: nothing to select |

Preset questions (no keyboard needed): *what does this mean* / *translate* /
*what is this reference* / *what does this word refer to here*. Free-form
questions route through the existing
[`KeyboardEntryActivity`](../../src/activities/util/KeyboardEntryActivity.h),
which is usable but slow with four buttons — hence presets first.

### Tier B — what makes it a companion

| ID | Feature | Behaviour | Notes |
|---|---|---|---|
| **B1** | **Resume brief** | Reopening a book after *N* days shows a short "where you left off" card | Context is only chapter title + previous page + the reader's own highlights. The highest-value item in this proposal: it is the one feature that is *proactive* and bound to reading progress. |
| **B2** | **Chapter recap + questions** | At a chapter boundary, optional recap plus two comprehension questions | Hooks the chapter transition / [`EndOfBookOptions`](../../src/activities/reader/EndOfBookOptions.h). Off by default. |
| **B3** | **Who's who** | Select a name → "who is this?", answered **only** from text already read | Cached per book, so re-asking is free and offline. High value in long fiction. |
| **B4** | **AI notes** | Any answer can be saved as a note anchored to the highlight | Store it in the shape KOReader uses (§7.5) so notes survive export and sync. |

### Tier C — later

- **Reading digest.** Weekly recap built from *saved highlights and notes*, not
  book text — cheap. Surfaces through
  [`reading-stats`](../../src/activities/apps/reading-stats/README.md) or a
  standby face.
- **Vocabulary review.** Spaced repetition over words captured via A2,
  scheduling entirely local; the network is touched only to generate example
  sentences. Compare KOReader's `vocabbuilder.koplugin`.
- **Spoken answers.** See §3.

## 7. Key design decisions

### 7.1 Spoiler guard (non-negotiable)

Every request carries a system instruction stating the reader's position —
*"the reader is in chapter N, X% through; use only the supplied text and do not
reveal anything beyond this point"* — and the supplied text is **clipped at the
current reading position**, using the `(spineIndex, visibleTextOffset)` pair
from §5.3.

A companion that spoils the ending when asked "who is this character" is worse
than no companion. This is the feature's defining constraint, not a setting.

Context is hard-capped by `PromptBuilder` at **4 KB** (2 KB on a constrained
target). Whole books are never sent.

### 7.2 Buffer, then paint once

SSE is consumed incrementally so that Back can abort mid-generation, but
rendering happens **once**, after the answer completes — or at most at paragraph
boundaries. A static "thinking" indicator covers the wait, optionally with the
existing `SoundFeedback` cue.

Token-by-token repaint on a panel with a 0.3–1 s refresh is not a degraded
experience; it is an unusable one.

### 7.3 Offline question queue

Alongside the live mode, the reader can mark a question and **keep reading**
with no radio activity. A later explicit *Sync companion* action brings Wi-Fi up
once, sends the queued questions in a single session, writes the answers into
the per-book note file, and disconnects.

This matches the on-demand connectivity model in
[SCOPE.md](../../SCOPE.md), removes per-question Wi-Fi wake cost, and is
arguably the better reading experience — no network latency interrupts a page.
It should be a peer of the live mode, not a degraded fallback.

### 7.4 Provider abstraction

Target the OpenAI-compatible `/chat/completions` shape. It is the de-facto
interchange format, so one implementation reaches most hosted providers and
local runtimes without per-vendor code. Provider-specific handling stays behind
`AiProvider`.

**A user-run proxy is the recommended configuration** (a home server, a NAS, a
small edge worker). It is better on three axes at once:

- The real provider credential never lands on the SD card.
- Heavy work (long context, retrieval, future TTS) moves off the MCU.
- Certificate handling collapses to a single pinned endpoint (§8.1).

Direct-to-provider stays supported for users who accept the trade-off.

### 7.5 Note format compatibility

CrossMux already syncs progress with KOReader ([`lib/KOReaderSync`](../../lib/KOReaderSync)).
AI notes should reuse the field set KOReader's annotations already carry —
`datetime`, `text`, `note`, `note_format`, `chapter`, `pageno`, `pos0`, `pos1`
(`frontend/apps/reader/modules/readerannotation.lua:66`) — so notes can be
exported to Markdown and read by existing KOReader tooling. Aligning field names
costs nothing now and is expensive to retrofit later.

Any new binary sidecar must follow the versioned-header convention in
[file-formats.md](../file-formats.md) and bump its format version before any
layout change ([AGENTS.md](../../AGENTS.md) Golden Rule 10).

## 8. Security and privacy

### 8.1 Certificate verification is required here

The existing `setInsecure()` calls (§4) skip server certificate and hostname
validation. For fetching a public dictionary that is a tolerable trade-off. For
a request carrying an API credential it is not: any party able to intercept the
connection on the local network obtains a key with real billing attached.

Two options, in order of preference:

1. **Self-hosted proxy with a pinned certificate** (§7.4). One trust anchor,
   and the provider credential never reaches the device.
2. **Bundle the root CA** for the configured endpoint and verify properly.

This must be resolved before the feature ships, not after.

### 8.2 Credential storage

Reuse the existing device-bound obfuscation envelope already used for WeRead
session data (`session.bin`, [file-formats.md](../file-formats.md), §"WeRead").
This is obfuscation, not encryption — the threat model is casual SD-card
inspection, and the documentation must say so plainly.

### 8.3 Informed consent

Book text leaves the device. Follow the WeRead precedent: a first-run disclaimer
with a persisted acceptance marker (`disclaimer.accepted`), stating which
endpoint receives data and what is sent. The feature is **off by default** and
inert until configured.

## 9. Resource budget

Answering the four questions in [SCOPE.md](../../SCOPE.md)'s acceptance test.
**All figures are estimates pending measurement on hardware.**

**1. User benefit** — §2.

**2. Resource cost** (estimated):

| Item | Estimate |
|---|---|
| Context buffer | 4 KB (2 KB on a constrained target) |
| Answer accumulation buffer | 8 KB |
| SSE line buffer + parser state | ~2 KB |
| TLS record buffers | 20–40 KB; ~8 KB if the server negotiates a smaller max fragment length |
| **Peak additional RAM** | **~35–55 KB**, allocated in PSRAM on S3 targets |
| Largest single block | the TLS record buffer |
| Flash | 40–60 KB for the new lib and activities |
| Steady state | **zero** — nothing is retained outside the activity; everything allocated in `onEnter()` is released in `onExit()` ([AGENTS.md](../../AGENTS.md) Golden Rule 9) |
| Persistent storage | per-book answer cache and notes, bounded, user-clearable |

**3. Power and lifetime** — Wi-Fi is raised only for an explicit request and
dropped immediately after; the queue mode (§7.3) amortises one connection across
many questions. No task outlives the activity. Idle sleep is unaffected because
nothing runs in the background.

**4. Maintenance** — No new third-party dependency: HTTP, TLS, and JSON parsing
all exist in-tree. New failure modes are network timeout, provider error
response, malformed SSE, and credential rejection; each needs a distinct,
translated message (`tr()`, Golden Rule 3).

**Baseline gating.** Ship behind a capability flag enabled on S3 + PSRAM targets
first. TLS itself is already proven on the C3 (`KOReaderSync` performs TLS
requests there today), so the constraint is buffer headroom rather than TLS
feasibility; a C3 build would need the reduced caps noted above and its own
measurement pass.

## 10. Milestones

| # | Deliverable | Verification |
|---|---|---|
| **M0** | `lib/AiCompanion/` protocol layer: SSE decode, incremental JSON, prompt assembly, byte caps | `test/ai_companion/` gtest suite, modelled on `test/streaming_json_parser/` |
| **M1** | `HttpDownloader::postJson()`; one real request end-to-end in the desktop simulator | `pio run -e simulator -t run_simulator` (the host build verifies certificates through the system trust store) |
| **M2** | **A2** dictionary fallback | Device: lookup miss path, heap before/after |
| **M3** | **A3** page summary, then **A1** range selection | Device: refresh behaviour, cancellation, `ESP.getFreeHeap()` / `getMaxAllocHeap()` across 20 requests |
| **M4** | **B1** resume brief, **B4** notes with KOReader-compatible fields | Device + export round-trip |

M0 and M1 are entirely host-side. Nothing needs to be flashed until M2.

## 11. Open questions

1. Should the C3 be supported at all in v1, or explicitly deferred?
2. Preset question list: fixed in firmware, or user-editable from SD?
3. Does the answer cache belong in the existing per-book cache directory
   (and therefore participate in cache invalidation and format versioning), or
   in a separate namespace that survives a cache clear?
4. Is the self-hosted proxy the *documented default*, with direct-to-provider
   marked advanced?

## 12. References

**CrossMux**
- [SCOPE.md](../../SCOPE.md) — acceptance test this proposal answers in §9
- [AGENTS.md](../../AGENTS.md) — golden rules 1, 2, 3, 9, 10
- [hardware-constraints.md](../engineering/hardware-constraints.md) — the resource protocol
- [waveshare-epaper-397.md](../engineering/waveshare-epaper-397.md) — target hardware contract
- [dictionary.md](../dictionary.md) — the lookup flow this feature extends
- [file-formats.md](../file-formats.md) — sidecar conventions, WeRead credential envelope

**KOReader** (referenced as prior art, not as code to port)
- `frontend/apps/reader/modules/readerhighlight.lua:159-215` — the highlight
  dialog's pluggable button table (`Wikipedia` / `Dictionary` / `Translate`),
  and `addToHighlightDialog()` at `:1519`. The model for adding one more verb to
  a selection without disturbing the others.
- `frontend/apps/reader/modules/readerannotation.lua:66` — the annotation record
  adopted in §7.5.
- `frontend/ui/trapper.lua:341,500` — cancellable network operations behind a
  dismissable progress widget; the same contract §7.2 implements via
  `DataCallback` returning `false`.
- `plugins/vocabbuilder.koplugin/` — spaced-repetition prior art for Tier C.
