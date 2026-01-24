/**
 * @file squeeze2diretta-wrapper.cpp
 * @brief Wrapper that bridges squeezelite STDOUT to Diretta output
 *
 * This wrapper launches squeezelite with STDOUT output and redirects
 * the raw PCM/DSD audio stream to a Diretta DAC using DirettaSync.
 *
 * Architecture:
 *   LMS -> squeezelite -> STDOUT (PCM) -> wrapper -> DirettaSync -> Diretta DAC
 *
 * Uses DirettaSync from DirettaRendererUPnP v2.0 for low-latency streaming:
 * - Lock-free ring buffer with SIMD optimizations (AVX2/AVX512)
 * - 70% latency reduction compared to v1.x
 * - Pull-model via DIRETTA::Sync API
 *
 * @author Dominique COMET
 * @date 2025-2026
 */

#include "DirettaSync.h"
#include "globals.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <memory>
#include <thread>
#include <chrono>

// Version
#define WRAPPER_VERSION "2.0.0"

// Global state
static pid_t squeezelite_pid = 0;
static bool running = true;
static std::unique_ptr<DirettaSync> g_diretta;  // For signal handler access

// Signal handler for clean shutdown
void signal_handler(int sig) {
    std::cout << "\nSignal " << sig << " received, shutting down..." << std::endl;
    running = false;

    if (squeezelite_pid > 0) {
        kill(squeezelite_pid, SIGTERM);
    }
}

// Parse command line arguments
struct Config {
    // Squeezelite options
    std::string lms_server = "";         // -s
    std::string player_name = "squeeze2diretta";  // -n
    std::string mac_address = "";        // -m
    std::string model_name = "SqueezeLite";  // -M
    std::string codecs = "";             // -c
    std::string rates = "";              // -r
    int sample_format = 24;              // -a (16, 24, or 32)

    // Diretta options
    int diretta_target = 0;              // 0-based index (-1 = auto first)
    int thread_mode = 1;
    unsigned int cycle_time = 2620;      // Auto-calculated by default
    bool cycle_time_auto = true;
    unsigned int mtu = 0;                // 0 = auto-detect

    // Other
    bool verbose = false;
    bool list_targets = false;
    std::string squeezelite_path = "squeezelite";  // Path to squeezelite binary
};

void print_usage(const char* prog) {
    std::cout << "squeeze2diretta v" << WRAPPER_VERSION << std::endl;
    std::cout << "Squeezelite to Diretta Bridge (Low-Latency Edition)" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << prog << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Squeezelite Options:" << std::endl;
    std::cout << "  -s <server>[:<port>]  LMS server address (default: autodiscovery)" << std::endl;
    std::cout << "  -n <name>             Player name (default: squeeze2diretta)" << std::endl;
    std::cout << "  -m <mac>              MAC address (format: ab:cd:ef:12:34:56)" << std::endl;
    std::cout << "  -M <model>            Model name (default: SqueezeLite)" << std::endl;
    std::cout << "  -c <codec1>,<codec2>  Restrict codecs (flac,pcm,mp3,ogg,aac,dsd...)" << std::endl;
    std::cout << "  -r <rates>            Supported sample rates" << std::endl;
    std::cout << "  -a <format>           Sample format: 16, 24 (default), or 32" << std::endl;
    std::cout << std::endl;
    std::cout << "Diretta Options:" << std::endl;
    std::cout << "  -t <number>           Diretta target number (default: 1 = first)" << std::endl;
    std::cout << "  -l                    List Diretta targets and exit" << std::endl;
    std::cout << "  --thread-mode <n>     THRED_MODE bitmask (default: 1)" << std::endl;
    std::cout << "  --cycle-time <us>     Transfer cycle time in microseconds (default: auto)" << std::endl;
    std::cout << "  --mtu <bytes>         MTU override (default: auto-detect)" << std::endl;
    std::cout << std::endl;
    std::cout << "Other:" << std::endl;
    std::cout << "  -v                    Verbose output" << std::endl;
    std::cout << "  -h, --help            Show this help" << std::endl;
    std::cout << "  --squeezelite <path>  Path to squeezelite binary" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << prog << " -s 192.168.1.100 -n \"Living Room\" -t 1" << std::endl;
    std::cout << "  " << prog << " -l                    # List available targets" << std::endl;
    std::cout << "  " << prog << " -v -t 2               # Verbose, second target" << std::endl;
    std::cout << std::endl;
}

