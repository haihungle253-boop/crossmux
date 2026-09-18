# Proposal: AI Reading Companion

> Status: **Draft v2 — for discussion.** Not an accepted feature.
> Filed under [SCOPE.md](../../SCOPE.md) ("Use CrossMux Issues to discuss a
> substantial addition before investing in it"). Every resource figure is an
> **estimate** unless marked as measured.
>
> **v2 reframes v1.** v1 designed a stateless lookup tool. This version designs
> a *stateful co-reading companion*. See §2 for what changed and why.

## 1. Summary

An opt-in **reading companion**: a configurable persona that reads the same book
at the same pace as the reader, remembers what the two of them have discussed,
and talks about the book — not a lookup tool that answers isolated questions.

Three claims define the design:

1. **Progress-synced.** The companion only ever sees text the reader has already
   passed. This is implemented by clipping context at the reader's position
   (§9.1). It doubles as spoiler protection, but its real purpose is that the
   two of them are genuinely reading the same book at the same point.
2. **Stateful.** Persona and conversation history persist across sessions and
   are replayed into every request. A companion that forgets yesterday is not a
   companion.
3. **Persona is data, not code.** The personality and the question set are
   plain text files the reader edits directly on the SD card, with no rebuild
   and no flash (§9.2). What the companion becomes is the reader's decision,
   not the implementer's.

## 2. What changed from v1, and why

v1 modelled the feature as an extension of dictionary lookup: select a word, get
an answer, forget. That shape cannot express "read this book with me" — it has
no memory, no voice of its own, and no continuity between sessions.

| | v1 (tool) | v2 (companion) |
|---|---|---|
| Entry point | select a word | a conversation the reader can open at any time |
| State | none; each request independent | persona + per-book conversation history |
| Register | neutral, factual | configurable personality |
| Context | current page | reading position + history + persona |
| Typical prompt | "what does this word mean" | "how did that chapter land for you" |

**The transport layer is unchanged.** SSE decoding, incremental JSON parsing,
context clipping, buffer-then-paint, credential handling and the resource
discipline all carry over verbatim. What changes is the layer above: persona,
memory, and how a conversation is driven from four buttons.

Two v1 features are **deprioritised, not dropped**: dictionary fallback and
range selection are useful, but they are tool behaviours and no longer lead.

## 3. Motivation

CrossMux ships an offline StarDict lookup ([docs/dictionary.md](../dictionary.md))
that answers "what does this word mean". Three things it structurally cannot do:

- **Discuss.** A reader who just finished a chapter may want to react to it, not
  look anything up. Nothing in the firmware serves that.
- **Remember.** Reading a novel takes weeks across many sessions. No feature
  carries anything from one session to the next except a page number.
- **Have a point of view.** Disagreement, preference and a recognisable voice
  are what separate a companion from a reference work.

## 4. Non-goals

- A general-purpose chat assistant, or any always-on / background networking.
  [SCOPE.md](../../SCOPE.md) excludes "unbounded background networking" and
  "persistent workloads that prevent normal sleep".
- On-device inference. No model runs on the ESP32.
- Sending whole books anywhere. Context is hard-capped (§9.1).
- **Voice interaction — deferred, not rejected.** The hardware supports it and
  the path is proven; the integration cost and a product-character conflict put
  it out of scope for this proposal. The evidence is recorded in §13 so the
  decision can be revisited without repeating the research.

## 5. Constraints

| Constraint | Source | Consequence |
|---|---|---|
| **No touch panel; four buttons** (Back/Left/Function/Right) plus the AXP2101 power key | [waveshare-epaper-397.md](../engineering/waveshare-epaper-397.md); the `waveshare_epaper_397_hardware` block in [platformio.ini](../../platformio.ini) declares no touch controller, unlike `metalio_eink4_hardware` (CST816S) | Free-text entry is impractical as the main path. Conversation must be drivable by a handful of fixed actions (§7.2). |
| **ESP32-C3 baseline: ~380 KB RAM, no PSRAM** | [AGENTS.md](../../AGENTS.md) Golden Rule 1; [hardware-constraints.md](../engineering/hardware-constraints.md) | Gate behind a capability flag, S3 + PSRAM first (§11). Conversation history makes the context budget larger than v1's, which sharpens this. |
| **E-ink refresh is 0.3–1 s** | [waveshare-epaper-397.md](../engineering/waveshare-epaper-397.md) | Never render per token (§9.4). Also rules out a conversational rhythm faster than one exchange per pause. |
| **Connectivity is on-demand** | [SCOPE.md](../../SCOPE.md); the `startActivityForResultWith<WifiSelectionActivity>` pattern at [`OpdsBookBrowserActivity.cpp:629`](../../src/activities/browser/OpdsBookBrowserActivity.cpp) | Conversation happens at reading pauses, not mid-page (§7.1). |
| **The TLS backend this board uses does not verify certificates** | [`HttpDownloader.cpp`](../../src/network/HttpDownloader.cpp) has two backends. `runGetWolf` calls `setInsecure()` (`:79`); `runGet` attaches `esp_crt_bundle_attach` (`:154`) and cannot perform an unverified handshake at all. `base` in [platformio.ini](../../platformio.ini) sets `-DFREEINK_NET_WOLFSSL=1`, and `waveshare_epaper_397_hardware` inherits it through `sound_feedback_hardware`, so this target takes the wolfSSL path. Also unverified: [`WeReadHttpClient.cpp:247`](../../lib/WeReadWebApi/src/WeReadHttpClient.cpp), [`KOReaderSyncClient.cpp:84,120,152,262`](../../lib/KOReaderSync/KOReaderSyncClient.cpp) | Unacceptable for a request carrying a bearer credential (§10.1). |

## 6. Architecture

### 6.1 Layering

Mirrors the split CrossMux already uses for WeRead — portable logic in `lib/`,
UI in `src/activities/` — so the logic layer is host-testable, as
`test/weread_webapi/` and `test/streaming_json_parser/` already are.

```
lib/AiCompanion/                 host-testable, no HAL dependency        [built]
  SseDecoder.{h,cpp}             frames "data: {...}\n\n" out of a byte stream
  AiChatParser.{h,cpp}           drives StreamingJsonParser; extracts delta content
  AiChatClient.{h,cpp}           one exchange: response bytes in, reply out
  PersonaStore.{h,cpp}           loads and validates the persona file
  QuestionSet.{h,cpp}            loads the editable question list
  ConversationStore.{h,cpp}      per-book history: append, trim, replay
  PromptBuilder.{h,cpp}          persona + history + clipped excerpt + caps
  AiNoteStore.{h,cpp}            saved exchanges as notes                 [planned, B3]

src/network/HttpDownloader                                                [built]
  postJson()                     streaming JSON POST, on both TLS backends

src/activities/reader/companion/
  CompanionFiles.{h,cpp}         SD paths, reads and atomic writes         [built]
  CompanionConfig.{h,cpp}        endpoint, model, credential from SD       [built]
  CompanionPageText.h            current page -> plain text for context    [built]
  CompanionChatActivity          the conversation surface (§7.2)           [built]
  CompanionSelectActivity        range selection                          [planned, M4]

  The reply is shown by the reader's existing DictionaryDefinitionActivity,
  already a paginated viewer with CJK wrapping and batched SD-font loading.
  Renaming it to a neutral PagedTextActivity is the tidy follow-up; writing a
  second text layout engine was not worth the better name.

src/activities/settings/
  CompanionSettingsActivity      a settings screen for the above           [not planned]
    Connection settings live in /companion/config.txt instead, for the same
    reason the persona does: they are configuration, and a four-button device
    is a poor place to type a URL and a credential.

test/ai_companion/               gtest suite, registered in test/CMakeLists.txt  [built]
```

### 6.2 Request flow

```
  trigger: chapter end | reader menu | selected passage
      |
      v
  PromptBuilder
      |-- persona.txt                         (resent every request, §9.2)
      |-- last N exchanges from history        (§9.3)
      |-- book excerpt clipped at reading position (§9.1)
      |-- position facts: chapter title, percent
      v
  AiHttpClient --POST--> provider (or self-hosted proxy, §9.6)
      |
      | SSE chunks arrive via HttpDownloader::DataCallback;
      | returning false aborts -> this is Back-to-cancel
      v
  SseDecoder -> AiChatParser -> PSRAM accumulation buffer
      |
      v
  CompanionAnswerView   single repaint (§9.4)
      |
      +--> ConversationStore.append(question, reply)
      +--> optionally AiNoteStore (§9.7)
```

### 6.3 Reuse points

**Reading position.** `EpubReaderActivity` already maintains
`(spineIndex, visibleTextOffset)` for KOReader progress sync
([`EpubReaderActivity.cpp:975-991`](../../src/activities/reader/EpubReaderActivity.cpp)),
backed by `Page::visibleTextOffset` ([`Page.h:90`](../../lib/Epub/Epub/Page.h)),
and resolves a chapter title via `getStatsChapterTitle()` (`:165`). That is
exactly the triple §9.1 needs. A chapter boundary is a change in `spineIndex`,
so the M2 trigger needs no new bookkeeping either.

**Streaming.** `HttpDownloader::DataCallback`
([`HttpDownloader.h:21`](../../src/network/HttpDownloader.h)) is a
`bool(const uint8_t*, size_t)` called per body chunk, where returning `false`
aborts the transfer. With the SAX-style
[`StreamingJsonParser`](../../lib/JsonParser/StreamingJsonParser.h), an SSE
response decodes without whole-body buffering, and cancellation is free.

`HttpDownloader` exposes GET only today; this proposal adds
`postJson(url, headers, body, DataCallback)` beside the existing `fetchUrl`
family.

**Per-book file paths.** `BookmarkUtil::getBookmarkPath(bookPath)`
([`BookmarkUtil.h`](../../src/util/BookmarkUtil.h)) already derives a per-book
sidecar path from a book path, with directory creation hidden inside
([`BookmarkFile.h`](../../src/util/BookmarkFile.h)). `ConversationStore` follows
the same pattern rather than inventing a second scheme.

**Text selection (M4).** `DictionaryWordSelectActivity` extracts
`WordBox { x, y, width, row, text }` from the laid-out `Page` and repaints
cursor moves differentially via a saved-pixel snapshot
(`DictionaryWordSelectActivity.h:77`). Widening `int selected` (`:58`) to a
`selStart`/`selEnd` pair yields range selection with the same repaint strategy.

### 6.4 Files on the SD card

The visible/hidden split matters and is not cosmetic. `/.crosspoint/` is a
**hidden** directory (`BookmarkUtil.cpp:6`, `ReadingBackground.h:13`), whereas
hand-managed content sits at a visible root — manually installed dictionaries
use `/dictionaries/`, with `/.dictionaries/` reserved for manager-installed ones
(`DictionaryRegistry.cpp:17`). Files the reader is expected to edit by hand must
follow the visible convention, or they cannot be found on operating systems that
hide dot-directories by default.

| Path | Visible | Written by | Purpose |
|---|---|---|---|
| `/companion/persona.txt` | **yes** | the reader | personality, in any language (§9.2) |
| `/companion/questions.txt` | **yes** | the reader | one preset question per line |
| `/companion/README.txt` | **yes** | shipped | how to edit the two files above |
| `/.crosspoint/companion/<book>/history.bin` | no | firmware | conversation history (§9.3) |
| `/.crosspoint/companion/config.bin` | no | firmware | endpoint, model, credential (§10.2) |
| `/.crosspoint/companion/disclaimer.accepted` | no | firmware | consent marker (§10.3) |

A missing or empty `persona.txt` is not an error: the companion falls back to a
built-in neutral persona and says so once.

Starter copies of the three visible files are kept in
[`assets/companion/`](../../assets/companion) and are covered by the
`ShippedAssets` tests, so an edit that pushes the persona past its cap or leaves
a stray encoding in it fails in CI rather than on a reader's device.

### 6.5 Entry points

- **Chapter end** — the M2 trigger, and the primary one (§7.1).
- **Reader menu** — one item beside `{MenuAction::DICTIONARY, StrId::STR_LOOKUP}`
  ([`EpubReaderMenuActivity.cpp:85`](../../src/activities/reader/EpubReaderMenuActivity.cpp)).
- **Long-press Confirm** — `LONG_PRESS_MENU_FUNCTION`
  ([`CrossPointSettings.h:205`](../../src/CrossPointSettings.h)) already offers
  `LP_MENU_DICTIONARY`. Append a new value at the **end** of the enum and of the
  `SettingsList.h` array; the comment above the enum warns that inserting in the
  middle silently reinterprets stored indices.

## 7. Interaction design

### 7.1 Conversation happens at pauses

A round trip costs a Wi-Fi association plus generation — several seconds. That is
acceptable at a chapter boundary, which is already a natural stop, and
unacceptable between pages.

So the companion is **invited, never interrupting**. It does not comment
spontaneously, and there is no per-page mode.

### 7.2 Driving a conversation with four buttons

Free-text entry exists — [`KeyboardEntryActivity`](../../src/activities/util/KeyboardEntryActivity.h)
— but four-button text entry is too slow to be the main path. Depth comes from
fixed actions instead, in two groups:

**Openers**, from the reader's own `questions.txt`, e.g.:

```
读完这一章，你有什么感受？
这一段用了什么文学手法？
你觉得这个角色现在在想什么？
```

**Continuations**, built in and always available after a reply:

```
[ 展开说说 ]   [ 换个角度 ]   [ 我不同意 ]   [ 那后来呢 ]   [ 自己输入 ]
```

Real conversation is mostly made of moves this generic — *go on*, *really?*,
*I'm not sure I agree*. Four fixed continuations plus a persona are enough to
reach genuine depth without typing.

**`我不同意` ("I disagree") is the load-bearing one.** It is the control that
turns a generated answer into an actual exchange, and it should be present from
M3 rather than added later.

### 7.3 Reply length

A companion's replies are longer than a tool's. Length is governed from
`persona.txt` (the shipped example asks for ~200 characters) rather than
hardcoded, so the reader tunes it against their own patience and page size.
`CompanionAnswerView` paginates whatever arrives.

## 8. Feature plan

### Tier A — the companion itself

| ID | Feature | Notes |
|---|---|---|
| **A1** | **Configurable persona** | `persona.txt`, resent on every request (§9.2) |
| **A2** | **Chapter-end conversation** | Zero selection; the first thing that feels like a companion |
| **A3** | **Multi-turn memory** | Per-book history, last N exchanges replayed (§9.3) |
| **A4** | **Continuation actions** | §7.2 |
| **A5** | **Editable question set** | `questions.txt` |

### Tier B — continuity and depth

| ID | Feature | Notes |
|---|---|---|
| **B1** | **Discuss a passage** | Range selection (§6.3) → "what do you make of this" |
| **B2** | **Resume brief** `[built]` | Reopening after 3 days: the openers begin with "where did we leave off", answered from the conversation rather than the open page. Distinct from v1's version because it draws on conversation history, not just the text. |
| **B3** | **Saved exchanges** | Persist an exchange as a note anchored to the position (§9.7) |
| **B4** | **Marked passages** | Mark while reading with no radio; discuss them together later (§9.5) |

### Tier C — later

- **Dictionary fallback.** Local StarDict miss → ask the model with the
  surrounding sentence. Carried over from v1; still worthwhile, no longer lead.
- **Reading digest.** Weekly recap from saved notes and exchanges, not book text.
  Surfaces via [`reading-stats`](../../src/activities/apps/reading-stats/README.md).
- **Per-book persona override.** Global persona is the v2 decision (§14); this is
  the natural extension if it proves too coarse.

## 9. Key design decisions

### 9.1 Progress-synced context

Every request states the reader's position — chapter, percent — and the supplied
excerpt is **clipped at the current reading position** using the
`(spineIndex, visibleTextOffset)` pair from §6.3.

This is what makes the claim "we are reading this together" true rather than
decorative, and it is also the spoiler guard. A companion that answers "who is
this character" by revealing the ending is worse than no companion. It is a
property of the design, not a setting.

Context is hard-capped by `PromptBuilder`. Whole books are never sent.

### 9.2 Persona is data, not code

`persona.txt` is free-form plain text in any language, written by the reader in
any text editor, taking effect on the next request with no rebuild and no
re-flash.

Rationale: the persona determines almost everything about how the feature feels,
and the reader is better placed than the implementer to judge it. Iteration on
it must not be gated on a development cycle.

Shipped example (`/companion/README.txt` explains the file; the content below is
illustrative, not enforced):

```
你是我的读书搭子，我们正在一起读同一本书，进度完全同步。

性格：博学但不端着，说话像朋友不像老师。
      有自己的偏好和判断，可以不同意我。

规矩：不要复述剧情（我刚读完，我知道）。
      说你注意到的细节、你的疑问、你的想法。
      回答控制在 200 字以内。
```

**The persona is resent in full on every request.** Personas drift when a model
is left to remember its own instructions across a long conversation; resending
costs tokens and removes the failure mode entirely.

### 9.3 Conversation memory

- **Scope: per book.** Reading two books means two independent relationships.
  Path derivation follows `BookmarkUtil` (§6.3).
- **Window: the last 8–10 exchanges**, oldest dropped first.
- **No summarisation in v2.** Compacting older history into a running summary
  preserves more, but costs an extra request per compaction and adds a failure
  mode; revisit once the window proves too short in practice.

History is appended after a reply completes, so an aborted or failed request
leaves no partial state. The file follows the versioned-header convention in
[file-formats.md](../file-formats.md), and its version must be bumped before any
layout change ([AGENTS.md](../../AGENTS.md) Golden Rule 10).

### 9.4 Buffer, then paint once

SSE is consumed incrementally so Back can abort mid-generation, but rendering
happens **once**, when the reply completes — or at most at paragraph boundaries.
A static indicator covers the wait, optionally with the existing `SoundFeedback`
cue.

Token-by-token repaint on a 0.3–1 s panel is not a degraded experience; it is an
unusable one.

### 9.5 Marking without the radio

The reader can mark a passage to discuss and **keep reading** with no network
activity. A later explicit action raises Wi-Fi once, works through the marked
passages in a single session, and disconnects.

This matches the on-demand model in [SCOPE.md](../../SCOPE.md), removes per-item
Wi-Fi wake cost, and keeps reading uninterrupted. It is a peer of the live mode,
not a fallback.

### 9.6 Provider abstraction

Target the OpenAI-compatible `/chat/completions` shape — the de-facto
interchange format, so one implementation reaches most hosted providers and
local runtimes.

v2 proposed an `AiProvider` abstraction and a dedicated `AiHttpClient`. Neither
was built, and both look like the wrong shape now: the endpoint, model and
credential are configuration rather than code, and the device already has an
HTTP client, so the transport became one `postJson()` on `HttpDownloader`
instead of a parallel stack. `AiChatClient` holds only the exchange, with the
transport injected. A second provider shape, if one is ever needed, is a reason
to add the abstraction then — not now.

**A user-run proxy is the recommended configuration** (home server, NAS, small
edge worker). It is better on three axes simultaneously: the provider credential
never lands on the SD card; heavy work moves off the MCU; and certificate
handling collapses to a single pinned endpoint (§10.1). Direct-to-provider
remains supported.

### 9.7 Saved exchanges use KOReader's note shape

CrossMux already syncs progress with KOReader ([`lib/KOReaderSync`](../../lib/KOReaderSync)).
Saved exchanges should reuse the fields KOReader annotations already carry —
`datetime`, `text`, `note`, `note_format`, `chapter`, `pageno`, `pos0`, `pos1`
(`frontend/apps/reader/modules/readerannotation.lua:66`) — so they export to
Markdown and are readable by existing KOReader tooling. Free now, expensive to
retrofit.

## 10. Security and privacy

### 10.1 Certificate verification is required here

On this target the wolfSSL backend (§5) skips certificate and hostname
validation. Tolerable for fetching a public dictionary; not for a request
carrying an API credential, where anyone able to intercept the connection
obtains a key with real billing attached.

The scope of the work is narrower than it first appears, and the precedent is
already in the tree: the sibling `runGet` backend verifies against the bundled
CA roots through `esp_crt_bundle_attach`, with a comment recording that the
build disables `CONFIG_ESP_TLS_INSECURE` so an unverified handshake cannot be
established there at all. The task is to give the wolfSSL path an equivalent
trust anchor, not to invent verification from nothing.

In order of preference:

1. **Self-hosted proxy with a pinned certificate** (§9.6) — one trust anchor,
   and the provider credential never reaches the device.
2. **Bundle the root CA** for the configured endpoint and verify properly.

Resolved before shipping, not after.

### 10.2 Credential storage

Reuse the device-bound obfuscation envelope already used for WeRead session data
(`session.bin`, [file-formats.md](../file-formats.md)). This is obfuscation, not
encryption — the threat model is casual SD-card inspection, and the
documentation must say so plainly.

### 10.3 Informed consent

Book text and conversation history leave the device. Follow the WeRead
precedent: a first-run disclaimer with a persisted acceptance marker, naming the
endpoint and what is sent. The feature is **off by default** and inert until
configured.

Conversation history is personal in a way a page number is not. The reader must
be able to delete a book's history, and deleting the book must offer to delete
it too.

## 11. Resource budget

Answering [SCOPE.md](../../SCOPE.md)'s acceptance test.
**All figures are estimates pending measurement on hardware.**

**1. User benefit** — §3.

**2. Resource cost** (estimated):

| Item | Estimate |
|---|---|
| Persona | ≤ 1 KB, capped on load |
| Conversation window (8–10 exchanges) | ≤ 4 KB |
| Book excerpt | ≤ 3 KB |
| **Assembled request context** | **≤ 8 KB**, hard cap (v1: 4 KB — the increase is history and persona) |
| Reply accumulation buffer | 8 KB |
| SSE line buffer + parser state | ~2 KB |
| TLS record buffers | 20–40 KB; ~8 KB if the server negotiates a smaller max fragment length |
| **Peak additional RAM** | **~45–65 KB**, in PSRAM on S3 targets |
| Largest single block | the TLS record buffer |
| Flash | 50–70 KB for the new lib and activities |
| Steady state | **zero** — nothing retained outside the activity; everything allocated in `onEnter()` is released in `onExit()` ([AGENTS.md](../../AGENTS.md) Golden Rule 9) |
| Persistent storage | per-book history, bounded by the window; user-clearable |

**3. Power and lifetime** — Wi-Fi is raised only for an invited exchange and
dropped immediately after; §9.5 amortises one association across many marked
passages. No task outlives the activity, and nothing runs in the background, so
idle sleep is unaffected.

**4. Maintenance** — No new third-party dependency: HTTP, TLS and JSON parsing
all exist in-tree. New failure modes are network timeout, provider error,
malformed SSE, credential rejection, and malformed or oversized persona /
question files; each needs a distinct translated message (`tr()`, Golden Rule 3).

**Baseline gating.** Ship behind a capability flag enabled on S3 + PSRAM targets
first. TLS itself is already proven on the C3 (`KOReaderSync` performs TLS
requests there today), so the constraint is buffer headroom, not TLS
feasibility. A C3 build would need a reduced context cap and its own measurement
pass; §14 asks whether it is worth supporting at all.

## 12. Milestones

| # | Deliverable | Verification |
|---|---|---|
| **M0** | `lib/AiCompanion/` transport: SSE decode, incremental JSON, prompt assembly, byte caps | `test/ai_companion/` gtest suite, modelled on `test/streaming_json_parser/` |
| **M1** | `HttpDownloader::postJson()`; one real exchange end-to-end in the desktop simulator | `pio run -e simulator -t run_simulator` (the host build verifies certificates through the system trust store) |
| **M2a** | Companion data layer: persona, question set and per-book history, all taking buffers so file I/O stays at the device edge | Host: parsing, caps, eviction, save/load round-trip, and the shipped `assets/companion/` files |
| **M2b-1** | `HttpDownloader::postJson()` and `CompanionFiles` — the device edge | CI: first compile of `lib/AiCompanion` into firmware |
| **M2b-2** | **A1 + A2 + A4 + A5** — the conversation activity, the reader-menu entry and the continuation actions | CI: compiles for the C3 baseline. Device: first exchange that reads as a companion; heap before/after |
| **M3** | **A3 + A4** — history and continuation actions | Device: a multi-turn conversation surviving a power cycle |
| **M4** | **A5 + B1** — editable questions, passage discussion | Device: selection, refresh behaviour, cancellation, heap across 20 exchanges |
| **M5** | **B3** — saved exchanges with KOReader fields | Device + export round-trip |
| — | **B2** — resume brief, brought forward: it needed only a timestamp per exchange and one conditional opener | Host: version 1 histories still load; the gap stays unknown when the clock is unset. Device: the offer appearing after a real gap |

M0 and M1 are entirely host-side; nothing is flashed before M2.

M0 through M2b-2 are implemented and pass CI, including the firmware build for
the ESP32-C3 baseline, as are the self-hosted proxy, the phone chat surface, and
B2. What remains before a reader can use this on hardware is a flash and an
actual chapter: no part of it has run on a device yet, and nothing here
establishes how it feels to read with.

### 12.1 What B2 rests on

The resume brief is the first feature that depends on the device knowing what
time it is, and CrossPoint's clock is not guaranteed to be set. Every part of
the feature is therefore built to be silent rather than wrong:
`TimeUtils::getCurrentValidTimestamp()` returns 0 when the clock is untrustworthy,
exchanges stored under an unset clock keep a timestamp of 0, and both cases leave
the gap unknown, which suppresses the offer instead of announcing a gap of
twenty thousand days.

The on-disk history format went to version 2 to carry the timestamps. Version 1
files still load, with their exchanges simply having no time: refusing them would
have thrown away a reader's existing conversations to gain a feature they had not
asked for.

One inaccuracy remains inherent to the device, and the proxy resolves it. The
reader counts the gap from its own local history, which holds only the turns it
made itself; an argument on the phone yesterday leaves the reader still believing
it has been a fortnight. So the proxy, which holds the superset, rewrites that
line the same way it replaces the history under it — and only ever rewrites a line
the reader already put there, because whether to offer a resume at all stays the
device's decision.

**M2 is the first milestone that delivers the actual product.** It is chosen as
the MVP because a chapter boundary needs no text selection, is already a reading
pause so latency is tolerable, and requires persona, position and context to all
be correct at once — a genuine end-to-end test of the idea.

## 13. Deferred: voice interaction

Recorded so the decision can be revisited without repeating the research.

**The hardware supports it.** In the Waveshare `ESP32-S3-ePaper-3.97` repository,
`ESP-IDF/02_Mic_test/components/codec_board/board_cfg.txt` defines this board as:

```
Board: S3_ePaper_3_97
i2s: {bclk: 14, ws: 47, dout: 48, din: 21, mclk: 13}
in_out: {codec: ES8311, pa: 39, use_mclk: 1, pa_gain:6}
```

Every pin matches the contract in
[waveshare-epaper-397.md](../engineering/waveshare-epaper-397.md) **except
`din: 21`**, the I²S input line, which CrossMux does not use. The board has a
microphone through the ES8311's ADC, and `ESP-IDF/02_Mic_test` records from it at
16 kHz. Waveshare also ships a prebuilt voice-assistant firmware for this exact
board at `Firmware/xiaozhi/ESP32-S3_e-Paper-3.97_xiaozhi.bin`, so voice AI on
this hardware is demonstrated, not hypothetical.

**Why it is still deferred.**

1. CrossMux has no audio input path at all — `lib/hal/` contains only
   `HalAudioOutput.{h,cpp}`. Capture, encode and upload would be built from
   nothing.
2. The shipped voice firmware is a dedicated appliance, not a reader. Hosting a
   voice pipeline inside the reader means two subsystems contending for RAM and
   CPU.
3. **Product character conflict.** Voice interaction wants low latency, an open
   microphone and a live connection. This device is built around a 0.3–1 s panel,
   on-demand networking and multi-day battery life. This is a conflict of
   character, not of feasibility, and it is the reason to wait.
4. Continuous capture plus a held connection removes the battery life that is the
   device's main advantage.

**If revisited**, three options, cheapest first: use a phone as the microphone
through the existing web server surface; keep text only; or build the capture
path natively. Flashing the shipped `xiaozhi` firmware is a zero-development way
to evaluate the experience first — it replaces CrossMux, so back up the SD card
and confirm the restore path beforehand.

## 14. Open questions

1. Should the C3 be supported in v1, or explicitly deferred?
2. Is a **global** persona (the v2 decision) too coarse in practice, or does a
   per-book override earn its complexity?
3. Is a **window of 8–10 exchanges** with oldest-dropped enough, or does
   summarisation become necessary sooner than expected?
4. Should conversation history live in the per-book cache directory — and so
   participate in cache invalidation — or in a namespace that survives a cache
   clear? Losing a month of conversation to a cache clear would be bad.
5. Is the self-hosted proxy the *documented default*, with direct-to-provider
   marked advanced?

## 15. References

**CrossMux**
- [SCOPE.md](../../SCOPE.md) — acceptance test answered in §11
- [AGENTS.md](../../AGENTS.md) — golden rules 1, 2, 3, 9, 10
- [hardware-constraints.md](../engineering/hardware-constraints.md) — the resource protocol
- [waveshare-epaper-397.md](../engineering/waveshare-epaper-397.md) — target hardware contract
- [dictionary.md](../dictionary.md) — the lookup flow Tier C extends
- [file-formats.md](../file-formats.md) — sidecar conventions, WeRead credential envelope

**Waveshare `ESP32-S3-ePaper-3.97`** (vendor repository, cited in §13)
- `ESP-IDF/02_Mic_test/` — microphone capture example and board pin table
- `Firmware/xiaozhi/` — prebuilt voice-assistant firmware for this board

**KOReader** (prior art, not code to port)
- `frontend/apps/reader/modules/readerhighlight.lua:159-215` — the highlight
  dialog's pluggable button table, and `addToHighlightDialog()` at `:1519`.
- `frontend/apps/reader/modules/readerannotation.lua:66` — the annotation record
  adopted in §9.7.
- `frontend/ui/trapper.lua:341,500` — cancellable network operations behind a
  dismissable widget; the same contract §9.4 implements via `DataCallback`
  returning `false`.
- `plugins/vocabbuilder.koplugin/` — spaced-repetition prior art for Tier C.
