// wolf-book - builds WolfBook.bin, the opening book used by Wolf Eval and Wulfy's Tarrasch.
//
//   wolf-book count --out batch.cnt [--max-ply 40] [--min-elo 2200] [--tmp DIR] [files.pgn ... | -]
//       Reads PGN games (files, or "-" for stdin) and counts how many games reached each
//       position in their first --max-ply plies. Games from a FEN, variants, and games where
//       either player is below --min-elo (or unrated) are skipped. Uses sorted runs on disk,
//       so millions of games fit in a few hundred MB of memory.
//   wolf-book merge --out all.cnt [--keep-min N] a.cnt b.cnt ...
//       Adds count files together, dropping positions seen fewer than N times (default 1).
//   wolf-book build --counts all.cnt --names DIR --out WolfBook.bin [--min-games 5] [--version TEXT]
//       Book = every position reached in at least --min-games games, plus every position on a
//       named line from DIR/*.tsv (Lichess's chess-openings files) and any extra .tsv given
//       with --extra FILE.
//   wolf-book stats all.cnt            how many positions there are at each threshold
//   wolf-book info WolfBook.bin        what's in a book
//   wolf-book name WolfBook.bin 1.e4 c5 2.Nf3 ...   classify a move sequence
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include "opening.h"
#include "thc.h"

using namespace wolf;

namespace {

const char kCountMagic[] = "WOLFCNT1";
typedef std::pair<uint64_t, uint32_t> KC;

// ------------------------------------------------------------- count files

bool writeCounts(const std::string &path, const std::vector<KC> &v)
{
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(kCountMagic, 1, 8, f);
    uint64_t n = v.size();
    std::fwrite(&n, 8, 1, f);
    for (const KC &kc : v) { std::fwrite(&kc.first, 8, 1, f); std::fwrite(&kc.second, 4, 1, f); }
    return std::fclose(f) == 0;
}

// Sequential reader for a sorted count file.
struct CountReader {
    std::FILE *f = nullptr;
    uint64_t left = 0;
    KC cur;
    bool open(const std::string &path)
    {
        f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char m[8];
        if (std::fread(m, 1, 8, f) != 8 || std::memcmp(m, kCountMagic, 8) != 0 || std::fread(&left, 8, 1, f) != 1) {
            std::fclose(f); f = nullptr; return false;
        }
        return true;
    }
    bool next()
    {
        if (!left) return false;
        if (std::fread(&cur.first, 8, 1, f) != 1 || std::fread(&cur.second, 4, 1, f) != 1) { left = 0; return false; }
        --left;
        return true;
    }
    ~CountReader() { if (f) std::fclose(f); }
};

// k-way merge of sorted count files, summing equal keys, keeping counts >= keepMin.
bool mergeFiles(const std::vector<std::string> &in, const std::string &out, uint32_t keepMin, uint64_t &kept)
{
    std::vector<std::unique_ptr<CountReader>> rs;
    typedef std::pair<uint64_t, size_t> HeapItem;
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> heap;
    for (const std::string &p : in) {
        rs.emplace_back(new CountReader());
        if (!rs.back()->open(p)) { std::fprintf(stderr, "Can't read %s\n", p.c_str()); return false; }
        if (rs.back()->next()) heap.push(HeapItem(rs.back()->cur.first, rs.size() - 1));
    }
    std::FILE *f = std::fopen(out.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "Can't write %s\n", out.c_str()); return false; }
    std::fwrite(kCountMagic, 1, 8, f);
    uint64_t n = 0;
    std::fwrite(&n, 8, 1, f);   // patched below
    kept = 0;
    while (!heap.empty()) {
        uint64_t key = heap.top().first;
        uint64_t sum = 0;
        while (!heap.empty() && heap.top().first == key) {
            size_t i = heap.top().second;
            heap.pop();
            sum += rs[i]->cur.second;
            if (rs[i]->next()) heap.push(HeapItem(rs[i]->cur.first, i));
        }
        if (sum >= keepMin) {
            uint32_t c = sum > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sum;
            std::fwrite(&key, 8, 1, f);
            std::fwrite(&c, 4, 1, f);
            ++kept;
        }
    }
    std::fseek(f, 8, SEEK_SET);
    std::fwrite(&kept, 8, 1, f);
    return std::fclose(f) == 0;
}

