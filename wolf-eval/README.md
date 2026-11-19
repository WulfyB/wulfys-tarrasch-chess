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

## Opening book

Reviews use an opening book so known theory isn't punished, and so every game gets an opening
name. The book is **`WolfBook.bin`**. Put it in Tarrasch's documents folder (next to
`Tarrasch.ini`) or next to `Tarrasch.exe`. The Tarrasch build from GitHub Actions already includes
the newest one, which is always on the [`opening-book` release](../../releases/tag/opening-book).

**What counts as book:** any position that at least 5 strong games reached in their first 20
moves (both players 2200+), plus every position on a named line. The book stores positions, not
move orders, so transpositions are recognised.

| Source | What it gives |
|---|---|
| [The Week in Chess](https://theweekinchess.com) (issues 920 onward, Aug 2012-now) | over-the-board games of strong players, weekly |
| [Lichess Elite Database](https://database.nikonoel.fr) | online games, 2400+ vs 2200+, no bullet |
| [lichess-org/chess-openings](https://github.com/lichess-org/chess-openings) (CC0, copy in `book/lichess`) | 3,865 named lines with ECO codes |

The book holds only position fingerprints and how many games reached each one, never the games.
Full credits, links and how each source is used: [book/CREDITS.md](book/CREDITS.md). Please consider
supporting The Week in Chess (Mark Crowther) and the Lichess Elite Database (nikonoel). Both are
free and each is maintained by one person.

**In a review:**
- Book moves show as **Book**. They are not judged and don't count toward accuracy, average loss,
  or mistake counts, so playing old theory never costs you.
- The opening is the deepest named position the game reached, for example
  `D58 Queen's Gambit Declined: Tartakower Defense (T)`.
- **(T)** (transposition): that position was reached by a different move order than its textbook
  line. The review says which opening it transposed from.
- **(O)** (out of book in between): the game left the book and later came back into a book
  position. Either the players found a real transposition, or the book is missing a line.
  Tarrasch shows **Add to my book**, which appends the line to `WolfReview my book.tsv`, and
  **Rerun review**. Every (O) game is also logged to `WolfReview book gaps.tsv`, so missing
  lines can be folded into the shared book.

**Your own lines:** `WolfReview my book.tsv` (next to `WolfReview.ini`) uses the same three
columns as the Lichess files (`ECO <tab> name <tab> 1. d4 d5 2. c4 ...`). Every position on each
line counts as book, and the last one gets the name.

**Keeping it updated:** `.github/workflows/opening-book.yml` runs on the 3rd of every month (from
the default branch). It downloads only the TWIC issues and Elite months that are new since the
last run, adds them to the saved tally (`WolfBook-counts.cnt` on the release), refreshes the
opening names, rebuilds `WolfBook.bin` and publishes it. You can also run it by hand from the
Actions tab, with options for the thresholds or for a fresh start.

**Tools:** `wolf-book` builds and inspects books:
- `wolf-book info WolfBook.bin` shows the version and size.
- `wolf-book name WolfBook.bin 1.c4 e6 2.Nc3 d5 3.d4` names a line and shows which moves are book.
- `wolf-eval --book FILE` / `--no-book` control the book from the command line.
