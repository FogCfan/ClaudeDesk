// Implémentation POSIX (macOS, Linux) de claude::Process : fork + execvp, trois tubes,
// l'enfant devient chef de son propre groupe pour pouvoir arrêter tout l'arbre (bash, MCP…).
#include "claude/Process.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace claude {

namespace {

void closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void setCloexec(int fd) { ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC); }

bool makePipe(int fds[2]) {
    if (::pipe(fds) != 0) return false;
    setCloexec(fds[0]);
    setCloexec(fds[1]);
    return true;
}

// Écrire dans un tube dont le lecteur est mort ne doit pas tuer l'application.
void ignoreSigpipeOnce() {
    static bool done = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)done;
}

}  // namespace

Process::~Process() { shutdown(500); }

bool Process::start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd) {
    if (pid_ > 0) shutdown(500);
    ignoreSigpipeOnce();
    error_.clear();
    exitCode_ = -1;
    reaped_ = false;

    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, status[2] = {-1, -1};
    if (!makePipe(in) || !makePipe(out) || !makePipe(err) || !makePipe(status)) {
        error_ = std::string("pipe a échoué : ") + std::strerror(errno);
        for (int* p : {in, out, err, status}) closeFd(p[0]), closeFd(p[1]);
        return false;
    }

    // Tout ce dont l'enfant a besoin est préparé avant fork (seuls des appels sûrs ensuite).
    std::vector<std::string> argvStore;
    argvStore.reserve(args.size() + 1);
    argvStore.push_back(exe);
    argvStore.insert(argvStore.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argvStore) argv.push_back(a.data());
    argv.push_back(nullptr);
    const char* dir = cwd.empty() ? nullptr : cwd.c_str();

    pid_t pid = ::fork();
    if (pid < 0) {
        error_ = std::string("fork a échoué : ") + std::strerror(errno);
        for (int* p : {in, out, err, status}) closeFd(p[0]), closeFd(p[1]);
        return false;
    }
    if (pid == 0) {
        // Enfant. En cas d'échec, errno est renvoyé au parent par le tube `status`.
        ::setpgid(0, 0);
        ::signal(SIGPIPE, SIG_DFL);
        int stage = 0;
        if (::dup2(in[0], 0) < 0 || ::dup2(out[1], 1) < 0 || ::dup2(err[1], 2) < 0) stage = 1;
        else if (dir && ::chdir(dir) != 0) stage = 2;
        else ::execvp(argv[0], argv.data()), stage = 3;
        int payload[2] = {stage, errno};
        ssize_t w = ::write(status[1], payload, sizeof payload);
        (void)w;
        ::_exit(127);
    }

    ::setpgid(pid, pid);  // évite la course avec le setpgid de l'enfant
    closeFd(in[0]);
    closeFd(out[1]);
    closeFd(err[1]);
    closeFd(status[1]);

    int payload[2] = {0, 0};
    ssize_t n;
    do n = ::read(status[0], payload, sizeof payload);
    while (n < 0 && errno == EINTR);
    closeFd(status[0]);
    if (n == (ssize_t)sizeof payload) {  // exec n'a pas eu lieu
        int st = 0;
        ::waitpid(pid, &st, 0);
        if (payload[0] == 2) error_ = "Dossier de travail invalide : " + cwd + " (" + std::strerror(payload[1]) + ")";
        else error_ = "Impossible de lancer « " + exe + " » (" + std::strerror(payload[1]) + ")";
        closeFd(in[1]);
        closeFd(out[0]);
        closeFd(err[0]);
        return false;
    }

    pid_ = pid;
    stdinFd_ = in[1];
    stdoutFd_ = out[0];
    stderrFd_ = err[0];
    outThread_ = std::thread(&Process::readerLoop, this, (std::intptr_t)stdoutFd_, false);
    errThread_ = std::thread(&Process::readerLoop, this, (std::intptr_t)stderrFd_, true);
    return true;
}

void Process::readerLoop(std::intptr_t handle, bool isStderr) {
    int fd = (int)handle;
    std::string pending;
    char buf[8192];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        pending.append(buf, (size_t)n);
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
    if (stdinFd_ < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = ::write(stdinFd_, data.data() + off, data.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

void Process::closeStdin() { closeFd(stdinFd_); }

bool Process::running() const {
    if (pid_ <= 0 || reaped_) return false;
    int st = 0;
    pid_t r = ::waitpid(pid_, &st, WNOHANG);
    if (r == 0) return true;
    reaped_ = true;
    if (r == pid_) exitCode_ = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
    return false;
}

void Process::kill() {
    if (running()) ::killpg(pid_, SIGKILL);
}

void Process::shutdown(int ms) {
    if (pid_ <= 0) return;
    closeStdin();
    auto waitFor = [&](int timeoutMs) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (running()) {
            if (std::chrono::steady_clock::now() >= end) return false;
            ::usleep(10 * 1000);
        }
        return true;
    };
    if (!waitFor(ms)) {
        // Le CLI lance des sous-processus (bash, MCP…) : on termine tout le groupe.
        ::killpg(pid_, SIGTERM);
        if (!waitFor(1500)) {
            ::killpg(pid_, SIGKILL);
            waitFor(2000);
        }
    } else {
        ::killpg(pid_, SIGTERM);  // sous-processus orphelins éventuels (le groupe peut être vide)
    }
    running();  // met à jour exitCode_
    cleanupHandles();
}

void Process::cleanupHandles() {
    // Les lectures se terminent quand le processus (et ses enfants) ferment leurs sorties.
    if (outThread_.joinable()) outThread_.join();
    if (errThread_.joinable()) errThread_.join();
    closeFd(stdinFd_);
    closeFd(stdoutFd_);
    closeFd(stderrFd_);
    pid_ = -1;
}

std::vector<ProcessLine> Process::poll() {
    std::lock_guard lock(mutex_);
    std::vector<ProcessLine> out(std::make_move_iterator(lines_.begin()), std::make_move_iterator(lines_.end()));
    lines_.clear();
    return out;
}

}  // namespace claude
