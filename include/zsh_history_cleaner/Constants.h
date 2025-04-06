#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <string>
#include <cstddef> // For size_t

// --- Configuration Constants ---
const int SHRED_PASSES = 32; // Number of overwrite passes
const std::string TMP_PREFIX = ".zsh_history_cleaner_"; // Prefix for temp file
const size_t SHRED_BUFFER_SIZE = 4096; // Buffer size for shredding

#endif // CONSTANTS_H