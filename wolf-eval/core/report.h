// Wolf Eval - plain-text game review, shared by the command-line tool and Wulfy's Tarrasch.
#pragma once
#include <string>
#include "analysis.h"

namespace wolf {

// The full review: header, one line per move, then each player's summary and rough Elo.
std::string formatReport(const GameReport &r);
// Just the two player summaries (used as a short game comment).
std::string formatSummary(const GameReport &r);

} // namespace wolf
