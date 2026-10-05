# wkr-core JSON-RPC protocol

`wkr-core serve` speaks JSON-RPC 2.0 over stdio: one request per line on
stdin, one response per line on stdout. Diagnostics go to stderr. A request
without an `id` is a notification and gets no response. There is at most one
active quiz session per server process; `session.start` replaces it.

Error codes: `-32700` parse error, `-32600` invalid request, `-32601` method
not found, `-32602` invalid params, `-32603` internal error (message is the
exception text, e.g. a missing API token on `sync.run`), `-32000` no active
session / no pending question.

## Methods

### `dashboard.get`
No params. Returns `{has_data, leeches[], due{pairs,burned}, drill{total,correct,current_streak}, missed_pairs[], sessions{session_count,avg_failures_per_session,sessions_last_7_days}, trend[]}`.
Each leech: `{subject_id, characters, meanings, percentage_correct, srs_stage, fails_meaning, neighbors_heading, neighbors[{subject_id,characters,meaning,srs_label}], advice}`.

### `sync.run`
Params: `session_gap_minutes?` (default 45). Blocks until the sync finishes.
Returns `{subjects, stats, assignments, study_materials, failures, similarity_edge_count}` (`-1` edges = graph rebuild skipped).

### `session.start`
Params: `kind` (`"drill"` or `"burned"`), `count?` (default 5), `only?` / `exclude?` (burned only; comma-separated types).
Returns `{total}`, or `{nothing_due: "<message>"}` when there is nothing to ask.

### `session.next`
Returns `{done: false, question}` or `{done: true, asked, correct}` (the session then ends). Repeats the same question until it is answered.
Question: `{kind, prompt, index, total, ...}`.
- `forced_choice` (drill): plus `shown_text`, `asks_reading`, `options[]` (display order; answer by 1-based number or option text).
- `production` (drill): free-text meaning.
- `meaning` / `reading` (burned): plus `characters`, `type_label`, `subject_id`. Readings accept hiragana, katakana or romaji.

### `session.answer`
Params: `answer` (string). Returns feedback `{correct, retry, detail, advice, reason}`.
`retry: true` (burned readings only) means a valid reading WaniKani does not want here: the same question stays pending and nothing is recorded. Otherwise the answer is persisted immediately (drill result and local SRS), so aborting later loses nothing.

### `session.abort`
Ends the session. Returns `{asked, correct}`.

### `kana.convert`
Params: `text`. Returns `{hiragana}`; used for the live romaji preview. Grading always happens in the backend.
