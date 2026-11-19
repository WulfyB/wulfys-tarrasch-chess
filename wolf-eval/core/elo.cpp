#include "elo.h"
#include <algorithm>
#include <cmath>

namespace wolf {

namespace {
// (Lichess-style game accuracy %, rating). Monotonic; linear between points.
const double kAccAnchors[][2] = {
    {0, 200},   {40, 500},  {55, 800},  {65, 1100}, {72, 1350}, {78, 1600},
    {83, 1850}, {87, 2100}, {90, 2300}, {93, 2500}, {95.5, 2700}, {97.5, 2850}, {99, 3000}, {100, 3100}};
constexpr int    kMin = 400, kMax = 3200;
constexpr double kAcplScale = 3100, kAcplDecay = 0.012;
constexpr double kWeightAccuracy = 0.6;
}

int eloFromAccuracy(double a) {
    a = std::clamp(a, 0.0, 100.0);
    const int n = sizeof kAccAnchors / sizeof kAccAnchors[0];
    for (int i = 1; i < n; ++i)
        if (a <= kAccAnchors[i][0]) {
            double t = (a - kAccAnchors[i - 1][0]) / (kAccAnchors[i][0] - kAccAnchors[i - 1][0]);
            return (int)std::lround(kAccAnchors[i - 1][1] + t * (kAccAnchors[i][1] - kAccAnchors[i - 1][1]));
        }
    return (int)kAccAnchors[n - 1][1];
}

int eloFromAcpl(double acpl) {
    return (int)std::lround(kAcplScale * std::exp(-kAcplDecay * std::max(0.0, acpl)));
}

EloEstimate estimateElo(const PlayerStats &p) {
    EloEstimate e;
    if (!p.hasAccuracy || p.moves < 1) return e;
    double est = kWeightAccuracy * eloFromAccuracy(p.accuracy) + (1 - kWeightAccuracy) * eloFromAcpl(p.acpl);
    est = std::clamp(est, (double)kMin, (double)kMax);
    // One game says little: +/-300 for a 40-move game, wider for short ones, never under 250.
    double half = std::clamp(300.0 * std::sqrt(40.0 / std::max(p.moves, 8)), 250.0, 700.0);
    e.valid = true;
    e.elo = (int)(std::lround(est / 10.0) * 10);
    e.low = std::max(kMin - 200, (int)(std::lround((est - half) / 50.0) * 50));
    e.high = std::min(kMax + 200, (int)(std::lround((est + half) / 50.0) * 50));
    e.veryRough = p.moves < 15;
    return e;
}

} // namespace wolf
