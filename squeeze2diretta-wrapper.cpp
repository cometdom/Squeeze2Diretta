/**
 * @file squeeze2diretta-wrapper.cpp
 * @brief Wrapper that bridges squeezelite STDOUT to Diretta output
 *
 * v2.0: In-band format signaling via 16-byte binary headers ("SQFH")
 * embedded in the audio stream by a patched squeezelite. This eliminates
 * the race condition of async stderr log parsing used in v1.x.
 *
 * Architecture:
 *   LMS -> squeezelite (patched) -> STDOUT [header|audio|header|audio|...]
 *     -> wrapper -> DirettaSync -> Diretta DAC
 *
 * Uses DirettaSync from DirettaRendererUPnP v2.0 for low-latency streaming:
 * - Lock-free ring buffer with SIMD optimizations (AVX2/AVX512)
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
#include <mutex>
#include <chrono>
#include <sstream>

// Version
#define WRAPPER_VERSION "2.0.1"

// ================================================================
// In-band format header (must match squeezelite output_stdout.c)
// ================================================================
struct __attribute__((packed)) SqFormatHeader {
    uint8_t  magic[4];       // "SQFH"
    uint8_t  version;        // Protocol version: 1
    uint8_t  channels;       // 2 for stereo
    uint8_t  bit_depth;      // PCM: 16/24/32, DSD: 1, DoP: 24
    uint8_t  dsd_format;     // 0=PCM, 1=DOP, 2=DSD_U32_LE, 3=DSD_U32_BE
    uint32_t sample_rate;    // Sample/frame rate in Hz (LE)
    uint8_t  reserved[4];    // Zero-filled
};

static_assert(sizeof(SqFormatHeader) == 16, "SqFormatHeader must be 16 bytes");

static constexpr uint8_t SQFH_MAGIC[4] = {'S', 'Q', 'F', 'H'};

// DSD format types (from header dsd_format field)
enum class DSDFormatType : uint8_t {
    NONE   = 0,  // PCM
    DOP    = 1,  // DSD over PCM
    U32_LE = 2,  // Native DSD Little Endian
    U32_BE = 3   // Native DSD Big Endian
};

// ================================================================
// Buffered pipe reader with peek support
// ================================================================
class PipeReader {
public:
    explicit PipeReader(int fd) : m_fd(fd), m_pos(0), m_len(0) {}

    // Read exactly n bytes (blocking). Returns false on EOF/error.
    bool readExact(void* dst, size_t n) {
        uint8_t* out = static_cast<uint8_t*>(dst);
        size_t remaining = n;

        while (remaining > 0) {
            // Serve from internal buffer first
            size_t avail = m_len - m_pos;
            if (avail > 0) {
                size_t chunk = std::min(avail, remaining);
                memcpy(out, m_buf + m_pos, chunk);
                m_pos += chunk;
                out += chunk;
                remaining -= chunk;
                continue;
            }

            // Buffer empty — refill from pipe
            ssize_t n_read = ::read(m_fd, m_buf, sizeof(m_buf));
            if (n_read <= 0) return false;  // EOF or error
            m_pos = 0;
            m_len = static_cast<size_t>(n_read);
        }
        return true;
    }

    // Peek at the next n bytes without consuming. Returns false if EOF/error
    // or fewer than n bytes available after one refill attempt.
    bool peek(void* dst, size_t n) {
        // If we have enough buffered data, peek directly
        size_t avail = m_len - m_pos;
        if (avail >= n) {
            memcpy(dst, m_buf + m_pos, n);
            return true;
        }

        // Compact buffer and refill
        if (avail > 0 && m_pos > 0) {
            memmove(m_buf, m_buf + m_pos, avail);
        }
        m_pos = 0;
        m_len = avail;

        // Read more data
        while (m_len < n) {
            ssize_t n_read = ::read(m_fd, m_buf + m_len, sizeof(m_buf) - m_len);
            if (n_read <= 0) return false;
            m_len += static_cast<size_t>(n_read);
        }

        memcpy(dst, m_buf, n);
        return true;
    }

    // Read up to n bytes (non-exact). Returns bytes read, or <=0 on EOF/error.
    // Stops before any embedded SQFH header to prevent consuming it as audio.
    ssize_t readUpTo(void* dst, size_t n) {
        // Always go through internal buffer so we can scan for headers
        size_t avail = m_len - m_pos;
        if (avail == 0) {
            // Buffer empty — refill from pipe
            ssize_t n_read = ::read(m_fd, m_buf, sizeof(m_buf));
            if (n_read <= 0) return n_read;
            m_pos = 0;
            m_len = static_cast<size_t>(n_read);
            avail = m_len;
        }

        size_t chunk = std::min(avail, n);

        // Scan for SQFH header embedded in the data.
        // Start at byte 1 — byte 0 was already verified by peek() to not be 'S'QFH.
        for (size_t i = 1; i + 3 <= chunk; i++) {
            if (m_buf[m_pos + i] == 'S' && m_buf[m_pos + i + 1] == 'Q' &&
                m_buf[m_pos + i + 2] == 'F' && m_buf[m_pos + i + 3] == 'H') {
                chunk = i;  // Stop before the header
                break;
            }
        }

        memcpy(dst, m_buf + m_pos, chunk);
        m_pos += chunk;
        return static_cast<ssize_t>(chunk);
    }

private:
    int m_fd;
    size_t m_pos;
    size_t m_len;
    uint8_t m_buf[65536];
};

// ================================================================
// Global state
// ================================================================
static pid_t squeezelite_pid = 0;
static bool running = true;
static std::unique_ptr<DirettaSync> g_diretta;

// Signal handler for clean shutdown
void signal_handler(int sig) {
    std::cout << "\nSignal " << sig << " received, shutting down..." << std::endl;
    running = false;

    if (squeezelite_pid > 0) {
        kill(squeezelite_pid, SIGTERM);
    }
}

// SIGUSR1 handler for runtime stats
void stats_signal_handler(int /*sig*/) {
    if (g_diretta) {
        g_diretta->dumpStats();
    }
}

