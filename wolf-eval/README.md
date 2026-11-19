# Wolf Eval

Chess.com-style **accuracy %** for your games, computed locally with any UCI engine
(by default whichever engine Tarrasch is set to use, read from `Tarrasch.ini`). Part of Wulfy's Tarrasch.

```
wolf-eval [--engine PATH] [--depth 30] [--method sameroot|lichess] [--json out.json] game.pgn
```

For every move it reports the engine's best move, your centipawn loss, a per-move accuracy,
and Lichess's inaccuracy / mistake / blunder labels, then each player's game accuracy and
average centipawn loss.

Depth defaults to 30 and is capped at 40 (movetime at 60 s per position) so a run can't tie the PC up forever.

## How the numbers work
* Evaluations become **win %** with Lichess's curve (capped at +/-10 pawns).
* A move's accuracy falls off exponentially with the win % it gave away (Lichess's fitted curve,
  including its +1 "uncertainty bonus").
* Game accuracy is the mean of a volatility-weighted mean and a harmonic mean of the move
  accuracies, exactly as Lichess does it (checked against a line-by-line translation of their
  Scala code on 500 random games: `tests/lichess_reference.py`).

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