// ------------------------------------------------------------- PGN streaming

struct GameText {
    std::string white, black, fen, variant, whiteElo, blackElo;
    std::string moves;
    void clear() { white.clear(); black.clear(); fen.clear(); variant.clear(); whiteElo.clear(); blackElo.clear(); moves.clear(); }
};

std::string tagValue(const std::string &line)
{
    size_t a = line.find('"'), b = line.rfind('"');
    return (a != std::string::npos && b > a) ? line.substr(a + 1, b - a - 1) : std::string();
}

// First maxPly SAN moves of the main line (comments, variations, NAGs, numbers skipped).
std::vector<std::string> mainLine(const std::string &t, int maxPly)
{
    std::vector<std::string> out;
    int depth = 0;
    size_t i = 0, n = t.size();
    while (i < n && (int)out.size() < maxPly) {
        char c = t[i];
        if (c == '{') { size_t e = t.find('}', i); i = e == std::string::npos ? n : e + 1; continue; }
        if (c == ';') { size_t e = t.find('\n', i); i = e == std::string::npos ? n : e + 1; continue; }
        if (c == '(') { ++depth; ++i; continue; }
        if (c == ')') { if (depth) --depth; ++i; continue; }
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        size_t j = i;
        while (j < n && !std::isspace((unsigned char)t[j]) && t[j] != '{' && t[j] != '(' && t[j] != ')' && t[j] != ';') ++j;
        std::string tok = t.substr(i, j - i);
        i = j;
        if (depth) continue;
        size_t k = 0;
        while (k < tok.size() && std::isdigit((unsigned char)tok[k])) ++k;
        if (k < tok.size() && tok[k] == '.') { while (k < tok.size() && tok[k] == '.') ++k; tok = tok.substr(k); }
        else if (k == tok.size()) continue;
        if (tok.empty() || tok[0] == '$' || tok == "*" || tok == "1-0" || tok == "0-1" || tok == "1/2-1/2") continue;
        for (char &ch : tok) if (ch == '0') ch = 'O';
        while (!tok.empty() && (tok.back() == '+' || tok.back() == '#' || tok.back() == '!' || tok.back() == '?')) tok.pop_back();
        out.push_back(tok);
    }
    return out;
}

struct Counter {
    int maxPly = 40, minElo = 2200;
    size_t chunk = 32u << 20;             // keys per sorted run (256 MB)
    std::string tmpDir = ".";
    std::vector<uint64_t> buf;
    std::vector<std::string> runs;
    uint64_t gamesSeen = 0, gamesUsed = 0, plies = 0, badMoves = 0;

    void flush()
    {
        if (buf.empty()) return;
        std::sort(buf.begin(), buf.end());
        std::vector<KC> v;
        for (size_t i = 0; i < buf.size();) {
            size_t j = i;
            while (j < buf.size() && buf[j] == buf[i]) ++j;
            v.push_back(KC(buf[i], (uint32_t)(j - i)));
            i = j;
        }
        std::string p = tmpDir + "/wolfbook_run" + std::to_string(runs.size()) + ".cnt";
        if (!writeCounts(p, v)) { std::fprintf(stderr, "Can't write %s\n", p.c_str()); std::exit(1); }
        runs.push_back(p);
        buf.clear();
        std::fprintf(stderr, "  run %zu: %zu positions (%llu games so far)\n", runs.size(), v.size(), (unsigned long long)gamesUsed);
    }

    void game(const GameText &g)
    {
        ++gamesSeen;
        if (!g.fen.empty() || (!g.variant.empty() && g.variant != "Standard")) return;
        int we = std::atoi(g.whiteElo.c_str()), be = std::atoi(g.blackElo.c_str());
        if (minElo > 0 && (we < minElo || be < minElo)) return;
        std::vector<std::string> san = mainLine(g.moves, maxPly);
        if (san.empty()) return;
        thc::ChessRules cr;
        std::vector<uint64_t> ks;
        for (const std::string &s : san) {
            thc::Move mv;
            if (!mv.NaturalIn(&cr, s.c_str())) { ++badMoves; break; }
            cr.PlayMove(mv);
            ks.push_back(positionKey(cr.ForsythPublish()));
        }
        std::sort(ks.begin(), ks.end());
        ks.erase(std::unique(ks.begin(), ks.end()), ks.end());   // a game counts once per position
        buf.insert(buf.end(), ks.begin(), ks.end());
        plies += ks.size();
        ++gamesUsed;
        if (gamesUsed % 200000 == 0) std::fprintf(stderr, "  %llu games used of %llu read\n", (unsigned long long)gamesUsed, (unsigned long long)gamesSeen);
        if (buf.size() >= chunk) flush();
    }

