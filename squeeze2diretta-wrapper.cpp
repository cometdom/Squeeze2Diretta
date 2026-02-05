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
#include <sys/stat.h>
#include <fcntl.h>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <sstream>
#include <regex>
#include <poll.h>

// Version
#define WRAPPER_VERSION "1.0.1"

// DSD format types
enum class DSDFormatType {
    NONE,       // Not DSD
    DOP,        // DSD over PCM
    U32_BE,     // Native DSD Big Endian
    U32_LE      // Native DSD Little Endian
};

// Global state
static pid_t squeezelite_pid = 0;
static bool running = true;
static std::unique_ptr<DirettaSync> g_diretta;  // For signal handler access
static std::atomic<unsigned int> g_current_sample_rate{0};  // Current detected sample rate (0 = not yet detected)
static std::atomic<bool> g_is_dsd{false};  // Current format is DSD (DoP or native)
static std::atomic<int> g_dsd_format_type{static_cast<int>(DSDFormatType::NONE)};  // Specific DSD format type
static std::atomic<bool> g_need_reopen{false};  // Flag when Diretta needs to be reopened
static std::atomic<bool> g_format_pending{false};  // Format change detected, waiting for sample rate

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
    int sample_format = 32;              // -a (16, 24, or 32) - PCM output bit depth for Diretta
    std::string dsd_format = ":u32be";   // -D [format]: "dop" for DoP, ":u32be" (default) or ":u32le" for native DSD
    bool wav_header = false;             // -W: Read WAV/AIFF headers, ignore server parameters

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
    std::cout << "  -D [:format]          Enable DSD output:" << std::endl;
    std::cout << "                          -D           = DoP (DSD over PCM)" << std::endl;
    std::cout << "                          -D :u32be    = Native DSD Big Endian (MSB)" << std::endl;
    std::cout << "                          -D :u32le    = Native DSD Little Endian (LSB)" << std::endl;
    std::cout << "  -W                    Read WAV/AIFF headers, ignore server parameters" << std::endl;
    std::cout << std::endl;
    std::cout << "Diretta Options:" << std::endl;
    std::cout << "  -t, --target <number> Diretta target number (default: 1 = first)" << std::endl;
    std::cout << "  -l, --list-targets    List Diretta targets and exit" << std::endl;
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
        else if (arg == "-l" || arg == "--list-targets") {
            config.list_targets = true;
        }
        else if (arg == "-v") {
            config.verbose = true;
        }
        else if (arg == "-W") {
            config.wav_header = true;
        }
        else if (arg == "-D") {
            // Check if next arg is a DSD format specifier (starts with :)
            if (i + 1 < argc && argv[i + 1][0] == ':') {
                config.dsd_format = argv[++i];  // e.g., ":u32be" or ":u32le"
            } else {
                config.dsd_format = "dop";  // Default to DoP if no format specified
            }
        }
        else if ((arg == "-s" || arg == "-n" || arg == "-m" || arg == "-M" ||
                  arg == "-c" || arg == "-r" || arg == "-a" ||
                  arg == "-t" || arg == "--target") && i + 1 < argc) {
            std::string value = argv[++i];

            if (arg == "-s") config.lms_server = value;
            else if (arg == "-n") config.player_name = value;
            else if (arg == "-m") config.mac_address = value;
            else if (arg == "-M") config.model_name = value;
            else if (arg == "-c") config.codecs = value;
            else if (arg == "-r") config.rates = value;
            else if (arg == "-a") config.sample_format = std::stoi(value);
            else if (arg == "-t" || arg == "--target") config.diretta_target = std::stoi(value) - 1;  // Convert to 0-based
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

std::vector<std::string> build_squeezelite_args(const Config& config, const std::string& output_path) {
    std::vector<std::string> args;

    args.push_back(config.squeezelite_path);

    // WAV/AIFF header parsing (squeezelite -W)
    // Read format from WAV/AIFF headers instead of trusting server parameters
    if (config.wav_header) {
        args.push_back("-W");
    }

    // Output to stdout ("-") - squeezelite outputs raw S32_LE at native sample rate
    args.push_back("-o");
    args.push_back(output_path);

    // Sample rates - CRITICAL for DSD support
    // When using STDOUT output, squeezelite needs to know supported rates
    // Default range covers all PCM rates and DSD frame rates (up to DSD512)
    args.push_back("-r");
    if (!config.rates.empty()) {
        // User specified max rate - create range from 44100 to max
        args.push_back("44100-" + config.rates);
    } else {
        // Default: support up to 768kHz (covers DSD512 frame rate of 705600)
        args.push_back("44100-768000");
    }

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

    // DSD output format (configurable via -D option)
    // Default: :u32be (native DSD Big Endian for LMS)
    // For Roon: use -D (DoP mode)
    args.push_back("-D");
    if (config.dsd_format != "dop") {
        args.push_back(config.dsd_format);  // :u32be or :u32le
    }
    // If "dop", just -D alone enables DoP mode in squeezelite

    // Debug logging - ALWAYS need decode+output for format detection
    // The monitor_squeezelite_stderr() function parses these logs to detect
    // sample rate and DSD format changes
    // Note: Always use all=info as squeezelite may not support comma-separated modules
    args.push_back("-d");
    args.push_back("all=info");

    // Note: Sample rates already handled above (lines 171-177)

    return args;
}

// Convert S32_LE PCM samples to lower bit depth (S24_3LE or S16_LE)
// Squeezelite always outputs S32_LE on stdout; this truncates the LSBs
// for DACs that don't support 32-bit PCM.
// Returns the number of output bytes written.
static size_t convert_pcm_bit_depth(const uint8_t* input, uint8_t* output,
                                     size_t num_frames, int channels,
                                     int output_bits) {
    const size_t total_samples = num_frames * channels;

    if (output_bits == 24) {
        // S32_LE → S24_3LE: drop byte 0 (LSB) from each 4-byte sample
        size_t out_pos = 0;
        for (size_t i = 0; i < total_samples; i++) {
            const size_t src = i * 4;
            output[out_pos]     = input[src + 1];
            output[out_pos + 1] = input[src + 2];
            output[out_pos + 2] = input[src + 3];
            out_pos += 3;
        }
        return out_pos;
    } else if (output_bits == 16) {
        // S32_LE → S16_LE: keep bytes 2-3 (MSBs) from each 4-byte sample
        size_t out_pos = 0;
        for (size_t i = 0; i < total_samples; i++) {
            const size_t src = i * 4;
            output[out_pos]     = input[src + 2];
            output[out_pos + 1] = input[src + 3];
            out_pos += 2;
        }
        return out_pos;
    }

    // 32-bit: no conversion needed
    size_t bytes = total_samples * 4;
    std::memcpy(output, input, bytes);
    return bytes;
}

// Monitor squeezelite stderr for sample rate changes
void monitor_squeezelite_stderr(int stderr_fd) {
    char buffer[4096];
    std::string line_buffer;

    // Regex patterns
    std::regex sample_rate_regex(R"(track start sample rate:\s*(\d+))");
    std::regex dsd_format_regex(R"(format:\s*(DOP|DSD_U32_BE|DSD_U32_LE))");  // Match native DSD format
    std::regex dop_contains_regex(R"(file contains DOP)");  // Match DoP from Roon/FLAC container
    std::regex pcm_codec_regex(R"(codec open: '[fpom]')");  // PCM codecs
    // NEW: Parse dsd_decode line which contains BOTH format AND rate
    // This arrives ~30ms earlier than "track start sample rate" for 44.1kHz family
    // Example: "dsd_decode:821 DSD512 stream, format: DSD_U32_BE, rate: 705600Hz"
    std::regex dsd_decode_regex(R"(dsd_decode:\d+\s+DSD\d+\s+stream,\s+format:\s*(DSD_U32_BE|DSD_U32_LE),\s+rate:\s*(\d+))");
    // Example: "dsd_decode:821 DoP stream, format: DOP, rate: 176400Hz"
    std::regex dop_decode_regex(R"(dsd_decode:\d+\s+DoP\s+stream,\s+format:\s*DOP,\s+rate:\s*(\d+))");

    while (running) {
        ssize_t bytes_read = read(stderr_fd, buffer, sizeof(buffer) - 1);

        if (bytes_read <= 0) {
            break;
        }

        buffer[bytes_read] = '\0';
        line_buffer += buffer;

        // Process complete lines
        size_t pos;
        while ((pos = line_buffer.find('\n')) != std::string::npos) {
            std::string line = line_buffer.substr(0, pos);
            line_buffer.erase(0, pos + 1);

            std::smatch match;

            // PRIORITY 1: Check for dsd_decode line which has BOTH format AND rate
            // This triggers ~30ms earlier than waiting for "track start sample rate"
            if (std::regex_search(line, match, dsd_decode_regex)) {
                std::string format = match[1].str();
                unsigned int rate = std::stoul(match[2].str());

                if (g_verbose) {
                    std::cout << "\n[DSD Decode] " << format << " at " << rate << "Hz (immediate trigger)" << std::endl;
                }

                // Store the specific DSD format type
                if (format == "DSD_U32_BE") {
                    g_dsd_format_type.store(static_cast<int>(DSDFormatType::U32_BE));
                } else if (format == "DSD_U32_LE") {
                    g_dsd_format_type.store(static_cast<int>(DSDFormatType::U32_LE));
                }

                g_is_dsd.store(true);
                g_current_sample_rate.store(rate);
                g_format_pending.store(false);  // No need to wait - we have everything
                g_need_reopen.store(true);  // Trigger immediately
            }
            // Check for DoP decode line
            else if (std::regex_search(line, match, dop_decode_regex)) {
                unsigned int rate = std::stoul(match[1].str());

                if (g_verbose) {
                    std::cout << "\n[DoP Decode] at " << rate << "Hz (immediate trigger)" << std::endl;
                }

                g_dsd_format_type.store(static_cast<int>(DSDFormatType::DOP));
                g_is_dsd.store(true);
                g_current_sample_rate.store(rate);
                g_format_pending.store(false);
                g_need_reopen.store(true);
            }
            // FALLBACK: Check for DSD format (DoP or native) - old method
            else if (std::regex_search(line, match, dsd_format_regex)) {
                std::string format = match[1].str();
                bool was_dsd = g_is_dsd.load();
                if (!was_dsd) {
                    if (g_verbose) {
                        std::cout << "\n[Format Detected] " << format << " (waiting for sample rate...)" << std::endl;
                    }

                    // Store the specific DSD format type
                    if (format == "DOP") {
                        g_dsd_format_type.store(static_cast<int>(DSDFormatType::DOP));
                    } else if (format == "DSD_U32_BE") {
                        g_dsd_format_type.store(static_cast<int>(DSDFormatType::U32_BE));
                    } else if (format == "DSD_U32_LE") {
                        g_dsd_format_type.store(static_cast<int>(DSDFormatType::U32_LE));
                    }

                    g_is_dsd.store(true);
                    g_format_pending.store(true);
                    // Don't trigger reopen yet - wait for sample rate
                }
            }
            // Check for DoP from Roon (different log format: "file contains DOP")
            else if (std::regex_search(line, dop_contains_regex)) {
                bool was_dsd = g_is_dsd.load();
                if (!was_dsd) {
                    if (g_verbose) {
                        std::cout << "\n[Format Detected] DoP (from Roon/container)" << std::endl;
                    }
                    g_dsd_format_type.store(static_cast<int>(DSDFormatType::DOP));
                    g_is_dsd.store(true);
                    g_format_pending.store(true);
                }
            }
            // Check for PCM codec (switch back from DSD to PCM)
            else if (std::regex_search(line, match, pcm_codec_regex)) {
                bool was_dsd = g_is_dsd.load();
                if (was_dsd) {
                    if (g_verbose) {
                        std::cout << "\n[Format Change] DSD -> PCM (waiting for sample rate...)" << std::endl;
                    }
                    g_is_dsd.store(false);
                    g_dsd_format_type.store(static_cast<int>(DSDFormatType::NONE));
                    g_format_pending.store(true);
                    // Don't trigger reopen yet - wait for sample rate
                }
            }

            // Check for sample rate changes
            if (std::regex_search(line, match, sample_rate_regex)) {
                unsigned int new_rate = std::stoul(match[1].str());
                unsigned int current_rate = g_current_sample_rate.load();
                bool format_pending = g_format_pending.load();

                // Trigger reopen if sample rate changed OR format is pending
                if (new_rate != current_rate || format_pending) {
                    if (g_verbose && new_rate != current_rate) {
                        std::cout << "\n[Sample Rate Change] " << current_rate
                                  << "Hz -> " << new_rate << "Hz" << std::endl;
                    }
                    g_current_sample_rate.store(new_rate);
                    g_format_pending.store(false);  // Clear pending flag
                    g_need_reopen.store(true);  // Now trigger reopen with correct rate
                }
            }

            // Optionally print the line (for debugging)
            if (g_verbose) {
                std::cerr << line << std::endl;
            }
        }
    }

    close(stderr_fd);
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

    // Validate and store PCM output bit depth
    const int output_bit_depth = config.sample_format;
    if (output_bit_depth != 16 && output_bit_depth != 24 && output_bit_depth != 32) {
        std::cerr << "Invalid sample format: " << output_bit_depth
                  << " (must be 16, 24, or 32)" << std::endl;
        return 1;
    }

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

    // Create pipe for squeezelite stdout
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::cerr << "Failed to create pipe: " << strerror(errno) << std::endl;
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    // Create pipe for squeezelite stderr (for sample rate detection)
    int stderrfd[2];
    if (pipe(stderrfd) == -1) {
        std::cerr << "Failed to create stderr pipe: " << strerror(errno) << std::endl;
        close(pipefd[0]);
        close(pipefd[1]);
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    // Build and display squeezelite command (with stdout output)
    std::vector<std::string> squeezelite_args = build_squeezelite_args(config, "-");
    if (g_verbose) {
        std::cout << "Squeezelite command: ";
        for (const auto& arg : squeezelite_args) {
            std::cout << arg << " ";
        }
        std::cout << std::endl;
    }

    // Fork and exec squeezelite
    squeezelite_pid = fork();

    if (squeezelite_pid == -1) {
        std::cerr << "Failed to fork" << std::endl;
        close(pipefd[0]);
        close(pipefd[1]);
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    if (squeezelite_pid == 0) {
        // Child process: redirect stdout and stderr to pipes, then run squeezelite
        close(pipefd[0]);    // Close read end of stdout pipe
        close(stderrfd[0]);  // Close read end of stderr pipe

        // Redirect stdout to pipe write end
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            std::cerr << "Failed to redirect stdout: " << strerror(errno) << std::endl;
            exit(1);
        }
        close(pipefd[1]);  // Close original pipe write fd

        // Redirect stderr to pipe write end
        if (dup2(stderrfd[1], STDERR_FILENO) == -1) {
            std::cerr << "Failed to redirect stderr: " << strerror(errno) << std::endl;
            exit(1);
        }
        close(stderrfd[1]);  // Close original stderr pipe write fd

        // Convert args to C-style array
        std::vector<char*> c_args;
        for (auto& arg : squeezelite_args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        // Execute squeezelite
        execvp(c_args[0], c_args.data());

        // If we get here, exec failed
        std::cerr << "Failed to execute squeezelite: " << strerror(errno) << std::endl;
        exit(1);
    }

    // Parent process: close write ends and setup pipes
    close(pipefd[1]);    // Close write end of stdout pipe
    close(stderrfd[1]);  // Close write end of stderr pipe
    int fifo_fd = pipefd[0];      // Read audio from stdout pipe
    int stderr_fd = stderrfd[0];  // Read logs from stderr pipe

    // Launch thread to monitor stderr for sample rate changes
    std::thread stderr_monitor(monitor_squeezelite_stderr, stderr_fd);
    stderr_monitor.detach();  // Run independently

    std::cout << "Squeezelite started (PID: " << squeezelite_pid << ")" << std::endl;
    std::cout << "Waiting for audio stream..." << std::endl;
    std::cout << std::endl;

    // Setup audio format based on config
    // Note: squeezelite -o - always outputs S32_LE (32-bit) on stdout.
    // The wrapper reads 32-bit and converts to output_bit_depth before sending to Diretta.
    AudioFormat format;
    format.sampleRate = 44100;  // Default, use -r to restrict rates
    format.bitDepth = output_bit_depth;  // Output bit depth for Diretta DAC
    format.channels = 2;
    format.isDSD = false;
    format.isCompressed = false;

    std::cout << "Audio format: " << format.sampleRate << "Hz / "
              << format.bitDepth << "-bit / " << format.channels << "ch";
    if (output_bit_depth < 32) {
        std::cout << " (converting from squeezelite 32-bit)";
    }
    std::cout << std::endl;

    // Open Diretta connection with format
    if (!g_diretta->open(format)) {
        std::cerr << "Failed to open Diretta output" << std::endl;
        kill(squeezelite_pid, SIGTERM);
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    std::cout << "Connected to Diretta DAC" << std::endl;
    std::cout << std::endl;

    // ================================================================
    // Wait for first track format detection
    // ================================================================
    // The initial Diretta open uses a default 44100Hz format. If the first
    // track is at a different rate (e.g., 192kHz), audio data would arrive
    // before the stderr monitor detects the actual format, causing noise.
    // By waiting here, we ensure the first track goes through the
    // reopen+burst-fill path with the correct format.
    std::cout << "Waiting for first track..." << std::endl;
    {
        auto wait_start = std::chrono::steady_clock::now();
        const auto format_wait_timeout = std::chrono::seconds(60);
        size_t pre_drain_bytes = 0;

        while (running && !g_need_reopen.load()) {
            // Timeout safety - fall back to default 44100Hz
            if (std::chrono::steady_clock::now() - wait_start > format_wait_timeout) {
                std::cerr << "WARNING: No format detected after 60s, proceeding with default" << std::endl;
                break;
            }

            // Drain any pre-format data from pipe to prevent squeezelite from blocking
            struct pollfd pfd = {fifo_fd, POLLIN, 0};
            int ret = poll(&pfd, 1, 100);  // 100ms poll interval
            if (ret > 0 && (pfd.revents & POLLIN)) {
                uint8_t drain[16384];
                ssize_t n = read(fifo_fd, drain, sizeof(drain));
                if (n == 0) {
                    std::cout << "Squeezelite pipe closed during format wait" << std::endl;
                    running = false;
                    break;
                }
                if (n > 0) {
                    pre_drain_bytes += n;
                }
            }
        }

        if (pre_drain_bytes > 0 && g_verbose) {
            std::cout << "[Format Wait] Drained " << pre_drain_bytes
                      << " bytes of pre-format data" << std::endl;
        }
        if (g_need_reopen.load()) {
            std::cout << "First track format detected - will open with correct format" << std::endl;
        }
    }

    std::cout << "Streaming audio..." << std::endl;
    std::cout << std::endl;

    // Read audio data from pipe and send to Diretta
    const size_t CHUNK_SIZE = 2048;  // frames per read (16384 bytes for DSD/PCM)

    // Squeezelite always outputs S32_LE (4 bytes per sample) regardless of -a setting
    const size_t SQZ_BYTES_PER_SAMPLE = 4;
    size_t bytes_per_frame = SQZ_BYTES_PER_SAMPLE * format.channels;  // Always 8 for stereo
    size_t buffer_size = CHUNK_SIZE * bytes_per_frame;

    std::vector<uint8_t> buffer(buffer_size);
    std::vector<uint8_t> pcm_convert_buffer;
    if (output_bit_depth < 32) {
        pcm_convert_buffer.resize(buffer_size);  // Always enough (output <= input)
    }
    uint64_t total_bytes = 0;
    uint64_t total_frames = 0;

    // For rate limiting - track timing to send at correct sample rate
    auto start_time = std::chrono::steady_clock::now();
    uint64_t frames_sent = 0;
    unsigned int rate_for_timing = format.sampleRate;  // Rate to use for timing calculations

    while (running) {
        // Check if Diretta needs to be reopened (sample rate or format change)
        if (g_need_reopen.load()) {
            unsigned int squeezelite_rate = g_current_sample_rate.load();  // Frame rate from squeezelite
            bool is_dsd = g_is_dsd.load();
            DSDFormatType dsd_format = static_cast<DSDFormatType>(g_dsd_format_type.load());
            g_need_reopen.store(false);

            // Calculate actual DSD bit rate and format parameters
            unsigned int actual_rate = squeezelite_rate;
            unsigned int bit_depth = static_cast<unsigned int>(output_bit_depth);

            if (is_dsd) {
                if (dsd_format == DSDFormatType::U32_BE || dsd_format == DSDFormatType::U32_LE) {
                    // Native DSD: squeezelite frame rate needs to be multiplied by 32
                    // to get the true DSD bit rate
                    // Example: DSD64 → squeezelite reports 88200 Hz → actual = 88200 * 32 = 2822400 Hz
                    actual_rate = squeezelite_rate * 32;
                    bit_depth = 1;  // DSD is 1-bit
                } else if (dsd_format == DSDFormatType::DOP) {
                    // DoP: Convert to native DSD for Diretta Target
                    // DoP at 176400 Hz contains 16 DSD bits per sample → DSD64 at 2822400 Hz
                    // DoP rate × 16 = DSD bit rate
                    actual_rate = squeezelite_rate * 16;  // 176400 × 16 = 2822400 for DSD64
                    bit_depth = 1;  // Native DSD is 1-bit
                }
            }

            // For DoP: convert to native DSD (Diretta Target may not support DoP passthrough)
            bool diretta_is_dsd = is_dsd;  // Both native DSD and DoP→DSD are DSD for Diretta

            std::cout << "\n[Reopening Diretta] ";
            if (dsd_format == DSDFormatType::DOP) {
                std::cout << "DoP→DSD (native) at " << actual_rate << "Hz";
                std::cout << " (DoP rate: " << squeezelite_rate << " Hz)";
            } else if (is_dsd) {
                std::cout << "DSD at " << actual_rate << "Hz";
                std::cout << " (squeezelite frame rate: " << squeezelite_rate << " Hz)";
            } else {
                std::cout << "PCM at " << actual_rate << "Hz";
            }
            std::cout << std::endl;

            // NOTE: Do NOT call close() here!
            // DirettaSync::open() has sophisticated format change handling that
            // properly handles DSD→PCM transitions (full SDK close/reopen).
            // Calling close() first sets m_open=false, which bypasses that logic.

            // CRITICAL: Wait for squeezelite to complete its format transition
            // The format change log arrives BEFORE all old-format data is flushed.
            // For extreme transitions (DSD512↔high-rate PCM), we need to wait
            // for squeezelite's internal buffers to drain to the pipe.
            bool isExtremeTransition =
                (format.isDSD && actual_rate >= 11289600 && !is_dsd) ||  // DSD256+ → PCM
                (!format.isDSD && format.sampleRate >= 176400 && is_dsd) || // High PCM → DSD
                (format.isDSD && !is_dsd && actual_rate >= 176400);  // DSD → High PCM

            if (isExtremeTransition) {
                std::cout << "[Format Transition] Extreme transition detected - waiting for squeezelite flush..." << std::endl;
                // Wait for squeezelite to flush its internal buffers
                // Squeezelite has ~1-2 seconds of internal buffering
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }

            // Drain residual data - wait for pipe to have data, then drain aggressively
            {
                struct pollfd pfd;
                pfd.fd = fifo_fd;
                pfd.events = POLLIN;

                std::vector<uint8_t> drain_buffer(16384);  // Larger drain buffer
                size_t total_drained = 0;
                size_t drain_limit = isExtremeTransition ? 262144 : 65536;  // 256KB for extreme, 64KB otherwise

                // For extreme transitions, do multiple drain rounds with small waits
                int drain_rounds = isExtremeTransition ? 5 : 1;
                for (int round = 0; round < drain_rounds && total_drained < drain_limit; round++) {
                    if (round > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }

                    // Drain what's available
                    while (total_drained < drain_limit) {
                        int ret = poll(&pfd, 1, 0);  // Non-blocking check
                        if (ret > 0 && (pfd.revents & POLLIN)) {
                            ssize_t drained = read(fifo_fd, drain_buffer.data(), drain_buffer.size());
                            if (drained > 0) {
                                total_drained += drained;
                            } else {
                                break;
                            }
                        } else {
                            break;  // No more data available
                        }
                    }
                }

                if (total_drained > 0 && g_verbose) {
                    std::cout << "[Format Transition] Drained " << total_drained
                              << " bytes of residual data" << std::endl;
                }
            }

            // Update format
            // For DoP: isDSD=false because DirettaSync should treat it as PCM
            // The DAC will recognize the DoP markers (0x05/0xFA) and extract DSD
            format.isDSD = diretta_is_dsd;
            format.sampleRate = actual_rate;  // True DSD bit rate or PCM rate
            format.isCompressed = false;
            format.bitDepth = bit_depth;  // 1 for native DSD, 32 for DoP/PCM

            // CRITICAL: Tell DirettaSync the source DSD format
            // For native DSD and DoP→DSD conversion, use DFF (MSB) format
            if (diretta_is_dsd) {
                format.dsdFormat = AudioFormat::DSDFormat::DFF;  // MSB (no bit reversal by DirettaSync)
                if (dsd_format == DSDFormatType::DOP) {
                    std::cout << "[DSD Format] DoP→DSD conversion, output as DFF (MSB)" << std::endl;
                } else {
                    std::cout << "[DSD Format] Set to DFF (MSB) - byte-swap in de-interleave" << std::endl;
                }
            }

            // Set rate for timing calculations
            // For native DSD and DoP: use squeezelite frame rate (not bit rate)
            // For PCM: use actual rate
            if (is_dsd) {
                rate_for_timing = squeezelite_rate;  // Frame rate (e.g., 88200 Hz for native, 176400 Hz for DoP)
            } else {
                rate_for_timing = actual_rate;  // PCM rate
            }

            // Reopen Diretta with new format
            if (!g_diretta->open(format)) {
                std::cerr << "Failed to reopen Diretta with new format" << std::endl;
                running = false;
                break;
            }

            // Calculate new bytes per frame first
            size_t new_bytes_per_frame;
            if (is_dsd) {
                new_bytes_per_frame = 4 * format.channels;  // 4 bytes * 2 ch = 8
            } else {
                // Always read 32-bit from squeezelite, regardless of output bit depth
                new_bytes_per_frame = SQZ_BYTES_PER_SAMPLE * format.channels;
            }

            // Set up buffer parameters for burst-fill
            bytes_per_frame = new_bytes_per_frame;
            buffer_size = CHUNK_SIZE * bytes_per_frame;
            buffer.resize(buffer_size);

            // ================================================================
            // BURST-FILL: Fill ring buffer until prefill is complete
            // ================================================================
            // This escapes the "equilibrium trap" where push rate equals pull rate,
            // causing prefill to never be reached. By disabling rate limiting until
            // prefill completes, we ensure the ring buffer fills faster than it drains.
            //
            // Without burst-fill: wrapper sends 12KB silence, prefill needs 4.5MB,
            // push rate = pull rate → ring buffer level stays constant → silence forever
            //
            // With burst-fill: wrapper pushes data as fast as pipe provides it,
            // ring buffer fills to prefill threshold, then normal playback begins.
            // ================================================================

            // Determine clock family for diagnostic purposes
            int clockFamily = (actual_rate % 44100 == 0) ? 441 :
                              (actual_rate % 48000 == 0) ? 480 : 0;

            if (g_verbose) {
                std::cout << "[Burst Fill] Starting prefill for " << (format.isDSD ? "DSD" : "PCM")
                          << " at " << actual_rate << "Hz (x" << clockFamily << " family)..." << std::endl;
                std::cout << "[Burst Fill] Prefill target: " << g_diretta->getPrefillTarget()
                          << " bytes" << std::endl;
                std::cout << "[Burst Fill] bytes_per_frame=" << bytes_per_frame
                          << " buffer_size=" << buffer_size << std::endl;
            }

            // DIAGNOSTIC: Log first DSD packet details for comparison
            bool first_dsd_packet_logged = false;

            auto burst_start = std::chrono::steady_clock::now();
            const auto burst_timeout = std::chrono::seconds(5);  // Safety timeout
            size_t burst_bytes_sent = 0;
            int burst_iterations = 0;
            int silence_fills = 0;

            // Planar buffer for DSD conversion during burst-fill
            std::vector<uint8_t> burst_planar_buffer(buffer_size);

            while (!g_diretta->isPrefillComplete() && running) {
                burst_iterations++;

                // Check timeout
                auto elapsed = std::chrono::steady_clock::now() - burst_start;
                if (elapsed > burst_timeout) {
                    std::cerr << "[Burst Fill] WARNING: Prefill timeout after 5s. "
                              << "Sent " << burst_bytes_sent << " bytes in "
                              << burst_iterations << " iterations." << std::endl;
                    break;
                }

                // Try to read data (with short timeout to stay responsive)
                struct pollfd pfd = {fifo_fd, POLLIN, 0};
                int ret = poll(&pfd, 1, 50);  // 50ms timeout

                if (ret > 0 && (pfd.revents & POLLIN)) {
                    ssize_t bytes_read = read(fifo_fd, buffer.data(), buffer_size);

                    if (bytes_read > 0) {
                        size_t num_frames = static_cast<size_t>(bytes_read) / bytes_per_frame;
                        size_t num_samples;

                        // Process and send based on format (same logic as main loop)
                        if (format.isDSD && dsd_format == DSDFormatType::DOP) {
                            // DoP → Native DSD conversion
                            size_t dsd_bytes_per_frame = 2 * format.channels;
                            size_t output_size = num_frames * dsd_bytes_per_frame;
                            if (burst_planar_buffer.size() < output_size) {
                                burst_planar_buffer.resize(output_size);
                            }
                            size_t bytes_per_channel = output_size / format.channels;

                            for (size_t frame = 0; frame < num_frames; frame++) {
                                size_t src_offset = frame * bytes_per_frame;
                                size_t dst_offset_L = frame * 2;
                                size_t dst_offset_R = bytes_per_channel + frame * 2;

                                burst_planar_buffer[dst_offset_L + 0] = buffer[src_offset + 2];
                                burst_planar_buffer[dst_offset_L + 1] = buffer[src_offset + 1];
                                burst_planar_buffer[dst_offset_R + 0] = buffer[src_offset + 6];
                                burst_planar_buffer[dst_offset_R + 1] = buffer[src_offset + 5];
                            }

                            num_samples = (output_size * 8) / format.channels;
                            g_diretta->sendAudio(burst_planar_buffer.data(), num_samples);
                            burst_bytes_sent += output_size;

                        } else if (format.isDSD && (dsd_format == DSDFormatType::U32_BE ||
                                                     dsd_format == DSDFormatType::U32_LE)) {
                            // Native DSD: interleaved → planar with byte-swap
                            if (burst_planar_buffer.size() < static_cast<size_t>(bytes_read)) {
                                burst_planar_buffer.resize(bytes_read);
                            }
                            size_t bytes_per_channel = bytes_read / format.channels;

                            for (size_t frame = 0; frame < num_frames; frame++) {
                                size_t src_offset = frame * bytes_per_frame;
                                size_t dst_offset_L = frame * 4;
                                size_t dst_offset_R = bytes_per_channel + frame * 4;

                                // Byte-swap: Squeezelite stdout writes LE memory order
                                burst_planar_buffer[dst_offset_L + 0] = buffer[src_offset + 3];
                                burst_planar_buffer[dst_offset_L + 1] = buffer[src_offset + 2];
                                burst_planar_buffer[dst_offset_L + 2] = buffer[src_offset + 1];
                                burst_planar_buffer[dst_offset_L + 3] = buffer[src_offset + 0];
                                burst_planar_buffer[dst_offset_R + 0] = buffer[src_offset + 7];
                                burst_planar_buffer[dst_offset_R + 1] = buffer[src_offset + 6];
                                burst_planar_buffer[dst_offset_R + 2] = buffer[src_offset + 5];
                                burst_planar_buffer[dst_offset_R + 3] = buffer[src_offset + 4];
                            }

                            num_samples = (static_cast<size_t>(bytes_read) * 8) / format.channels;

                            // DIAGNOSTIC: Log first DSD packet for clock family comparison
                            if (!first_dsd_packet_logged && g_verbose && buffer[0] != 0x69) {
                                first_dsd_packet_logged = true;
                                std::cout << "[DIAG] First DSD packet (x" << clockFamily << " family):" << std::endl;
                                std::cout << "  num_frames=" << num_frames << " bytes_read=" << bytes_read
                                          << " num_samples=" << num_samples << std::endl;
                                std::cout << "  Raw input (first 32 bytes): ";
                                for (int i = 0; i < 32 && i < bytes_read; i++) {
                                    printf("%02x ", buffer[i]);
                                }
                                std::cout << std::endl;
                            }

                            g_diretta->sendAudio(burst_planar_buffer.data(), num_samples);
                            burst_bytes_sent += bytes_read;

                        } else {
                            // PCM: convert bit depth if needed, then send
                            num_samples = num_frames;
                            if (output_bit_depth < 32) {
                                size_t out_bytes = convert_pcm_bit_depth(
                                    buffer.data(), burst_planar_buffer.data(),
                                    num_frames, format.channels, output_bit_depth);
                                g_diretta->sendAudio(burst_planar_buffer.data(), num_samples);
                                burst_bytes_sent += out_bytes;
                            } else {
                                g_diretta->sendAudio(buffer.data(), num_samples);
                                burst_bytes_sent += bytes_read;
                            }
                        }
                    }
                } else {
                    // No data available - send silence to help reach prefill
                    // This prevents deadlock if pipe is temporarily empty
                    silence_fills++;
                    const size_t SILENCE_CHUNK = 4096;
                    std::vector<uint8_t> silence(SILENCE_CHUNK, format.isDSD ? 0x69 : 0x00);
                    size_t silence_samples = format.isDSD ?
                        (SILENCE_CHUNK * 8) / format.channels : SILENCE_CHUNK / bytes_per_frame;
                    g_diretta->sendAudio(silence.data(), silence_samples);
                    burst_bytes_sent += SILENCE_CHUNK;
                }

                // Log progress periodically (verbose mode only)
                if (g_verbose && burst_iterations % 20 == 0) {
                    float level = g_diretta->getBufferLevel() * 100.0f;
                    std::cout << "[Burst Fill] Progress: " << std::fixed << std::setprecision(1)
                              << level << "%, sent " << burst_bytes_sent << " bytes"
                              << " (silence fills: " << silence_fills << ")" << std::endl;
                }
            }

            // Report burst-fill results (verbose mode only)
            if (g_verbose) {
                auto burst_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - burst_start);
                std::cout << "[Burst Fill] Complete: " << burst_bytes_sent << " bytes in "
                          << burst_elapsed.count() << "ms (" << burst_iterations << " iterations, "
                          << silence_fills << " silence fills)" << std::endl;
            }

            // Reset timing for rate-limited steady-state
            start_time = std::chrono::steady_clock::now();
            frames_sent = 0;

            std::cout << "[Diretta Reopened] Ready for ";
            if (dsd_format == DSDFormatType::DOP) {
                std::cout << "DoP→DSD";
            } else if (diretta_is_dsd) {
                std::cout << "DSD";
            } else {
                std::cout << "PCM";
            }
            std::cout << " at " << actual_rate << "Hz" << std::endl;
        }

        ssize_t bytes_read = read(fifo_fd, buffer.data(), buffer_size);

        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                std::cout << "Squeezelite closed" << std::endl;
            } else if (errno != EINTR) {
                std::cerr << "Error reading from squeezelite: " << strerror(errno) << std::endl;
            }
            break;
        }

        // Calculate number of samples for DirettaSync
        size_t num_frames = static_cast<size_t>(bytes_read) / bytes_per_frame;
        size_t num_samples;

        if (format.isDSD) {
            // For DSD: numSamples = (totalBytes * 8) / channels
            // This represents the total number of DSD bits divided by channels
            num_samples = (static_cast<size_t>(bytes_read) * 8) / format.channels;
        } else {
            // For PCM: samples = frames (each frame is one sample per channel)
            num_samples = num_frames;
        }

        // Debug first few reads and periodic sample range checks
        bool show_detail = (total_frames < CHUNK_SIZE * 5);
        unsigned int current_rate = format.sampleRate;
        bool show_periodic = (total_frames % (current_rate * 2) < CHUNK_SIZE);  // Every 2 seconds

        if (g_verbose && (show_detail || show_periodic)) {
            std::cout << "Read: " << bytes_read << " bytes, " << num_frames << " frames" << std::endl;

            // Check if data is silence (all zeros) or has audio content
            int32_t* samples = reinterpret_cast<int32_t*>(buffer.data());
            int32_t max_sample = 0;
            int32_t min_sample = 0;
            for (size_t i = 0; i < num_frames * 2 && i < 100; i++) {  // Check first 100 samples
                if (samples[i] > max_sample) max_sample = samples[i];
                if (samples[i] < min_sample) min_sample = samples[i];
            }
            std::cout << "  Sample range: [" << min_sample << " .. " << max_sample << "]" << std::endl;

            // Show first few bytes in hex (only for initial reads)
            if (show_detail) {
                std::cout << "  First 16 bytes: ";
                for (int i = 0; i < 16 && i < bytes_read; i++) {
                    printf("%02x ", buffer[i]);
                }
                std::cout << std::endl;
            }
        }

        // Send to Diretta
        size_t written;
        DSDFormatType current_dsd_format = static_cast<DSDFormatType>(g_dsd_format_type.load());

        if (format.isDSD && current_dsd_format == DSDFormatType::DOP) {
            // DoP → Native DSD conversion
            // DoP format (S32_LE): [padding][DSD_LSB][DSD_MSB][marker]
            // Each 32-bit sample contains 16 bits of DSD data
            // Output: planar native DSD [L L L...][R R R...]

            // Output is half the size (16 bits out of 32 bits per sample)
            size_t dsd_bytes_per_frame = 2 * format.channels;  // 2 bytes per channel
            size_t output_size = num_frames * dsd_bytes_per_frame;
            std::vector<uint8_t> planar_buffer(output_size);
            size_t bytes_per_channel = output_size / format.channels;

            // Extract DSD data and de-interleave
            for (size_t frame = 0; frame < num_frames; frame++) {
                size_t src_offset = frame * bytes_per_frame;  // 8 bytes per stereo DoP frame
                size_t dst_offset_L = frame * 2;  // 2 bytes per DSD group
                size_t dst_offset_R = bytes_per_channel + frame * 2;

                // Extract DSD from L channel (bytes 1-2 of 32-bit sample, MSB first for DFF)
                planar_buffer[dst_offset_L + 0] = buffer[src_offset + 2];  // DSD MSB
                planar_buffer[dst_offset_L + 1] = buffer[src_offset + 1];  // DSD LSB

                // Extract DSD from R channel
                planar_buffer[dst_offset_R + 0] = buffer[src_offset + 6];  // DSD MSB
                planar_buffer[dst_offset_R + 1] = buffer[src_offset + 5];  // DSD LSB
            }

            // Debug: Show DoP conversion for first packet with real data
            static bool shown_dop_conversion = false;
            if (!shown_dop_conversion && buffer[3] != 0) {
                shown_dop_conversion = true;
                std::cout << "\n[DEBUG] DoP→DSD Conversion:" << std::endl;
                std::cout << "  DoP marker (should be 0x05 or 0xFA): 0x"
                          << std::hex << (int)buffer[3] << std::dec << std::endl;
                std::cout << "  Input (DoP) 8 bytes: ";
                for (int i = 0; i < 8; i++) printf("%02x ", buffer[i]);
                std::cout << std::endl;
                std::cout << "  Output (DSD) 4 bytes: ";
                for (int i = 0; i < 2; i++) printf("%02x ", planar_buffer[i]);
                std::cout << "| ";
                for (int i = 0; i < 2; i++) printf("%02x ", planar_buffer[bytes_per_channel + i]);
                std::cout << std::endl << std::endl;
            }

            // Calculate DSD samples for DirettaSync
            // Each byte = 8 DSD bits, so total DSD bits = output_size * 8 / channels
            num_samples = (output_size * 8) / format.channels;
            written = g_diretta->sendAudio(planar_buffer.data(), num_samples);

        } else if (format.isDSD && (current_dsd_format == DSDFormatType::U32_BE ||
                                     current_dsd_format == DSDFormatType::U32_LE)) {
            // For native DSD: Convert from interleaved to planar format
            // Squeezelite sends: [L0L0L0L0 R0R0R0R0 L1L1L1L1 R1R1R1R1...]
            // DirettaSync expects: [L0L0L0L0 L1L1L1L1...][R0R0R0R0 R1R1R1R1...]

            std::vector<uint8_t> planar_buffer(bytes_read);
            size_t bytes_per_channel = bytes_read / format.channels;

            // De-interleave: separate L and R channels
            // Squeezelite's dsd.c packs DSD bytes into uint32_t with byte[0] at MSB (<<24).
            // On a little-endian machine, this uint32_t is stored in memory as [LSB...MSB],
            // and output_stdout.c writes it via S32_LE format (memcpy), preserving LE order.
            // So on the pipe: byte[0]=last DSD byte, byte[3]=first DSD byte.
            // We must byte-swap to restore correct temporal order (first DSD byte first).
            for (size_t frame = 0; frame < num_frames; frame++) {
                size_t src_offset = frame * bytes_per_frame;
                size_t dst_offset_L = frame * 4;  // 4 bytes per DSD group
                size_t dst_offset_R = bytes_per_channel + frame * 4;

                // L channel: byte-swap from LE pipe order to correct temporal order
                planar_buffer[dst_offset_L + 0] = buffer[src_offset + 3];
                planar_buffer[dst_offset_L + 1] = buffer[src_offset + 2];
                planar_buffer[dst_offset_L + 2] = buffer[src_offset + 1];
                planar_buffer[dst_offset_L + 3] = buffer[src_offset + 0];

                // R channel: byte-swap
                planar_buffer[dst_offset_R + 0] = buffer[src_offset + 7];
                planar_buffer[dst_offset_R + 1] = buffer[src_offset + 6];
                planar_buffer[dst_offset_R + 2] = buffer[src_offset + 5];
                planar_buffer[dst_offset_R + 3] = buffer[src_offset + 4];
            }

            // Debug: Show before/after conversion for first packet with real data
            static bool shown_conversion = false;
            if (!shown_conversion && buffer[0] != 0) {
                shown_conversion = true;
                std::cout << "\n[DEBUG] DSD Conversion Example:" << std::endl;
                std::cout << "  Input (interleaved), first 32 bytes:" << std::endl;
                std::cout << "    ";
                for (int i = 0; i < 32 && i < bytes_read; i++) {
                    printf("%02x ", buffer[i]);
                    if ((i + 1) % 8 == 0) std::cout << std::endl << "    ";
                }
                std::cout << std::endl;
                std::cout << "  Output (planar), first 16 bytes L + first 16 bytes R:" << std::endl;
                std::cout << "    L: ";
                for (int i = 0; i < 16 && i < bytes_per_channel; i++) {
                    printf("%02x ", planar_buffer[i]);
                }
                std::cout << std::endl << "    R: ";
                for (int i = 0; i < 16 && i < bytes_per_channel; i++) {
                    printf("%02x ", planar_buffer[bytes_per_channel + i]);
                }
                std::cout << std::endl << std::endl;
            }

            written = g_diretta->sendAudio(planar_buffer.data(), num_samples);
        } else {
            // PCM: convert bit depth if needed, then send
            if (output_bit_depth < 32) {
                convert_pcm_bit_depth(buffer.data(), pcm_convert_buffer.data(),
                                      num_frames, format.channels, output_bit_depth);
                written = g_diretta->sendAudio(pcm_convert_buffer.data(), num_samples);
            } else {
                written = g_diretta->sendAudio(buffer.data(), num_samples);
            }
        }

        // Debug first few writes
        if (g_verbose && show_detail) {
            std::cout << "Sent: " << written << " bytes to Diretta" << std::endl;
        }

        if (written == 0 && g_diretta->isPlaying()) {
            // Buffer might be full, yield and retry
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        total_bytes += static_cast<uint64_t>(bytes_read);
        total_frames += num_frames;
        frames_sent += num_frames;

        // Rate limiting: Sleep to maintain correct playback speed
        // Calculate expected time for frames sent so far
        // Use rate_for_timing (frame rate for DSD, sample rate for PCM)
        auto expected_time = start_time + std::chrono::microseconds(
            (frames_sent * 1000000ULL) / rate_for_timing);
        auto now = std::chrono::steady_clock::now();

        // If we're ahead of schedule, sleep
        if (now < expected_time) {
            std::this_thread::sleep_until(expected_time);
        }

        // Print progress every ~10 seconds
        if (g_verbose && total_frames % (rate_for_timing * 10) < CHUNK_SIZE) {
            double seconds = static_cast<double>(total_frames) / static_cast<double>(rate_for_timing);
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

    close(fifo_fd);

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
