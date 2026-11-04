#pragma once
// Lecture de l'historique de Claude Code (~/.claude/projects/<dossier encodé>/<session>.jsonl)
// pour importer des sessions passées (faites dans le terminal) dans l'espace de travail.
#include <cstdint>
#include <string>
#include <vector>

#include "claude/Transcript.h"

namespace claude {

struct HistorySession {
    std::string sessionId;
    std::string file;
    std::string title;      // ai-title ou premier message
    std::string firstPrompt;
    std::string cwd;
    int64_t start = 0, end = 0;
    int userMessages = 0;
    int64_t sizeBytes = 0;
};

struct HistoryProject {
    std::string dirName;  // C--Dev-Platformer2D
    std::string cwd;      // C:\Dev\Platformer2D (lu dans les sessions)
    int sessions = 0;
    int64_t lastModified = 0;
};

// Nom de dossier utilisé par Claude Code pour un chemin (non alphanumérique -> '-').
std::string encodeProjectDir(const std::string& path);

std::vector<HistoryProject> listHistoryProjects();
std::vector<HistorySession> listHistorySessions(const std::string& projectDirName);
// Sessions correspondant à un dossier de travail (comparaison insensible à la casse).
std::vector<HistorySession> listHistorySessionsForCwd(const std::string& cwd);

// Convertit une session en transcript ClaudeDesk.
std::vector<Entry> loadHistoryTranscript(const std::string& file);

}  // namespace claude
