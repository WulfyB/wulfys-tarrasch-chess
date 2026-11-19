#include "history.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include "elo.h"
#include "pgn.h"

namespace wolf {

namespace {

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool truthy(const std::string &v)
{
    std::string l = lower(trim(v));
    return l == "1" || l == "yes" || l == "true" || l == "on";
}

std::string folderOf(const std::string &path)
{
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? std::string() : path.substr(0, p + 1);
}

std::string fmt1(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

std::string csvField(const std::string &s)
{
    if (s.find_first_of(",\"\r\n") == std::string::npos)
        return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

// Splits CSV text into records; handles quoted fields with commas, quotes and newlines.
std::vector<std::vector<std::string>> parseCsv(const std::string &text)
{
    std::vector<std::vector<std::string>> out;
    std::vector<std::string> rec;
    std::string f;
    bool q = false, any = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { f += '"'; ++i; }
                else q = false;
            } else f += c;
            continue;
        }
        if (c == '"') { q = true; any = true; }
        else if (c == ',') { rec.push_back(f); f.clear(); any = true; }
        else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !f.empty()) { rec.push_back(f); out.push_back(rec); }
            rec.clear(); f.clear(); any = false;
        } else { f += c; any = true; }
    }
    if (any || !f.empty()) { rec.push_back(f); out.push_back(rec); }
    return out;
}

const char *kColumns[] = {
    "reviewed_at", "game_id", "game_date", "event", "white", "black", "result",
    "player", "color", "opponent", "accuracy", "acpl", "moves", "best",
    "inaccuracies", "mistakes", "blunders", "est_elo", "elo_low", "elo_high",
    "depth", "method", "engine",
    "eco", "opening", "transposed", "out_of_book_gap", "book_moves", "book_version"
};
const int kNumColumns = (int)(sizeof kColumns / sizeof kColumns[0]);

std::vector<std::string> toFields(const HistoryRow &r)
{
    return { r.reviewedAt, r.gameId, r.gameDate, r.event, r.white, r.black, r.result,
             r.player, r.color, r.opponent, fmt1(r.accuracy), fmt1(r.acpl),
             std::to_string(r.moves), std::to_string(r.best), std::to_string(r.inaccuracies),
             std::to_string(r.mistakes), std::to_string(r.blunders), std::to_string(r.estElo),
             std::to_string(r.eloLow), std::to_string(r.eloHigh), std::to_string(r.depth),
             r.method, r.engine,
             r.eco, r.opening, r.transposed ? "yes" : "no", r.outOfBookGap ? "yes" : "no",
             std::to_string(r.bookMoves), r.bookVersion };
}

bool sameKey(const HistoryRow &a, const HistoryRow &b)
{
    return a.gameId == b.gameId && a.color == b.color && sameSetup(a, b);
}

} // namespace

// ------------------------------------------------------------------ settings

const char *methodKey(Method m) { return m == Method::Lichess ? "lichess" : "sameroot"; }

bool parseMethod(const std::string &text, Method &m)
{
    std::string l = lower(trim(text));
    if (l == "sameroot" || l == "same-root" || l == "best") { m = Method::SameRoot; return true; }
    if (l == "lichess")                                     { m = Method::Lichess;  return true; }
    return false;
}

bool loadSettings(const std::string &iniPath, ReviewSettings &s)
{
    std::ifstream in(iniPath, std::ios::binary);
    if (!in) {
        std::FILE *probe = std::fopen(iniPath.c_str(), "rb");
        if (probe) { std::fclose(probe); return false; }
        return true;   // no file yet: defaults
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = trim(line.substr(3));  // UTF-8 BOM
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = lower(trim(line.substr(0, eq))), v = trim(line.substr(eq + 1));
        if (k == "depth") {
            int d = std::atoi(v.c_str());
            if (d >= 1 && d <= 40) s.depth = d;
        } else if (k == "method") {
            parseMethod(v, s.method);
        } else if (k == "record_history") {
            s.recordHistory = truthy(v);
        } else if (k == "history_file") {
            s.historyFile = v;
        } else if (k == "use_book") {
            s.useBook = truthy(v);
        } else if (k == "book_file") {
            s.bookFile = v;
        }
    }
    return true;
}