Config parse_args(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            exit(0);
        }
        else if (arg == "-l") {
            config.list_targets = true;
        }
        else if (arg == "-v") {
            config.verbose = true;
        }
        else if ((arg == "-s" || arg == "-n" || arg == "-m" || arg == "-M" ||
                  arg == "-c" || arg == "-r" || arg == "-a" || arg == "-t") && i + 1 < argc) {
            std::string value = argv[++i];

            if (arg == "-s") config.lms_server = value;
            else if (arg == "-n") config.player_name = value;
            else if (arg == "-m") config.mac_address = value;
            else if (arg == "-M") config.model_name = value;
            else if (arg == "-c") config.codecs = value;
            else if (arg == "-r") config.rates = value;
            else if (arg == "-a") config.sample_format = std::stoi(value);
            else if (arg == "-t") config.diretta_target = std::stoi(value) - 1;  // Convert to 0-based
        }
        else if (arg == "--thread-mode" && i + 1 < argc) {
            config.thread_mode = std::stoi(argv[++i]);
        }
        else if (arg == "--cycle-time" && i + 1 < argc) {
            config.cycle_time = static_cast<unsigned int>(std::stoi(argv[++i]));
            config.cycle_time_auto = false;
        }
        else if (arg == "--mtu" && i + 1 < argc) {
            config.mtu = static_cast<unsigned int>(std::stoi(argv[++i]));
        }
        else if (arg == "--squeezelite" && i + 1 < argc) {
            config.squeezelite_path = argv[++i];
        }
    }

    return config;
}

std::vector<std::string> build_squeezelite_args(const Config& config) {
    std::vector<std::string> args;

    args.push_back(config.squeezelite_path);

    // Output to STDOUT with specified format
    args.push_back("-o");
    args.push_back("-");

    args.push_back("-a");
    args.push_back(std::to_string(config.sample_format));

    // Player name
    args.push_back("-n");
    args.push_back(config.player_name);

    // Model name
    args.push_back("-M");
    args.push_back(config.model_name);

    // LMS server (if specified)
    if (!config.lms_server.empty()) {
        args.push_back("-s");
        args.push_back(config.lms_server);
    }

    // MAC address (if specified)
    if (!config.mac_address.empty()) {
        args.push_back("-m");
        args.push_back(config.mac_address);
    }

    // Codecs (if specified)
    if (!config.codecs.empty()) {
        args.push_back("-c");
        args.push_back(config.codecs);
    }

    // Sample rates (if specified)
    if (!config.rates.empty()) {
        args.push_back("-r");
        args.push_back(config.rates);
    }

    return args;
}

