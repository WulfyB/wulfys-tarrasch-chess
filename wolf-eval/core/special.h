// Wolf Eval - great (!) and brilliant (!!) moves.
//
// Brilliant (!!): a sound sacrifice. All of:
//   * not a book move
//   * the engine's best move, or within kBrilliantMaxLossCp of it
//   * gives up at least kSacrificePoints of material (pawn 1, knight/bishop 3, rook 5, queen 9):
//       - in the engine's line after the move the mover is that far behind once it's taken, and
//         stays that far behind for two more of its own moves (or until mate) - winning the
//         material straight back is a tactic, not a sacrifice, or
//       - the moved piece is left hanging and the engine's reply doesn't take it (it can't)
//   * the mover is still clearly better afterwards (>= kBrilliantMinAfterCp, or mating)
//   * the mover wasn't already crushing (>= kAlreadyWinningCp) - unless it was also the only move
//
// Great (!): the only good move. All of:
//   * not a book move, not brilliant
//   * the engine's best move
//   * the second-best move would be at least a mistake (win chances drop >= kOnlyMoveGap)
//   * it matters: the mover stays >= kGreatKeepCp, or it saves a game the second-best move loses
//   * it isn't obvious: not a recapture on the square just captured on, and >= 3 legal moves
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
    bool  book = false;
};

struct SpecialResult {
    Special     kind = Special::None;
    std::string why;                        // short reason, e.g. "sacrifices a bishop (3) and stays +2.4"
};

SpecialResult judgeSpecial(const SpecialInput &in);

} // namespace wolf
