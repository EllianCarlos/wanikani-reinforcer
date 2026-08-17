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
project's own review process. This is why `wkr drill` — which quizzes you
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

```sh
cmake -B build && cmake --build build
```

The build treats warnings as errors (`-Wall -Wextra -Werror`), so a clean
build is a clean build.

## Usage

```sh
wkr sync [--session-gap-minutes N]   # pull subjects/stats/assignments from WaniKani, detect failures
wkr report                           # print your current leeches, confusion pairs, and focus advice
wkr drill [--count N]                # run an interactive local quiz over your top confusion pairs
wkr stats                            # print local drill history, session summary, and leech accuracy trend
```

- `wkr sync` is the only command that talks to the network. Run it first,
  and periodically afterwards — the inference gets better with more synced
  sessions over time (see Design notes below). `--session-gap-minutes`
  controls how large a gap between two failures must be before they're
  treated as separate study sessions; it defaults to 45.
- `wkr report` and `wkr drill` are both purely local — they only read from
  the SQLite database `wkr sync` populated, and they don't need an API
  token configured at all (only `wkr sync` does). `--count` controls how
  many questions `wkr drill` asks; it defaults to 5.
- `wkr stats` is purely local, like `wkr report`/`wkr drill`. It needs at
  least one `wkr drill` run for drill-history stats, at least one `wkr
  sync` for session stats, and at least two separate `wkr sync` runs on
  different days for a leech's accuracy trend to show anything — each
  section prints its own honest "nothing yet" line until then.
- Output is colourized (bold/red for leeches, cyan for focus advice, green
  for correct drill answers, red for incorrect ones) when stdout is an
  interactive terminal, and plain text otherwise (e.g. when piped to a file
  or another program).

## Testing

```sh
ctest --test-dir build
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
(or a low-review-history account), most of what `wkr report`/`wkr drill`
surfaces will be single-leech "likely" candidates and cold-start detections
(`current_streak == 0` on the very first snapshot wkr has seen for a
subject) rather than confirmed co-failures — still useful, just less
targeted than after a week or two of regular syncing.
