#include "claude/Process.h"

#include "claude/CommandLine.h"
#include "util/Strings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace claude {

Process::~Process() { shutdown(500); }

bool Process::start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd) {
    if (process_) shutdown(500);
    error_.clear();
    exitCode_ = -1;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        error_ = "CreatePipe a échoué";
        return false;
    }
    // Les extrémités côté parent ne doivent pas être héritées par l'enfant.
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};

    std::wstring cmd = util::widen(buildCommandLine(exe, args));
    std::wstring wcwd = util::widen(cwd);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        DWORD e = GetLastError();
        error_ = "Impossible de lancer « " + exe + " » (erreur Windows " + std::to_string(e) + ")";
        if (!cwd.empty() && e == ERROR_DIRECTORY) error_ += " — dossier de travail invalide : " + cwd;
        CloseHandle(inW);
        CloseHandle(outR);
        CloseHandle(errR);
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    stdinWrite_ = inW;
    stdoutRead_ = outR;
    stderrRead_ = errR;
    outThread_ = std::thread(&Process::readerLoop, this, (std::intptr_t)stdoutRead_, false);
    errThread_ = std::thread(&Process::readerLoop, this, (std::intptr_t)stderrRead_, true);
    return true;
}

void Process::readerLoop(std::intptr_t handle, bool isStderr) {
    std::string pending;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile((HANDLE)handle, buf, sizeof buf, &n, nullptr) && n > 0) {
        pending.append(buf, n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending.erase(0, pos + 1);
            std::lock_guard lock(mutex_);
            lines_.push_back({isStderr, std::move(line)});
        }
    }
    if (!pending.empty()) {
        std::lock_guard lock(mutex_);
        lines_.push_back({isStderr, std::move(pending)});
    }
}

bool Process::write(const std::string& data) {
    if (!stdinWrite_) return false;
    DWORD written = 0;
    size_t off = 0;
    while (off < data.size()) {
        if (!WriteFile((HANDLE)stdinWrite_, data.data() + off, (DWORD)(data.size() - off), &written, nullptr)) return false;
        off += written;
    }
    return true;
}

void Process::closeStdin() {
    if (stdinWrite_) {
        CloseHandle((HANDLE)stdinWrite_);
        stdinWrite_ = nullptr;
    }
}

bool Process::running() const {
    if (!process_) return false;
    DWORD code = 0;
    if (GetExitCodeProcess((HANDLE)process_, &code) && code == STILL_ACTIVE) return true;
    exitCode_ = (int)code;
    return false;
}

void Process::kill() {
    if (process_ && running()) TerminateProcess((HANDLE)process_, 1);
}

void Process::shutdown(int ms) {
    if (!process_) return;
    closeStdin();
    if (WaitForSingleObject((HANDLE)process_, (DWORD)ms) == WAIT_TIMEOUT) {
        // Le CLI lance des sous-processus (bash, MCP…) : on termine tout l'arbre.
        std::wstring cmd = L"taskkill /T /F /PID " + std::to_wstring(GetProcessId((HANDLE)process_));
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        TerminateProcess((HANDLE)process_, 1);
        WaitForSingleObject((HANDLE)process_, 2000);
    }
    running();  // met à jour exitCode_
    cleanupHandles();
}

void Process::cleanupHandles() {
    // Les lectures se terminent quand le processus ferme ses sorties.
    if (outThread_.joinable()) outThread_.join();
    if (errThread_.joinable()) errThread_.join();
    for (void** h : {&process_, &stdinWrite_, &stdoutRead_, &stderrRead_}) {
        if (*h) {
            CloseHandle((HANDLE)*h);
            *h = nullptr;
        }
    }
}

std::vector<ProcessLine> Process::poll() {
    std::lock_guard lock(mutex_);
    std::vector<ProcessLine> out(std::make_move_iterator(lines_.begin()), std::make_move_iterator(lines_.end()));
    lines_.clear();
    return out;
}

}  // namespace claude
