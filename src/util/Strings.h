#pragma once
// Petits utilitaires texte : UTF-8 <-> UTF-16 (Windows), base64, dates, slugs.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace util {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

std::string trim(std::string_view s);
std::string toLower(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool containsInsensitive(std::string_view haystack, std::string_view needle);
std::vector<std::string> split(std::string_view s, char sep);
std::vector<std::string> splitLines(std::string_view s);
void replaceAll(std::string& s, std::string_view from, std::string_view to);

// Coupe à `maxBytes` sans casser un caractère UTF-8 ; ajoute "…" si tronqué.
std::string truncateUtf8(std::string_view s, size_t maxBytes);
// Première ligne non vide, tronquée — pratique pour les titres.
std::string firstLine(std::string_view s, size_t maxBytes = 80);

// Nom de fichier sûr (accents conservés, caractères interdits Windows retirés).
std::string sanitizeFileName(std::string_view name);

std::string base64Encode(const std::vector<uint8_t>& data);

// Horodatage ISO-8601 local (2026-11-04T10:12:00) et secondes Unix.
int64_t nowUnix();
std::string isoFromUnix(int64_t t);
int64_t unixFromIso(std::string_view iso);  // 0 si invalide
std::string humanDate(int64_t t);            // "04/11 10:12"
std::string humanDuration(int64_t seconds);  // "2 h 05", "12 min", "45 s"
std::string humanTokens(int64_t n);          // "12,3 k", "1,20 M"
std::string formatUsd(double usd);           // "0,0123 $"

std::string newUuid();  // UUID v4 aléatoire

}  // namespace util
