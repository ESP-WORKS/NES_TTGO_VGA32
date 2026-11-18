#ifndef SMS_SD_CASE_H
#define SMS_SD_CASE_H

// Resolver FAT de caminhos com a grafia real de cada componente.
// Inspirado em MC-1000 v0.7 (opendir/strcasecmp), mas usando a interface
// Arduino SD/File que ja e empregada pelo navegador SMS.
#include <SD.h>
#include <string>
#include <ctype.h>

namespace SMSSDCase {
inline bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}
inline std::string basename(const char* path) {
    std::string name = path ? path : "";
    const size_t slash = name.find_last_of("/\\");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}
inline bool resolve(const std::string& path, std::string& actual, bool wantDir) {
    actual.clear();
    if (path.empty() || path[0] != '/') return false;
    if (path == "/") { actual = "/"; return true; }
    // Caminho existente: nao fazer varredura desnecessaria no SD.
    // Se o driver FAT ja resolver maiusculas/minusculas, funciona direto.
    File direct = SD.open(path.c_str());
    if (direct) {
        const bool valid = direct.isDirectory() == wantDir;
        direct.close();
        if (valid) { actual = path; return true; }
    }
    std::string parent = "/";
    size_t from = 1;
    while (from < path.size()) {
        while (from < path.size() && path[from] == '/') ++from;
        if (from == path.size()) break;
        size_t end = path.find('/', from);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(from, end - from);
        if (part == "." || part == ".." || part.empty()) return false;
        const bool final = end == path.size() || path.find_first_not_of('/', end) == std::string::npos;
        File dir = SD.open(parent.c_str());
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }
        std::string match;
        while (true) {
            File entry = dir.openNextFile();
            if (!entry) break;
            const bool typeOK = entry.isDirectory() == (final ? wantDir : true);
            const std::string name = basename(entry.name());
            if (typeOK && equalsIgnoreCase(name, part)) {
                match = name;
                entry.close();
                break;
            }
            entry.close();
        }
        dir.close();
        if (match.empty()) return false;
        parent = (parent == "/" ? "/" : parent + "/") + match;
        from = end + 1;
    }
    actual = parent;
    return !actual.empty();
}
inline bool resolveDirectory(const std::string& path, std::string& actual) {
    return resolve(path, actual, true);
}
inline bool resolveFile(const std::string& path, std::string& actual) {
    return resolve(path, actual, false);
}
inline bool ensureDirectory(const std::string& path, std::string& actual) {
    if (resolveDirectory(path, actual)) return true;
    const size_t split = path.find_last_of('/');
    if (split == std::string::npos || split + 1 >= path.size()) return false;
    std::string parent;
    if (!ensureDirectory(path.substr(0, split).empty() ? "/" : path.substr(0, split), parent))
        return false;
    const std::string full = (parent == "/" ? "/" : parent + "/") + path.substr(split + 1);
    if (!SD.mkdir(full.c_str())) return false;
    actual = full;
    return true;
}
} // namespace SMSSDCase
#endif
