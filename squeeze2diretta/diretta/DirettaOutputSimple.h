/**
 * @file DirettaOutputSimple.h
 * @brief Simplified Diretta output for squeeze2diretta
 * 
 * This is a lightweight version of DirettaOutput specifically designed
 * for squeeze2diretta. Unlike the full DirettaOutput used in DirettaRendererUPnP,
 * this version assumes audio is already decoded by squeezelite and only
 * handles raw PCM/DSD output to the Diretta DAC.
 * 
 * @author Dominique COMET
 * @date 2025
 */

#ifndef DIRETTA_OUTPUT_SIMPLE_H
#define DIRETTA_OUTPUT_SIMPLE_H

#include <cstdint>
#include <string>
#include <DIRETTA/Sync.h>
#include <ACQUA/Clock.h>

/**
 * @brief Audio format descriptor
 */
struct AudioFormat {
    uint32_t sampleRate;    // Sample rate in Hz (44100, 48000, 96000, etc.)
    uint8_t bitDepth;       // Bit depth (16, 24, 32)
    uint8_t channels;       // Number of channels (typically 2 for stereo)
    bool isDSD;             // true for DSD, false for PCM
};

/**
 * @brief Simplified Diretta output handler
 * 
 * This class provides a minimal interface for sending audio to Diretta DACs.
 * It expects audio data to already be decoded (PCM or DSD) and handles:
 * - Connection to Diretta target
 * - Buffer management
 * - Sample rate/format changes
 * - Playback control (play/pause)
 */
class DirettaOutputSimple {
public:
    DirettaOutputSimple();
    ~DirettaOutputSimple();
    
    // Prevent copying
    DirettaOutputSimple(const DirettaOutputSimple&) = delete;
    DirettaOutputSimple& operator=(const DirettaOutputSimple&) = delete;
    
    /**
     * @brief List available Diretta targets on the network
     * Prints to stdout with target numbers, names, and addresses
     */
    void listAvailableTargets();
    
    /**
     * @brief Select a Diretta target by index
     * @param index Target index (0-based: 0 = first target)
     * @return true if target was found and selected
     */
    bool selectTarget(int index);
    
    /**
     * @brief Open connection to Diretta DAC with specified format
     * @param format Audio format (sample rate, bit depth, channels, DSD)
     * @param bufferSeconds Buffer size in seconds (1.0-5.0 recommended)
     * @return true if connection was successful
     */
    bool open(const AudioFormat& format, float bufferSeconds);
    
    /**
     * @brief Send audio samples to Diretta DAC
     * @param data Pointer to audio data (interleaved PCM or DSD)
     * @param numSamples Number of samples (frames * channels)
     * @return true if data was sent successfully
     * 
     * Expected data format:
     * - PCM 16-bit: int16_t samples, little-endian
     * - PCM 24-bit: int32_t samples, MSB-aligned (upper 24 bits)
     * - PCM 32-bit: int32_t samples, little-endian
     * - DSD: uint8_t samples (DSD bitstream)
     */
    bool sendAudio(const uint8_t* data, size_t numSamples);
    
    /**
     * @brief Change audio format (sample rate / bit depth)
     * @param newFormat New audio format
     * @return true if format change was successful
     * 
     * Note: This closes and reopens the Diretta connection.
     * There will be a brief silence during the transition.
     */
    bool changeFormat(const AudioFormat& newFormat);
    
    /**
     * @brief Pause playback
     */
    void pause();
    
    /**
     * @brief Resume playback after pause
     */
    void resume();
    
    /**
     * @brief Close connection to Diretta DAC
     */
    void close();
    
    /**
     * @brief Check if currently connected and playing
     */
    bool isPlaying() const { return m_playing; }
    
    /**
     * @brief Check if paused
     */
    bool isPaused() const { return m_isPaused; }
    
    // ===== Advanced Configuration =====
    
    /**
     * @brief Set Diretta THRED_MODE
     * @param mode Bitmask value (default: 1 = Critical)
     * Must be called before open()
     */
    void setThredMode(int mode) { m_thredMode = mode; }
    
    /**
     * @brief Set transfer cycle time
     * @param microseconds Maximum cycle time in microseconds (default: 10000)
     * Must be called before open()
     */
    void setCycleTime(int microseconds) { m_cycleTime = microseconds; }
    
    /**
     * @brief Set minimum transfer cycle time
     * @param microseconds Minimum cycle time in microseconds (default: 333)
     * Must be called before open()
     */
    void setCycleMinTime(int microseconds) { m_cycleMinTime = microseconds; }
    
    /**
     * @brief Set information packet cycle time
     * @param microseconds Info cycle time in microseconds (default: 5000)
     * Must be called before open()
     */
    void setInfoCycle(int microseconds) { m_infoCycle = microseconds; }
    
    /**
     * @brief Set MTU override
     * @param mtu MTU in bytes (0 = auto-detect, 1500/9000/16128 typical)
     * Must be called before open()
     */
    void setMTU(int mtu) { m_mtu = mtu; }
    
    /**
     * @brief Get current audio format
     */
    const AudioFormat& getCurrentFormat() const { return m_currentFormat; }
    
private:
    // Internal methods
    bool connectToTarget();
    void disconnectFromTarget();
    DIRETTA::FormatID buildFormatID(const AudioFormat& format);
    
    // Diretta SDK objects
    DIRETTA::SyncBuffer* m_syncBuffer;
    
    // Target information
    std::string m_targetAddress;
    int m_selectedTargetIndex;
    
    // Current state
    AudioFormat m_currentFormat;
    float m_bufferSeconds;
    bool m_playing;
    bool m_isPaused;
    
    // Advanced configuration
    int m_thredMode;        // THRED_MODE (default: 1)
    int m_cycleTime;        // Transfer cycle max time µs (default: 10000)
    int m_cycleMinTime;     // Transfer cycle min time µs (default: 333)
    int m_infoCycle;        // Info cycle time µs (default: 5000)
    int m_mtu;              // MTU override (0 = auto)
    
    // Statistics
    uint64_t m_totalSamplesSent;
};

#endif // DIRETTA_OUTPUT_SIMPLE_H
