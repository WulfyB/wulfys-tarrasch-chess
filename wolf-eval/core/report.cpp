#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "report.h"
#include "elo.h"

namespace wolf {

namespace {
void add(std::string &s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s += buf;
}

void player(std::string &s, const char *side, const std::string &name, const PlayerStats &p) {
    add(s, "\n  %s: %s\n", side, name.c_str());
    add(s, "    Accuracy %.1f%%   avg loss %.1f cp   best moves %d/%d\n", p.accuracy, p.acpl, p.best, p.moves);
    add(s, "    Inaccuracies %d   Mistakes %d   Blunders %d", p.inaccuracies, p.mistakes, p.blunders);
    if (p.bookMoves) add(s, "   Book moves %d (not counted)", p.bookMoves);
    if (p.brilliant || p.great) add(s, "   Brilliant %d   Great %d", p.brilliant, p.great);
    s += "\n";
    EloEstimate e = estimateElo(p);
    if (e.valid)
        add(s, "    Played like roughly %d Elo (likely %d-%d)%s\n", e.elo, e.low, e.high,
            e.veryRough ? "  - very rough, short game" : "  - a rough one-game estimate");
}
}

std::string formatSummary(const GameReport &r) {
    std::string s;
    player(s, "White", r.white, r.whiteStats);
    player(s, "Black", r.black, r.blackStats);
    return s;
}

std::string formatReport(const GameReport &r) {
    std::string s;
    add(s, "%s vs %s   %s   (%s, %s)\n", r.white.c_str(), r.black.c_str(), r.result.c_str(), r.event.c_str(), r.date.c_str());
    const char *method = r.method == Method::Lichess ? "Lichess method" : "same-root method";
    if (r.movetimeMs > 0) add(s, "Engine: %s, %d ms per position, %s\n\n", r.engineName.c_str(), r.movetimeMs, method);
    else                  add(s, "Engine: %s, depth %d, %s\n", r.engineName.c_str(), r.depth, method);
    if (!r.opening.label().empty()) {
        std::vector<std::string> mv;
        for (const MoveReport &m : r.moves) mv.push_back(std::to_string(m.moveNumber) + (m.white ? ". " : "... ") + m.san);
        s += "Opening: " + r.opening.label() + "\n  " + r.opening.details(mv) + "\n";
    }
    s += "\n";
    add(s, "  Move              Value     Best        Loss  Acc.\n");
    for (const MoveReport &m : r.moves) {
        char num[16];
        std::snprintf(num, sizeof num, m.white ? "%d." : "%d...", m.moveNumber);
        if (m.book)
            add(s, "  %-6s %-9s %7s   %-10s %5s  %5s  Book\n", num, m.san.c_str(), m.after.str().c_str(), "", "", "");
        else
            add(s, "  %-6s %-9s %7s   %-10s %5d  %5.1f  %s\n", num, m.san.c_str(), m.after.str().c_str(),
                m.isBest ? "(best)" : m.bestSan.c_str(), m.cpl, m.accuracy,
                m.special != Special::None ? (std::string(specialName(m.special)) + " " + specialSymbol(m.special) + " - " + m.specialWhy).c_str()
                                           : judgementName(m.judgement));
    }
    s += formatSummary(r);
    return s;
}

} // namespace wolf
