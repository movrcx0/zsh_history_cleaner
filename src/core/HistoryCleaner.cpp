#include "../../include/zsh_history_cleaner/HistoryCleaner.h"
#include "../../include/zsh_history_cleaner/Constants.h"
#include "../../include/zsh_history_cleaner/Utils.h"
#include "../../include/zsh_history_cleaner/SecureDelete.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <regex>
#include <stdexcept>
#include <system_error>
#include <csignal>
#include <unistd.h>     // For geteuid, access
#include <sys/stat.h>   // For access mode constants
#include <cstdio>       // For std::remove
#include <cstdlib>      // For std::exit
#include <limits>       // For numeric_limits
#include <memory>       // For unique_ptr
#include <random>       // For random_device, mt19937
#include <cstring>      // For strerror
#include <cerrno>       // For errno

namespace fs = std::filesystem;

// Define the static member variable
HistoryCleaner* HistoryCleaner::g_cleaner_instance = nullptr;

// --- Constructor ---
HistoryCleaner::HistoryCleaner(int argc, char* argv[]) {
    // Initialize shred passes from constant
    shredPasses_ = SHRED_PASSES;

    // Basic regex for Zsh history: : <timestamp>:<duration>;<command>
    // Allows optional spaces around ':' and ';'
    // Captures the timestamp (group 1)
    try {
         // Make regex slightly more robust to leading/trailing whitespace on line
         historyEntryRegex_ = std::regex(R"(^\s*:\s*(\d+):\d+\s*;.*$)");
    } catch (const std::regex_error& e) {
         // Use errorExit from Utils.h
         errorExit(std::string("Regex compilation failed: ") + e.what());
    }

    parseArguments(argc, argv); // Parse arguments first
    resolveHistoryPath();       // Then resolve path based on potential --histfile arg
    checkPermissions();         // Check permissions early before potentially lengthy operations

    g_cleaner_instance = this; // Set global instance for signal handler
    setupSignalHandlers();
}

// --- Destructor ---
HistoryCleaner::~HistoryCleaner() {
    cleanup();
    g_cleaner_instance = nullptr; // Clear global instance
}

// --- Resource Management ---
void HistoryCleaner::cleanup() {
    // Clean up any temporary files that may exist
    try {
        std::error_code ec;
        
        // Clean up temporary processing file if it exists
        if (!tempFilePath_.empty() && fs::exists(tempFilePath_, ec)) {
            fs::remove(tempFilePath_, ec);
            if (ec) {
                std::cerr << "Warning: Failed to remove temporary file: " << tempFilePath_.string()
                         << " (" << ec.message() << ")" << std::endl;
            }
            tempFilePath_.clear();
        }

        // Note: We don't automatically clean up backup files as they should be preserved
        // The user might want to recover from them in case of issues
        
    } catch (const std::exception& e) {
        std::cerr << "Error during cleanup: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown error during cleanup" << std::endl;
    }
}

void HistoryCleaner::cleanupAndExit(int signal) {
    const char* signame = "";
    switch (signal) {
        case SIGINT:  signame = "SIGINT";  break;
        case SIGTERM: signame = "SIGTERM"; break;
        case SIGHUP:  signame = "SIGHUP";  break;
        default:      signame = "Unknown";  break;
    }
    
    // Use write() for signal safety
    const char msg[] = "\nReceived signal: ";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    write(STDERR_FILENO, signame, strlen(signame));
    write(STDERR_FILENO, "\nCleaning up...\n", 15);

    cleanup();  // Clean up resources

    // Reset signal to default handler and re-raise
    std::signal(signal, SIG_DFL);
    raise(signal);
}

// --- Signal Handling ---
void HistoryCleaner::setupSignalHandlers() {
    std::signal(SIGINT, HistoryCleaner::staticSignalHandler);  // Ctrl+C
    std::signal(SIGTERM, HistoryCleaner::staticSignalHandler); // Termination request
    std::signal(SIGHUP, HistoryCleaner::staticSignalHandler);  // Hangup
}

void HistoryCleaner::staticSignalHandler(int signal) {
    if (g_cleaner_instance) {
        g_cleaner_instance->interrupted_ = 1; // Set the flag (volatile ensures visibility)
        g_cleaner_instance->cleanupAndExit(signal); // Perform cleanup and exit
    } else {
        const char msg[] = "\nTermination signal received, but no active cleaner instance. Forcing exit.\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1); // Use write for signal safety
        _Exit(128 + signal); // Use _Exit for signal safety
    }
}

// --- Core Logic ---
void HistoryCleaner::run() {
    // Check interruption flag at the beginning
    if (interrupted_) {
        std::cerr << "Interrupted before starting main execution. Exiting." << std::endl;
        return;
    }

    try {
        if (interactive_) {
            runInteractive();
        } else {
            runNonInteractive();
        }
    } catch (const std::exception& e) {
        std::cerr << "\nRuntime Error: " << e.what() << std::endl;
        std::exit(1); // Exit with a non-zero code
    } catch (...) {
        std::cerr << "\nUnknown Runtime Error occurred." << std::endl;
        std::exit(2);
    }

    // Check interruption flag again at the end
     if (interrupted_) {
        std::cerr << "Operation interrupted during execution." << std::endl;
    }
}

void HistoryCleaner::runNonInteractive() {
    std::cout << "Running in non-interactive mode." << std::endl;
    std::cout << "History file: " << effectiveHistoryFilePath_.string() << std::endl;

    try {
        calculateTimestamps(); // Calculate based on command-line args
    } catch (const std::exception& e) {
        errorExit(std::string("Error calculating timestamps: ") + e.what());
    }

    // Check for interruption after potentially slow date parsing
    if (interrupted_) { std::cerr << "Interrupted after timestamp calculation.\n"; return; }

    std::cout << "Processing entries between: " << epochToString(startTimestamp_)
              << " and " << epochToString(endTimestamp_) << std::endl;

    if (dryRun_) {
        std::cout << "\n--- Dry Run Mode ---" << std::endl;
        if (!processHistory(std::cout)) { // Process and print to cout
             errorExit("Dry run failed during history processing.");
        }
        std::cout << "--- End Dry Run ---" << std::endl;
    } else {
        // Process history, writing kept entries to temp file
        std::ofstream null_stream; // Effectively /dev/null for processHistory output
        null_stream.setstate(std::ios_base::badbit); // Ensure it's not writable

        if (!processHistory(null_stream)) {
             errorExit("Failed to process history file.");
        }

        // Check for interruption after processing
        if (interrupted_) { std::cerr << "Interrupted after processing history.\n"; return; }

        std::cout << "History cleaning complete." << std::endl;
    }
}

