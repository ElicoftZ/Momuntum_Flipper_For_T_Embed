# Hotspot Arcade — 20 new games (implementation plan for Codex)

Target repo: `Flipper-Zero-ESP32-Port/` (T-Embed / ESP32-S3 single-chip port of
hotspot-arcade). This document is the complete brief: architecture, exact touch
points, per-game specs, and ordering. **Everything here runs offline** — the board
is its own SoftAP, phones join it, no internet is used or needed at any point.

Upstream licence is NOASSERTION — do not redistribute vendored upstream code
without checking. New game code written here is ours.

---

## 0. State of the tree right now (READ FIRST)

A previous session started three of these games and stopped mid-way. **The tree
currently has ids, constants and state structs for RPS / Math Rush / Simon but NO
implementations and NO dispatch wiring.** It still compiles (unused structs), but
selecting game 21/22/23 would do nothing.

Already in place:
- `components/hotspot_arcade_service/vendor/engine/ha_proto.h`
  - `HA_FW_VERSION` bumped 20 -> 21
  - `HA_GAME_RPS = 21`, `HA_GAME_MATHRUSH = 22`, `HA_GAME_SIMON = 23`
- `components/hotspot_arcade_service/vendor/engine/ha_games.h`
  - `RPS_*`, `MATH_*`, `SIMON_*` `#define` block (after the `REACT_*` defines)
  - `struct RpsState`, `struct MathState`, `struct SimonState` (before `ReactState`)
  - `_rps`, `_math`, `_simon` added to the game-state union

Still missing for those three, and for every other game: engine methods, all six
dispatch hooks, service/FAP name tables, and the whole web UI.

Two helpers the specs below assume but which **do not exist yet — add them**:
- `int partyDeadlineSec(const Party& pt)` — ceil seconds until `pt.deadline`,
  clamped at 0. Mirror the existing `partyCountdownSec()` exactly.
- `int usedPlayerCount()` — count of `_p[i].used`, several games need it.

---

## 1. Architecture you must respect

One chip. Upstream's Flipper<->ESP32 UART split is collapsed: networking lives in
firmware (`components/hotspot_arcade_runtime/`), game logic in
`components/hotspot_arcade_service/` (C++ shim + vendored `vendor/engine/`), and
the on-device UI is a FAP (`applications_user/hotspot_arcade/`) that reaches the
service through exported `hotspot_arcade_service_*` symbols.

**Phones are the real UI.** The board serves ONE gzipped page,
`sdcard/apps_assets/hotspot_arcade/web/index.html.gz`, and all gameplay is
WebSocket JSON between that page and `Engine` in `ha_games.h`.

### Chip constraints that shape every decision
- `CONFIG_FREERTOS_UNICORE=y` — AP + HTTP + WS + UI all share core 0.
- Internal DRAM is the scarce resource, not total heap. Measure with
  `heap_caps_get_largest_free_block()`, never free size.
- The game-state union is sized by its **largest** member (chess/battleship
  arrays). Small new structs cost **zero** additional RAM. Keep every new state
  struct well under those.
- **No new SD content packs.** Every game below generates its own content, or uses
  a small `static const char* const` table which lives in flash, not RAM. This
  avoids all `HA_MSG_CONTENT_*` work, `tools/stage_hotspot_arcade_assets.py`
  changes, and SD staging.
- No new FAP API symbols are needed (`tools/add_symbol.py` is NOT involved) —
  the FAP only ever names games, it does not call into them.

### The web bundle is loaded whole into RAM
`hotspot_arcade_runtime.c:216-232` reads `index.html.gz` into one
`heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` block, **falling back to
internal DRAM** if PSRAM fails. Today: 262 KB raw / **63.5 KB gzipped**.

Twenty games will roughly double it (~110-120 KB gzipped). That is fine in PSRAM
but the internal fallback would be fatal. **Required change (P0-b):** make the
fallback allocation failure a hard, logged error rather than a silent internal-DRAM
grab, and log `web=%u bytes` against the largest free PSRAM block at start.

---

## 2. P0 — fix these BEFORE adding any game

