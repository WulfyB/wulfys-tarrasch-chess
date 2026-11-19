// wolf-eval: chess.com-style accuracy for a PGN game, using any UCI engine.
//
//   wolf-eval [options] game.pgn
//     --engine PATH     UCI engine (default: the engine Tarrasch is set to use, from Tarrasch.ini)
//     --depth N         search depth per position (default 22, max 40; 30 = "deep")
//     --movetime MS     time per position instead of depth (max 60000)
//     --game K          analyse only game K (1-based) of a multi-game PGN
//     --threads N       engine threads (default: Tarrasch's setting if raised, else half your CPU threads)
//     --hash MB         engine hash in MB (default: Tarrasch's setting, at least 256)
//     --json FILE       also write a JSON report
//     --method M        "sameroot" (default: your move vs the best move from the same position)
//                       or "lichess" (Lichess's exact before/after method)
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "elo.h"
#include "report.h"
#include "../core/analysis.h"
#include "../core/pgn.h"
#include "../core/uci_engine.h"

namespace fs = std::filesystem;
using namespace wolf;

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <shlobj.h>
static fs::path documentsFolder() {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) out = p;   // follows OneDrive redirection
    if (p) CoTaskMemFree(p);
    return out;
}
#endif

// Tarrasch keeps its settings in Tarrasch.ini (next to Tarrasch.exe for portable installs,
// otherwise in Documents\Tarrasch). EngineExeFile= is the engine the user picked in Tarrasch.
static std::vector<fs::path> tarraschDirs() {
    std::vector<fs::path> dirs;
#ifdef _WIN32
    fs::path docs = documentsFolder();
    if (!docs.empty()) dirs.push_back(docs / "Tarrasch");
    for (const char *env : {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"})
        if (const char *v = std::getenv(env)) dirs.push_back(fs::path(v) / "Tarrasch");
#endif
    dirs.push_back(fs::current_path());
    return dirs;
}

struct TarraschSettings {
    std::string iniPath, enginePath;
    int hashMb = 0, cores = 0;              // 0 = not set
};

static std::string iniValue(const std::string &raw) {
    std::string v;
    for (size_t i = 0; i < raw.size(); ++i) {          // wxFileConfig escapes backslashes as "\\"
        if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') ++i;
        v += raw[i];
    }
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

static TarraschSettings readTarraschIni() {
    TarraschSettings t;
    std::error_code ec;
    for (auto &dir : tarraschDirs()) {
        fs::path ini = dir / "Tarrasch.ini";
        std::ifstream f(ini);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq), val = iniValue(line.substr(eq + 1));
            if (key == "EngineExeFile" && fs::is_regular_file(val, ec)) t.enginePath = val;
            else if (key == "EngineHash") t.hashMb = std::atoi(val.c_str());
            else if (key == "EngineMaxCpuCores") t.cores = std::atoi(val.c_str());
        }
        if (!t.enginePath.empty()) { t.iniPath = ini.string(); return t; }
        t = TarraschSettings();
    }
    return t;
}

// Fallback: the most recently modified Stockfish in a Tarrasch Engines folder.
static std::string newestEngineInTarrasch() {
    std::error_code ec;
    std::string best; fs::file_time_type bestTime{};
    for (auto &dir : tarraschDirs()) {
        fs::path root = dir / "Engines";
        if (!fs::is_directory(root, ec)) continue;
        for (auto &e : fs::recursive_directory_iterator(root, ec)) {
            std::string lower = e.path().filename().string();
            for (char &c : lower) c = (char)std::tolower((unsigned char)c);
            if (lower.find("stockfish") == std::string::npos || !e.is_regular_file(ec)) continue;
#ifdef _WIN32
            if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".exe") continue;
#endif
            auto t = fs::last_write_time(e.path(), ec);
            if (best.empty() || t > bestTime) { best = e.path().string(); bestTime = t; }
        }
    }
    return best;
}

static std::string fmtTime(double secs) {
    char b[32];
    int s = (int)(secs + 0.5);
    if (s < 60) std::snprintf(b, sizeof b, "%ds", s);
    else if (s < 3600) std::snprintf(b, sizeof b, "%dm %02ds", s / 60, s % 60);
    else std::snprintf(b, sizeof b, "%dh %02dm", s / 3600, (s % 3600) / 60);
    return b;
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
        char b[320];
        EloEstimate e = estimateElo(p);
        std::snprintf(b, sizeof b, "{\"accuracy\":%.1f,\"acpl\":%.1f,\"moves\":%d,\"best\":%d,\"inaccuracies\":%d,\"mistakes\":%d,\"blunders\":%d,\"estElo\":%d,\"estLow\":%d,\"estHigh\":%d}",
                      p.accuracy, p.acpl, p.moves, p.best, p.inaccuracies, p.mistakes, p.blunders, e.elo, e.low, e.high);
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
    std::printf("\n%s\n", formatReport(r).c_str());
}