void HistoryCleaner::checkPermissions() {
    // Check for interruption
    if (interrupted_) { throw std::runtime_error("Interrupted during permission check."); }

    std::error_code ec;
    bool needNewPath = false;

    do {
        fs::path parentDir = effectiveHistoryFilePath_.parent_path();
        needNewPath = false;

        // 1. Check parent directory existence and permissions first
        if (!fs::exists(parentDir, ec)) {
            if (!interactive_) {
                errorExit("History file directory does not exist: " + parentDir.string());
            }
            std::cerr << "Error: History file directory does not exist: " << parentDir.string() << std::endl;
            needNewPath = true;
        }
        if (ec) {
            if (!interactive_) {
                errorExit("Error checking existence of history file directory: " + parentDir.string() + " (" + ec.message() + ")");
            }
            std::cerr << "Error checking directory: " << ec.message() << std::endl;
            needNewPath = true;
        }
        if (!fs::is_directory(parentDir, ec)) {
            if (!interactive_) {
                errorExit("Path containing history file is not a directory: " + parentDir.string());
            }
            std::cerr << "Error: Not a directory: " << parentDir.string() << std::endl;
            needNewPath = true;
        }
        if (ec) {
            if (!interactive_) {
                errorExit("Error checking if parent path is a directory: " + parentDir.string() + " (" + ec.message() + ")");
            }
            std::cerr << "Error checking directory: " << ec.message() << std::endl;
            needNewPath = true;
        }

        // Check write permission in the directory using access()
        if (access(parentDir.c_str(), W_OK) != 0) {
            if (!interactive_) {
                errorExit("Cannot write to history file directory (check permissions): " + parentDir.string() + " (" + strerror(errno) + ")");
            }
            std::cerr << "Error: Cannot write to directory: " << parentDir.string() << std::endl;
            needNewPath = true;
        }

        // 2. Check history file itself (if it exists)
        bool exists = fs::exists(effectiveHistoryFilePath_, ec);
        if (ec) {
            if (!interactive_) {
                errorExit("Error checking existence of history file: " + effectiveHistoryFilePath_.string() + " (" + ec.message() + ")");
            }
            std::cerr << "Error checking file: " << ec.message() << std::endl;
            needNewPath = true;
        }

        if (exists) {
            // Check it's a regular file (or symlink to one eventually)
            auto status = fs::status(effectiveHistoryFilePath_, ec);
            if (ec) {
                if (!interactive_) {
                    errorExit("Error getting status of history file: " + effectiveHistoryFilePath_.string() + " (" + ec.message() + ")");
                }
                std::cerr << "Error checking file status: " << ec.message() << std::endl;
                needNewPath = true;
            }
            if (!fs::is_regular_file(status)) {
                if (!interactive_) {
                    errorExit("History file path exists but is not a regular file: " + effectiveHistoryFilePath_.string());
                }
                std::cerr << "Error: Not a regular file: " << effectiveHistoryFilePath_.string() << std::endl;
                needNewPath = true;
            }

            // Check read permission on the history file itself using access()
            if (access(effectiveHistoryFilePath_.c_str(), R_OK) != 0) {
                if (!interactive_) {
                    errorExit("Cannot read history file (check permissions): " + effectiveHistoryFilePath_.string() + " (" + strerror(errno) + ")");
                }
                std::cerr << "Error: Cannot read file: " << effectiveHistoryFilePath_.string() << std::endl;
                needNewPath = true;
            }
            // Check write permission on the file itself using access()
            if (access(effectiveHistoryFilePath_.c_str(), W_OK) != 0) {
                if (!interactive_) {
                    errorExit("Cannot write to history file (check permissions): " + effectiveHistoryFilePath_.string() + " (" + strerror(errno) + ")");
                }
                std::cerr << "Error: Cannot write to file: " << effectiveHistoryFilePath_.string() << std::endl;
                needNewPath = true;
            }
        } else {
            std::cout << "Info: History file does not exist: " << effectiveHistoryFilePath_.string() << ". Will be created." << std::endl;
            // If it doesn't exist, directory write permission (checked above) is sufficient.
        }

        if (needNewPath && interactive_) {
            std::cout << "\n❓ Please enter a new history file path: ";
            std::string newPath;
            if (!std::getline(std::cin, newPath)) {
                errorExit("Input error or EOF detected during path input.");
            }
            if (interrupted_) {
                throw std::runtime_error("Interrupted during path input.");
            }

            // Trim whitespace
            newPath.erase(0, newPath.find_first_not_of(" \t"));
            newPath.erase(newPath.find_last_not_of(" \t") + 1);

            if (newPath.empty()) {
                std::cout << "⚠️ Path cannot be empty. Please try again." << std::endl;
                needNewPath = true;
                continue;
            }

            // Update the path and resolve it
            historyFilePath_ = newPath;
            resolveHistoryPath();
        }
    } while (needNewPath && interactive_);

    if (needNewPath && !interactive_) {
        errorExit("Cannot proceed with invalid history file path in non-interactive mode.");
    }
}