// ================================================================
// Configuration
// ================================================================
struct Config {
    // Squeezelite options
    std::string lms_server = "";
    std::string player_name = "squeeze2diretta";
    std::string mac_address = "";
    std::string model_name = "SqueezeLite";
    std::string codecs = "";
    std::string rates = "";
    int sample_format = 32;              // -a (16, 24, or 32)
    std::string dsd_format = ":u32be";   // -D format
    bool wav_header = false;             // -W

    // Diretta options
    int diretta_target = 0;
    int thread_mode = 1;
    unsigned int cycle_time = 2620;
    bool cycle_time_auto = true;
    unsigned int mtu = 0;

    // Other
    bool verbose = false;
    bool quiet = false;
    bool list_targets = false;
    std::string squeezelite_path = "squeezelite";
};

void print_usage(const char* prog) {
    std::cout << "squeeze2diretta v" << WRAPPER_VERSION << std::endl;
    std::cout << "Squeezelite to Diretta Bridge" << std::endl;
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
    std::cout << "  -a <format>           Sample format: 16, 24, or 32 (default)" << std::endl;
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
    std::cout << "  -v                    Verbose output (debug level)" << std::endl;
    std::cout << "  -q, --quiet           Quiet mode (warnings and errors only)" << std::endl;
    std::cout << "  -h, --help            Show this help" << std::endl;
    std::cout << "  --squeezelite <path>  Path to squeezelite binary" << std::endl;
    std::cout << std::endl;
    std::cout << "NOTE: Requires patched squeezelite with in-band format headers." << std::endl;
    std::cout << "      Run setup-squeezelite.sh to build the patched version." << std::endl;
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
        else if (arg == "-q" || arg == "--quiet") {
            config.quiet = true;
        }
        else if (arg == "-W") {
            config.wav_header = true;
        }
        else if (arg == "-D") {
            if (i + 1 < argc && argv[i + 1][0] == ':') {
                config.dsd_format = argv[++i];
            } else {
                config.dsd_format = "dop";
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
            else if (arg == "-t" || arg == "--target") config.diretta_target = std::stoi(value) - 1;
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

    if (config.wav_header) {
        args.push_back("-W");
    }

    // Output to stdout
    args.push_back("-o");
    args.push_back(output_path);

    // Sample rates
    args.push_back("-r");
    if (!config.rates.empty()) {
        args.push_back("44100-" + config.rates);
    } else {
        args.push_back("44100-768000");
    }

    // Player name
    args.push_back("-n");
    args.push_back(config.player_name);

    // Model name
    args.push_back("-M");
    args.push_back(config.model_name);

    // LMS server
    if (!config.lms_server.empty()) {
        args.push_back("-s");
        args.push_back(config.lms_server);
    }

    // MAC address
    if (!config.mac_address.empty()) {
        args.push_back("-m");
        args.push_back(config.mac_address);
    }

    // Codecs
    if (!config.codecs.empty()) {
        args.push_back("-c");
        args.push_back(config.codecs);
    }

    // DSD output format
    args.push_back("-D");
    if (config.dsd_format != "dop") {
        args.push_back(config.dsd_format);
    }

    // v2.0: We still enable info-level logging for squeezelite's own diagnostics
    // (visible on stderr which passes through to parent), but we no longer parse it.
    args.push_back("-d");
    args.push_back("all=info");

    return args;
}

// ================================================================
// DSD de-interleave: interleaved S32_LE → planar with byte-swap
//
// Squeezelite packs DSD bytes MSB-first into uint32_t (dsd.c),
// then outputs S32_LE (little-endian) via output_stdout.c.
// On the pipe: byte[0]=last DSD byte, byte[3]=first DSD byte.
// We byte-swap to restore correct temporal order.
// ================================================================
static void deinterleave_dsd_native(const uint8_t* src, uint8_t* dst,
                                     size_t num_frames, size_t bytes_per_frame,
                                     size_t channels) {
    size_t bytes_per_channel = num_frames * 4;

    for (size_t frame = 0; frame < num_frames; frame++) {
        size_t src_offset = frame * bytes_per_frame;
        size_t dst_offset_L = frame * 4;
        size_t dst_offset_R = bytes_per_channel + frame * 4;

        // L channel: byte-swap from LE pipe order to correct temporal order
        dst[dst_offset_L + 0] = src[src_offset + 3];
        dst[dst_offset_L + 1] = src[src_offset + 2];
        dst[dst_offset_L + 2] = src[src_offset + 1];
        dst[dst_offset_L + 3] = src[src_offset + 0];

        // R channel: byte-swap
        dst[dst_offset_R + 0] = src[src_offset + 7];
        dst[dst_offset_R + 1] = src[src_offset + 6];
        dst[dst_offset_R + 2] = src[src_offset + 5];
        dst[dst_offset_R + 3] = src[src_offset + 4];
    }
}

// ================================================================
// DoP → Native DSD conversion: extract DSD bits from DoP S32_LE
//
// DoP format (S32_LE): [padding][DSD_LSB][DSD_MSB][marker]
// Each 32-bit sample contains 16 bits of DSD data.
// Output: planar native DSD [L L L...][R R R...]
// ================================================================
static void convert_dop_to_native_dsd(const uint8_t* src, uint8_t* dst,
                                       size_t num_frames, size_t bytes_per_frame,
                                       size_t channels) {
    size_t dsd_bytes_per_frame = 2 * channels;
    size_t output_size = num_frames * dsd_bytes_per_frame;
    size_t bytes_per_channel = output_size / channels;

    for (size_t frame = 0; frame < num_frames; frame++) {
        size_t src_offset = frame * bytes_per_frame;
        size_t dst_offset_L = frame * 2;
        size_t dst_offset_R = bytes_per_channel + frame * 2;

        // Extract DSD from L channel (bytes 1-2, MSB first for DFF)
        dst[dst_offset_L + 0] = src[src_offset + 2];  // DSD MSB
        dst[dst_offset_L + 1] = src[src_offset + 1];  // DSD LSB

        // Extract DSD from R channel
        dst[dst_offset_R + 0] = src[src_offset + 6];  // DSD MSB
        dst[dst_offset_R + 1] = src[src_offset + 5];  // DSD LSB
    }
}

// ================================================================
// Main
// ================================================================
int main(int argc, char* argv[]) {

    std::cout << "================================================================" << std::endl;
    std::cout << "  squeeze2diretta v" << WRAPPER_VERSION << std::endl;
    std::cout << "  Squeezelite to Diretta Bridge" << std::endl;
    std::cout << "  Using DirettaSync from DirettaRendererUPnP v2.0" << std::endl;
    std::cout << "  In-band format signaling (no stderr parsing)" << std::endl;
    std::cout << "================================================================" << std::endl;
    std::cout << std::endl;

    // Parse arguments
    Config config = parse_args(argc, argv);

    // Validate PCM output bit depth
    const int output_bit_depth = config.sample_format;
    if (output_bit_depth != 16 && output_bit_depth != 24 && output_bit_depth != 32) {
        LOG_ERROR("Invalid sample format: " << output_bit_depth << " (must be 16, 24, or 32)");
        return 1;
    }

    g_verbose = config.verbose;
    if (config.verbose) {
        g_logLevel = LogLevel::DEBUG;
    } else if (config.quiet) {
        g_logLevel = LogLevel::WARN;
    }

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
    signal(SIGUSR1, stats_signal_handler);

    // Create DirettaSync instance
    g_diretta = std::make_unique<DirettaSync>();

    DirettaConfig direttaConfig;
    direttaConfig.threadMode = config.thread_mode;
    direttaConfig.cycleTime = config.cycle_time;
    direttaConfig.cycleTimeAuto = config.cycle_time_auto;
    direttaConfig.mtu = config.mtu;

    if (config.diretta_target >= 0) {
        g_diretta->setTargetIndex(config.diretta_target);
    }
    if (config.mtu > 0) {
        g_diretta->setMTU(config.mtu);
    }

    LOG_INFO("Initializing Diretta...");

    if (!g_diretta->enable(direttaConfig)) {
        LOG_ERROR("Failed to enable Diretta. Check that a Diretta target is available.");
        LOG_ERROR("Use -l to list available targets.");
        if (g_logRing) delete g_logRing;
        return 1;
    }

    LOG_INFO("Diretta enabled successfully");

    // Create pipe for squeezelite stdout (audio + headers)
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        LOG_ERROR("Failed to create pipe: " << strerror(errno));
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    // Build squeezelite command
    std::vector<std::string> squeezelite_args = build_squeezelite_args(config, "-");
    if (g_logLevel >= LogLevel::DEBUG) {
        std::cout << "Squeezelite command: ";
        for (const auto& arg : squeezelite_args) {
            std::cout << arg << " ";
        }
        std::cout << std::endl;
    }

    // Fork and exec squeezelite
    squeezelite_pid = fork();

    if (squeezelite_pid == -1) {
        LOG_ERROR("Failed to fork");
        close(pipefd[0]);
        close(pipefd[1]);
        g_diretta->disable();
        if (g_logRing) delete g_logRing;
        return 1;
    }

    if (squeezelite_pid == 0) {
        // Child process: redirect stdout to pipe, let stderr pass through
        close(pipefd[0]);  // Close read end

        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            LOG_ERROR("Failed to redirect stdout: " << strerror(errno));
            exit(1);
        }
        close(pipefd[1]);

        // v2.0: stderr is NOT redirected — squeezelite logs pass through
        // to the parent process stderr for debugging (visible with -v)

        // Convert args to C-style array
        std::vector<char*> c_args;
        for (auto& arg : squeezelite_args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());

        LOG_ERROR("Failed to execute squeezelite: " << strerror(errno));
        exit(1);
    }

    // Parent process
    close(pipefd[1]);  // Close write end
    int fifo_fd = pipefd[0];

    LOG_INFO("Squeezelite started (PID: " << squeezelite_pid << ")");
    LOG_INFO("Waiting for first track header...");
    LOG_INFO("");

    // ================================================================
    // Main loop: synchronous header-based format detection
    // ================================================================
    // The patched squeezelite writes a 16-byte "SQFH" header to stdout
    // only when the format changes (or for the first track). Same-format
    // gapless transitions emit no header — audio flows uninterrupted.
    // No stderr parsing or race conditions.
    // ================================================================

    PipeReader reader(fifo_fd);

    // Squeezelite always outputs S32_LE (4 bytes per sample)
    const size_t SQZ_BYTES_PER_SAMPLE = 4;
    const size_t PIPE_BUF_SIZE = 16384;
    const float RING_HIGH_WATER = 0.75f;  // Wait when ring buffer > 75% full

    // Current format state
    AudioFormat current_format;
    DSDFormatType current_dsd_type = DSDFormatType::NONE;
    unsigned int current_rate = 0;
    uint8_t current_depth = 0;
    bool diretta_open = false;

    // Streaming state
    uint64_t total_bytes = 0;
    uint64_t total_frames = 0;

    std::vector<uint8_t> audio_buf(PIPE_BUF_SIZE);
    std::vector<uint8_t> planar_buf(PIPE_BUF_SIZE);

    while (running) {
        // ============================================================
        // Phase 1: Read format header (blocking)
        // ============================================================
        SqFormatHeader hdr;
        if (!reader.readExact(&hdr, sizeof(hdr))) {
            if (running) {
                LOG_INFO("Squeezelite pipe closed");
            }
            break;
        }

        // Validate magic
        if (memcmp(hdr.magic, SQFH_MAGIC, 4) != 0) {
            LOG_ERROR("Expected SQFH header, got: "
                      << std::hex << (int)hdr.magic[0] << " " << (int)hdr.magic[1]
                      << " " << (int)hdr.magic[2] << " " << (int)hdr.magic[3]
                      << std::dec);
            LOG_ERROR("Stream desynchronized. Is squeezelite patched for v2.0?");
            running = false;
            break;
        }

        // Parse header
        DSDFormatType dsd_type = static_cast<DSDFormatType>(hdr.dsd_format);
        bool is_dsd = (dsd_type != DSDFormatType::NONE);

        LOG_DEBUG("\n[Header] v" << (int)hdr.version
                  << " ch=" << (int)hdr.channels
                  << " depth=" << (int)hdr.bit_depth
                  << " dsd=" << (int)hdr.dsd_format
                  << " rate=" << hdr.sample_rate << "Hz");

        // ============================================================
        // Phase 2: Determine if format changed
        // ============================================================
        bool format_changed = (hdr.sample_rate != current_rate ||
                                hdr.dsd_format != static_cast<uint8_t>(current_dsd_type) ||
                                hdr.bit_depth != current_depth);

        if (format_changed) {
            // Calculate actual DSD bit rate and Diretta format
            unsigned int actual_rate = hdr.sample_rate;
            unsigned int bit_depth = static_cast<unsigned int>(output_bit_depth);

            if (is_dsd) {
                if (dsd_type == DSDFormatType::U32_BE || dsd_type == DSDFormatType::U32_LE) {
                    // Native DSD: frame rate × 32 = DSD bit rate
                    actual_rate = hdr.sample_rate * 32;
                    bit_depth = 1;
                } else if (dsd_type == DSDFormatType::DOP) {
                    // DoP: carrier rate × 16 = DSD bit rate
                    actual_rate = hdr.sample_rate * 16;
                    bit_depth = 1;
                }
            }

            if (dsd_type == DSDFormatType::DOP) {
                LOG_INFO("\n[Format Change] DoP->DSD at " << actual_rate << "Hz"
                          << " (DoP rate: " << hdr.sample_rate << "Hz)");
            } else if (is_dsd) {
                LOG_INFO("\n[Format Change] DSD at " << actual_rate << "Hz"
                          << " (frame rate: " << hdr.sample_rate << "Hz)");
            } else {
                LOG_INFO("\n[Format Change] PCM at " << actual_rate << "Hz / "
                          << (int)hdr.bit_depth << "-bit");
            }

            // Don't call close() before open() — let open() handle the transition
            // internally. close() sets m_open=false which prevents open() from
            // detecting the format change and doing the critical SDK close/reopen
            // needed for sample rate changes (e.g., 48kHz → 44.1kHz).

            // Build AudioFormat for DirettaSync
            AudioFormat format;
            format.sampleRate = actual_rate;
            format.bitDepth = bit_depth;
            format.channels = hdr.channels;
            format.isDSD = is_dsd;
            format.isCompressed = false;

            if (is_dsd) {
                format.dsdFormat = AudioFormat::DSDFormat::DFF;  // MSB (byte-swap in de-interleave)
                LOG_DEBUG("[DSD Format] "
                          << (dsd_type == DSDFormatType::DOP ? "DoP->DSD" : "Native DSD")
                          << " as DFF (MSB)");
            }

            // Open Diretta with new format
            if (!g_diretta->open(format)) {
                LOG_ERROR("Failed to open Diretta with new format");
                running = false;
                break;
            }

            // Squeezelite always outputs MSB-aligned S32_LE
            if (!is_dsd) {
                g_diretta->setS24PackModeHint(DirettaRingBuffer::S24PackMode::MsbAligned);
            }

            diretta_open = true;
            current_format = format;
            current_rate = hdr.sample_rate;
            current_dsd_type = dsd_type;
            current_depth = hdr.bit_depth;

            // ========================================================
            // Burst-fill: fill ring buffer before rate-limited playback
            // ========================================================
            LOG_DEBUG("[Burst Fill] Starting prefill...");

            size_t bytes_per_frame = SQZ_BYTES_PER_SAMPLE * hdr.channels;
            auto burst_start = std::chrono::steady_clock::now();
            const auto burst_timeout = std::chrono::seconds(5);
            size_t burst_bytes = 0;

            while (!g_diretta->isPrefillComplete() && running) {
                auto elapsed = std::chrono::steady_clock::now() - burst_start;
                if (elapsed > burst_timeout) {
                    LOG_WARN("[Burst Fill] Timeout after 5s");
                    break;
                }

                // Peek for next header (new track during burst)
                uint8_t peek_buf[4];
                if (reader.peek(peek_buf, 4) && memcmp(peek_buf, SQFH_MAGIC, 4) == 0) {
                    LOG_DEBUG("[Burst Fill] Next track header during burst");
                    break;
                }

                ssize_t n = reader.readUpTo(audio_buf.data(), PIPE_BUF_SIZE);
                if (n <= 0) break;

                size_t num_frames = static_cast<size_t>(n) / bytes_per_frame;
                size_t num_samples;

                if (is_dsd && dsd_type == DSDFormatType::DOP) {
                    size_t dsd_bytes_per_frame = 2 * hdr.channels;
                    size_t output_size = num_frames * dsd_bytes_per_frame;
                    if (planar_buf.size() < output_size) planar_buf.resize(output_size);
                    convert_dop_to_native_dsd(audio_buf.data(), planar_buf.data(),
                                               num_frames, bytes_per_frame, hdr.channels);
                    num_samples = (output_size * 8) / hdr.channels;
                    g_diretta->sendAudio(planar_buf.data(), num_samples);
                    burst_bytes += output_size;
                } else if (is_dsd) {
                    if (planar_buf.size() < static_cast<size_t>(n)) planar_buf.resize(n);
                    deinterleave_dsd_native(audio_buf.data(), planar_buf.data(),
                                             num_frames, bytes_per_frame, hdr.channels);
                    num_samples = (static_cast<size_t>(n) * 8) / hdr.channels;
                    g_diretta->sendAudio(planar_buf.data(), num_samples);
                    burst_bytes += n;
                } else {
                    num_samples = num_frames;
                    g_diretta->sendAudio(audio_buf.data(), num_samples);
                    burst_bytes += n;
                }
            }

            if (g_logLevel >= LogLevel::DEBUG) {
                auto burst_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - burst_start);
                LOG_DEBUG("[Burst Fill] Complete: " << burst_bytes << " bytes in "
                          << burst_elapsed.count() << "ms");
            }

            if (dsd_type == DSDFormatType::DOP) LOG_INFO("[Ready] DoP->DSD at " << actual_rate << "Hz");
            else if (is_dsd) LOG_INFO("[Ready] DSD at " << actual_rate << "Hz");
            else LOG_INFO("[Ready] PCM at " << actual_rate << "Hz");

        } else {
            // Same format — gapless transition, no reopen needed
            LOG_DEBUG("[Gapless] Same format, continuing stream");
        }

        // ============================================================
        // Phase 3: Stream audio until next header or EOF
        // ============================================================
        size_t bytes_per_frame = SQZ_BYTES_PER_SAMPLE * hdr.channels;
        unsigned int rate_for_timing = is_dsd ? hdr.sample_rate : current_format.sampleRate;

        while (running) {
            // Check for next track header
            uint8_t peek_buf[4];
            if (reader.peek(peek_buf, 4) && memcmp(peek_buf, SQFH_MAGIC, 4) == 0) {
                break;  // Next track — back to outer loop for header parsing
            }

            ssize_t bytes_read = reader.readUpTo(audio_buf.data(), PIPE_BUF_SIZE);

            if (bytes_read <= 0) {
                if (bytes_read == 0) {
                    LOG_INFO("Squeezelite pipe closed");
                } else if (errno != EINTR) {
                    LOG_ERROR("Error reading from pipe: " << strerror(errno));
                }
                running = false;
                break;
            }

            size_t num_frames = static_cast<size_t>(bytes_read) / bytes_per_frame;
            size_t num_samples;

            // Consumer-driven flow control: wait for space BEFORE pushing
            // push() is non-blocking and truncates if full — must wait first
            // to avoid silently dropping audio data
            if (g_diretta->isPrefillComplete()) {
                while (running) {
                    float level = g_diretta->getBufferLevel();
                    if (level <= RING_HIGH_WATER) break;
                    std::unique_lock<std::mutex> lock(g_diretta->getFlowMutex());
                    g_diretta->waitForSpace(lock, std::chrono::milliseconds(50));
                }
            }

            // Process and send based on format
            if (current_format.isDSD && current_dsd_type == DSDFormatType::DOP) {
                // DoP → Native DSD
                size_t dsd_bytes_per_frame = 2 * hdr.channels;
                size_t output_size = num_frames * dsd_bytes_per_frame;
                if (planar_buf.size() < output_size) planar_buf.resize(output_size);
                convert_dop_to_native_dsd(audio_buf.data(), planar_buf.data(),
                                           num_frames, bytes_per_frame, hdr.channels);
                num_samples = (output_size * 8) / hdr.channels;
                g_diretta->sendAudio(planar_buf.data(), num_samples);

            } else if (current_format.isDSD) {
                // Native DSD: interleaved → planar with byte-swap
                if (planar_buf.size() < static_cast<size_t>(bytes_read)) {
                    planar_buf.resize(bytes_read);
                }
                deinterleave_dsd_native(audio_buf.data(), planar_buf.data(),
                                         num_frames, bytes_per_frame, hdr.channels);
                num_samples = (static_cast<size_t>(bytes_read) * 8) / hdr.channels;
                g_diretta->sendAudio(planar_buf.data(), num_samples);

            } else {
                // PCM: send raw S32_LE — DirettaSync handles 32→24/16 conversion
                num_samples = num_frames;
                g_diretta->sendAudio(audio_buf.data(), num_samples);
            }

            total_bytes += static_cast<uint64_t>(bytes_read);
            total_frames += num_frames;

            // Progress (debug level, every ~10 seconds)
            if (g_logLevel >= LogLevel::DEBUG && total_frames % (rate_for_timing * 10) < (PIPE_BUF_SIZE / bytes_per_frame)) {
                double seconds = static_cast<double>(total_frames) / static_cast<double>(rate_for_timing);
                LOG_DEBUG("Streamed: " << std::fixed << std::setprecision(1)
                          << seconds << "s (" << (total_bytes / 1024 / 1024) << " MB)");
            }
        }
    }

    // Cleanup
    LOG_INFO("");
    LOG_INFO("Shutting down...");

    if (diretta_open) {
        g_diretta->close();
    }
    g_diretta->disable();
    g_diretta.reset();

    close(fifo_fd);

    if (squeezelite_pid > 0) {
        kill(squeezelite_pid, SIGTERM);
        waitpid(squeezelite_pid, nullptr, 0);
    }

    if (g_logRing) {
        delete g_logRing;
        g_logRing = nullptr;
    }

    LOG_INFO("Stopped");
    LOG_INFO("Total streamed: " << total_frames << " frames ("
              << (total_bytes / 1024 / 1024) << " MB)");

    return 0;
}
