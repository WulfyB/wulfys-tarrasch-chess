// Wolf Eval - review settings (WolfReview.ini) and review history (WolfReview history.csv).
//
// The history keeps one row per player per reviewed game: accuracy, ACPL, error counts,
// the rough Elo estimate, and HOW the game was reviewed (depth, method, engine).
// Numbers from different depths / methods / engines aren't comparable, so the summaries
// here only ever average rows that were reviewed the same way ("like with like").
// Reviewing the same game again the same way replaces its old rows instead of adding more.
#pragma once
#include <string>
#include <vector>
#include "analysis.h"

namespace wolf {

// ------------------------------------------------------------------ settings

struct ReviewSettings {
    int         depth = 18;
    Method      method = Method::SameRoot;
    bool        recordHistory = true;
    std::string historyFile;      // empty = "WolfReview history.csv" next to the .ini
    bool        useBook = true;   // mark book moves (not punished) and name the opening
    std::string bookFile;         // empty = "WolfBook.bin" next to the .ini (or next to Tarrasch.exe)
};

// Missing file or keys keep the defaults. Returns false only if the file exists but can't be read.
bool loadSettings(const std::string &iniPath, ReviewSettings &s);
bool saveSettings(const std::string &iniPath, const ReviewSettings &s);
// The history file to use: s.historyFile, or "WolfReview history.csv" in the ini's folder.
std::string historyPath(const std::string &iniPath, const ReviewSettings &s);

// Files next to the .ini: your own book additions and the log of (O) gaps.
std::string myBookPath(const std::string &iniPath);     // "WolfReview my book.tsv"
std::string bookGapsPath(const std::string &iniPath);   // "WolfReview book gaps.tsv"

// Loads the opening book for a review: s.bookFile, else WolfBook.bin next to the .ini, else the
// first of `fallbacks` that exists; then your own additions on top. `status` says what was loaded
// (or why not). Returns false if no book could be loaded (the review then runs without one).
bool loadReviewBook(const std::string &iniPath, const ReviewSettings &s, const std::vector<std::string> &fallbacks,
                    OpeningBook &book, std::string &status);
// Appends one Lichess-style TSV line to your book additions (creating the file with a header).
bool addToMyBook(const std::string &iniPath, const std::string &tsvLine, std::string &err);
// Logs an (O) gap so the book can be checked and improved later. Same game + line = one entry.
bool logBookGap(const std::string &iniPath, const GameReport &rep, const std::string &gameId,
                const std::string &tsvLine, std::string &err);

const char *methodKey(Method m);              // "sameroot" / "lichess"
bool parseMethod(const std::string &text, Method &m);

// ------------------------------------------------------------------ history

struct HistoryRow {
    std::string reviewedAt;       // "2026-11-19 15:30"
    std::string gameId;           // fingerprint of players + date + moves
    std::string gameDate, event, white, black, result;
    std::string player, color, opponent;   // color: "white" / "black"
    double      accuracy = 0, acpl = 0;
    int         moves = 0, best = 0, inaccuracies = 0, mistakes = 0, blunders = 0;
    int         estElo = 0, eloLow = 0, eloHigh = 0;
    int         depth = 0;
    std::string method;           // methodKey()
    std::string engine;
    std::string eco, opening;     // from the opening book (empty without one)
    bool        transposed = false, outOfBookGap = false;   // (T) and (O)
    int         bookMoves = 0;    // this player's book moves (not counted in accuracy)
    std::string bookVersion;
};

std::string gameFingerprint(const PgnGame &g);
// One row per player who has an accuracy (usually two).
std::vector<HistoryRow> historyRows(const GameReport &rep, const std::string &gameId, const std::string &reviewedAt);

std::vector<HistoryRow> readHistory(const std::string &csvPath);   // missing file = empty
// Adds the rows, replacing earlier rows for the same game + colour + depth + method + engine.
// err is set if the file can't be written (e.g. it is open in Excel).
bool recordHistory(const std::string &csvPath, const std::vector<HistoryRow> &rows, std::string &err);

// Same review setup = same depth, method and engine.
bool sameSetup(const HistoryRow &a, const HistoryRow &b);

struct PlayerHistory {
    int    games = 0;
    double avgAccuracy = 0, avgAcpl = 0, avgElo = 0;
    int    bestElo = 0, worstElo = 0;
};
// Averages a player's rows that were reviewed with the same setup as `like`
// (name match ignores case and surrounding spaces). Rows for excludeGameId are skipped,
// so the game being shown doesn't count towards its own "earlier games" line.
PlayerHistory summarizePlayer(const std::vector<HistoryRow> &rows, const std::string &player,
                              const HistoryRow &like, const std::string &excludeGameId);

std::string nowStamp();   // local time "YYYY-MM-DD HH:MM"

} // namespace wolf