void HistoryCleaner::calculateTimestamps() {
    // Calculate timestamps based on mode
    std::time_t now = nowEpoch();
    std::tm timeinfo;
    localtime_r(&now, &timeinfo);

    switch (mode_) {
        case Mode::TODAY:
            // Set start to beginning of today
            timeinfo.tm_hour = 0;
            timeinfo.tm_min = 0;
            timeinfo.tm_sec = 0;
            startTimestamp_ = std::mktime(&timeinfo);
            endTimestamp_ = std::numeric_limits<std::time_t>::max();
            break;

        case Mode::LAST_7_DAYS:
            // Set start to 7 days ago
            startTimestamp_ = now - (7 * 24 * 60 * 60);
            endTimestamp_ = std::numeric_limits<std::time_t>::max();
            break;

        case Mode::LAST_30_DAYS:
            // Set start to 30 days ago
            startTimestamp_ = now - (30 * 24 * 60 * 60);
            endTimestamp_ = std::numeric_limits<std::time_t>::max();
            break;

        case Mode::SPECIFIC_DAY:
            startTimestamp_ = dateToEpoch(specificDateStr_, preciseTime_);
            if (preciseTime_) {
                endTimestamp_ = startTimestamp_; // Exact time match
            } else {
                // End at end of the day
                std::tm date_tm;
                localtime_r(&startTimestamp_, &date_tm);
                date_tm.tm_hour = 23;
                date_tm.tm_min = 59;
                date_tm.tm_sec = 59;
                endTimestamp_ = std::mktime(&date_tm);
            }
            break;

        case Mode::BETWEEN:
            startTimestamp_ = dateToEpoch(startDateStr_, preciseTime_);
            endTimestamp_ = dateToEpoch(endDateStr_, preciseTime_);
            if (!preciseTime_) {
                // Adjust end to end of day
                std::tm end_tm;
                localtime_r(&endTimestamp_, &end_tm);
                end_tm.tm_hour = 23;
                end_tm.tm_min = 59;
                end_tm.tm_sec = 59;
                endTimestamp_ = std::mktime(&end_tm);
            }
            break;

        case Mode::BEFORE:
            startTimestamp_ = 0;
            endTimestamp_ = dateToEpoch(specificDateStr_, preciseTime_);
            if (!preciseTime_) {
                // Adjust to end of previous day
                endTimestamp_ -= 1; // One second before midnight
            }
            break;

        case Mode::AFTER:
            startTimestamp_ = dateToEpoch(specificDateStr_, preciseTime_);
            endTimestamp_ = std::numeric_limits<std::time_t>::max();
            break;

        case Mode::OLDER_THAN:
            startTimestamp_ = 0;
            endTimestamp_ = now - (olderThanDays_ * 24 * 60 * 60);
            break;

        case Mode::NEWER_THAN:
            startTimestamp_ = now - (olderThanDays_ * 24 * 60 * 60);
            endTimestamp_ = std::numeric_limits<std::time_t>::max();
            break;

        case Mode::ALL_TIME:
            startTimestamp_ = 0;
            endTimestamp_ = std::numeric_limits<std::time_t>::max();
            break;

        case Mode::NONE:
            throw std::runtime_error("Mode not set before calculating timestamps");
    }
}

void HistoryCleaner::resolveHistoryPath() {
    // Check for interruption
    if (interrupted_) { throw std::runtime_error("Interrupted during path resolution."); }

    std::error_code ec;
    fs::path initialPath = historyFilePath_; // Keep original for messages

    // Try to get canonical path (resolves symlinks, makes absolute, requires existence of intermediate dirs)
    // Use weakly_canonical first as the file itself might not exist yet.
    effectiveHistoryFilePath_ = fs::weakly_canonical(historyFilePath_, ec);

    if (ec) {
        // If weakly_canonical fails, try absolute path (doesn't require existence)
        ec.clear(); // Clear previous error
        effectiveHistoryFilePath_ = fs::absolute(historyFilePath_, ec);
        if (ec) {
            // If absolute also fails (e.g., path is syntactically invalid), use the provided path
            // Further checks (exists, permissions) will happen in checkPermissions().
            effectiveHistoryFilePath_ = initialPath; // Revert to original input path
            std::cerr << "Warning: Could not resolve path for '" << initialPath.string()
                     << "'. Using provided path directly. Error: " << ec.message() << std::endl;
        } else {
            // Absolute path worked, but canonical didn't (e.g., non-existent component)
            std::cerr << "Warning: Could not get canonical path for '" << initialPath.string()
                     << "'. Using absolute path: " << effectiveHistoryFilePath_.string()
                     << ". Check path validity." << std::endl;
        }
    }
}