    void stream(std::istream &in)
    {
        GameText g;
        bool inMoves = false;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == '[') {
                if (inMoves) { game(g); g.clear(); inMoves = false; }
                if      (line.compare(0, 7, "[White ") == 0)    g.white = tagValue(line);
                else if (line.compare(0, 7, "[Black ") == 0)    g.black = tagValue(line);
                else if (line.compare(0, 5, "[FEN ") == 0)      g.fen = tagValue(line);
                else if (line.compare(0, 9, "[Variant ") == 0)  g.variant = tagValue(line);
                else if (line.compare(0, 10, "[WhiteElo ") == 0) g.whiteElo = tagValue(line);
                else if (line.compare(0, 10, "[BlackElo ") == 0) g.blackElo = tagValue(line);
                continue;
            }
            if (line.empty()) continue;
            inMoves = true;
            g.moves += line;
            g.moves += '\n';
        }
        if (inMoves) game(g);
    }
};

int usage()
{
    std::fprintf(stderr,
        "usage:\n"
        "  wolf-book count --out batch.cnt [--max-ply 40] [--min-elo 2200] [--tmp DIR] [files.pgn ... | -]\n"
        "  wolf-book merge --out all.cnt [--keep-min N] a.cnt b.cnt ...\n"
        "  wolf-book build --counts all.cnt --names DIR --out WolfBook.bin [--min-games 5] [--extra F.tsv] [--version TEXT]\n"
        "  wolf-book stats all.cnt\n"
        "  wolf-book info WolfBook.bin\n"
        "  wolf-book name WolfBook.bin 1.e4 c5 2.Nf3 ...\n");
    return 2;
}

int cmdCount(int argc, char **argv)
{
    Counter c;
    std::string out;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return std::string(argv[++i]); };
        if (a == "--out") out = next();
        else if (a == "--max-ply") c.maxPly = std::atoi(next().c_str());
        else if (a == "--min-elo") c.minElo = std::atoi(next().c_str());
        else if (a == "--tmp") c.tmpDir = next();
        else if (a == "--chunk-mb") c.chunk = (size_t)std::atoi(next().c_str()) * (1u << 20) / 8;
        else files.push_back(a);
    }
    if (out.empty() || files.empty()) return usage();
    std::ios::sync_with_stdio(false);
    for (const std::string &p : files) {
        if (p == "-") c.stream(std::cin);
        else {
            std::ifstream in(p, std::ios::binary);
            if (!in) { std::fprintf(stderr, "Can't read %s\n", p.c_str()); return 1; }
            c.stream(in);
        }
    }
    c.flush();
    uint64_t kept = 0;
    if (c.runs.empty()) { writeCounts(out, std::vector<KC>()); }
    else if (!mergeFiles(c.runs, out, 1, kept)) return 1;
    for (const std::string &r : c.runs) std::remove(r.c_str());
    std::fprintf(stderr, "Read %llu games, used %llu (%llu plies, %llu stopped at an unreadable move); %llu positions -> %s\n",
                 (unsigned long long)c.gamesSeen, (unsigned long long)c.gamesUsed, (unsigned long long)c.plies,
                 (unsigned long long)c.badMoves, (unsigned long long)kept, out.c_str());
    return 0;
}

int cmdMerge(int argc, char **argv)
{
    std::string out;
    uint32_t keepMin = 1;
    std::vector<std::string> in;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--keep-min" && i + 1 < argc) keepMin = (uint32_t)std::atoi(argv[++i]);
        else in.push_back(a);
    }
    if (out.empty() || in.empty()) return usage();
    uint64_t kept = 0;
    if (!mergeFiles(in, out, keepMin, kept)) return 1;
    std::fprintf(stderr, "%llu positions (seen %u+ times) -> %s\n", (unsigned long long)kept, keepMin, out.c_str());
    return 0;
}