### P0-a. Game-change crash (blocks all 20 games)
Documented root cause: `hotspot_arcade_service_select_game` (service.cpp:1515) runs
`Engine::selectGame()` **inline on the FAP's Furi thread**, which reaches
`httpd_queue_work()` -> lwIP `sendto` -> `sys_thread_sem_get` ->
`pthread_getspecific` -> NULL+0x10. Furi threads have no pthread TLS.

Symptom: `Guru Meditation (LoadProhibited), EXCVADDR=0x10` whenever the game is
changed **with a phone connected**. With no client it never reaches the send and
appears to work.

**Fix shape:** `select_game` must not run engine work on the caller's thread. Post
a request to the existing `ha_engine` task (it already has an 8 KB PSRAM stack)
and let that task call `Engine::selectGame()`. Same treatment for any other
service entry point that can reach `haWsSendWs()` from a non-lwIP thread.

Do **not** raise httpd `config.stack_size` — that was tried and reverted; it
regressed Start Session to out-of-memory because that stack is internal DRAM.

Adding 20 games multiplies exposure to this bug by 20. It is genuinely first.

### P0-b. Web bundle allocation
See above. Also re-check `web=%u bytes` in the start log after the bundle grows.

### P0-c. Add the two missing helpers
`partyDeadlineSec()` and `usedPlayerCount()` (section 0).

---

## 3. The six dispatch hooks — every game touches all of them

For a game `foo` with id `HA_GAME_FOO`, in `ha_games.h`:

| # | Location | Add |
|---|---|---|
| 1 | `dispatchClear(uint8_t id)` (~line 1316) | `else if(id == HA_GAME_FOO) fooClear();` |
| 2 | `tick(uint32_t now)` (~1355) | `else if(_active == HA_GAME_FOO) fooTick(now);` |
| 3 | `partyRosterChanged()` (~3318) | `else if(_active == HA_GAME_FOO) fooCheckStart();` |
| 4 | `pushAll()` (~1880) | `else if(_active == HA_GAME_FOO) haWsSendWs(_p[pid].wsId, fooJson(pid));` |
| 5 | `gameName(uint8_t g)` (~1956) | `case HA_GAME_FOO: return "foo";` |
| 6 | `onMsg` dispatch (~1465-1620) | `fooReady(pid, r)` in the `"ready"` branch, `fooAgain(pid)` in the `"again"` branch, plus the game's own message type |

Every per-game handler **must** early-out on `if(_active != HA_GAME_FOO) return;`
— the `"ready"` / `"again"` branches call every game's handler unconditionally.

Outside `ha_games.h`:

| File | Change |
|---|---|
| `vendor/engine/ha_proto.h` | new `HA_GAME_*` id |
| `hotspot_arcade_service.cpp:290` `game_name()` | `case HA_GAME_FOO: return "foo";` |
| `hotspot_arcade_service.cpp:1199` round-result keys | add `"foo"` to the array |
| `hotspot_arcade_service.cpp:1154` and `:1517` | bound checks `<= HA_GAME_FRANKENDRAW` -> `<= HA_GAME_TUGOFWAR` (the new highest id) |
| `applications_user/hotspot_arcade/hotspot_arcade.c:20` | append the display name to `hotspot_arcade_game_names[]`, **in id order** |
| `applications_user/hotspot_arcade/scenes/hotspot_arcade_scene_games.c:4` | `HOTSPOT_ARCADE_GAME_COUNT` 20 -> 40 |

