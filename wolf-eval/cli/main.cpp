// wolf-eval: chess.com-style accuracy for a PGN game, using any UCI engine.
//
//   wolf-eval [options] game.pgn
//     --engine PATH     UCI engine (default: first Stockfish found in Tarrasch's Engines folder)
//     --depth N         search depth per position (default 18)
//     --movetime MS     time per position instead of depth
//     --game K          analyse only game K (1-based) of a multi-game PGN
//     --threads N       engine threads (default 2)
//     --hash MB         engine hash (default 128)
//     --json FILE       also write a JSON report
//     --method M        "sameroot" (default: your move vs the best move from the same position)
//                       or "lichess" (Lichess's exact before/after method)
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "../core/analysis.h"
#include "../core/pgn.h"
#include "../core/uci_engine.h"

namespace fs = std::filesystem;
using namespace wolf;

static std::string findTarraschStockfish() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    for (const char *env : {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"})
        if (const char *v = std::getenv(env)) roots.push_back(fs::path(v) / "Tarrasch" / "Engines");
#endif
    roots.push_back(fs::current_path() / "Engines");
    std::error_code ec;
    for (auto &root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        std::string best;
        for (auto &e : fs::recursive_directory_iterator(root, ec)) {
            std::string name = e.path().filename().string();
            std::string lower = name;
            for (char &c : lower) c = (char)std::tolower((unsigned char)c);
            if (lower.find("stockfish") == std::string::npos || !e.is_regular_file(ec)) continue;
#ifdef _WIN32
            if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".exe") continue;
#endif
            if (e.path().string() > best) best = e.path().string();   // highest version name wins
        }
        if (!best.empty()) return best;
    }
    return "";
}

