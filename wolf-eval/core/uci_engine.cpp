#include "uci_engine.h"
#include <sstream>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <csignal>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace wolf {

// ---------------------------------------------------------------- process plumbing
#ifdef _WIN32
struct UciEngine::Impl {
    HANDLE childIn = nullptr;     // we write here
    HANDLE childOut = nullptr;    // we read here
    PROCESS_INFORMATION pi{};
    std::string buf;

    bool start(const std::string &path, std::string &err) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE outRead, outWrite, inRead, inWrite;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&inRead, &inWrite, &sa, 0)) {
            err = "CreatePipe failed"; return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead; si.hStdOutput = outWrite; si.hStdError = outWrite;
        std::string cmd = "\"" + path + "\"";
        std::vector<char> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(0);
        // Run the engine from its own folder so it finds its network files.
        std::string dir = path.substr(0, path.find_last_of("\\/") == std::string::npos ? 0 : path.find_last_of("\\/"));
        BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                 nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
        CloseHandle(outWrite); CloseHandle(inRead);
        if (!ok) { err = "Could not start engine: " + path; CloseHandle(outRead); CloseHandle(inWrite); return false; }
        childIn = inWrite; childOut = outRead;
        return true;
    }
    bool write(const std::string &s) {
        DWORD w = 0;
        return WriteFile(childIn, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size();
    }
    bool readLine(std::string &line) {
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                line = buf.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, nl + 1);
                return true;
            }
            char tmp[4096]; DWORD r = 0;
            if (!ReadFile(childOut, tmp, sizeof tmp, &r, nullptr) || r == 0) return false;
            buf.append(tmp, r);
        }
    }
    void close() {
        if (childIn) { CloseHandle(childIn); childIn = nullptr; }
        if (pi.hProcess) {
            if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread); pi.hProcess = nullptr;
        }
        if (childOut) { CloseHandle(childOut); childOut = nullptr; }
    }
};
#else
struct UciEngine::Impl {
    int toChild = -1, fromChild = -1;
    pid_t pid = -1;
    std::string buf;

    bool start(const std::string &path, std::string &err) {
        int in[2], out[2];
        if (pipe(in) || pipe(out)) { err = "pipe failed"; return false; }
        pid = fork();
        if (pid < 0) { err = "fork failed"; return false; }
        if (pid == 0) {
            dup2(in[0], 0); dup2(out[1], 1); dup2(out[1], 2);
            ::close(in[1]); ::close(out[0]);
            execl(path.c_str(), path.c_str(), (char *)nullptr);
            _exit(127);
        }
        ::close(in[0]); ::close(out[1]);
        toChild = in[1]; fromChild = out[0];
        signal(SIGPIPE, SIG_IGN);
        return true;
    }
    bool write(const std::string &s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t w = ::write(toChild, s.data() + off, s.size() - off);
            if (w <= 0) return false;
            off += w;
        }
        return true;
    }
    bool readLine(std::string &line) {
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                line = buf.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, nl + 1);
                return true;
            }
            char tmp[4096];
            ssize_t r = ::read(fromChild, tmp, sizeof tmp);
            if (r <= 0) return false;
            buf.append(tmp, r);
        }
    }
    void close() {
        if (toChild >= 0) { ::close(toChild); toChild = -1; }
        if (pid > 0) {
            int st; for (int k = 0; k < 20 && waitpid(pid, &st, WNOHANG) == 0; ++k) usleep(100000);
            if (waitpid(pid, &st, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
            pid = -1;
        }
        if (fromChild >= 0) { ::close(fromChild); fromChild = -1; }
    }
};
#endif

// ---------------------------------------------------------------- UCI protocol
UciEngine::UciEngine() : impl_(new Impl) {}
UciEngine::~UciEngine() { stop(); }

bool UciEngine::send(const std::string &line) { return impl_->write(line + "\n"); }
bool UciEngine::readLine(std::string &line) { return impl_->readLine(line); }

bool UciEngine::waitFor(const std::string &prefix, std::string *lastLine) {
    std::string line;
    while (readLine(line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) { if (lastLine) *lastLine = line; return true; }
    }
    return false;
}

bool UciEngine::start(const std::string &path, std::string &err) {
    if (!impl_->start(path, err)) return false;
    if (!send("uci")) { err = "engine did not accept input"; return false; }
    std::string line;
    while (readLine(line)) {
        if (line.rfind("id name ", 0) == 0) name_ = line.substr(8);
        if (line == "uciok") break;
    }
    if (line != "uciok") { err = "engine did not answer 'uci' (is this a UCI engine?)"; return false; }
    send("isready");
    if (!waitFor("readyok")) { err = "engine did not become ready"; return false; }
    return true;
}

bool UciEngine::setOption(const std::string &name, const std::string &value) {
    return send("setoption name " + name + " value " + value);
}

void UciEngine::newGame() {
    send("ucinewgame");
    send("isready");
    waitFor("readyok");
}

EngineResult UciEngine::analyse(const std::string &fen, int depth, int movetimeMs, const std::string &onlyMove) {
    EngineResult r;
    send("position fen " + fen);
    std::string go = movetimeMs > 0 ? "go movetime " + std::to_string(movetimeMs) : "go depth " + std::to_string(depth);
    if (!onlyMove.empty()) go += " searchmoves " + onlyMove;
    send(go);
    std::string line;
    while (readLine(line)) {
        if (line.rfind("info ", 0) == 0) {
            std::istringstream ss(line);
            std::string tok;
            int d = 0, mpv = 1; bool haveScore = false, bound = false, mate = false; int val = 0;
            while (ss >> tok) {
                if (tok == "depth") ss >> d;
                else if (tok == "multipv") ss >> mpv;
                else if (tok == "score") {
                    std::string kind; ss >> kind >> val;
                    mate = (kind == "mate"); haveScore = true;
                } else if (tok == "lowerbound" || tok == "upperbound") bound = true;
                else if (tok == "pv") break;
            }
            if (haveScore && !bound && mpv == 1) { r.isMate = mate; r.value = val; r.depth = d; r.ok = true; }
        } else if (line.rfind("bestmove", 0) == 0) {
            std::istringstream ss(line);
            std::string b, mv; ss >> b >> mv;
            if (mv != "(none)" && mv != "0000") r.bestMoveUci = mv;
            break;
        }
    }
    return r;
}

void UciEngine::stop() {
    if (!impl_) return;
    send("quit");
    impl_->close();
}

} // namespace wolf