bool saveSettings(const std::string &iniPath, const ReviewSettings &s)
{
    std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << "; Wulfy's Tarrasch - Game review settings\r\n"
           "; Tarrasch updates this file when you change the review settings, but you can edit it too.\r\n"
           "\r\n"
           "[WolfReview]\r\n"
           "; How deep the engine searches each position (1-40). The menu offers 14, 18, 22 and 26.\r\n"
        << "depth = " << s.depth << "\r\n"
        << "; sameroot = best move vs. the move played (recommended)\r\n"
           "; lichess  = evaluation before vs. after each move (Lichess's exact method)\r\n"
        << "method = " << methodKey(s.method) << "\r\n"
        << "; yes = add every review to the history file below\r\n"
        << "record_history = " << (s.recordHistory ? "yes" : "no") << "\r\n"
        << "; Leave empty for \"WolfReview history.csv\" next to this file.\r\n"
        << "history_file = " << s.historyFile << "\r\n"
        << "; yes = use the opening book: book moves aren't judged, and the opening is named\r\n"
        << "use_book = " << (s.useBook ? "yes" : "no") << "\r\n"
        << "; Leave empty for WolfBook.bin next to this file (or next to Tarrasch.exe).\r\n"
        << "; Your own additions go in \"WolfReview my book.tsv\" next to this file.\r\n"
        << "book_file = " << s.bookFile << "\r\n";
    return (bool)out;
}

std::string historyPath(const std::string &iniPath, const ReviewSettings &s)
{
    if (!trim(s.historyFile).empty())
        return trim(s.historyFile);
    return folderOf(iniPath) + "WolfReview history.csv";
}

std::string myBookPath(const std::string &iniPath)   { return folderOf(iniPath) + "WolfReview my book.tsv"; }
std::string bookGapsPath(const std::string &iniPath) { return folderOf(iniPath) + "WolfReview book gaps.tsv"; }