int main(int argc, char **argv) {
    std::string enginePath, pgnPath, jsonPath;
    AnalysisOptions opt;
    opt.depth = 22;
    const int MAX_DEPTH = 40, MAX_MOVETIME = 60000;
    int gameIndex = 0, threads = 0, hash = 0;     // 0 = decide below
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
        else if (a == "-h" || a == "--help") { std::printf("usage: wolf-eval [--engine PATH] [--depth N (default 22, max 40)] [--threads N] [--hash MB] [--movetime MS] [--method sameroot|lichess] [--game K] [--json FILE] game.pgn\n"); return 0; }
        else pgnPath = a;
    }
    if (pgnPath.empty()) { std::fprintf(stderr, "usage: wolf-eval [options] game.pgn   (--help for options)\n"); return 2; }
    if (opt.depth < 1 || opt.depth > MAX_DEPTH) { std::fprintf(stderr, "--depth must be between 1 and %d.\n", MAX_DEPTH); return 2; }
    if (opt.movetimeMs < 0 || opt.movetimeMs > MAX_MOVETIME) { std::fprintf(stderr, "--movetime must be at most %d ms.\n", MAX_MOVETIME); return 2; }
    std::string engineSource = "--engine";
    TarraschSettings ts = readTarraschIni();
    if (enginePath.empty() && !ts.enginePath.empty()) {
        enginePath = ts.enginePath;
        engineSource = "Tarrasch's setting (" + ts.iniPath + ")";
    }
    int hw = (int)std::thread::hardware_concurrency();
    if (hw <= 0) hw = 2;
    if (threads <= 0) threads = ts.cores > 1 ? ts.cores : std::max(1, hw / 2);
    threads = std::min(threads, hw);
    if (hash <= 0) hash = std::max(256, ts.hashMb);
    if (enginePath.empty()) {
        enginePath = newestEngineInTarrasch();
        if (!enginePath.empty()) engineSource = "newest Stockfish in Tarrasch's Engines folder";
    }
    if (enginePath.empty()) { std::fprintf(stderr, "No engine found. Pass --engine PATH (e.g. your Stockfish .exe).\n"); return 2; }
    std::fprintf(stderr, "Using engine %s\n  (from %s)\n  %d thread%s, %d MB hash, ", enginePath.c_str(), engineSource.c_str(), threads, threads == 1 ? "" : "s", hash);
    if (opt.movetimeMs > 0) std::fprintf(stderr, "%d ms per position\n", opt.movetimeMs);
    else std::fprintf(stderr, "depth %d\n", opt.depth);

    std::string text;
    if (!readFile(pgnPath, text)) { std::fprintf(stderr, "Cannot read %s\n", pgnPath.c_str()); return 1; }
    std::vector<PgnGame> games = parsePgn(text);
    if (games.empty()) { std::fprintf(stderr, "No games found in %s\n", pgnPath.c_str()); return 1; }

    UciEngine engine;
    std::string err;
    if (!engine.start(enginePath, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    engine.setOption("Threads", std::to_string(threads));
    engine.setOption("Hash", std::to_string(hash));

    auto started = std::chrono::steady_clock::now();
    opt.progress = [&started](int done, int total) {
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        double left = done > 0 ? secs / done * (total - done) : 0;
        char eta[48];
        if (done >= total) std::snprintf(eta, sizeof eta, "done in %s", fmtTime(secs).c_str());
        else if (done < 2) std::snprintf(eta, sizeof eta, "estimating time...");
        else std::snprintf(eta, sizeof eta, "about %s left", fmtTime(left).c_str());
        std::fprintf(stderr, "\r  position %d/%d   %-24s", done, total, eta);
        std::fflush(stderr);
        return true;
    };

    int rc = 0;
    for (size_t g = 0; g < games.size(); ++g) {
        if (gameIndex > 0 && int(g) + 1 != gameIndex) continue;
        started = std::chrono::steady_clock::now();
        GameReport rep = analyseGame(games[g], engine, opt);
        std::fprintf(stderr, "\n");
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
