#ifndef SECURE_DELETE_H
#define SECURE_DELETE_H

#include <filesystem> // Requires C++17
#include <iostream>   // For std::cerr default argument
#include <iosfwd>     // For forward declaration of std::ostream
#include <cstring>    // For strerror
#include <cerrno>     // For errno

namespace fs = std::filesystem;

// Best-effort secure delete implementation
// Returns true on successful deletion (including overwrite passes), false otherwise.
// Logs warnings/errors to the provided ostream.
bool secureDelete(const fs::path& filepath, int passes, std::ostream& log = std::cerr);

#endif // SECURE_DELETE_H