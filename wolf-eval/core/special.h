// Wolf Eval - great (!) and brilliant (!!) moves.
//
// Brilliant (!!): a sound sacrifice. All of:
//   * not a book move
//   * the engine's best move, or within kBrilliantMaxLossCp of it
//   * gives up at least kSacrificePoints of material (pawn 1, knight/bishop 3, rook 5, queen 9):
//       - in the engine's line the opponent takes it on its first or second reply, and the mover's
//         next move doesn't win it straight back (that would just be a trade or a tactic) -
//         getting it back later, because the opponent is forced to return it, still counts, or
//       - the moved piece is left hanging and the engine's reply doesn't take it (it can't)
//     Only the start of the line counts: a sacrifice further down the line belongs to that later
//     move, and a line that's cut short never counts as "the material stayed lost".
//   * the mover is still clearly better afterwards (>= kBrilliantMinAfterCp, or mating)
//   * the mover wasn't already crushing (>= kAlreadyWinningCp) - unless it was also the only move,
//     or the sacrifice forces mate (sacrificing for mate always counts)
//
// Great (!): the only good move. All of:
//   * not a book move, not brilliant
//   * the engine's best move
//   * the second-best move would be at least a mistake (win chances drop >= kOnlyMoveGap)
//   * it matters: the mover stays >= kGreatKeepCp, or it saves a game the second-best move loses
//   * it was critical: the second-best move doesn't still keep the mover at +3.0 or more
//   * it isn't obvious: not taking free material (an undefended piece, or one worth at least the
//     piece taking it), not a recapture, not checkmate itself, not a king stepping out of check,
//     not the next move of the mover's own combination that was already judged, and >= 3 legal moves
//
// Neither changes accuracy; they are shown on top of it.
#pragma once
#include <string>
#include <vector>
#include "accuracy.h"

namespace wolf {

enum class Special { None, Great, Brilliant };
const char *specialName(Special s);          // "", "Great", "Brilliant"
const char *specialSymbol(Special s);        // "", "!", "!!"

const int    kSacrificePoints     = 2;
const int    kBrilliantMaxLossCp  = 20;
const int    kBrilliantMinAfterCp = 100;
const int    kAlreadyWinningCp    = 600;
const double kOnlyMoveGap         = 0.2;     // = a mistake on Lichess's scale
const int    kGreatKeepCp         = 80;
const int    kGreatSavesFromCp    = -150;    // second-best this bad (or worse) ...
const int    kGreatSavedToCp      = -80;     // ... while the move keeps the mover at least here
const int    kGreatStillWinningCp = 300;     // if the second-best still keeps +3.0, the move wasn't critical

struct SpecialInput {
    std::string fenBefore;                  // position before the move
    std::string playedUci;                  // e.g. "d3h7"
    std::vector<std::string> line;          // engine line starting with the played move (may be just the move)
    Score best, played;                     // White's point of view
    bool  hasSecond = false;
    Score second;                           // second-best move, White's point of view
    bool  isBest = false;                   // the played move is the engine's first choice
    std::string prevMoveUci;                // opponent's previous move ("" at the start)
    bool  prevWasCapture = false;
    bool  followsUp = false;                // the move continues the mover's own earlier special move's line
    bool  book = false;
};

struct SpecialResult {
    Special     kind = Special::None;
    std::string why;                        // short reason, e.g. "sacrifices a bishop (3) and stays +2.4"
};

SpecialResult judgeSpecial(const SpecialInput &in);

} // namespace wolf
