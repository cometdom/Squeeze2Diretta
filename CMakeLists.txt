cmake_minimum_required(VERSION 3.10)
project(squeeze2diretta VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)

# Find required packages
find_package(Threads REQUIRED)

# Check for Diretta SDK
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/diretta-sdk/lib/libDIRETTA.so")
    message(FATAL_ERROR 
        "\n"
        "═══════════════════════════════════════════════════════\n"
        "  Diretta SDK NOT FOUND!\n"
        "═══════════════════════════════════════════════════════\n"
        "\n"
        "Please download the Diretta Host SDK from:\n"
        "  https://www.diretta.link/hostsdk.html\n"
        "\n"
        "Extract to: ${CMAKE_SOURCE_DIR}/diretta-sdk/\n"
        "\n"
        "Expected structure:\n"
        "  diretta-sdk/\n"
        "    ├── include/\n"
        "    │   ├── DIRETTA/\n"
        "    │   └── ACQUA/\n"
        "    └── lib/\n"
        "        ├── libDIRETTA.so\n"
        "        └── libACQUA.so\n"
    )
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/diretta
    ${CMAKE_SOURCE_DIR}/diretta-sdk/include
)

# Link directories
link_directories(
    ${CMAKE_SOURCE_DIR}/diretta-sdk/lib
)

# Sources
set(WRAPPER_SOURCES
    squeeze2diretta-wrapper.cpp
    diretta/DirettaOutputSimple.cpp
)

# Create executable
add_executable(squeeze2diretta
    ${WRAPPER_SOURCES}
)

# Link libraries
target_link_libraries(squeeze2diretta
    ${CMAKE_THREAD_LIBS_INIT}
    DIRETTA
    ACQUA
    dl
)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O3")

# Install
install(TARGETS squeeze2diretta DESTINATION bin)

# Install wrapper script that ensures squeezelite is available
install(FILES ${CMAKE_SOURCE_DIR}/scripts/squeeze2diretta-check.sh 
        DESTINATION bin
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ
                    WORLD_EXECUTE WORLD_READ)

# Print configuration
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "  squeeze2diretta wrapper configuration")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "")
message(STATUS "Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "Diretta SDK:       FOUND")
message(STATUS "")
message(STATUS "NOTE: This wrapper requires 'squeezelite' to be installed")
message(STATUS "      Install squeezelite from your package manager:")
message(STATUS "")
message(STATUS "      Ubuntu/Debian:")
message(STATUS "        sudo apt-get install squeezelite")
message(STATUS "")
message(STATUS "      Or build from source:")
message(STATUS "        https://github.com/ralph-irving/squeezelite")
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "")
