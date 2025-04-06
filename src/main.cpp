#include "../include/zsh_history_cleaner/HistoryCleaner.h" // Includes all necessary definitions for the cleaner
#include <iostream>                             // For cerr
#include <exception>                            // For std::exception

int main(int argc, char* argv[]) {
    // Set locale to user's default to potentially handle date/time formats correctly
    // Although strptime/strftime behavior with locale can be complex.
    // std::setlocale(LC_ALL, ""); // Uncomment if locale-specific parsing/formatting is desired

    try {
        // HistoryCleaner constructor handles argument parsing, path resolution,
        // permission checks, and signal handler setup.
        // RAII ensures cleanup via destructor even if run() throws (though run catches internally).
        HistoryCleaner cleaner(argc, argv);

        // Run the cleaner (either interactive or non-interactive based on args)
        cleaner.run();

        // run() handles its own errors internally or exits.
        // If run() completes without exiting/throwing, it's considered success.
        return 0; // Indicate successful completion

    } catch (const std::exception& e) {
        // Catch exceptions that might occur during constructor or initial setup
        // (though constructor often calls errorExit directly).
        // Also catches potential re-throws from run() if modified later.
        std::cerr << "Fatal Error during initialization or unhandled runtime issue: " << e.what() << std::endl;
        return 1; // General error
    } catch (...) {
        // Catch any other unexpected errors (e.g., non-std exceptions)
        std::cerr << "Fatal Error: An unknown exception occurred." << std::endl;
        return 2; // Unknown error
    }
}