### Web (`applications_user/hotspot_arcade/assets/web/index.html.gz`)
The bundle is readable, formatted HTML+CSS+JS — decompress, edit, recompress. Per
game add:
1. `SCREENS[]` (~line 690) — the screen id
2. `GAME_SCREEN{}` — `foo: "foo"`
3. `GAME_LABEL{}` — `foo: "Display Name"` (this map alone drives the phone's game picker)
4. `GAME_TITLE_KEY{}` — `foo: "foo.title"`
5. `GAME_MIN{}` (~1278) if it needs >2 players; `GAME_DUEL{}` if 1v1
6. a `<section id="foo" class="screen hide">` block
7. an IIFE with `A.handlers.foo = function(m) { route("foo"); ... }`
8. English strings in `MESSAGES.en`

**i18n:** `t()` falls back to `MESSAGES.en` for any missing key, so **add English
only**. Do not invent German or Portuguese translations.

**Styling: reuse, do not invent.** The look is a dark terminal theme driven by CSS
vars already defined (`--orange #FF8200`, `--bg #0B0B0C`, `--surface`, `--line`,
`--good`, `--bad`, `--info`, monospace `--font`, `--radius: 2px`, `--tap: 52px`).
Reuse the existing classes: `.screen`, `.bar`, `.btn`, `.btn.big`, `.players`,
`.podium`, `.count`, `.count-num`, `.me`, `.game-item`, `.opts`. New CSS only for
genuinely new shapes (a grid, a board), and it must use the same vars — no new
colours, no rounded corners, no non-monospace type.

**Shared JS helpers to reuse:** `A.readyLobby({players,listId,readyId,meId})`,
`A.countdown(id, sec)`, `A.podium(id, board)`, `A.showLead(scores, big)`,
`A.hideLead()`, `A.sfx("buzz"|"win"|"lose"|"tick"|"start")`, `A.vibe(ms|[..])`,
`t(key, params)`, `esc(s)`, `send(obj)`, `$(id)`, `route(screen)`.

### Rebuild the bundle (both copies, they must stay identical)

```bash
gzip -9 -n -c applications_user/hotspot_arcade/assets/web/index.html > applications_user/hotspot_arcade/assets/web/index.html.gz
```

```bash
cp applications_user/hotspot_arcade/assets/web/index.html.gz sdcard/apps_assets/hotspot_arcade/web/index.html.gz
```

`sdcard/apps_assets/hotspot_arcade - Copy` is stale leftover — ignore it.

---

## 4. Three implementation patterns

Pick the right one per game; they differ enormously in cost.

### Pattern A — "Arcade" (solo, client-side)
The entire game runs in the phone's JS. The board holds **only** the score and the
session high score. Engine state struct is ~40 bytes; there is no server tick, no
per-frame traffic, no physics on core 0.

- Client sends `{t:"score", v:<int>}` at game over (and optionally `{t:"begin"}`).
- Engine: `fooClear()`, `fooScore(pid, v)`, `fooJson(pid)`, `fooReady`, `fooAgain`.
  No `fooTick()` needed beyond the countdown phase; still register it in `tick()`
  for the lobby countdown.
- State: `Party pt; int best[HA_MAX_PLAYERS+1]; int last[HA_MAX_PLAYERS+1];`
- Screen: a lobby, the canvas/grid, a live score, and a shared leaderboard strip
  via `A.showLead()`. **Works with exactly one player** — `partyAllReady()`
  already returns true at `n >= 1`.
- Anti-cheat: none. It is a party toy on a local AP; do not build validation.

**This is the pattern for all 10 solo games. Do these first — they are cheap.**

### Pattern B — "Party round" (server-authoritative, N players)
Copy `reactClear/reactReady/reactCheckStart/reactArm/reactTap/reactReveal/
reactAgain/reactTick/reactJson` (ha_games.h ~3795-3950) verbatim and adapt. Phases:
`0 lobby, 1 countdown, 2 play, 3 reveal, 4 final`. Where a game needs a fifth
phase use **5**, never 3, so that **4 is always final** across every party game.

### Pattern C — "Duel" (1v1)
Reuse the `DuelMatch` machinery (`isDuel()`, `duelClear`, `duelMove`, `matchOf`,
`duelRematch`, challenge list). Only Gomoku and Nim below use it, and both are
board-shaped enough to fit `DuelMatch`'s existing `kind` field.

---

## 5. The 20 games

Ids continue from the three already claimed. **Final id map:**

| id | const | name (web key) | display name | pattern | min |
|----|-------|-----|-----|---------|-----|
| 21 | `HA_GAME_RPS` | `rps` | Rock Paper Scissors | B | 2 |
| 22 | `HA_GAME_MATHRUSH` | `math` | Math Rush | B (solo-ok) | 1 |
| 23 | `HA_GAME_SIMON` | `simon` | Simon Says | B (solo-ok) | 1 |
| 24 | `HA_GAME_IMPOSTOR` | `impostor` | Who Is The Impostor | B | 4 |
| 25 | `HA_GAME_BULLS` | `bulls` | Bulls & Cows | A | 1 |
| 26 | `HA_GAME_2048` | `g2048` | 2048 | A | 1 |
| 27 | `HA_GAME_SNAKE` | `snake` | Snake | A | 1 |
| 28 | `HA_GAME_MINES` | `mines` | Minesweeper | A | 1 |
| 29 | `HA_GAME_MEMORY` | `memory` | Memory Match | A | 1 |
| 30 | `HA_GAME_PUZZLE15` | `p15` | 15 Puzzle | A | 1 |
| 31 | `HA_GAME_HILO` | `hilo` | Higher or Lower | A | 1 |
| 32 | `HA_GAME_AIM` | `aim` | Aim Trainer | A | 1 |
| 33 | `HA_GAME_ODDONE` | `oddone` | Odd One Out | B | 3 |
| 34 | `HA_GAME_DICE` | `dice` | Liar's Dice | B | 2 |
| 35 | `HA_GAME_WORDBOMB` | `bomb` | Word Bomb | B | 2 |
| 36 | `HA_GAME_NIM` | `nim` | Nim | C | 1v1 |
| 37 | `HA_GAME_GOMOKU` | `gomoku` | Gomoku | C | 1v1 |
| 38 | `HA_GAME_CATEGORIES` | `cats` | Categories | B | 3 |
| 39 | `HA_GAME_BIDWARS` | `bid` | Bid Wars | B | 3 |
| 40 | `HA_GAME_TUGOFWAR` | `tug` | Tug of War | B | 2 |

`hotspot_arcade_scene_games.c` -> `HOTSPOT_ARCADE_GAME_COUNT 40U`.
Service bound checks -> `<= HA_GAME_TUGOFWAR`.

**Solo-playable (10):** 22, 23, 25, 26, 27, 28, 29, 30, 31, 32. Each works with a
single phone and no other players. Set `GAME_MIN` to 1 for these — but note
`gameMin()` defaults to 2, so they need explicit entries: add
`math:1, simon:1, bulls:1, g2048:1, snake:1, mines:1, memory:1, p15:1, hilo:1, aim:1`
to `GAME_MIN` in the web bundle, and drop the `(n+)` tag for min == 1 in
`gameMenuItem()`.

---

### 21. Rock Paper Scissors — `rps`, Pattern B, 5 rounds
Simultaneous throws, **pairwise** scored: every thrower is compared against every
other thrower, +100 per opponent beaten. A player who never throws is skipped on
both sides, so idling neither earns nor gifts points.
- Constants already present: `RPS_ROUNDS 5`, `RPS_SECS 10`, `RPS_REVEAL_MS 4500`, `RPS_WIN_POINTS 100`.
- Struct already present: `Party pt; int8_t pick[N+1]; int gained[N+1];` (`pick` -1 = no throw)
- Client -> `{t:"throw", c:0|1|2}` (rock/paper/scissors). First throw is final.
- Round settles early once all used players have thrown.
- `rpsJson` phases: `lobby` (you, players) / `countdown` (sec) / `throw` (round,
  rounds, pick, waiting, sec, scores) / `reveal` (round, rounds, you, picks[] of
  {pid,nick,avatar,pick,gained}, scores) / `final` (board = `triviaBoard()`).
- UI: three `.btn.big` pads with fist/palm/scissors glyphs; reveal shows the full
  table with each player's throw and the points they took.

### 22. Math Rush — `math`, Pattern B, 8 questions, **solo-ok**
Board generates `a + b` (2..99 each), `a - b` (a 10..99, b 1..a-1, so val >= 1), or
`a x b` (2..12 each). Four options: the answer plus three distinct non-negative
decoys within +/-10, shuffled.
- Constants present: `MATH_ROUNDS 8`, `MATH_SECS 12`, `MATH_REVEAL_MS 3000`,
  `MATH_BASE_POINTS 100`, `MATH_SPEED_POINTS 100`.
- Struct present: `Party pt; char q[20]; int opts[4]; uint8_t correct; uint32_t qStart; int8_t ans[N+1]; int gained[N+1];`
- Client reuses the existing `{t:"answer", c:0..3}` message — add `mathAnswer(pid, v)`
  alongside `triviaAnswer` / `wyrAnswer` in that branch.
- Score: `100 + 100 * (window - elapsed) / window`, floor 0. First answer stands.
- UI: reuse trivia's `.opts` four-button layout verbatim; the question is the
  big centred string.

### 23. Simon Says — `simon`, Pattern B, elimination, **solo-ok**
Phases `0 lobby, 1 countdown, 2 playback, 3 repeat, 5 level reveal, 4 final`.
Sequence grows by one pad per level; a wrong pad or the input timeout puts you out;
last player standing wins. Solo: runs until the single player misses.
- Constants present: `SIMON_MAX_LEN 20`, `SIMON_STEP_MS 700`, `SIMON_LEAD_MS 900`,
  `SIMON_INPUT_SECS 20`, `SIMON_REVEAL_MS 2500`, `SIMON_LEVEL_POINTS 100`.
- Struct present: `Party pt; uint8_t seq[20]; uint8_t len; uint8_t pos[N+1]; bool alive[N+1]; bool done[N+1]; bool missed[N+1]; uint32_t showUntil;`
- The whole sequence is pushed to every client each level (they all have to play it
  back anyway) with `lead` and `step` so the phone animates on the same clock.
- Client -> `{t:"pad", n:0..3}`.
- End: `alive == 0`, or (`usedPlayerCount() > 1` and `alive <= 1`), or `len >= 20`.
- UI: 2x2 pads. Colours must come from existing vars — `--orange`, `--good`,
  `--info`, `--bad` — not new hexes. Pad lights up via a `.lit` class.

### 24. Who Is The Impostor — `impostor`, Pattern B, min 4  [user request]
Every round, all players but one get the same secret **word**; the impostor gets
`???` and must bluff. Each player types a one-word clue about the word, everyone
reads all clues, then everyone votes. If the impostor is voted out, the crew each
score; if not, the impostor scores. The impostor also gets a bonus for correctly
guessing the word after being caught.
- Phases: `0 lobby, 1 countdown, 2 clue, 3 reading, 5 vote, 6 impostor-guess, 4 final`.
  (Again: 4 stays final.)
- Content: a `static const char* const IMPOSTOR_WORDS[]` table in flash, ~80 short
  nouns (`"PIZZA"`, `"AIRPORT"`, `"DENTIST"`, ...). **In flash, not RAM, and not on
  the SD card.** Pick with `esp_random() % count`, avoid immediate repeats.
- State: `Party pt; uint8_t impostor; uint8_t wordIdx; char clue[N+1][16]; int8_t vote[N+1]; uint8_t votedOut; bool caught; char guess[20];`
  Sizing note: `clue` is 13 * 16 = 208 bytes — well inside the union's existing
  largest member, so it costs nothing.
- Client -> `{t:"clue", text:"..."}` (already a message type — guard on `_active`),
  `{t:"vote", who:pid}`, `{t:"guess", text:"..."}`.
- Scoring: impostor survives a vote +300; crew +150 each if the impostor is voted
  out; impostor +200 extra for guessing the word.
- `impostorJson(pid)` **must be per-pid**: only the impostor's own payload may
  carry `"impostor":true`, and only the crew's may carry the word. This is the one
  game where leaking a field to the wrong pid ruins it — mirror how `wwJson(pid)`
  and `spyfallJson(pid)` already handle hidden roles.
- Min 4 players (`GAME_MIN.impostor = 4`); 3 makes the impostor obvious.
- UI: reuse the werewolf/spyfall screens' shape — a role card, a text input, a
  clue list, then a player-button vote grid.

### 25. Bulls & Cows — `bulls`, Pattern A, **solo**
Guess a 4-digit code with distinct digits. Each guess returns bulls (right digit,
right place) and cows (right digit, wrong place). Fewer guesses = more points.
Entirely client-side; the code is generated in JS. Score = `max(0, 1100 - 100*guesses)`
on a win, 0 on giving up.
- UI: a numeric keypad, a scrolling history list of `guess — 2B 1C`.

### 26. 2048 — `g2048`, Pattern A, **solo**
Classic 4x4 slide-and-merge, swipe controls (`touchstart`/`touchend` delta). Score
is the game's own score, reported at game over. Grid uses `--surface-2` tiles,
`--orange` for the high tiles, monospace numbers — **no new palette**.

### 27. Snake — `snake`, Pattern A, **solo**
`requestAnimationFrame` on a `<canvas>`, swipe or 4 on-screen arrows. Wall/self
collision ends it. Score = food eaten * 10. Keep the canvas at device pixel ratio
so it stays crisp; ~20x20 cells.

### 28. Minesweeper — `mines`, Pattern A, **solo**
9x9 / 10 mines. Tap to reveal, long-press (or a flag-mode toggle button) to flag.
First tap is always safe (generate the field after it). Score = `max(0, 600 - seconds)`
on a clear, 0 on a mine.

### 29. Memory Match — `memory`, Pattern A, **solo**
4x4 grid of 8 emoji pairs, flip two, keep matches. Score = `max(0, 1000 - 20*moves - 5*seconds)`.

### 30. 15 Puzzle — `p15`, Pattern A, **solo**
4x4 sliding tiles. **Generate by shuffling from solved with random legal moves**
(never a random permutation — half of those are unsolvable). Score =
`max(0, 1500 - 5*moves - 2*seconds)`.

### 31. Higher or Lower — `hilo`, Pattern A, **solo**
Card-based streak game off a generated 52-card deck. Guess whether the next card
is higher or lower; equal is a push. Score = streak * 100, one life.

### 32. Aim Trainer — `aim`, Pattern A, **solo**
30 seconds; a target appears at a random spot, tap it, next one spawns. Score =
`hits*50 - misses*20`, plus an average-reaction-ms line in the reveal. Targets are
`--orange` circles on `--surface`.

### 33. Odd One Out — `oddone`, Pattern B, min 3
Every round the board picks a set of four items where three share a property and
one does not (generate from a small flash table of themed quads, ~40 entries).
Everyone picks the odd one; correct = 100 + speed bonus. Same shape as Math Rush,
so implement it right after that one and share the option-button UI.

### 34. Liar's Dice — `dice`, Pattern B, min 2
Each player rolls 5 hidden dice. Turn order; a player either raises the bid
("four 3s") or calls "liar". On a call, all dice are revealed: if the bid holds the
caller loses a die, otherwise the bidder does. Out at zero dice; last player wins.
- Phases: `0 lobby, 1 countdown, 2 bidding, 5 reveal, 4 final`.
- State: `Party pt; uint8_t dice[N+1][5]; uint8_t nDice[N+1]; uint8_t turn; uint8_t bidQty; uint8_t bidFace; uint8_t lastBidder; bool out[N+1];`
- Per-pid JSON again: a player sees only their own dice until a call.
- Client -> `{t:"bid", qty:n, face:n}` / `{t:"liar"}`.
- The most complex of the 20 — schedule it last of the party games.

### 35. Word Bomb — `bomb`, Pattern B, min 2
A letter pair/trio is shown; the player whose turn it is must type a word
containing it before the timer runs out, then the bomb passes on with a shorter
fuse. Fail = lose a life; last standing wins.
- **No dictionary.** Accept any submission that is >= 3 letters, alphabetic, and
  contains the trio, and that has not already been used this round (keep a small
  ring of the last 32 accepted words). Say so in the UI ("no dictionary — play
  honest"). A real word list would not fit and is not worth the flash.
- Trios come from a flash table of ~60 common English fragments (`"ING"`, `"TRA"`,
  `"ONE"`, ...).
- Client -> `{t:"word", text:"..."}`.

### 36. Nim — `nim`, Pattern C, 1v1
21 counters, take 1-3, whoever takes the last one loses. Trivial to implement
inside the existing duel machinery — reuse `DuelMatch` with a new `kind`, store the
remaining count in the board array's first cell. Client -> the existing
`{t:"move", n:1|2|3}`.

### 37. Gomoku — `gomoku`, Pattern C, 1v1
Five in a row on 13x13. Also a `DuelMatch` `kind`; the board fits the existing
array if you size it as 169 cells — **check `DuelMatch`'s board dimension first**
and widen it only if that does not blow past the union's current largest member.
If it would, drop to 9x9 (five-in-a-row still works) rather than growing the union.
Client -> `{t:"move", n:0..168}`.

### 38. Categories — `cats`, Pattern B, min 3
A letter and a category ("Animals", "Cities", "Food" — ~30 entries in a flash
table). Everyone types an answer starting with that letter. Then everyone votes
each answer up or down; answers that survive score 100, and a **unique** surviving
answer scores 200.
- Phases: `0 lobby, 1 countdown, 2 write, 5 judge, 3 reveal, 4 final`.
- Reuses Fill-the-Blank's screen shape almost exactly — read `fillblankJson` first.

### 39. Bid Wars — `bid`, Pattern B, min 3
Each round a prize worth 100-500 is announced. Everyone secretly bids from a
starting bank of 1000 points. Highest **unique** bid wins the prize and pays their
bid; ties cancel out and pay nothing. Six rounds; richest wins.
- Pure numbers — no content table at all, the cheapest party game here.
- State: `Party pt; int bank[N+1]; int bidv[N+1]; int prize; int winner;`
- Client -> `{t:"bid", v:n}`. UI: a slider plus a numeric field, both clamped to
  the player's bank.

### 40. Tug of War — `tug`, Pattern B, min 2
Two teams (auto-split by pid parity, or first-half/second-half of the roster). Tap
as fast as you can for 20 seconds; the rope position is `tapsA - tapsB`, capped.
Team over the line wins, everyone on it scores.
- The one game with a per-tick push: **rate-limit the broadcast to ~5 Hz**
  (`_lastTug` + a 200 ms gate, exactly like `PONG_TICK_MS` gates `pongTick`), and
  count taps locally on the phone, sending a batched `{t:"taps", n:k}` every 200 ms
  rather than one message per tap. On a unicore chip a 12-phone tap storm at one
  message per tap will starve the AP.

---

## 6. Suggested order of work

1. **P0-a, P0-b, P0-c** (section 2). Nothing else is testable until P0-a lands.
2. **Finish 21, 22, 23** — structs and constants already exist; this is just
   methods, the six hooks, and three web screens. It also validates the whole
   pipeline end to end (engine -> service -> FAP names -> web -> gzip -> SD).
3. **The seven remaining solo Pattern-A games** (25-32). Cheapest per game, and
   they are the ones the user can actually test alone. Do all seven's engine side
   in one pass — they share `fooScore`/`fooJson` almost verbatim — then the seven
   screens.
4. **24 Who Is The Impostor** (the explicitly requested game).
5. **Party games 33, 39, 40, 35, 38** in that order (cheapest first).
6. **Duels 36, 37.**
7. **34 Liar's Dice** last.

Flash and hardware-test after **each** of steps 2, 3, 4 — not at the end. The user
tests every milestone on real hardware.

## 7. Verification checklist per game

- [ ] Firmware builds clean (`winbuild.py` — **it exits 0 without building if the
      IDF python env is missing; confirm the build actually ran**)
- [ ] FAP builds and reports **all symbols resolved** (no new symbols expected)
- [ ] `index.html` regzipped into **both** asset paths, byte-identical
- [ ] FAP copied to `sdcard/apps/Games/hotspot_arcade.fap`
- [ ] Game appears in the FAP's on-device game list at the right position
- [ ] Game appears in the phone's picker with the right min-players tag
- [ ] Switching **to** and **away from** it with a phone connected does not reset
      the board (this is the P0-a regression test)
- [ ] Solo games: playable with exactly one phone joined
- [ ] `heap_caps_get_largest_free_block()` at AP start is unchanged from before

## 8. Things that will bite you

- `furi_assert` is a **no-op** in this build (`FURI_DEBUG` is never defined), so
  every `furi_assert(ptr)` NULL guard is a live NULL deref. Use real `if(!p) return;`.
- Reboot the board between AP start attempts while debugging — a failed
  `esp_wifi_deinit` leaks ~10 KB of internal DRAM each time.
- "network error" (rc=6) in older logs meant **out of memory**, not networking.
- When chasing a reset, capture **raw** console — a keyword filter once hid the
  Guru Meditation text for several debugging rounds.
- Do not add anything to the SD card that the firmware version does not pin;
  `sdcard.zip` is version-locked to the firmware by the webflasher.
