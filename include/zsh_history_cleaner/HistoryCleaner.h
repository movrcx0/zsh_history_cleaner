#ifndef HISTORY_CLEANER_H
#define HISTORY_CLEANER_H

#include <filesystem> // Requires C++17
#include <string>
#include <vector>
#include <regex>
#include <ctime>
#include <csignal>     // For sig_atomic_t
#include <iosfwd>      // For std::ostream forward declaration
#include <optional>    // For std::optional (used for filterRegex_)

namespace fs = std::filesystem;

class HistoryCleaner {
public:
    // Defines the different cleaning operations available
    enum class Mode { NONE, TODAY, LAST_7_DAYS, LAST_30_DAYS, SPECIFIC_DAY, BETWEEN, BEFORE, AFTER, OLDER_THAN, NEWER_THAN, ALL_TIME }; // Added NEWER_THAN

    // Constructor: Parses command-line arguments to configure the cleaner.
    HistoryCleaner(int argc, char* argv[]);

    // Destructor: Ensures cleanup, especially of temporary files.
    ~HistoryCleaner();

    // Prevent copy/move operations to avoid issues with resource management (temp files, signals)
    HistoryCleaner(const HistoryCleaner&) = delete;
    HistoryCleaner& operator=(const HistoryCleaner&) = delete;
    HistoryCleaner(HistoryCleaner&&) = delete;
    HistoryCleaner& operator=(HistoryCleaner&&) = delete;

    // Main entry point to start the cleaning process based on configuration.
    void run();

    // Public signal handler callback (static version needed for C-style signal registration)
    // Made public for simplicity, could be refactored with a dedicated SignalHandler class.
    static void staticSignalHandler(int signal);

private:
    // Path to temporary file being processed (if any)
    fs::path tempFilePath_;
    // Path to backup file (if created)
    fs::path backupFilePath_;

    // --- Configuration Members ---
    fs::path historyFilePath_;          // Path provided by user or default
    fs::path effectiveHistoryFilePath_; // Resolved absolute path of the history file
    Mode mode_ = Mode::NONE;            // Selected cleaning mode
    std::string startDateStr_;          // Start date for 'between' mode (YYYY-MM-DD)
    std::string endDateStr_;            // End date for 'between' mode (YYYY-MM-DD)
    std::string specificDateStr_;       // Date for 'before'/'after'/'specific_day' modes
    int olderThanDays_ = -1;            // Number of days for 'older_than' mode
    bool doBackup_ = false;             // Flag to create a backup before deletion
    bool dryRun_ = false;               // Flag to simulate deletion without actual changes
    bool interactive_ = true;           // Flag indicating interactive mode (vs. command-line args)
    bool preciseTime_ = false;          // Flag indicating if precise time should be used/required for dates
    int shredPasses_ = 32;              // Number of passes for secure delete (read from Constants.h)

    // --- State Members ---
    std::time_t startTimestamp_ = 0;    // Start timestamp for filtering (inclusive)
    std::time_t endTimestamp_ = 0;      // End timestamp for filtering (inclusive)
    std::regex historyEntryRegex_;      // Regex to parse Zsh history entries

    // Content Filters
    std::vector<std::string> filterKeywords_;      // Multiple keywords to filter entries by
    std::vector<std::string> filterRegexStrs_;     // Multiple regex patterns to filter entries by
    std::vector<std::regex> filterRegexes_;        // Compiled regex objects

    volatile sig_atomic_t interrupted_ = 0; // Flag set by signal handler for graceful shutdown

    // --- Private Helper Methods ---

    // Parses command-line arguments and sets configuration members.
    void parseArguments(int argc, char* argv[]);

    // Prints usage instructions and exits.
    void usage(const std::string& progName);

    // Resolves the history file path to an absolute path and checks existence.
    void resolveHistoryPath();

    // Sets up signal handlers for SIGINT, SIGTERM, SIGHUP.
    void setupSignalHandlers();

    // Instance-specific signal handler logic.
    // Ensures proper cleanup of resources before exiting on signal.
    // This is critical for maintaining filesystem consistency and security.
    void cleanupAndExit(int signal);

    // Cleanup any temporary files or resources
    void cleanup();

    // Runs the interactive menu-driven mode.
    void runInteractive();

    // Runs the non-interactive mode based on command-line arguments.
    void runNonInteractive();

    // Calculates the start and end timestamps based on the selected mode.
    void calculateTimestamps();

    // Core logic: Reads history, filters entries, writes to new file.
    // Returns true if processing was successful.
    bool processHistory(std::ostream& output);

    // Creates a backup of the original history file.
    bool backupHistoryFile();

    // Performs the final steps: backup (if requested), secure delete.
    bool performCleanup(std::ostream& output);

    // Validates necessary permissions (read history, write directory).
    void checkPermissions();

    // Process a single command block and determine if it should be deleted
    // Returns true if the block should be deleted, false if it should be kept
    bool processCommandBlock(const std::string& block, unsigned long long lineNum,
                           std::ostream& output, unsigned long long& keptCount,
                           unsigned long long& deletedCount);

    // Static pointer to the current instance for the static signal handler.
    // This is a common pattern but has limitations (only one instance).
    static HistoryCleaner* g_cleaner_instance;
};

#endif // HISTORY_CLEANER_H