void HistoryCleaner::parseArguments(int argc, char* argv[]) {
    // Determine default history file path ($HISTFILE or fallback)
    std::string histfilePathStr = getEnvVar("HISTFILE", "");
    if (histfilePathStr.empty()) {
        // Use HOME env var if available, otherwise assume current dir (less ideal)
        std::string homeDir = getEnvVar("HOME", "");
        if (homeDir.empty()) {
            std::cerr << "Warning: Cannot determine home directory (HOME not set). Using relative path '.zsh_history'." << std::endl;
            histfilePathStr = ".zsh_history";
        } else {
            histfilePathStr = homeDir + "/.zsh_history";
        }
    }
    historyFilePath_ = histfilePathStr;

    // Track if any mode-affecting arguments were provided
    bool hasNonHistfileArgs = false;
    // Start in interactive mode unless changed by mode-affecting arguments
    interactive_ = true;

    std::vector<std::string> args(argv + 1, argv + argc);

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
        } else if (arg == "--mode") {
            if (i + 1 >= args.size()) errorExit("--mode requires an argument.");
            std::string modeStr = args[++i];
            if (modeStr == "today") mode_ = Mode::TODAY;
            else if (modeStr == "last_7_days") mode_ = Mode::LAST_7_DAYS;
            else if (modeStr == "last_30_days") mode_ = Mode::LAST_30_DAYS;
            else if (modeStr == "between") mode_ = Mode::BETWEEN;
            else if (modeStr == "specific_day") mode_ = Mode::SPECIFIC_DAY;
            else if (modeStr == "before") mode_ = Mode::BEFORE;
            else if (modeStr == "after") mode_ = Mode::AFTER;
            else if (modeStr == "all") mode_ = Mode::ALL_TIME;
            else if (modeStr == "older_than") mode_ = Mode::OLDER_THAN;
            else if (modeStr == "newer_than") mode_ = Mode::NEWER_THAN;
            else errorExit("Invalid mode: '" + modeStr + "'. Use -h for options.");
            hasNonHistfileArgs = true;
        } else if (arg == "--precise") {
            preciseTime_ = true;
        } else if (arg == "--start-date") {
            if (i + 1 >= args.size()) errorExit("--start-date requires a DATE argument.");
            startDateStr_ = args[++i];
            hasNonHistfileArgs = true;
        } else if (arg == "--end-date") {
            if (i + 1 >= args.size()) errorExit("--end-date requires a DATE argument.");
            endDateStr_ = args[++i];
            hasNonHistfileArgs = true;
        } else if (arg == "--date") {
            if (i + 1 >= args.size()) errorExit("--date requires a DATE argument.");
            specificDateStr_ = args[++i];
            hasNonHistfileArgs = true;
        } else if (arg == "--backup") {
            doBackup_ = true;
            hasNonHistfileArgs = true;
        } else if (arg == "--dry-run") {
            dryRun_ = true;
            hasNonHistfileArgs = true;
        } else if (arg == "--histfile") {
            if (i + 1 >= args.size()) errorExit("--histfile requires a PATH argument.");
            historyFilePath_ = args[++i];
            // Don't set interactive_ = false here, allow --histfile alone to work in interactive mode
        } else if (arg == "--keyword") {
            if (i + 1 >= args.size()) errorExit("--keyword requires one or more STRING arguments.");
            while (i + 1 < args.size() && args[i + 1][0] != '-') {
                filterKeywords_.push_back(args[++i]);
            }
            hasNonHistfileArgs = true;
        } else if (arg == "--regex") {
            if (i + 1 >= args.size()) errorExit("--regex requires one or more PATTERN arguments.");
            while (i + 1 < args.size() && args[i + 1][0] != '-') {
                std::string regexStr = args[++i];
                try {
                    std::regex compiled(regexStr, std::regex::ECMAScript);
                    filterRegexStrs_.push_back(regexStr);
                    filterRegexes_.push_back(compiled);
                } catch (const std::regex_error& e) {
                    errorExit(std::string("Invalid regex pattern provided to --regex: ") + e.what());
                }
            }
            hasNonHistfileArgs = true;
        } else if (arg == "--days") {
            if (i + 1 >= args.size()) errorExit("--days requires a positive integer argument.");
            std::string daysStr = args[++i];
            try {
                olderThanDays_ = std::stoi(daysStr);
                if (olderThanDays_ <= 0) {
                    errorExit("--days requires a positive integer.");
                }
            } catch (...) {
                errorExit("Invalid number provided for --days: '" + daysStr + "'.");
            }
            hasNonHistfileArgs = true;
        } else if (arg == "--passes") {
            if (i + 1 >= args.size()) errorExit("--passes requires a positive integer argument.");
            std::string passesStr = args[++i];
            try {
                int passes = std::stoi(passesStr);
                if (passes <= 0) {
                    errorExit("--passes requires a positive integer.");
                }
                shredPasses_ = passes;
            } catch (...) {
                errorExit("Invalid number provided for --passes: '" + passesStr + "'.");
            }
            hasNonHistfileArgs = true;
        } else {
            errorExit("Unknown option: '" + arg + "'. Use -h or --help for usage.");
        }
    }

    // Set interactive mode based on arguments
    interactive_ = !hasNonHistfileArgs;

    // Validation for non-interactive mode
    if (hasNonHistfileArgs) {
        if (mode_ == Mode::NONE) {
            errorExit("The --mode option is required when running non-interactively. Use -h for options.");
        }

        // Validate date arguments based on mode
        switch (mode_) {
            case Mode::BETWEEN:
                if (startDateStr_.empty() || endDateStr_.empty()) {
                    errorExit("--start-date and --end-date are required for 'between' mode.");
                }
                break;
            case Mode::SPECIFIC_DAY:
            case Mode::BEFORE:
            case Mode::AFTER:
                if (specificDateStr_.empty()) {
                    errorExit("--date is required for 'specific_day', 'before', or 'after' mode.");
                }
                break;
            case Mode::OLDER_THAN:
                if (olderThanDays_ <= 0) {
                    errorExit("--days <positive_integer> is required for 'older_than' mode.");
                }
                if (!startDateStr_.empty() || !endDateStr_.empty() || !specificDateStr_.empty() || preciseTime_) {
                    errorExit("'older_than' mode cannot be used with --start-date, --end-date, --date, or --precise.");
                }
                break;
            default: // TODAY, LAST_7_DAYS, LAST_30_DAYS, ALL_TIME
                if (!startDateStr_.empty() || !endDateStr_.empty() || !specificDateStr_.empty() || olderThanDays_ > 0) {
                    std::cerr << "Warning: Date/days arguments are ignored for the selected mode." << std::endl;
                }
                if (preciseTime_) {
                    std::cerr << "Warning: --precise flag is ignored for the selected mode." << std::endl;
                }
                break;
        }

        // Dry run implies no backup needed
        if (dryRun_ && doBackup_) {
            std::cout << "Info: --backup option ignored when --dry-run is specified." << std::endl;
            doBackup_ = false;
        }
    }
}

