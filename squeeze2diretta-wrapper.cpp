/**
 * @file squeeze2diretta-wrapper.cpp
 * @brief Wrapper that bridges squeezelite STDOUT to Diretta output
 * 
 * This wrapper launches squeezelite with STDOUT output and redirects
 * the raw PCM/DSD audio stream to a Diretta DAC using DirettaOutputSimple.
 * 
 * Architecture:
 *   LMS → squeezelite → STDOUT (PCM) → wrapper → DirettaOutputSimple → Diretta DAC
 * 
 * @author Dominique COMET
 * @date 2025
 */

#include "DirettaOutputSimple.h"
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

// Version
#define WRAPPER_VERSION "1.0.0"

// Global state
static pid_t squeezelite_pid = 0;
static bool running = true;

// Signal handler for clean shutdown
void signal_handler(int sig) {
    std::cout << "\n⚠️  Signal " << sig << " received, shutting down..." << std::endl;
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
    unsigned diretta_target = 1;
    float buffer_seconds = 2.0f;
    int thread_mode = 1;
    int cycle_time = 10000;
    int cycle_min_time = 333;
    int info_cycle = 5000;
    int mtu = 0;
    
    // Other
    bool verbose = false;
    bool list_targets = false;
    std::string squeezelite_path = "squeezelite";  // Path to squeezelite binary
};

void print_usage(const char* prog) {
    std::cout << "squeeze2diretta-wrapper v" << WRAPPER_VERSION << std::endl;
    std::cout << "Bridges squeezelite to Diretta protocol DACs" << std::endl;
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
    std::cout << "  -t <number>           Diretta target number (default: 1)" << std::endl;
    std::cout << "  -l                    List Diretta targets and exit" << std::endl;
    std::cout << "  -b <seconds>          Buffer size in seconds (default: 2.0)" << std::endl;
    std::cout << "  --thread-mode <n>     THRED_MODE bitmask (default: 1)" << std::endl;
    std::cout << "  --cycle-time <µs>     Transfer cycle max time (default: 10000)" << std::endl;
    std::cout << "  --cycle-min-time <µs> Transfer cycle min time (default: 333)" << std::endl;
    std::cout << "  --info-cycle <µs>     Info packet cycle time (default: 5000)" << std::endl;
    std::cout << "  --mtu <bytes>         MTU override (default: auto)" << std::endl;
    std::cout << std::endl;
    std::cout << "Other:" << std::endl;
    std::cout << "  -v                    Verbose output" << std::endl;
    std::cout << "  -h, --help            Show this help" << std::endl;
    std::cout << "  --squeezelite <path>  Path to squeezelite binary" << std::endl;
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
                  arg == "-c" || arg == "-r" || arg == "-a" || arg == "-t" || 
                  arg == "-b") && i + 1 < argc) {
            std::string value = argv[++i];
            
            if (arg == "-s") config.lms_server = value;
            else if (arg == "-n") config.player_name = value;
            else if (arg == "-m") config.mac_address = value;
            else if (arg == "-M") config.model_name = value;
            else if (arg == "-c") config.codecs = value;
            else if (arg == "-r") config.rates = value;
            else if (arg == "-a") config.sample_format = std::stoi(value);
            else if (arg == "-t") config.diretta_target = std::stoi(value);
            else if (arg == "-b") config.buffer_seconds = std::stof(value);
        }
        else if (arg == "--thread-mode" && i + 1 < argc) {
            config.thread_mode = std::stoi(argv[++i]);
        }
        else if (arg == "--cycle-time" && i + 1 < argc) {
            config.cycle_time = std::stoi(argv[++i]);
        }
        else if (arg == "--cycle-min-time" && i + 1 < argc) {
            config.cycle_min_time = std::stoi(argv[++i]);
        }
        else if (arg == "--info-cycle" && i + 1 < argc) {
            config.info_cycle = std::stoi(argv[++i]);
        }
        else if (arg == "--mtu" && i + 1 < argc) {
            config.mtu = std::stoi(argv[++i]);
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
    
    std::cout << "════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  squeeze2diretta-wrapper v" << WRAPPER_VERSION << std::endl;
    std::cout << "  Squeezelite → Diretta Bridge" << std::endl;
    std::cout << "════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;
    
    // Parse arguments
    Config config = parse_args(argc, argv);
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create Diretta output
    DirettaOutputSimple diretta;
    
    // Configure Diretta
    diretta.setThredMode(config.thread_mode);
    diretta.setCycleTime(config.cycle_time);
    diretta.setCycleMinTime(config.cycle_min_time);
    diretta.setInfoCycle(config.info_cycle);
    if (config.mtu > 0) {
        diretta.setMTU(config.mtu);
    }
    
    // List targets if requested
    if (config.list_targets) {
        diretta.listAvailableTargets();
        return 0;
    }
    
    // Select Diretta target
    if (!diretta.selectTarget(config.diretta_target - 1)) {  // Convert 1-based to 0-based
        std::cerr << "Failed to select Diretta target " << config.diretta_target << std::endl;
        return 1;
    }
    
    // Create pipe for squeezelite STDOUT
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::cerr << "Failed to create pipe" << std::endl;
        return 1;
    }
    
    // Fork and exec squeezelite
    squeezelite_pid = fork();
    
    if (squeezelite_pid == -1) {
        std::cerr << "Failed to fork" << std::endl;
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
    
    std::cout << "✓ Squeezelite started (PID: " << squeezelite_pid << ")" << std::endl;
    std::cout << "✓ Waiting for audio stream..." << std::endl;
    std::cout << std::endl;
    
    // We need to detect the audio format from squeezelite's output
    // For now, assume the format based on config
    AudioFormat format;
    format.sampleRate = 44100;  // Will be updated dynamically
    format.bitDepth = config.sample_format;
    format.channels = 2;
    format.isDSD = false;
    
    // Open Diretta connection
    if (!diretta.open(format, config.buffer_seconds)) {
        std::cerr << "Failed to open Diretta output" << std::endl;
        kill(squeezelite_pid, SIGTERM);
        return 1;
    }
    
    std::cout << "✓ Connected to Diretta DAC" << std::endl;
    std::cout << "✓ Streaming audio..." << std::endl;
    std::cout << std::endl;
    
    // Read audio data from pipe and send to Diretta
    const size_t CHUNK_SIZE = 8192;  // frames
    size_t bytes_per_frame = (format.bitDepth / 8) * format.channels;
    size_t buffer_size = CHUNK_SIZE * bytes_per_frame;
    
    std::vector<uint8_t> buffer(buffer_size);
    
    while (running) {
        ssize_t bytes_read = read(pipefd[0], buffer.data(), buffer_size);
        
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                std::cout << "Squeezelite closed" << std::endl;
            } else {
                std::cerr << "Error reading from squeezelite: " << strerror(errno) << std::endl;
            }
            break;
        }
        
        // Calculate number of samples
        size_t num_samples = bytes_read / bytes_per_frame * format.channels;
        
        // Send to Diretta
        if (!diretta.sendAudio(buffer.data(), num_samples)) {
            std::cerr << "Failed to send audio to Diretta" << std::endl;
            break;
        }
    }
    
    // Cleanup
    std::cout << std::endl;
    std::cout << "Shutting down..." << std::endl;
    
    diretta.close();
    close(pipefd[0]);
    
    if (squeezelite_pid > 0) {
        kill(squeezelite_pid, SIGTERM);
        waitpid(squeezelite_pid, nullptr, 0);
    }
    
    std::cout << "✓ Stopped" << std::endl;
    
    return 0;
}
