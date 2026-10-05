# wkr — WaniKani Reinforcer

`wkr` is a small local CLI that reads your [WaniKani](https://www.wanikani.com)
review statistics, infers which kanji, vocabulary, and radicals you tend to
confuse with each other, and reports focus advice plus an optional local
drill. WaniKani's own review-history API was disabled in 2023 and never
recorded what you actually typed, so this isn't a transcript of your
mistakes — it's a statistical inference from failure counters, streaks, and
review timing (which subjects you got wrong close together in time, and how
visually/phonetically/semantically similar those subjects are to each
other).

## Your WaniKani data is never modified

`wkr` cannot write anything to your WaniKani account. The HTTP layer
(`src/http.{h,cpp}`) exposes exactly one function, `http::get()`. There is no
`POST`, `PUT`, `PATCH`, or `DELETE` call anywhere in the codebase — this is a
structural guarantee, not a promise in a comment, and you can verify it
yourself at any time:

```sh
grep -rn "CURLOPT_POST\|CUSTOMREQUEST\|CURLOPT_UPLOAD" src/
```

That command should always print nothing. It's also run as part of this
project's own review process. This is why the drill — which quizzes you
interactively — never touches your real SRS stages, review queue, or
streaks: it only ever reads from WaniKani and writes to its own local
SQLite database.

## Setup

1. **Enter the dev shell** (provides cmake, ninja, pkg-config, libcurl,
   sqlite3, nlohmann_json, and Catch2):

   ```sh
   nix develop
   ```

2. **Get a WaniKani API token**: on wanikani.com, go to Settings → API
   Tokens → Generate a new token. `wkr` only ever performs GET requests, so
   the default read-only scopes are all it needs — no special/elevated
   scope is required.

3. **Tell `wkr` about your token**, either:
   - set the `WANIKANI_API_TOKEN` environment variable, or
   - write it to `$XDG_CONFIG_HOME/wanikani-reinforcer/token` (falls back to
     `~/.config/wanikani-reinforcer/token` if `XDG_CONFIG_HOME` is unset),
     with file mode `0600`:

     ```sh
     mkdir -p ~/.config/wanikani-reinforcer
     echo "your-token-here" > ~/.config/wanikani-reinforcer/token
     chmod 600 ~/.config/wanikani-reinforcer/token
     ```

   The environment variable takes priority if both are present. `wkr` warns
   (without ever printing the token itself) if the token file isn't `0600`.

## Build

The repo is a monorepo: a C++ backend (`backend/`, binary `wkr-core`) and a
Rust/ratatui terminal UI (`frontend/`, binary `wkr`) that talks to it over
line-delimited JSON-RPC on the backend's stdio (see `docs/protocol.md`).

```sh
cmake -S backend -B backend/build -G Ninja && cmake --build backend/build
cargo build --release --manifest-path frontend/Cargo.toml
```

The backend build treats warnings as errors (`-Wall -Wextra -Werror`). The
TUI looks for `wkr-core` in `$WKR_CORE`, then next to its own executable,
then on `PATH`; during development:
`WKR_CORE=backend/build/wkr-core cargo run --manifest-path frontend/Cargo.toml`.

## Usage

`wkr` with no arguments opens the full-screen TUI:

| Key | Action |
|---|---|
| `d` | drill the confusion pairs that are due |
| `b` | review items you have burned on WaniKani |
| `s` | sync with WaniKani |
| `r` / `j` `k` | refresh / scroll the dashboard |
| `1`-`9` or type + `Enter` | answer; `Esc` aborts a session; `q` quits |

With arguments, `wkr` passes them to `wkr-core`, so scripts and cron keep
working:

```sh
wkr sync [--session-gap-minutes N]   # pull subjects/stats/assignments from WaniKani, detect failures
wkr report                           # print your current leeches, confusion pairs (with local SRS stage), and focus advice
wkr stats                            # print local drill history, session summary, and leech accuracy trend
wkr serve                            # speak JSON-RPC on stdio (what the TUI runs)
```

- `sync` is the only command that talks to the network. Run it first,
  and periodically afterwards — the inference gets better with more synced
  sessions over time (see Design notes below). `--session-gap-minutes`
  controls how large a gap between two failures must be before they're
  treated as separate study sessions; it defaults to 45. In the TUI, `s`
  does the same.
- The drill and burned review are purely local — they only read from
  the SQLite database `sync` populated, and they don't need an API
  token configured at all (only `sync` does). Each session asks 5 questions.
- The drill schedules each confusion pair with its own local
  spaced-repetition stage (stored in the `pair_srs` table; it never touches
  your WaniKani SRS). A pair you have just drilled is not asked again until
  it is due, and when nothing is due the TUI says when the next
  review is. Stages 1 to 8 come back
  after 4h, 8h, 1d, 2d, 7d, 14d, 30d and 120d; a correct answer moves up
  one stage, an incorrect one drops two (never below stage 1), and a
  correct answer on stage 8 burns the pair, which is then never asked
  again. `wkr report` shows each look-alike's stage, e.g. `[new]`,
  `[stage 3, due in 2d]` or `[burned]`.
- The burned review covers subjects WaniKani reports as burned
  (SRS stage 9), which WaniKani itself never shows again. Each item is
  asked its meaning first (same answer matching as the drill) and then,
  for kanji and vocabulary, its reading, always in that order and one item
  at a time. Readings can be typed in hiragana, katakana or romaji (`inu`,
  `nihon`, `gakkou`); the TUI previews the kana as you type. Every prompt
  is tagged `[Radical]`, `[Kanji]` or `[Vocabulary]`, since the same
  characters can exist as all three. If you type a real reading that
  WaniKani doesn't accept for that subject (e.g. こ for the kanji 子), you
  get a hint and the same question again, not a miss. An item only counts
  as correct when both answers are right. It keeps its own schedule in the
  `burned_srs` table: a correct answer returns the item after 30d, 60d,
  120d and then 240d (staying there), while a miss brings it back after 1d.
  Items you have never reviewed here are due immediately. The
  `session.start` RPC accepts `only`/`exclude` type filters
  (`radical`, `kanji`, `vocabulary`, the last including kana-only
  vocabulary); `exclude` is applied after `only`.
- `stats` is purely local, like `report`. It needs at least one drill for
  drill-history stats, at least one `sync` for session stats, and at least
  two separate syncs on different days for a leech's accuracy trend to show
  anything — each section prints its own honest "nothing yet" line until
  then. The TUI dashboard shows the same sections.
- CLI output is colourized (bold/red for leeches, cyan for focus advice)
  when stdout is an interactive terminal, and plain text otherwise (e.g.
  when piped to a file or another program).

## Testing

```sh
ctest --test-dir backend/build        # or backend/build/wkr_tests
cargo test --manifest-path frontend/Cargo.toml
```

All tests are local and unit/integration-level — none require a live
WaniKani token or network access.

## Data location

`wkr` stores everything it syncs in one SQLite database at
`$XDG_DATA_HOME/wanikani-reinforcer/wkr.db`, falling back to
`~/.local/share/wanikani-reinforcer/wkr.db` if `XDG_DATA_HOME` is unset. The
directory is created automatically on first sync. Deleting this file resets
`wkr` to a clean slate (it will re-fetch everything from WaniKani on the
next `wkr sync`).

## Design notes

`wkr` scores confusion pairs two ways:

- **Co-failure**: subjects you failed within the same study session
  (see `--session-gap-minutes`) that are also linked by wkr's own
  similarity graph (shared components, WaniKani's visually-similar list,
  shared readings, overlapping meanings, or similar character shape) get a
  score that decays with how long ago that session was.
- **"Likely" candidates**: a leech (a subject you keep failing, ranked by
  incorrect answers relative to its current streak) that hasn't yet
  co-failed with anything gets paired with its single strongest
  similarity-graph neighbor instead, at a discounted score — a guess rather
  than an observation.

Honest limits: real co-failure pairs need several sessions of `wkr sync`
polling to accumulate, since they depend on two subjects actually being
missed close together in time across multiple reviews. On a fresh sync
(or a low-review-history account), most of what `wkr report` and the drill
surfaces will be single-leech "likely" candidates and cold-start detections
(`current_streak == 0` on the very first snapshot wkr has seen for a
subject) rather than confirmed co-failures — still useful, just less
targeted than after a week or two of regular syncing.