void HistoryCleaner::runInteractive() {
    // Note: Precise time input is complex for interactive mode, so it's disabled if --precise is used.
    if (preciseTime_) {
        errorExit("Interactive mode is not supported with the --precise flag. Please provide all arguments on the command line.");
    }

    std::cout << "\n✨ Welcome to the Zsh History Cleaner ✨" << std::endl;
    std::cout << "---------------------------------------" << std::endl;
    std::cout << "⚙️ History File: " << effectiveHistoryFilePath_.string() << std::endl;

    // --- Select Mode ---
    std::vector<std::pair<std::string, Mode>> options = {
        {"Today", Mode::TODAY},
        {"Last 7 Days", Mode::LAST_7_DAYS},
        {"Last 30 Days", Mode::LAST_30_DAYS},
        {"Specific Day", Mode::SPECIFIC_DAY},
        {"Date Range (Between)", Mode::BETWEEN},
        {"Everything Before a Date", Mode::BEFORE},
        {"Everything After a Date", Mode::AFTER},
        {"Older Than X Days", Mode::OLDER_THAN},
        {"Newer Than X Days", Mode::NEWER_THAN},
        {"All Time", Mode::ALL_TIME},
        {"Quit", Mode::NONE}
    };

    std::cout << "\n❓ Please choose a cleaning mode:" << std::endl;
    for (size_t i = 0; i < options.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << options[i].first << std::endl;
    }

    int choice = 0;
    int max_choice = static_cast<int>(options.size());
    while (choice < 1 || choice > max_choice) {
        if (interrupted_) { std::cerr << "\nInterrupted during interactive input.\n"; return; }

        std::cout << "\nEnter choice (1-" << max_choice << "): ";
        std::string choiceStr;
        if (!std::getline(std::cin, choiceStr)) {
            std::cerr << "\nInput error or EOF detected. Exiting." << std::endl;
            return;
        }

        if (interrupted_) { std::cerr << "\nInterrupted during interactive input.\n"; return; }

        try {
            choiceStr.erase(0, choiceStr.find_first_not_of(" \t"));
            choiceStr.erase(choiceStr.find_last_not_of(" \t") + 1);
            if (!choiceStr.empty()) {
                choice = std::stoi(choiceStr);
            }
        } catch (...) {
            choice = 0;
        }

        if (choice < 1 || choice > max_choice) {
            std::cout << "Invalid choice. Please enter a number between 1 and " << max_choice << "." << std::endl;
            choice = 0;
        }
    }

    mode_ = options[choice - 1].second;

    if (mode_ == Mode::NONE) {
        std::cout << "\n👋 Exiting. No changes made." << std::endl;
        return;
    }

    // --- Get Date Inputs if Needed ---
    auto getDateInput = [&](const std::string& prompt, bool isSpecificDay = false) -> std::string {
        std::string inputStr;
        while (true) {
            if (interrupted_) throw std::runtime_error("Interrupted during date input.");
            
            // For specific day mode, we don't need time input as it covers the whole day
            if (isSpecificDay) {
                std::cout << prompt << " (YYYY-MM-DD): ";
            } else {
                std::cout << prompt << " (YYYY-MM-DD [HH:MM:SS optional]): ";
            }
            
            if (!std::getline(std::cin, inputStr)) {
                throw std::runtime_error("Input error or EOF detected during date input.");
            }
            if (interrupted_) throw std::runtime_error("Interrupted during date input.");

            inputStr.erase(0, inputStr.find_first_not_of(" \t"));
            inputStr.erase(inputStr.find_last_not_of(" \t") + 1);

            if (inputStr.empty()) {
                std::cout << "⚠️ Input cannot be empty." << std::endl;
                continue;
            }

            try {
                dateToEpoch(inputStr, false);
                return inputStr;
            } catch (const std::exception& e) {
                std::cout << "⚠️ Invalid format or value: " << e.what() << std::endl;
            }
        }
    };

    try {
        if (mode_ == Mode::BETWEEN) {
            startDateStr_ = getDateInput("❓ Enter Start Date", false);
            endDateStr_ = getDateInput("❓ Enter End Date", false);
        } else if (mode_ == Mode::SPECIFIC_DAY) {
            specificDateStr_ = getDateInput("❓ Enter Date", true);
        } else if (mode_ == Mode::BEFORE || mode_ == Mode::AFTER) {
            specificDateStr_ = getDateInput("❓ Enter Date", false);
        } else if (mode_ == Mode::OLDER_THAN || mode_ == Mode::NEWER_THAN) {
            while (olderThanDays_ <= 0) {
                if (interrupted_) throw std::runtime_error("Interrupted during days input.");
                std::string prompt = mode_ == Mode::OLDER_THAN ?
                    "❓ Enter number of days (e.g., 90 to delete entries older than 90 days): " :
                    "❓ Enter number of days (e.g., 90 to delete entries newer than 90 days): ";
                std::cout << prompt;
                std::string daysStr;
                if (!std::getline(std::cin, daysStr)) {
                    throw std::runtime_error("Input error or EOF detected during days input.");
                }
                if (interrupted_) throw std::runtime_error("Interrupted during days input.");
                try {
                    daysStr.erase(0, daysStr.find_first_not_of(" \t"));
                    daysStr.erase(daysStr.find_last_not_of(" \t") + 1);
                    if (!daysStr.empty()) {
                        olderThanDays_ = std::stoi(daysStr);
                        if (olderThanDays_ <= 0) {
                            std::cout << "⚠️ Number of days must be positive." << std::endl;
                            olderThanDays_ = -1;
                        }
                    }
                } catch (...) {
                    olderThanDays_ = -1;
                    std::cout << "⚠️ Please enter a valid positive number." << std::endl;
                }
            }
        }
    } catch (const std::runtime_error& e) {
        std::cerr << "\n" << e.what() << std::endl;
        return;
    }

    // --- Get Filter Input ---
    char filterChoice = 'n';
    try {
        std::cout << "\n❓ Add content filter? (k=Keyword, r=Regex, [N]o): ";
        std::string filterChoiceStr;
        if (!std::getline(std::cin, filterChoiceStr)) {
            throw std::runtime_error("Input error or EOF detected during filter choice.");
        }
        if (interrupted_) throw std::runtime_error("Interrupted during filter choice.");
        if (!filterChoiceStr.empty()) {
            filterChoiceStr.erase(0, filterChoiceStr.find_first_not_of(" \t"));
            if (!filterChoiceStr.empty()) {
                filterChoice = std::tolower(filterChoiceStr[0]);
            }
        }

        if (filterChoice == 'k') {
            std::cout << "❓ Enter keyword to filter by: ";
            std::string keyword;
            if (!std::getline(std::cin, keyword)) {
                throw std::runtime_error("Input error or EOF detected during keyword input.");
            }
            if (interrupted_) throw std::runtime_error("Interrupted during keyword input.");
            keyword.erase(0, keyword.find_first_not_of(" \t"));
            keyword.erase(keyword.find_last_not_of(" \t") + 1);
            if (keyword.empty()) {
                std::cout << "⚠️ Keyword cannot be empty. No filter applied." << std::endl;
            } else {
                filterKeywords_.push_back(keyword);
                std::cout << "   Keyword added: '" << keyword << "'" << std::endl;
                
                char addMore = 'n';
                std::cout << "❓ Add another keyword? (y/[N]): ";
                std::string moreInput;
                if (std::getline(std::cin, moreInput) && !moreInput.empty()) {
                    addMore = std::tolower(moreInput[0]);
                }
                
                while (addMore == 'y') {
                    std::cout << "❓ Enter additional keyword: ";
                    std::string additionalKeyword;
                    if (!std::getline(std::cin, additionalKeyword)) {
                        break;
                    }
                    if (interrupted_) throw std::runtime_error("Interrupted during keyword input.");
                    
                    additionalKeyword.erase(0, additionalKeyword.find_first_not_of(" \t"));
                    additionalKeyword.erase(additionalKeyword.find_last_not_of(" \t") + 1);
                    
                    if (!additionalKeyword.empty()) {
                        filterKeywords_.push_back(additionalKeyword);
                        std::cout << "   Keyword added: '" << additionalKeyword << "'" << std::endl;
                    } else {
                        std::cout << "⚠️ Keyword cannot be empty. Skipped." << std::endl;
                    }
                    
                    addMore = 'n';
                    std::cout << "❓ Add another keyword? (y/[N]): ";
                    if (std::getline(std::cin, moreInput) && !moreInput.empty()) {
                        addMore = std::tolower(moreInput[0]);
                    }
                }
            }
        } else if (filterChoice == 'r') {
            std::cout << "❓ Enter regex pattern (ECMAScript syntax): ";
            std::string regexStr;
            if (!std::getline(std::cin, regexStr)) {
                throw std::runtime_error("Input error or EOF detected during regex input.");
            }
            if (interrupted_) throw std::runtime_error("Interrupted during regex input.");
            
            regexStr.erase(0, regexStr.find_first_not_of(" \t"));
            regexStr.erase(regexStr.find_last_not_of(" \t") + 1);

            if (regexStr.empty()) {
                std::cout << "⚠️ Regex pattern cannot be empty. No filter applied." << std::endl;
            } else {
                try {
                    std::regex compiled(regexStr, std::regex::ECMAScript);
                    filterRegexStrs_.push_back(regexStr);
                    filterRegexes_.push_back(compiled);
                    std::cout << "   Regex compiled successfully: /" << regexStr << "/" << std::endl;
                    
                    char addMore = 'n';
                    std::cout << "❓ Add another regex pattern? (y/[N]): ";
                    std::string moreInput;
                    if (std::getline(std::cin, moreInput) && !moreInput.empty()) {
                        addMore = std::tolower(moreInput[0]);
                    }
                    
                    while (addMore == 'y') {
                        std::cout << "❓ Enter additional regex pattern: ";
                        std::string additionalRegex;
                        if (!std::getline(std::cin, additionalRegex)) {
                            break;
                        }
                        if (interrupted_) throw std::runtime_error("Interrupted during regex input.");
                        
                        additionalRegex.erase(0, additionalRegex.find_first_not_of(" \t"));
                        additionalRegex.erase(additionalRegex.find_last_not_of(" \t") + 1);
                        
                        if (!additionalRegex.empty()) {
                            try {
                                std::regex additionalCompiled(additionalRegex, std::regex::ECMAScript);
                                filterRegexStrs_.push_back(additionalRegex);
                                filterRegexes_.push_back(additionalCompiled);
                                std::cout << "   Regex compiled successfully: /" << additionalRegex << "/" << std::endl;
                            } catch (const std::regex_error& e) {
                                std::cerr << "⚠️ Invalid regex pattern: " << e.what() << ". Skipped." << std::endl;
                            }
                        } else {
                            std::cout << "⚠️ Regex pattern cannot be empty. Skipped." << std::endl;
                        }
                        
                        addMore = 'n';
                        std::cout << "❓ Add another regex pattern? (y/[N]): ";
                        if (std::getline(std::cin, moreInput) && !moreInput.empty()) {
                            addMore = std::tolower(moreInput[0]);
                        }
                    }
                } catch (const std::regex_error& e) {
                    std::cerr << "⚠️ Invalid regex pattern: " << e.what() << ". No filter applied." << std::endl;
                }
            }
        }
    } catch (const std::runtime_error& e) {
        std::cerr << "\n" << e.what() << std::endl;
        return;
    }

    // --- Get Backup Preference ---
    std::cout << "\n❓ Create backup before cleaning? (y/[N]): ";
    std::string backupStr;
    if (std::getline(std::cin, backupStr) && !backupStr.empty()) {
        char backupChoice = std::tolower(backupStr[0]);
        doBackup_ = (backupChoice == 'y');
    }

    // --- Get Dry Run Preference ---
    std::cout << "❓ Perform dry run (no changes made)? (y/[N]): ";
    std::string dryRunStr;
    if (std::getline(std::cin, dryRunStr) && !dryRunStr.empty()) {
        char dryRunChoice = std::tolower(dryRunStr[0]);
        dryRun_ = (dryRunChoice == 'y');
    } else {
        dryRun_ = false; // Default to no for dry run
    }

    // --- Get Passes Preference (if not dry run) ---
    if (!dryRun_) {
        std::cout << "❓ Number of secure deletion passes? [32]: ";
        std::string passesStr;
        if (std::getline(std::cin, passesStr) && !passesStr.empty()) {
            try {
                int passes = std::stoi(passesStr);
                if (passes <= 0) {
                    std::cout << "⚠️ Invalid number of passes. Using default (32)." << std::endl;
                } else {
                    shredPasses_ = passes;
                }
            } catch (...) {
                std::cout << "⚠️ Invalid input. Using default (32)." << std::endl;
            }
        }

        // --- Get Final Confirmation ---
        std::cout << "\n⚠️ Are you sure you want to proceed with deletion? This cannot be undone! (y/[N]): ";
        std::string confirmStr;
        if (!std::getline(std::cin, confirmStr) || confirmStr.empty() ||
            std::tolower(confirmStr[0]) != 'y') {
            std::cout << "Operation cancelled." << std::endl;
            return;
        }
    }

    // --- Calculate Timestamps ---
    try {
        calculateTimestamps();
    } catch (const std::exception& e) {
        errorExit(std::string("Error calculating timestamps: ") + e.what());
    }

    // --- Process History ---
    std::cout << "\nProcessing entries between: " << epochToString(startTimestamp_)
              << " and " << epochToString(endTimestamp_) << std::endl;

    if (dryRun_) {
        std::cout << "\n--- Dry Run Mode ---" << std::endl;
        if (!processHistory(std::cout)) {
            errorExit("Dry run failed during history processing.");
        }
        std::cout << "--- End Dry Run ---" << std::endl;
    } else {
        std::ofstream null_stream;
        null_stream.setstate(std::ios_base::badbit);

        if (!processHistory(null_stream)) {
            errorExit("Failed to process history file.");
        }

        if (interrupted_) { std::cerr << "Interrupted after processing history.\n"; return; }

        std::cout << "History cleaning complete." << std::endl;
    }
}