int cmdBuild(int argc, char **argv)
{
    std::string counts, names, out, version;
    std::vector<std::string> extras;
    uint32_t minGames = 5;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (i + 1 >= argc) return usage();
        if (a == "--counts") counts = argv[++i];
        else if (a == "--names") names = argv[++i];
        else if (a == "--out") out = argv[++i];
        else if (a == "--version") version = argv[++i];
        else if (a == "--extra") extras.push_back(argv[++i]);
        else if (a == "--min-games") minGames = (uint32_t)std::atoi(argv[++i]);
        else return usage();
    }
    if (out.empty() || names.empty()) return usage();
    OpeningBook book;
    std::string err;
    int lines = 0;
    for (const char *f : {"a", "b", "c", "d", "e"}) {
        int n = book.addTsvFile(names + "/" + f + ".tsv", err);
        if (n < 0) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        lines += n;
    }
    for (const std::string &x : extras) {
        int n = book.addTsvFile(x, err);
        if (n < 0) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        lines += n;
    }
    std::vector<KC> kc;
    if (!counts.empty()) {
        CountReader r;
        if (!r.open(counts)) { std::fprintf(stderr, "Can't read %s\n", counts.c_str()); return 1; }
        while (r.next()) if (r.cur.second >= minGames) kc.push_back(r.cur);
    }
    size_t fromGames = kc.size();
    book.setCounts(std::move(kc));
    book.version = version;
    if (!book.save(out, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::fprintf(stderr, "%s: %zu positions (%zu from games with %u+ games, the rest from %d named lines), %zu names\n",
                 out.c_str(), book.positions(), fromGames, minGames, lines, book.namedLines());
    return 0;
}

int cmdStats(int argc, char **argv)
{
    if (argc < 3) return usage();
    CountReader r;
    if (!r.open(argv[2])) { std::fprintf(stderr, "Can't read %s\n", argv[2]); return 1; }
    const uint32_t th[] = {1, 2, 3, 5, 10, 20, 50, 100, 1000};
    uint64_t n[9] = {};
    while (r.next())
        for (int i = 0; i < 9; ++i) if (r.cur.second >= th[i]) ++n[i];
    for (int i = 0; i < 9; ++i)
        std::printf("reached in %4u+ games: %10llu positions (%6.1f MB as a book)\n", th[i], (unsigned long long)n[i], n[i] * 12.0 / 1e6);
    return 0;
}

int cmdInfo(int argc, char **argv)
{
    if (argc < 3) return usage();
    OpeningBook b;
    std::string err;
    if (!b.load(argv[2], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::printf("%s\n  version: %s\n  positions: %zu\n  named lines: %zu\n", argv[2], b.version.c_str(), b.positions(), b.namedLines());
    return 0;
}

int cmdName(int argc, char **argv)
{
    if (argc < 4) return usage();
    OpeningBook b;
    std::string err;
    if (!b.load(argv[2], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::string text;
    for (int i = 3; i < argc; ++i) text += std::string(argv[i]) + " ";
    std::vector<std::string> san = mainLine(text, 400);
    OpeningInfo info = classifyOpening(b, "", san);
    std::vector<std::string> numbered;
    for (size_t i = 0; i < san.size(); ++i)
        numbered.push_back(std::to_string(i / 2 + 1) + (i % 2 ? "... " : ". ") + san[i]);
    std::printf("%s\n%s\n", info.label().empty() ? "(not in book)" : info.label().c_str(), info.details(numbered).c_str());
    for (size_t i = 0; i < san.size(); ++i)
        std::printf("  %-12s %s %u\n", numbered[i].c_str(), info.bookMove[i] ? "book" : "    ", info.games[i]);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    if (cmd == "count") return cmdCount(argc, argv);
    if (cmd == "merge") return cmdMerge(argc, argv);
    if (cmd == "build") return cmdBuild(argc, argv);
    if (cmd == "stats") return cmdStats(argc, argv);
    if (cmd == "info")  return cmdInfo(argc, argv);
    if (cmd == "name")  return cmdName(argc, argv);
    return usage();
}
