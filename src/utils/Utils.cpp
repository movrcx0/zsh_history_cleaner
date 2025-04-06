#include "../../include/zsh_history_cleaner/Utils.h"
#include "../../include/zsh_history_cleaner/Constants.h" // Might be needed indirectly or directly later

#include <iostream>
#include <cstdlib>      // For std::getenv, std::exit
#include <chrono>       // For system_clock
#include <ctime>        // For time_t, tm, strptime, mktime, localtime_r, strftime
#include <string>
#include <stdexcept>    // For runtime_error
#include <algorithm>    // For std::tolower
#include <cctype>       // For std::tolower with unsigned char cast

// Get environment variable safely
std::string getEnvVar(const std::string& name, const std::string& defaultValue) {
    const char* value = std::getenv(name.c_str());
    return (value == nullptr) ? defaultValue : std::string(value);
}

// Print error message and exit
[[noreturn]] void errorExit(const std::string& message, int exitCode) {
    std::cerr << "Error: " << message << std::endl;
    // In a multi-file project, direct cleanup from here is harder.
    // Rely on RAII or registered cleanup functions if needed.
    std::exit(exitCode);
}

// Convert date string (YYYY-MM-DD [HH:MM[:SS]]) to Unix epoch timestamp
// - If precise is true, requires time component
// - If precise is false, allows date-only format and applies defaultTime
std::time_t dateToEpoch(const std::string& dateStr, bool precise, const std::string& defaultTime) {
    std::tm t{}; // Zero-initialize
    const char* input = dateStr.c_str();
    char* parse_end = nullptr;

    // Try parsing with different formats in order of specificity
    const char* formats[] = {
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%d %H:%M",
        "%Y-%m-%d"
    };

    for (const char* format : formats) {
        t = {}; // Reset for each attempt
        parse_end = strptime(input, format, &t);
        
        if (parse_end != nullptr) {
            // Skip trailing whitespace
            while (*parse_end != '\0' && std::isspace(static_cast<unsigned char>(*parse_end))) {
                parse_end++;
            }
            
            // Check for unexpected trailing characters
            if (*parse_end != '\0') {
                continue; // Try next format if there are trailing characters
            }

            // Successfully parsed
            bool isDateOnly = (format == formats[2]);
            
            // Handle time components based on format matched
            if (format == formats[1]) { // HH:MM format
                t.tm_sec = 0;
            } else if (isDateOnly) {
                if (precise) {
                    throw std::runtime_error("Time component required when using precise mode: '" + dateStr +
                                           "'. Use YYYY-MM-DD HH:MM or YYYY-MM-DD HH:MM:SS");
                }
                
                // Apply default time for date-only format
                int hour = 0, min = 0, sec = 0;
                if (sscanf(defaultTime.c_str(), "%d:%d:%d", &hour, &min, &sec) == 3) {
                    t.tm_hour = hour;
                    t.tm_min = min;
                    t.tm_sec = sec;
                } else {
                    t.tm_hour = t.tm_min = t.tm_sec = 0;
                    std::cerr << "Warning: Invalid defaultTime format '" << defaultTime
                             << "'. Using 00:00:00." << std::endl;
                }
            }

            // Convert to epoch
            t.tm_isdst = -1; // Let system determine DST
            std::time_t epoch = mktime(&t);
            if (epoch == -1) {
                throw std::runtime_error("Failed to convert to timestamp: '" + dateStr + "'");
            }
            return epoch;
        }
    }

    // If we get here, no format matched
    throw std::runtime_error("Invalid date/time format: '" + dateStr +
                           "'. Use YYYY-MM-DD" + (precise ? " HH:MM[:SS]" : "[ HH:MM[:SS]]"));
}

// Get current epoch time
std::time_t nowEpoch() {
    // C++11 way
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

// Format epoch time to string (for debugging/output)
std::string epochToString(std::time_t epoch) {
    // Handle special case for max time_t (effectively infinity)
    if (epoch == std::numeric_limits<std::time_t>::max()) {
        return "∞";  // Unicode infinity symbol
    }
    
    std::tm tm_snapshot{};
    
    // Convert to local time using thread-safe localtime_r
    if (localtime_r(&epoch, &tm_snapshot) == nullptr) {
        return "Invalid timestamp";
    }
    
    // Initial buffer size (should be enough for most cases)
    size_t bufSize = 32;
    std::string result;
    result.resize(bufSize);
    
    while (true) {
        // Attempt to format the time
        size_t len = strftime(&result[0], result.size(), "%Y-%m-%d %H:%M:%S %Z", &tm_snapshot);
        
        if (len > 0) {
            // Success - resize string to actual length and return
            result.resize(len);
            return result;
        }
        
        // If failed, double the buffer size and try again
        if (bufSize >= 256) { // Reasonable maximum size
            return "Error formatting timestamp";
        }
        
        bufSize *= 2;
        result.resize(bufSize);
    }
}


// Ask Yes/No question
bool askYesNo(const std::string& prompt, bool defaultYes) {
    while (true) {
        std::cout << prompt << (defaultYes ? " [Y/n]: " : " [y/N]: ");
        std::string answerStr;
        if (!std::getline(std::cin, answerStr)) {
             // Handle EOF or input error gracefully
             std::cerr << "\nInput error or EOF detected. Assuming default ("
                       << (defaultYes ? "Yes" : "No") << ")." << std::endl;
             // Consider if throwing an exception or returning a specific state is better
             return defaultYes;
        }

        // Trim leading/trailing whitespace for robustness
        auto first_char = answerStr.find_first_not_of(" \t\n\r\f\v");
        if (first_char == std::string::npos) { // String is all whitespace or empty
             return defaultYes;
        }
        auto last_char = answerStr.find_last_not_of(" \t\n\r\f\v");
        answerStr = answerStr.substr(first_char, last_char - first_char + 1);


        if (answerStr.empty()) { // Check again after trimming (shouldn't be needed but safe)
             return defaultYes;
        }

        // Use static_cast<unsigned char> before tolower for safety with potential non-ASCII chars
        char firstChar = static_cast<char>(std::tolower(static_cast<unsigned char>(answerStr[0])));
        if (firstChar == 'y') {
            return true;
        } else if (firstChar == 'n') {
            return false;
        } else {
            std::cout << "Invalid input. Please answer yes (y) or no (n)." << std::endl;
        }
    }
}