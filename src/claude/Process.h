#pragma once
// Processus enfant avec stdin/stdout/stderr redirigés (Win32).
// Deux threads lisent stdout et stderr ligne par ligne et empilent les lignes
// dans des files protégées : le thread UI les récupère avec poll().
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace claude {

struct ProcessLine {
    bool isStderr = false;
    std::string text;
};

class Process {
public:
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // `exe` : chemin de l'exécutable, `args` : arguments (sans argv[0]).
    // `cwd` vide = répertoire courant. Retourne false (et remplit error()) si échec.
    bool start(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd);

    bool write(const std::string& data);  // écrit sur stdin
    void closeStdin();                    // signale la fin de l'entrée (le CLI se termine proprement)
    void kill();                          // arrêt immédiat
    // Ferme stdin puis attend jusqu'à `ms` millisecondes avant de tuer.
    void shutdown(int ms = 1500);

    bool running() const;
    int exitCode() const { return exitCode_; }
    const std::string& error() const { return error_; }

    // Récupère les lignes reçues depuis le dernier appel.
    std::vector<ProcessLine> poll();

private:
    void readerLoop(void* handle, bool isStderr);
    void cleanupHandles();

    void* process_ = nullptr;
    void* stdinWrite_ = nullptr;
    void* stdoutRead_ = nullptr;
    void* stderrRead_ = nullptr;
    std::thread outThread_, errThread_;
    std::mutex mutex_;
    std::deque<ProcessLine> lines_;
    mutable int exitCode_ = -1;
    std::string error_;
};

}  // namespace claude
