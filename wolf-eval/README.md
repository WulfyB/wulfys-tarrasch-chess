# Wolf Eval

Chess.com-style **accuracy %** for your games, computed locally with any UCI engine
(by default whichever engine Tarrasch is set to use, read from `Tarrasch.ini`). Part of Wulfy's Tarrasch.

```
wolf-eval [--engine PATH] [--depth 22] [--method sameroot|lichess] [--json out.json] game.pgn
```

For every move it reports the engine's best move, your centipawn loss, a per-move accuracy,
and Lichess's inaccuracy / mistake / blunder labels, then each player's game accuracy and
average centipawn loss.

Depth defaults to 22 (about what the big sites use for game review); use `--depth 30` for a deep look. Depth is capped at 40 and movetime at 60 s per position so a run can't tie the PC up forever. Threads and hash follow Tarrasch's engine settings when you've raised them (otherwise half your CPU threads and 256 MB), and a progress line shows the time left.

## How the numbers work
* Evaluations become **win %** with Lichess's curve (capped at +/-10 pawns).
* A move's accuracy falls off exponentially with the win % it gave away (Lichess's fitted curve,
  including its +1 "uncertainty bonus").
* Game accuracy is the mean of a volatility-weighted mean and a harmonic mean of the move
  accuracies, exactly as Lichess does it (checked against a line-by-line translation of their
  Scala code on 500 random games: `tests/lichess_reference.py`).

### Rough Elo estimate
Each player also gets a "played like roughly N Elo (likely A-B)" line (and `estElo`/`estLow`/`estHigh`
in the JSON). It blends game accuracy and ACPL through simple, documented curves (`core/elo.cpp`) and
gives a wide range, because one game is a tiny sample: even machine-learning models that also use clock
times miss by about 180 points on average. Short games (under 15 moves) are flagged as very rough.

### Methods
* `sameroot` (default): for each position the engine scores the best move **and the move you
  played from the same position** (`searchmoves`). Older engines swing their evaluation by side to
  move; comparing two moves from one root cancels that out.
* `lichess`: Lichess's exact pairing, evaluation before the move vs evaluation after it. Best with a
  modern NNUE Stockfish at good depth.

## Build
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
build/wolf-tests                                   # unit tests
python3 tests/lichess_reference.py build/wolf-tests  # cross-check against Lichess's formula
```
Uses Tarrasch's own chess library (`../src/thc.cpp`) for move legality and notation.

## Review settings and history

Tarrasch's **Commands > Game review** keeps two files in Tarrasch's documents folder
(the same folder as `Tarrasch.ini`, usually `Documents\Tarrasch`):

**`WolfReview.ini`**: the review settings. Tarrasch rewrites it whenever you press OK in the
review settings popup, and you can also edit it by hand:

```ini
[WolfReview]
depth = 18              ; 1-40 (the popup offers 14, 18, 22, 26; other values show as "From WolfReview.ini")
method = sameroot       ; sameroot (best move vs. move played) or lichess
record_history = yes    ; add every review to the history CSV
history_file =          ; empty = "WolfReview history.csv" next to the .ini
```

**`WolfReview history.csv`** has one row per player per reviewed game, for spreadsheets and
anything else we build later. The command-line tool can add to the same file with
`wolf-eval --history "path\to\WolfReview history.csv" game.pgn`.

| column | meaning |
|---|---|
| reviewed_at | when the review ran (local time) |
| game_id | fingerprint of players, date, start position and moves (same game = same id) |
| game_date, event, white, black, result | from the game's PGN tags |
| player, color, opponent | whose row this is |
| accuracy, acpl | game accuracy % and average centipawn loss |
| moves, best, inaccuracies, mistakes, blunders | that player's move counts |
| est_elo, elo_low, elo_high | the rough one-game Elo estimate and its likely range |
| depth, method, engine | **how** the game was reviewed |

Like with like: accuracy and Elo numbers from different depths, methods or engines aren't
comparable, so every average the app shows uses only rows with the same depth, method and
engine. Reviewing the same game again with the same setup replaces its old rows. A different
depth adds a separate set of rows, so you can compare them.