bool HistoryCleaner::processCommandBlock(const std::string& block, unsigned long long lineNum,
                                       std::ostream& output, unsigned long long& keptCount,
                                       unsigned long long& deletedCount) {
    std::smatch match;
    
    // Extract timestamp from the first line of the block
    size_t firstLineEnd = block.find('\n');
    if (firstLineEnd == std::string::npos) {
        std::cerr << "Warning: Malformed history entry near line " << lineNum << ". Keeping block." << std::endl;
        keptCount++;
        return false;
    }

    std::string firstLine = block.substr(0, firstLineEnd);
    if (!std::regex_match(firstLine, match, historyEntryRegex_)) {
        std::cerr << "Warning: Invalid history entry format near line " << lineNum << ". Keeping block." << std::endl;
        keptCount++;
        return false;
    }

    try {
        std::time_t timestamp = std::stoll(match[1].str());
        if (timestamp >= startTimestamp_ && timestamp <= endTimestamp_) {
            // Time matches, now check content filters (if any)
            bool shouldDelete = false; // Don't delete unless filters match

            // Extract command part for both keyword and regex matching
            size_t cmdStart = firstLine.find(';');
            if (cmdStart != std::string::npos) {
                std::string command = firstLine.substr(cmdStart + 1);
                command.erase(0, command.find_first_not_of(" \t"));

                // If no filters are present, we should delete based on time only
                if (filterKeywords_.empty() && filterRegexes_.empty()) {
                    shouldDelete = true;
                } else {
                    // Check keywords (ANY keyword must match)
                    if (!filterKeywords_.empty()) {
                        for (const auto& keyword : filterKeywords_) {
                            if (command.find(keyword) != std::string::npos) {
                                shouldDelete = true;
                                break;
                            }
                        }
                    }

                    // Check regexes (ANY regex must match)
                    if (!filterRegexes_.empty() && !shouldDelete) { // Only check if not already marked for deletion
                        for (const auto& regex : filterRegexes_) {
                            if (std::regex_search(command, regex)) {
                                shouldDelete = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (shouldDelete) {
                deletedCount++;
                if (dryRun_) {
                    output << "--- Would delete (Entry ending line " << lineNum << "): ---\n"
                           << block
                           << "-------------------------------------------\n";
                }
                return true;
            }
        }
    } catch (const std::out_of_range& oor) {
        std::cerr << "Warning: Timestamp out of range near line " << lineNum << ". Keeping entry." << std::endl;
        keptCount++;
        return false;
    } catch (const std::invalid_argument& ia) {
        std::cerr << "Warning: Invalid timestamp format near line " << lineNum << ". Keeping entry." << std::endl;
        keptCount++;
        return false;
    }

    keptCount++;
    return false;
}

bool HistoryCleaner::processHistory(std::ostream& output) {
    // Check for interruption before opening files
    if (interrupted_) { std::cerr << "Interrupted before processing history.\n"; return false; }

    // First pass: Read and identify entries to keep
    std::vector<std::string> keptEntries;
    std::ifstream historyIn(effectiveHistoryFilePath_, std::ios::binary);
    if (!historyIn && fs::exists(effectiveHistoryFilePath_)) {
        std::cerr << "Error: Cannot open history file for reading: " << effectiveHistoryFilePath_.string() << std::endl;
        return false;
    }

    std::string line;
    unsigned long long lineNum = 0;
    unsigned long long keptCount = 0;
    unsigned long long deletedCount = 0;
    std::string currentCommandBlock; // Stores the lines of the current history entry

    while (true) {
        // Check for interruption in the loop
        if (interrupted_) { std::cerr << "\nInterrupted during history processing.\n"; return false; }

        if (!std::getline(historyIn, line)) {
            // End of file or read error
            if (historyIn.eof()) {
                break; // Normal end of file
            } else {
                std::cerr << "Error reading history file near line " << lineNum << std::endl;
                return false; // File read error
            }
        }

        lineNum++;

        // Handle potential \r line endings (Windows/mixed environments)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Check if the line starts a new history entry
        std::smatch match;
        bool isNewEntry = std::regex_match(line, match, historyEntryRegex_);

        if (isNewEntry) {
            // --- Process the PREVIOUS command block ---
            if (!currentCommandBlock.empty()) {
                bool shouldDelete = processCommandBlock(currentCommandBlock, lineNum - 1, output, keptCount, deletedCount);
                if (!shouldDelete && !dryRun_) {
                    keptEntries.push_back(currentCommandBlock);
                }
            }
            // --- Start the NEW command block ---
            currentCommandBlock = line + '\n';
        } else {
            // --- Append to the CURRENT command block ---
            // This line is a continuation (multiline command) or part of a malformed entry
            if (!currentCommandBlock.empty()) { // Only append if we are inside a block
                currentCommandBlock += line + '\n';
            } else {
                // This line appears before the first valid timestamp entry
                // Treat it as a block to be kept (cannot determine its timestamp)
                std::cerr << "Warning: Line found before first valid history entry timestamp at line " << lineNum << ". Keeping line." << std::endl;
                if (!dryRun_) {
                    keptEntries.push_back(line + '\n');
                }
                keptCount++;
            }
        }
    }

    // Process the last block if any
    if (!currentCommandBlock.empty()) {
        bool shouldDelete = processCommandBlock(currentCommandBlock, lineNum, output, keptCount, deletedCount);
        if (!shouldDelete && !dryRun_) {
            keptEntries.push_back(currentCommandBlock);
        }
    }

    historyIn.close();

    std::cout << "Processing complete. Lines read: " << lineNum
              << ", Entries kept: " << keptCount
              << ", Entries " << (dryRun_ ? "to be deleted" : "deleted") << ": " << deletedCount << std::endl;

    if (dryRun_) {
        return true;
    }

    // Generate random filename for the new history file
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 35); // 0-9, A-Z
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string randomStr;
    for (int i = 0; i < 15; ++i) {
        randomStr += charset[dist(gen)];
    }
    tempFilePath_ = effectiveHistoryFilePath_.parent_path() / randomStr;

    // Write kept entries to the new file
    std::ofstream newFile(tempFilePath_, std::ios::binary);
    if (!newFile) {
        std::cerr << "Error: Cannot create new history file" << std::endl;
        return false;
    }

    for (const auto& entry : keptEntries) {
        newFile << entry;
        if (!newFile) {
            std::cerr << "Error: Failed to write to new history file" << std::endl;
            newFile.close();
            cleanup();  // This will handle removing the temp file
            return false;
        }
    }

    newFile.close();

    // Now we can safely replace the original file
    if (!performCleanup(output)) {
        cleanup();  // This will handle removing the temp file
        return false;
    }

    // Rename new file to original name
    std::error_code ec;
    fs::rename(tempFilePath_, effectiveHistoryFilePath_, ec);
    if (ec) {
        std::cerr << "Error: Failed to rename new history file" << std::endl;
        cleanup();  // This will handle removing the temp file
        return false;
    }
    tempFilePath_.clear();  // Successfully renamed, clear the path

    return true;
}

bool HistoryCleaner::backupHistoryFile() {
    // Check for interruption
    if (interrupted_) { std::cerr << "Interrupted before backup.\n"; return false; }

    // Generate random filename for backup
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 35); // 0-9, A-Z
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string randomStr;
    for (int i = 0; i < 15; ++i) {
        randomStr += charset[dist(gen)];
    }
    backupFilePath_ = effectiveHistoryFilePath_.parent_path() / (effectiveHistoryFilePath_.filename().string() + ".backup_" + randomStr);

    std::error_code ec;
    fs::copy_file(effectiveHistoryFilePath_, backupFilePath_, fs::copy_options::overwrite_existing, ec);

    if (ec) {
        std::cerr << "Error: Failed to create backup file: " << backupFilePath_.string()
                   << " (" << ec.message() << ")" << std::endl;
        backupFilePath_.clear();  // Clear the path since backup failed
        return false;
    }
    std::cout << "Backup created: " << backupFilePath_.string() << std::endl;
    return true;
}

bool HistoryCleaner::performCleanup(std::ostream& output) {
    // Check for interruption
    if (interrupted_) { std::cerr << "Interrupted before final cleanup steps.\n"; return false; }

    if (dryRun_) {
        output << "Dry run: No changes made." << std::endl;
        return true;
    }

    // 1. Backup original file if requested
    if (doBackup_) {
        if (!backupHistoryFile()) {
            std::cerr << "Backup failed. Aborting cleanup to preserve original file." << std::endl;
            return false;
        }
         // Check for interruption after backup
         if (interrupted_) { std::cerr << "Interrupted after backup.\n"; return false; }
    }

    // 2. Securely delete the original history file
    output << "Securely deleting original history file: " << effectiveHistoryFilePath_.string() << std::endl;
    if (!secureDelete(effectiveHistoryFilePath_, shredPasses_, std::cerr)) {
        std::cerr << "Error: Secure delete of original history file failed." << std::endl;
        std::cerr << "The original file might still exist (potentially overwritten or partially deleted)." << std::endl;
        return false;
    }
    output << "Original history file securely deleted." << std::endl;

    return true;
}

void HistoryCleaner::usage(const std::string& progName) {
    std::cout << "Usage: " << progName << " [OPTIONS]\n\n"
              << "✨ Securely cleans Zsh history file entries based on time criteria. ✨\n\n"
              << "Interactive Mode (default, no options provided):\n"
              << " Launches a menu to select the cleaning mode, dates (if applicable),\n"
              << " and confirms options like dry-run and backup before proceeding.\n\n"
              << "Options (disables interactive mode):\n"
              << " --mode <MODE>         Specify the cleaning mode. Required if not interactive.\n"
              << "                       Modes: today, last_7_days, last_30_days, specific_day,\n"
              << "                              between, before, after, older_than, newer_than, all\n"
              << " --start-date <DATE>   Start date for 'between' mode (YYYY-MM-DD). Inclusive.\n"
              << " --end-date <DATE>     End date for 'between' mode (YYYY-MM-DD). Inclusive.\n"
              << " --date <DATE>         Specific date for 'specific_day', 'before', or 'after' modes.\n"
              << "                       Format: YYYY-MM-DD\n"
              << " --days <N>           Number of days for 'older_than' mode (positive integer).\n"
              << " --precise            Use precise time input (HH:MM or HH:MM:SS) for dates.\n"
              << "                      Not available in interactive mode.\n"
              << " --keyword <STRING...> Delete only entries containing any of these exact strings.\n"
              << "                      Multiple keywords can be provided as separate arguments.\n"
              << "                      Cannot be used with --regex. Applies after time filtering.\n"
              << " --regex <PATTERN...>  Delete only entries matching any of these regex patterns.\n"
              << "                      Multiple patterns can be provided as separate arguments.\n"
              << "                      Cannot be used with --keyword. Applies after time filtering.\n"
              << " --backup             Create a backup of the original history file before deletion.\n"
              << "                      Ignored if --dry-run is used.\n"
              << " --dry-run            Simulate the process. Shows which entries would be deleted\n"
              << "                      without modifying the actual history file.\n"
              << " --histfile <PATH>    Specify a different history file path.\n"
              << "                      (Default: $HISTFILE env var, or $HOME/.zsh_history)\n"
              << " --passes <N>         Number of secure deletion passes (default: 32).\n"
              << " -h, --help           Show this help message and exit.\n\n"
              << "Examples:\n"
              << "  " << progName << "                     # Run in interactive mode\n"
              << "  " << progName << " --mode today --backup\n"
              << "  " << progName << " --mode specific_day --date 2024-03-15\n"
              << "  " << progName << " --mode between --start-date 2023-01-01 --end-date 2023-12-31 --dry-run\n"
              << "  " << progName << " --mode before --date 2024-01-01 --precise\n"
              << "  " << progName << " --mode after --date 2024-04-01 --backup\n"
              << "  " << progName << " --mode last_7_days --keyword \"sudo apt update\" \"sudo timeshift\"\n"
              << "  " << progName << " --mode today --regex \"git\\s+(commit|push)\" \"sudo\\s+-E\"\n"
              << "  " << progName << " --mode all --backup\n"
              << "  " << progName << " --mode older_than --days 90 --backup\n"
              << "  " << progName << " --mode newer_than --days 90 --backup\n\n"
              << "Notes:\n"
              << "- Date format is YYYY-MM-DD.\n"
              << "- Time format (with --precise) is HH:MM or HH:MM:SS.\n"
              << "- Time interpretation depends on the mode and --precise flag:\n"
              << "  'today', 'last_7_days', 'last_30_days': Based on current local time.\n"
              << "  'between', 'specific_day', 'before', 'after': Use start/end of day unless --precise.\n"
              << "  'before' deletes entries strictly *before* the specified time.\n"
              << "  'after': Deletes entries timestamped *from* the specified time onwards.\n"
              << "  'older_than': Deletes entries older than the specified number of days from now.\n"
              << "  'newer_than': Deletes entries newer than the specified number of days from now.\n"
              << "  'older_than': Deletes entries older than the specified number of days from now.\n"
              << "- Keyword/Regex filters apply *after* the time-based filtering.\n"
              << "- Uses a multi-pass overwrite (secure delete) for the original file.\n"
              << "  Effectiveness depends on filesystem, hardware, and OS behavior.\n"
              << "- Requires read/write permissions on the history file and write permissions\n"
              << "  in its directory.\n"
              << "- Interrupted operations (Ctrl+C, SIGTERM) attempt to set a flag for graceful\n"
              << "  shutdown, which includes cleaning up temporary files.\n";
    std::exit(0);
}