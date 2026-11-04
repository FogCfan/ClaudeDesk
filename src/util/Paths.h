#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace util {

fs::path homeDir();          // %USERPROFILE%
fs::path appDataDir();       // %APPDATA%\ClaudeDesk (créé si absent)
fs::path documentsDir();     // %USERPROFILE%\Documents
fs::path claudeConfigDir();  // %USERPROFILE%\.claude

// Lecture / écriture de fichiers texte entiers (UTF-8). L'écriture passe par un
// fichier temporaire puis un renommage, pour ne jamais laisser un JSON à moitié écrit.
bool readFile(const fs::path& p, std::string& out);
bool writeFileAtomic(const fs::path& p, const std::string& content);
bool readBinary(const fs::path& p, std::vector<unsigned char>& out);

// Chemin UTF-8 <-> fs::path sans dépendre de la page de code ANSI.
fs::path pathFromUtf8(const std::string& s);
std::string pathToUtf8(const fs::path& p);

// Ouvre un dossier/fichier dans l'Explorateur ou une URL dans le navigateur.
void openInShell(const std::string& target);

// Relance l'exécutable courant avec les mêmes arguments (l'appelant doit ensuite quitter).
bool relaunchSelf();

}  // namespace util
