/**
 * @file DirettaOutputSimple.cpp
 * @brief Simplified Diretta output implementation for squeeze2diretta
 * 
 * @author Dominique COMET
 * @date 2025
 */

#include "DirettaOutputSimple.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

DirettaOutputSimple::DirettaOutputSimple()
    : m_syncBuffer(nullptr)
    , m_selectedTargetIndex(-1)
    , m_bufferSeconds(2.0f)
    , m_playing(false)
    , m_isPaused(false)
    , m_thredMode(1)
    , m_cycleTime(10000)
    , m_cycleMinTime(333)
    , m_infoCycle(5000)
    , m_mtu(0)
    , m_totalSamplesSent(0)
{
    m_currentFormat = {0};
    
    // Create SyncBuffer instance
    m_syncBuffer = new DIRETTA::SyncBuffer();
}

DirettaOutputSimple::~DirettaOutputSimple() {
    close();
    
    if (m_syncBuffer) {
        delete m_syncBuffer;
        m_syncBuffer = nullptr;
    }
}

void DirettaOutputSimple::listAvailableTargets() {
    std::cout << "Scanning for Diretta targets..." << std::endl;
    std::cout << std::endl;
    
    if (!m_syncBuffer) {
        std::cerr << "Error: SyncBuffer not initialized" << std::endl;
        return;
    }
    
    // Discover targets
    int count = m_syncBuffer->find();
    
    if (count == 0) {
        std::cout << "No Diretta targets found on the network." << std::endl;
        std::cout << std::endl;
        std::cout << "Troubleshooting:" << std::endl;
        std::cout << "  1. Ensure your Diretta DAC is powered on" << std::endl;
        std::cout << "  2. Check network connection" << std::endl;
        std::cout << "  3. Verify firewall settings (UDP broadcast must be allowed)" << std::endl;
        return;
    }
    
    std::cout << "Found " << count << " Diretta target(s):" << std::endl;
    std::cout << std::endl;
    
    for (int i = 0; i < count; i++) {
        DIRETTA::SinkName name = m_syncBuffer->getSinkName(i);
        
        std::cout << "Target #" << (i + 1) << ":" << std::endl;
        std::cout << "  Name:    " << name.get() << std::endl;
        std::cout << "  Index:   " << i << std::endl;
        std::cout << std::endl;
    }
    
    std::cout << "Usage: squeeze2diretta -t <number>" << std::endl;
    std::cout << "Example: squeeze2diretta -t 1 (for first target)" << std::endl;
}

bool DirettaOutputSimple::selectTarget(int index) {
    if (!m_syncBuffer) {
        std::cerr << "Error: SyncBuffer not initialized" << std::endl;
        return false;
    }
    
    // Discover targets
    int count = m_syncBuffer->find();
    
    if (count == 0) {
        std::cerr << "No Diretta targets found" << std::endl;
        return false;
    }
    
    if (index < 0 || index >= count) {
        std::cerr << "Invalid target index: " << index 
                  << " (available: 0-" << (count - 1) << ")" << std::endl;
        return false;
    }
    
    // Get target info
    DIRETTA::SinkName name = m_syncBuffer->getSinkName(index);
    DIRETTA::IPv4 addr = m_syncBuffer->getSinkAddr(index);
    
    m_selectedTargetIndex = index;
    m_targetAddress = addr.toString();
    
    std::cout << "Selected Diretta target #" << (index + 1) << ": " 
              << name.get() << " (" << m_targetAddress << ")" << std::endl;
    
    return true;
}