int main(int argc, char* argv[]) {

    std::cout << "================================================================" << std::endl;
    std::cout << "  squeeze2diretta v" << WRAPPER_VERSION << " (Low-Latency Edition)" << std::endl;
    std::cout << "  Squeezelite to Diretta Bridge" << std::endl;
    std::cout << "  Using DirettaSync from DirettaRendererUPnP v2.0" << std::endl;
    std::cout << "================================================================" << std::endl;
    std::cout << std::endl;

    // Parse arguments
    Config config = parse_args(argc, argv);

    // Set global verbose flag for DirettaSync logging
    g_verbose = config.verbose;

    // Allocate log ring for async logging if verbose
    if (g_verbose) {
        g_logRing = new LogRing();
    }

    // List targets if requested
    if (config.list_targets) {
        DirettaSync::listTargets();
        if (g_logRing) delete g_logRing;
        return 0;
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create DirettaSync instance
    g_diretta = std::make_unique<DirettaSync>();

    // Configure and enable Diretta
    DirettaConfig direttaConfig;
    direttaConfig.threadMode = config.thread_mode;
    direttaConfig.cycleTime = config.cycle_time;
    direttaConfig.cycleTimeAuto = config.cycle_time_auto;
    direttaConfig.mtu = config.mtu;

    // Set target index before enable
    if (config.diretta_target >= 0) {
        g_diretta->setTargetIndex(config.diretta_target);
    }

    // Set MTU if specified
    if (config.mtu > 0) {
        g_diretta->setMTU(config.mtu);
    }

    std::cout << "Initializing Diretta..." << std::endl;

    if (!g_diretta->enable(direttaConfig)) {
        std::cerr << "Failed to enable Diretta. Check that a Diretta target is available." << std::endl;
        std::cerr << "Use -l to list available targets." << std::endl;
        if (g_logRing) delete g_logRing;
        return 1;
    }

    std::cout << "Diretta enabled successfully" << std::endl;

    // Create pipe for squeezelite STDOUT
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::cerr << "Failed to create pipe" << std::endl;
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    // Fork and exec squeezelite
    squeezelite_pid = fork();

    if (squeezelite_pid == -1) {
        std::cerr << "Failed to fork" << std::endl;
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    if (squeezelite_pid == 0) {
        // Child process: run squeezelite

        // Redirect STDOUT to pipe
        close(pipefd[0]);  // Close read end
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        // Build arguments
        std::vector<std::string> args = build_squeezelite_args(config);
        std::vector<char*> c_args;
        for (auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        // Execute squeezelite
        execvp(c_args[0], c_args.data());

        // If we get here, exec failed
        std::cerr << "Failed to execute squeezelite: " << strerror(errno) << std::endl;
        exit(1);
    }

    // Parent process: read from pipe and send to Diretta
    close(pipefd[1]);  // Close write end

    std::cout << "Squeezelite started (PID: " << squeezelite_pid << ")" << std::endl;
    std::cout << "Waiting for audio stream..." << std::endl;
    std::cout << std::endl;

    // Setup audio format based on config
    AudioFormat format;
    format.sampleRate = 44100;  // Will be dynamically adjusted
    format.bitDepth = static_cast<uint32_t>(config.sample_format);
    format.channels = 2;
    format.isDSD = false;
    format.isCompressed = false;

    // Open Diretta connection with format
    if (!g_diretta->open(format)) {
        std::cerr << "Failed to open Diretta output" << std::endl;
        kill(squeezelite_pid, SIGTERM);
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    std::cout << "Connected to Diretta DAC" << std::endl;
    std::cout << "Streaming audio..." << std::endl;
    std::cout << std::endl;

    // Read audio data from pipe and send to Diretta
    const size_t CHUNK_SIZE = 8192;  // frames per read
    size_t bytes_per_frame = (format.bitDepth / 8) * format.channels;
    size_t buffer_size = CHUNK_SIZE * bytes_per_frame;

    std::vector<uint8_t> buffer(buffer_size);
    uint64_t total_bytes = 0;
    uint64_t total_frames = 0;

    while (running) {
        ssize_t bytes_read = read(pipefd[0], buffer.data(), buffer_size);

        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                std::cout << "Squeezelite closed" << std::endl;
            } else if (errno != EINTR) {
                std::cerr << "Error reading from squeezelite: " << strerror(errno) << std::endl;
            }
            break;
        }

        // Calculate number of samples (DirettaSync expects samples, not frames)
        // For PCM: samples = frames (each frame is one sample per channel)
        size_t num_frames = static_cast<size_t>(bytes_read) / bytes_per_frame;
        size_t num_samples = num_frames;  // DirettaSync's sendAudio expects frames for PCM

        // Send to Diretta
        size_t written = g_diretta->sendAudio(buffer.data(), num_samples);

        if (written == 0 && g_diretta->isPlaying()) {
            // Buffer might be full, yield and retry
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        total_bytes += static_cast<uint64_t>(bytes_read);
        total_frames += num_frames;

        // Print progress every ~10 seconds at 44.1kHz
        if (g_verbose && total_frames % (44100 * 10) < CHUNK_SIZE) {
            double seconds = static_cast<double>(total_frames) / 44100.0;
            std::cout << "Streamed: " << std::fixed << std::setprecision(1)
                      << seconds << "s (" << (total_bytes / 1024 / 1024) << " MB)" << std::endl;
        }
    }

    // Cleanup
    std::cout << std::endl;
    std::cout << "Shutting down..." << std::endl;

    g_diretta->close();
    g_diretta->disable();
    g_diretta.reset();

    close(pipefd[0]);

    if (squeezelite_pid > 0) {
        kill(squeezelite_pid, SIGTERM);
        waitpid(squeezelite_pid, nullptr, 0);
    }

    // Cleanup log ring
    if (g_logRing) {
        delete g_logRing;
        g_logRing = nullptr;
    }

    std::cout << "Stopped" << std::endl;
    std::cout << "Total streamed: " << total_frames << " frames ("
              << (total_bytes / 1024 / 1024) << " MB)" << std::endl;

    return 0;
}