static std::string jsonEscape(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

static void writeJson(const GameReport &r, const std::string &path) {
    std::ofstream f(path);
    auto stats = [&](const PlayerStats &p) {
        char b[256];
        std::snprintf(b, sizeof b, "{\"accuracy\":%.1f,\"acpl\":%.1f,\"moves\":%d,\"best\":%d,\"inaccuracies\":%d,\"mistakes\":%d,\"blunders\":%d}",
                      p.accuracy, p.acpl, p.moves, p.best, p.inaccuracies, p.mistakes, p.blunders);
        return std::string(b);
    };
    f << "{\"white\":\"" << jsonEscape(r.white) << "\",\"black\":\"" << jsonEscape(r.black)
      << "\",\"result\":\"" << jsonEscape(r.result) << "\",\"engine\":\"" << jsonEscape(r.engineName)
      << "\",\"depth\":" << r.depth << ",\"whiteStats\":" << stats(r.whiteStats)
      << ",\"blackStats\":" << stats(r.blackStats) << ",\"moves\":[";
    for (size_t i = 0; i < r.moves.size(); ++i) {
        const MoveReport &m = r.moves[i];
        char b[512];
        std::snprintf(b, sizeof b, "%s{\"ply\":%d,\"san\":\"%s\",\"best\":\"%s\",\"isBest\":%s,\"before\":\"%s\",\"after\":\"%s\",\"cpl\":%d,\"accuracy\":%.1f,\"judgement\":\"%s\"}",
                      i ? "," : "", m.ply, jsonEscape(m.san).c_str(), jsonEscape(m.bestSan).c_str(), m.isBest ? "true" : "false",
                      m.before.str().c_str(), m.after.str().c_str(), m.cpl, m.accuracy, judgementName(m.judgement));
        f << b;
    }
    f << "]}\n";
}

static void printReport(const GameReport &r) {
    std::printf("\n%s vs %s   %s   (%s, %s)\n", r.white.c_str(), r.black.c_str(), r.result.c_str(), r.event.c_str(), r.date.c_str());
    const char *method = r.method == Method::Lichess ? "Lichess method" : "same-root method";
    if (r.movetimeMs > 0) std::printf("Engine: %s, %d ms per position, %s\n\n", r.engineName.c_str(), r.movetimeMs, method);
    else                  std::printf("Engine: %s, depth %d, %s\n\n", r.engineName.c_str(), r.depth, method);

    std::printf("  Move              Value     Best        Loss  Acc.  \n");
    for (const MoveReport &m : r.moves) {
        char num[16];
        std::snprintf(num, sizeof num, m.white ? "%d." : "%d...", m.moveNumber);
        std::printf("  %-6s %-9s %7s   %-10s %5d  %5.1f  %s%s\n", num, m.san.c_str(), m.after.str().c_str(),
                    m.isBest ? "(best)" : m.bestSan.c_str(), m.cpl, m.accuracy,
                    judgementName(m.judgement), m.isBest ? "" : "");
    }
    auto line = [](const char *side, const std::string &name, const PlayerStats &p) {
        std::printf("\n  %-5s %-20s accuracy %5.1f%%   avg loss %5.1f cp   best moves %d/%d   inaccuracies %d  mistakes %d  blunders %d",
                    side, name.c_str(), p.accuracy, p.acpl, p.best, p.moves, p.inaccuracies, p.mistakes, p.blunders);
    };
    line("White", r.white, r.whiteStats);
    line("Black", r.black, r.blackStats);
    std::printf("\n\n");
}

int main(int argc, char **argv) {
    std::string enginePath, pgnPath, jsonPath;
    AnalysisOptions opt;
    int gameIndex = 0, threads = 2, hash = 128;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--engine") enginePath = next("--engine");
        else if (a == "--depth") opt.depth = std::atoi(next("--depth").c_str());
        else if (a == "--movetime") opt.movetimeMs = std::atoi(next("--movetime").c_str());
        else if (a == "--game") gameIndex = std::atoi(next("--game").c_str());
        else if (a == "--threads") threads = std::atoi(next("--threads").c_str());
        else if (a == "--hash") hash = std::atoi(next("--hash").c_str());
        else if (a == "--json") jsonPath = next("--json");
        else if (a == "--method") {
            std::string m = next("--method");
            if (m == "lichess") opt.method = Method::Lichess;
            else if (m == "sameroot") opt.method = Method::SameRoot;
            else { std::fprintf(stderr, "--method must be sameroot or lichess\n"); return 2; }
        }
        else if (a == "-h" || a == "--help") { std::printf("usage: wolf-eval [--engine PATH] [--depth N] [--movetime MS] [--game K] [--json FILE] game.pgn\n"); return 0; }
        else pgnPath = a;
    }
    if (pgnPath.empty()) { std::fprintf(stderr, "usage: wolf-eval [options] game.pgn   (--help for options)\n"); return 2; }
    if (enginePath.empty()) enginePath = findTarraschStockfish();
    if (enginePath.empty()) { std::fprintf(stderr, "No engine found. Pass --engine PATH (e.g. Tarrasch's Engines\\stockfish*.exe).\n"); return 2; }

    std::string text;
    if (!readFile(pgnPath, text)) { std::fprintf(stderr, "Cannot read %s\n", pgnPath.c_str()); return 1; }
    std::vector<PgnGame> games = parsePgn(text);
    if (games.empty()) { std::fprintf(stderr, "No games found in %s\n", pgnPath.c_str()); return 1; }

    UciEngine engine;
    std::string err;
    if (!engine.start(enginePath, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    engine.setOption("Threads", std::to_string(threads));
    engine.setOption("Hash", std::to_string(hash));

    opt.progress = [](int done, int total) { std::fprintf(stderr, "\r  analysing %d/%d   ", done, total); std::fflush(stderr); return true; };

    int rc = 0;
    for (size_t g = 0; g < games.size(); ++g) {
        if (gameIndex > 0 && int(g) + 1 != gameIndex) continue;
        GameReport rep = analyseGame(games[g], engine, opt);
        std::fprintf(stderr, "\r                          \r");
        if (!rep.ok) { std::fprintf(stderr, "Game %zu: %s\n", g + 1, rep.error.c_str()); rc = 1; continue; }
        printReport(rep);
        if (!jsonPath.empty()) {
            std::string p = jsonPath;
            if (games.size() > 1 && gameIndex == 0) p += "." + std::to_string(g + 1) + ".json";
            writeJson(rep, p);
        }
    }
    return rc;
}