static bool fileExists(const std::string &p)
{
    std::FILE *f = std::fopen(p.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

bool loadReviewBook(const std::string &iniPath, const ReviewSettings &s, const std::vector<std::string> &fallbacks,
                    OpeningBook &book, std::string &status)
{
    book = OpeningBook();
    std::vector<std::string> tries;
    if (!trim(s.bookFile).empty()) tries.push_back(trim(s.bookFile));
    else {
        tries.push_back(folderOf(iniPath) + "WolfBook.bin");
        for (const std::string &f : fallbacks) tries.push_back(f);
    }
    std::string err, from;
    for (const std::string &p : tries) {
        if (!fileExists(p)) continue;
        if (book.load(p, err)) { from = p; break; }
        status = err;
        return false;
    }
    std::string mine = myBookPath(iniPath);
    int added = fileExists(mine) ? book.addTsvFile(mine, err) : 0;
    if (from.empty() && added <= 0) {
        status = "No opening book found (WolfBook.bin), so every move is judged.";
        return false;
    }
    status = from.empty() ? "Using only your own book additions." : "Opening book: " + (book.version.empty() ? from : book.version);
    if (added > 0) status += " + " + std::to_string(added) + " line" + (added == 1 ? "" : "s") + " of your own";
    return true;
}

bool addToMyBook(const std::string &iniPath, const std::string &tsvLine, std::string &err)
{
    std::string p = myBookPath(iniPath);
    bool fresh = !fileExists(p);
    std::ofstream out(p, std::ios::binary | std::ios::app);
    if (!out) { err = "Couldn't write " + p; return false; }
    if (fresh)
        out << "# Your own opening book lines, added on top of WolfBook.bin. Same format as Lichess's\r\n"
               "# chess-openings files: ECO <tab> name <tab> moves. Every position on a line counts as book.\r\n"
               "eco\tname\tpgn\r\n";
    out << tsvLine << "\r\n";
    return (bool)out;
}

bool logBookGap(const std::string &iniPath, const GameReport &rep, const std::string &gameId,
                const std::string &tsvLine, std::string &err)
{
    std::string p = bookGapsPath(iniPath), text;
    if (readFile(p, text) && text.find(gameId + "\t") != std::string::npos && text.find(tsvLine) != std::string::npos)
        return true;   // already logged
    bool fresh = text.empty();
    std::ofstream out(p, std::ios::binary | std::ios::app);
    if (!out) { err = "Couldn't write " + p; return false; }
    if (fresh)
        out << "game_id\tlogged_at\twhite\tblack\tdate\topening\tgap\teco\tname\tpgn\r\n";
    char gap[64];
    std::snprintf(gap, sizeof gap, "plies %d-%d, back at %d", rep.opening.gapFirstPly, rep.opening.gapLastPly, rep.opening.reentryPly);
    out << gameId << "\t" << nowStamp() << "\t" << rep.white << "\t" << rep.black << "\t" << rep.date << "\t"
        << rep.opening.label() << "\t" << gap << "\t" << tsvLine << "\r\n";
    return (bool)out;
}

// ------------------------------------------------------------------ history

std::string gameFingerprint(const PgnGame &g)
{
    // FNV-1a over the things that identify a game; the same game always gets the same id.
    std::string key = g.header("White") + "|" + g.header("Black") + "|" + g.header("Date") + "|" +
                      g.header("FEN", "") + "|";
    for (const std::string &m : g.sanMoves) key += m + " ";
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ULL; }
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", h);
    return b;
}

std::vector<HistoryRow> historyRows(const GameReport &rep, const std::string &gameId, const std::string &reviewedAt)
{
    std::vector<HistoryRow> rows;
    for (int side = 0; side < 2; ++side) {
        const PlayerStats &p = side == 0 ? rep.whiteStats : rep.blackStats;
        if (!p.hasAccuracy || p.moves == 0)
            continue;
        HistoryRow r;
        r.reviewedAt = reviewedAt;
        r.gameId = gameId;
        r.gameDate = rep.date; r.event = rep.event; r.white = rep.white; r.black = rep.black; r.result = rep.result;
        r.player   = side == 0 ? rep.white : rep.black;
        r.opponent = side == 0 ? rep.black : rep.white;
        r.color    = side == 0 ? "white" : "black";
        r.accuracy = p.accuracy; r.acpl = p.acpl;
        r.moves = p.moves; r.best = p.best; r.inaccuracies = p.inaccuracies; r.mistakes = p.mistakes; r.blunders = p.blunders;
        EloEstimate e = estimateElo(p);
        if (e.valid) { r.estElo = e.elo; r.eloLow = e.low; r.eloHigh = e.high; }
        r.depth = rep.depth;
        r.method = methodKey(rep.method);
        r.engine = rep.engineName;
        r.eco = rep.opening.known ? rep.opening.eco : std::string();
        r.opening = rep.opening.known ? rep.opening.name : std::string();
        r.transposed = rep.opening.transposed;
        r.outOfBookGap = rep.opening.gap;
        r.bookMoves = p.bookMoves;
        r.bookVersion = rep.bookVersion;
        rows.push_back(r);
    }
    return rows;
}

std::vector<HistoryRow> readHistory(const std::string &csvPath)
{
    std::vector<HistoryRow> rows;
    std::string text;
    if (!readFile(csvPath, text))
        return rows;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text = text.substr(3);
    std::vector<std::vector<std::string>> recs = parseCsv(text);
    if (recs.empty())
        return rows;
    // Find columns by header name, so the file still reads if columns are added or reordered.
    std::vector<int> idx(kNumColumns, -1);
    for (size_t c = 0; c < recs[0].size(); ++c)
        for (int k = 0; k < kNumColumns; ++k)
            if (lower(trim(recs[0][c])) == kColumns[k]) idx[k] = (int)c;
    for (size_t i = 1; i < recs.size(); ++i) {
        const std::vector<std::string> &f = recs[i];
        auto get = [&](int k) { return idx[k] >= 0 && idx[k] < (int)f.size() ? f[idx[k]] : std::string(); };
        auto num = [&](int k) { return std::atof(get(k).c_str()); };
        HistoryRow r;
        r.reviewedAt = get(0); r.gameId = get(1); r.gameDate = get(2); r.event = get(3);
        r.white = get(4); r.black = get(5); r.result = get(6);
        r.player = get(7); r.color = get(8); r.opponent = get(9);
        r.accuracy = num(10); r.acpl = num(11);
        r.moves = (int)num(12); r.best = (int)num(13); r.inaccuracies = (int)num(14);
        r.mistakes = (int)num(15); r.blunders = (int)num(16);
        r.estElo = (int)num(17); r.eloLow = (int)num(18); r.eloHigh = (int)num(19);
        r.depth = (int)num(20); r.method = get(21); r.engine = get(22);
        r.eco = get(23); r.opening = get(24); r.transposed = get(25) == "yes"; r.outOfBookGap = get(26) == "yes";
        r.bookMoves = (int)num(27); r.bookVersion = get(28);
        if (r.gameId.empty() || r.depth <= 0)
            continue;   // not one of ours (or a blank line)
        rows.push_back(r);
    }
    return rows;
}

bool recordHistory(const std::string &csvPath, const std::vector<HistoryRow> &rows, std::string &err)
{
    std::vector<HistoryRow> all = readHistory(csvPath);
    std::vector<HistoryRow> kept;
    for (const HistoryRow &old : all) {
        bool replaced = false;
        for (const HistoryRow &r : rows)
            if (sameKey(old, r)) { replaced = true; break; }
        if (!replaced)
            kept.push_back(old);
    }
    for (const HistoryRow &r : rows)
        kept.push_back(r);

    std::ofstream out(csvPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "Couldn't write " + csvPath + " (is it open in Excel?)";
        return false;
    }
    out << "\xEF\xBB\xBF";   // BOM so Excel reads names with accents correctly
    for (int k = 0; k < kNumColumns; ++k)
        out << (k ? "," : "") << kColumns[k];
    out << "\r\n";
    for (const HistoryRow &r : kept) {
        std::vector<std::string> f = toFields(r);
        for (size_t k = 0; k < f.size(); ++k)
            out << (k ? "," : "") << csvField(f[k]);
        out << "\r\n";
    }
    if (!out) {
        err = "Couldn't finish writing " + csvPath;
        return false;
    }
    return true;
}

bool sameSetup(const HistoryRow &a, const HistoryRow &b)
{
    return a.depth == b.depth && lower(a.method) == lower(b.method) && trim(a.engine) == trim(b.engine);
}

PlayerHistory summarizePlayer(const std::vector<HistoryRow> &rows, const std::string &player,
                              const HistoryRow &like, const std::string &excludeGameId)
{
    PlayerHistory h;
    std::string who = lower(trim(player));
    double acc = 0, acpl = 0, elo = 0;
    int eloN = 0;
    for (const HistoryRow &r : rows) {
        if (lower(trim(r.player)) != who || !sameSetup(r, like) || r.gameId == excludeGameId)
            continue;
        ++h.games;
        acc += r.accuracy;
        acpl += r.acpl;
        if (r.estElo > 0) {
            elo += r.estElo;
            if (eloN == 0 || r.estElo > h.bestElo) h.bestElo = r.estElo;
            if (eloN == 0 || r.estElo < h.worstElo) h.worstElo = r.estElo;
            ++eloN;
        }
    }
    if (h.games) { h.avgAccuracy = acc / h.games; h.avgAcpl = acpl / h.games; }
    if (eloN) h.avgElo = elo / eloN;
    return h;
}

std::string nowStamp()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    return b;
}

} // namespace wolf
