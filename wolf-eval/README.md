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
