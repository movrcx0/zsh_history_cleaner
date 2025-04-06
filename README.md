# Zsh History Cleaner

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg)](https://github.com/movrcx0/zsh-history-cleaner)

A secure command-line tool for selectively cleaning Zsh shell history with advanced filtering capabilities and secure deletion.

## Table of Contents
- [Features](#features)
- [Installation](#installation)
  - [Prerequisites](#prerequisites)
  - [Building from Source](#building-from-source)
- [Usage](#usage)
  - [Interactive Mode](#interactive-mode)
  - [Command-line Mode](#command-line-mode)
  - [Options](#options)
- [Security Considerations](#security-considerations)
  - [Secure Deletion](#secure-deletion)
  - [Data Safety](#data-safety)
  - [Permissions](#permissions)
- [Error Handling](#error-handling)
- [Contributing](#contributing)
- [License](#license)
- [Acknowledgments](#acknowledgments)
- [Author](#author)
- [Version History](#version-history)
- [Support](#support)

## Features

- **Multiple Cleaning Modes**:
  - Today's entries
  - Last 7 days
  - Last 30 days
  - Specific day
  - Date range (between two dates)
  - Everything before a date
  - Everything after a date
  - Entries older than X days
  - Entries newer than X days
  - All time

- **Advanced Filtering**:
  - Time-based filtering with precise timestamp support
  - Keyword-based filtering (exact string matching)
  - Regular expression pattern matching
  - Combine time and content filters

- **Security Features**:
  - Secure multi-pass overwrite of deleted entries
  - Random filenames for temporary files
  - No multiple copies of sensitive data
  - Proper cleanup on interruption
  - Permission checks before operations

- **User Interface**:
  - Interactive mode with guided menu
  - Command-line mode for scripting
  - Dry-run option to preview changes
  - Backup creation option
  - Detailed progress feedback

## Project Structure

```
zsh-history-cleaner/
├── include/                   # Public headers
│   └── zsh_history_cleaner/  # Project headers
│       ├── Constants.h       # Constants and configurations
│       ├── HistoryCleaner.h  # Main cleaner interface
│       ├── SecureDelete.h    # Secure deletion utilities
│       └── Utils.h           # Common utilities
├── src/                      # Implementation files
│   ├── core/                # Core functionality
│   │   ├── HistoryCleaner.cpp
│   │   └── SecureDelete.cpp
│   ├── utils/              # Utility functions
│   │   └── Utils.cpp
│   └── main.cpp           # Main entry point
├── .gitignore
└── README.md
```

## Installation

### Prerequisites

- C++17 compatible compiler
- CMake 3.10 or higher
- POSIX-compliant operating system (Linux, macOS)
- Zsh shell (for the history file format)

### Building from Source

1. Clone the repository:
   ```bash
   git clone https://github.com/movrcx0/zsh-history-cleaner.git
   cd zsh-history-cleaner
   ```

2. Create a build directory and build using CMake:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```

3. (Optional) Install system-wide:
   ```bash
   sudo make install
   ```

   Or manually copy the binary:
   ```bash
   sudo cp zsh_history_cleaner /usr/local/bin/
   ```

Note: You can also build in debug mode with:
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```

## Testing the Build

After building with CMake, you can test the executable:

```bash
# From the build directory
./zsh_history_cleaner --help    # Test if the program runs and shows help

# Test with dry-run mode (safe testing, no actual changes)
./zsh_history_cleaner --mode today --dry-run

# Test with verbose output
make VERBOSE=1                  # See detailed compilation commands
```

Common issues and solutions:
- If CMake fails, check if you have CMake installed: `cmake --version`
- If compilation fails, verify you have a C++17 compatible compiler
- If the program doesn't run, ensure all source files are in the correct directories

## Usage

### Interactive Mode

Simply run the program without arguments for an interactive menu:
```bash
zsh_history_cleaner
```

You can also specify a custom history file while still using interactive mode:
```bash
zsh_history_cleaner --histfile /path/to/history/file
```

The interactive mode will guide you through:
1. Validating the history file path (with prompts for a new path if needed)
2. Selecting a cleaning mode
3. Entering dates if required
4. Adding optional content filters
5. Choosing backup and dry-run preferences

If the default history file cannot be found or accessed, the interactive mode will:
- Display the specific error (permissions, non-existent directory, etc.)
- Prompt for a new file path
- Validate the new path before proceeding
- Continue prompting until a valid path is provided or the program is interrupted

### Command-line Mode

For scripting or direct usage, provide options on the command line:

```bash
# Delete today's entries (with backup)
zsh_history_cleaner --mode today --backup

# Delete entries from a specific day
zsh_history_cleaner --mode specific_day --date 2024-03-15

# Delete entries between dates (dry run)
zsh_history_cleaner --mode between --start-date 2023-01-01 --end-date 2023-12-31 --dry-run

# Delete entries with precise time
zsh_history_cleaner --mode before --date "2024-01-01 09:00:00" --precise

# Delete entries matching keywords
zsh_history_cleaner --mode last_7_days --keyword "sudo apt update" "sudo timeshift"

# Delete entries matching regex patterns
zsh_history_cleaner --mode today --regex "git\s+(commit|push)" "sudo\s+-E"

# Delete entries older than 90 days
zsh_history_cleaner --mode older_than --days 90 --backup

# Delete entries newer than 90 days
zsh_history_cleaner --mode newer_than --days 90 --backup
```

### Options

```
--mode <MODE>         Cleaning mode (required in non-interactive mode)
--start-date <DATE>   Start date for 'between' mode
--end-date <DATE>     End date for 'between' mode
--date <DATE>         Date for 'specific_day', 'before', or 'after' modes
--days <N>           Days for 'older_than' mode
--precise            Enable precise time matching
--keyword <STRING...> Filter by exact strings
--regex <PATTERN...>  Filter by regex patterns
--backup             Create backup before cleaning
--dry-run            Preview changes without modifying
--histfile <PATH>    Custom history file path
--passes <N>         Number of secure deletion passes (default: 32)
-h, --help           Show help message
```

The tool will also prompt for the number of secure deletion passes in interactive mode when not doing a dry run.

## Security Considerations

### Secure Deletion

The tool uses a multi-pass overwrite for secure deletion:
- Multiple random data passes
- Zero-fill pass
- Sync to disk between passes
- Random file names for all temporary files

Note: The effectiveness of secure deletion depends on:
- File system type (journaling, COW, etc.)
- Storage medium (SSD wear leveling, etc.)
- Operating system behavior

### Data Safety

- Backup option for safety
- Dry-run mode to preview changes
- Permission checks before operations
- Graceful handling of interruptions
- No multiple copies of sensitive data

### Permissions

Required permissions:
- Read access to the history file
- Write access to the history file
- Write access to the containing directory

## Error Handling

The tool provides detailed error messages for:
- Permission issues
- Invalid dates/times
- Invalid regex patterns
- File I/O errors
- Interruption handling

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests
5. Submit a pull request

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Copyright (c) 2024-2025 [@movrcx0](https://github.com/movrcx0)

## Acknowledgments

- Inspired by the need for secure and selective Zsh history cleaning
- Built with modern C++ features for robustness and security
- Designed with UNIX philosophy in mind

## Author

[@movrcx0](https://github.com/movrcx0)

## Version History

- 1.0.0
  - Initial release
  - Basic cleaning modes
  - Secure deletion
  - Interactive interface

## Support

For bugs, questions, and discussions, please use the [GitHub Issues](https://github.com/movrcx0/zsh-history-cleaner/issues).