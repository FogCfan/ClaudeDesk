#pragma once
#include <string>
#include <vector>

namespace claude {

// Échappe un argument selon les règles de CommandLineToArgvW / CRT (Windows uniquement :
// sous POSIX les arguments sont passés tels quels à execvp).
std::string quoteArg(const std::string& arg);
std::string buildCommandLine(const std::string& exe, const std::vector<std::string>& args);

// Cherche l'exécutable natif de Claude Code : claude.exe dans le PATH, sinon
// derrière le lanceur npm claude.cmd, sinon %USERPROFILE%\.local\bin.
// macOS / Linux : `claude` dans le PATH, ~/.local/bin, Homebrew…
// Retourne "" si introuvable.
std::string findClaudeExecutable();

}  // namespace claude
