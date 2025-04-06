cmake_minimum_required(VERSION 3.10)
project(zsh_history_cleaner VERSION 1.0.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -pedantic -O2)
endif()

# Define source files
set(SOURCES
    src/main.cpp
    src/core/HistoryCleaner.cpp
    src/core/SecureDelete.cpp
    src/utils/Utils.cpp
)

# Define header files
set(HEADERS
    include/zsh_history_cleaner/Constants.h
    include/zsh_history_cleaner/HistoryCleaner.h
    include/zsh_history_cleaner/SecureDelete.h
    include/zsh_history_cleaner/Utils.h
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Add include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Install target
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)