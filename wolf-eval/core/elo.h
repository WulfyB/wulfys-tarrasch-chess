// Wolf Eval - rough one-game performance rating from accuracy and average centipawn loss.
//
// There is no published formula for this, and nobody can do it precisely: even machine-learning
// models that also use clock times miss by about 180 rating points on average. So this is a
// transparent heuristic with a deliberately wide range:
//   * game accuracy (Lichess-style) mapped through hand-picked anchor points, and
//   * ACPL mapped through an exponential decay (about 2450 at 20 cp, 1500 at 60 cp, 950 at 100 cp),
// blended 60/40, plus a range that widens for short games.
// The anchors live in elo.cpp so they are easy to recalibrate against your own games.
#pragma once
#include "analysis.h"

namespace wolf {

struct EloEstimate {
    bool valid = false;
    int  elo = 0, low = 0, high = 0;   // estimate and likely range
    bool veryRough = false;             // fewer than 15 moves: take with a big pinch of salt
};

int eloFromAccuracy(double accuracy);
int eloFromAcpl(double acpl);
EloEstimate estimateElo(const PlayerStats &p);

} // namespace wolf