bool DirettaOutputSimple::open(const AudioFormat& format, float bufferSeconds) {
    if (m_selectedTargetIndex < 0) {
        std::cerr << "Error: No target selected. Use selectTarget() first." << std::endl;
        return false;
    }
    
    std::cout << "[DirettaOutput] Opening connection..." << std::endl;
    std::cout << "  Format: ";
    if (format.isDSD) {
        std::cout << "DSD" << (format.sampleRate / 44100) << " (" 
                  << format.sampleRate << "Hz)";
    } else {
        std::cout << "PCM " << (int)format.bitDepth << "-bit " 
                  << format.sampleRate << "Hz";
    }
    std::cout << " " << (int)format.channels << "ch" << std::endl;
    std::cout << "  Buffer: " << bufferSeconds << " seconds" << std::endl;
    
    m_currentFormat = format;
    m_bufferSeconds = bufferSeconds;
    
    // ===== STEP 1: Open SyncBuffer =====
    m_syncBuffer->open(
        DIRETTA::Sync::THRED_MODE(m_thredMode),
        ACQUA::Clock::MilliSeconds(100),
        0, "squeeze2diretta", 0, 0, 0, 0,
        DIRETTA::Sync::MSMODE_AUTO
    );
    
    // ===== STEP 2: Set Sink (Target) =====
    int mtu = (m_mtu > 0) ? m_mtu : 16128;  // Default to jumbo frames if not specified
    
    m_syncBuffer->setSink(
        m_targetAddress,
        ACQUA::Clock::MilliSeconds(100),
        false,
        mtu
    );
    
    // ===== STEP 3: Format Negotiation =====
    DIRETTA::FormatID formatID = buildFormatID(format);
    
    m_syncBuffer->setSinkConfigure(formatID);
    
    // Verify format accepted by target
    DIRETTA::FormatID configuredFormat = m_syncBuffer->getSinkConfigure();
    
    if (configuredFormat != formatID) {
        std::cout << "  ⚠️  Target modified format:" << std::endl;
        std::cout << "     Requested: 0x" << std::hex << static_cast<uint32_t>(formatID) << std::dec << std::endl;
        std::cout << "     Accepted:  0x" << std::hex << static_cast<uint32_t>(configuredFormat) << std::dec << std::endl;
    }
    
    // ===== STEP 4: Configure Transfer =====
    bool isLowBitrate = (format.bitDepth <= 16 && format.sampleRate <= 48000 && !format.isDSD);
    
    if (isLowBitrate) {
        // Low bitrate: smaller packets
        m_syncBuffer->configTransferAuto(
            ACQUA::Clock::MicroSeconds(m_infoCycle),
            ACQUA::Clock::MicroSeconds(m_cycleMinTime),
            ACQUA::Clock::MicroSeconds(m_cycleTime)
        );
    } else {
        // Hi-Res: jumbo frames for maximum performance
        m_syncBuffer->configTransferVarMax(
            ACQUA::Clock::MicroSeconds(m_infoCycle)
        );
    }
    
    // ===== STEP 5: Setup Buffer =====
    int framesPerSecond = format.sampleRate;
    int totalFrames = static_cast<int>(framesPerSecond * bufferSeconds);
    
    m_syncBuffer->setupBuffer(totalFrames, 4, false);
    
    // ===== STEP 6: Connect =====
    m_syncBuffer->connect(0, 0);
    
    // Wait for connection (timeout: 10 seconds)
    int timeoutMs = 10000;
    int waitedMs = 0;
    
    while (!m_syncBuffer->is_connect() && waitedMs < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        waitedMs += 100;
    }
    
    if (!m_syncBuffer->is_connect()) {
        std::cerr << "[DirettaOutput] ❌ Connection timeout" << std::endl;
        return false;
    }
    
    // ===== STEP 7: Start Playback =====
    m_syncBuffer->play();
    m_playing = true;
    m_isPaused = false;
    m_totalSamplesSent = 0;
    
    std::cout << "[DirettaOutput] ✓ Connected and ready" << std::endl;
    
    return true;
}

bool DirettaOutputSimple::sendAudio(const uint8_t* data, size_t numSamples) {
    if (!m_playing || !m_syncBuffer) {
        return false;
    }
    
    if (m_isPaused) {
        return true;  // Silently discard while paused
    }
    
    // Calculate data size
    size_t dataSize;
    
    if (m_currentFormat.isDSD) {
        // DSD: each sample is 1 bit, packed into bytes
        // For DSD, numSamples is in bits per channel
        dataSize = (numSamples * m_currentFormat.channels) / 8;
    } else {
        // PCM: depends on bit depth
        size_t bytesPerSample = (m_currentFormat.bitDepth / 8) * m_currentFormat.channels;
        dataSize = numSamples * bytesPerSample;
    }
    
    // Create stream buffer
    DIRETTA::Stream stream;
    stream.resize(dataSize);
    
    // Copy data
    std::memcpy(stream.get(), data, dataSize);
    
    // Send to Diretta
    m_syncBuffer->setStream(stream);
    
    m_totalSamplesSent += numSamples;
    
    return true;
}

bool DirettaOutputSimple::changeFormat(const AudioFormat& newFormat) {
    std::cout << "[DirettaOutput] Format change requested" << std::endl;
    
    if (m_playing) {
        // Stop current playback
        m_syncBuffer->stop();
        disconnectFromTarget();
    }
    
    // Reopen with new format
    return open(newFormat, m_bufferSeconds);
}

