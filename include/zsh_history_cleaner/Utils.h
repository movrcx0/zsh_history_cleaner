#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <ctime>
#include <stdexcept> // For runtime_error in dateToEpoch

// Get environment variable safely
std::string getEnvVar(const std::string& name, const std::string& defaultValue);

// Print error message and exit
[[noreturn]] void errorExit(const std::string& message, int exitCode = 1);

// Convert date string (YYYY-MM-DD [HH:MM:SS]) to Unix epoch timestamp
// If precise is true, dateStr MUST include time (HH:MM or HH:MM:SS), otherwise throws.
// If precise is false, defaultTime is appended if time is missing.
std::time_t dateToEpoch(const std::string& dateStr, bool precise, const std::string& defaultTime = "00:00:00");

// Get current epoch time
std::time_t nowEpoch();

// Format epoch time to string (for debugging/output)
std::string epochToString(std::time_t epoch);

// Ask Yes/No question
bool askYesNo(const std::string& prompt, bool defaultYes);

#endif // UTILS_H