void DirettaOutputSimple::pause() {
    if (!m_playing || m_isPaused) {
        return;
    }
    
    std::cout << "[DirettaOutput] Pausing..." << std::endl;
    
    m_syncBuffer->stop();
    m_isPaused = true;
    
    std::cout << "[DirettaOutput] ✓ Paused" << std::endl;
}

void DirettaOutputSimple::resume() {
    if (!m_playing || !m_isPaused) {
        return;
    }
    
    std::cout << "[DirettaOutput] Resuming..." << std::endl;
    
    m_syncBuffer->play();
    m_isPaused = false;
    
    std::cout << "[DirettaOutput] ✓ Resumed" << std::endl;
}

void DirettaOutputSimple::close() {
    if (!m_playing) {
        return;
    }
    
    std::cout << "[DirettaOutput] Closing connection..." << std::endl;
    
    m_syncBuffer->stop();
    disconnectFromTarget();
    
    m_playing = false;
    m_isPaused = false;
    
    std::cout << "[DirettaOutput] ✓ Closed (sent " << m_totalSamplesSent << " samples)" << std::endl;
}

void DirettaOutputSimple::disconnectFromTarget() {
    if (m_syncBuffer) {
        m_syncBuffer->disconnect();
        m_syncBuffer->close();
    }
}

DIRETTA::FormatID DirettaOutputSimple::buildFormatID(const AudioFormat& format) {
    DIRETTA::FormatID formatID = DIRETTA::FormatID::FMT_INVALID;
    
    if (format.isDSD) {
        // DSD format
        formatID = DIRETTA::FormatID::FMT_DSD_SIZ_32;  // 32-bit containers for DSD
        
        // Add DSD rate
        switch (format.sampleRate) {
            case 2822400:   formatID |= DIRETTA::FormatID::FMT_DSD_R64; break;   // DSD64
            case 5644800:   formatID |= DIRETTA::FormatID::FMT_DSD_R128; break;  // DSD128
            case 11289600:  formatID |= DIRETTA::FormatID::FMT_DSD_R256; break;  // DSD256
            case 22579200:  formatID |= DIRETTA::FormatID::FMT_DSD_R512; break;  // DSD512
            case 45158400:  formatID |= DIRETTA::FormatID::FMT_DSD_R1024; break; // DSD1024
            default:
                std::cerr << "Unsupported DSD rate: " << format.sampleRate << std::endl;
                return DIRETTA::FormatID::FMT_INVALID;
        }
        
    } else {
        // PCM format
        switch (format.bitDepth) {
            case 16: formatID = DIRETTA::FormatID::FMT_PCM_SIGNED_16; break;
            case 24: formatID = DIRETTA::FormatID::FMT_PCM_SIGNED_24; break;
            case 32: formatID = DIRETTA::FormatID::FMT_PCM_SIGNED_32; break;
            default:
                std::cerr << "Unsupported bit depth: " << (int)format.bitDepth << std::endl;
                return DIRETTA::FormatID::FMT_INVALID;
        }
        
        // Add sample rate
        switch (format.sampleRate) {
            case 44100:  formatID |= DIRETTA::FormatID::FMT_PCM_R44100; break;
            case 48000:  formatID |= DIRETTA::FormatID::FMT_PCM_R48000; break;
            case 88200:  formatID |= DIRETTA::FormatID::FMT_PCM_R88200; break;
            case 96000:  formatID |= DIRETTA::FormatID::FMT_PCM_R96000; break;
            case 176400: formatID |= DIRETTA::FormatID::FMT_PCM_R176400; break;
            case 192000: formatID |= DIRETTA::FormatID::FMT_PCM_R192000; break;
            case 352800: formatID |= DIRETTA::FormatID::FMT_PCM_R352800; break;
            case 384000: formatID |= DIRETTA::FormatID::FMT_PCM_R384000; break;
            case 705600: formatID |= DIRETTA::FormatID::FMT_PCM_R705600; break;
            case 768000: formatID |= DIRETTA::FormatID::FMT_PCM_R768000; break;
            default:
                std::cerr << "Unsupported sample rate: " << format.sampleRate << std::endl;
                return DIRETTA::FormatID::FMT_INVALID;
        }
    }
    
    // Add channel configuration (always stereo for now)
    formatID |= DIRETTA::FormatID::FMT_CH_2_0;
    
    return formatID;
}
