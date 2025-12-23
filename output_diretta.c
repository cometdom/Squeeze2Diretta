/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *  (c) Ralph Irving 2015-2025
 *  (c) Dominique COMET 2025 - Diretta output backend
 *      
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * Diretta output backend for squeezelite
 * 
 * This backend sends audio directly to Diretta protocol DACs over the network,
 * bypassing ALSA/PortAudio and providing bit-perfect playback.
 * 
 * Architecture:
 *   Squeezelite decode → outputbuf → output_thread_diretta → DirettaOutputSimple → Diretta DAC
 */

#include "squeezelite.h"

#if DIRETTA

#ifdef __cplusplus
extern "C" {
#endif

// Include C++ Diretta output class
#include "DirettaOutputSimple.h"

// Diretta-specific state
static struct {
	DirettaOutputSimple* diretta;
	unsigned target_index;      // Which Diretta target to use (0-based)
	float buffer_seconds;       // Buffer size in seconds
	int thread_mode;            // THRED_MODE setting
	int cycle_time;             // Transfer cycle time (µs)
	int cycle_min_time;         // Transfer cycle min time (µs)
	int info_cycle;             // Info cycle time (µs)
	int mtu_override;           // MTU override (0 = auto)
	bool running;
	pthread_t thread;
	AudioFormat current_format;
} diretta_state;

// Forward declarations
static void *output_thread_diretta(void *arg);

void list_diretta_targets(void) {
	LOG_INFO("Scanning for Diretta targets...");
	
	// Create temporary instance to list targets
	DirettaOutputSimple temp;
	temp.listAvailableTargets();
}

void output_init_diretta(log_level level, unsigned target, float buffer, 
                         int thread_mode, int cycle_time, int cycle_min_time,
                         int info_cycle, int mtu) {
	
	loglevel = level;
	
	LOG_INFO("initializing Diretta output");
	LOG_INFO("  target: %u", target);
	LOG_INFO("  buffer: %.1f seconds", buffer);
	LOG_INFO("  thread_mode: %d", thread_mode);
	LOG_INFO("  cycle_time: %d µs", cycle_time);
	LOG_INFO("  info_cycle: %d µs", info_cycle);
	if (mtu > 0) {
		LOG_INFO("  MTU override: %d bytes", mtu);
	}
	
	// Initialize state
	memset(&diretta_state, 0, sizeof(diretta_state));
	diretta_state.target_index = target;
	diretta_state.buffer_seconds = buffer;
	diretta_state.thread_mode = thread_mode;
	diretta_state.cycle_time = cycle_time;
	diretta_state.cycle_min_time = cycle_min_time;
	diretta_state.info_cycle = info_cycle;
	diretta_state.mtu_override = mtu;
	diretta_state.running = false;
	
	// Create Diretta output instance (C++ object)
	diretta_state.diretta = new DirettaOutputSimple();
	
	// Configure advanced settings
	diretta_state.diretta->setThredMode(thread_mode);
	diretta_state.diretta->setCycleTime(cycle_time);
	diretta_state.diretta->setCycleMinTime(cycle_min_time);
	diretta_state.diretta->setInfoCycle(info_cycle);
	if (mtu > 0) {
		diretta_state.diretta->setMTU(mtu);
	}
	
	// Set target (convert from 1-based to 0-based index)
	if (!diretta_state.diretta->selectTarget(target - 1)) {
		LOG_ERROR("Failed to select Diretta target %u", target);
		delete diretta_state.diretta;
		diretta_state.diretta = NULL;
		exit(1);
	}
	
	LOG_INFO("Diretta output initialized successfully");
}

static void *output_thread_diretta(void *arg) {
	// This thread continuously pulls decoded audio from squeezelite's output buffer
	// and sends it to the Diretta DAC
	
	LOG_INFO("Diretta output thread started");
	
	while (diretta_state.running) {
		
		LOCK_O;
		
		// Check how many frames are available in output buffer
		frames_t frames = _output_frames(outputbuf);
		
		if (frames == 0) {
			// No data available, wait a bit
			UNLOCK_O;
			usleep(10000);  // 10ms
			continue;
		}
		
		// Limit to reasonable chunk size (e.g., 8192 frames at a time)
		if (frames > 8192) {
			frames = 8192;
		}
		
		// Get pointer to audio data in output buffer
		// outputbuf->readp points to the next frame to read
		u8_t *buf = (u8_t*)outputbuf->readp;
		
		// Calculate data size based on current format
		// Squeezelite stores audio in the format: samples_per_frame * bytes_per_sample
		// For stereo 24-bit: 2 channels * 4 bytes (stored as 32-bit with padding) = 8 bytes/frame
		
		size_t bytes_per_frame;
		if (diretta_state.current_format.isDSD) {
			// DSD: 1 byte per sample for DSD64, more for higher rates
			bytes_per_frame = diretta_state.current_format.channels;
		} else {
			// PCM: channels * (bitDepth / 8)
			// Note: Squeezelite uses 32-bit containers for 24-bit audio
			size_t bytes_per_sample = (diretta_state.current_format.bitDepth <= 16) ? 
			                           (diretta_state.current_format.bitDepth / 8) : 4;
			bytes_per_frame = diretta_state.current_format.channels * bytes_per_sample;
		}
		
		size_t data_size = frames * bytes_per_frame;
		
		UNLOCK_O;
		
		// Send to Diretta
		// Note: DirettaOutputSimple expects numSamples = total samples (frames * channels)
		size_t num_samples = frames * diretta_state.current_format.channels;
		
		if (!diretta_state.diretta->sendAudio(buf, num_samples)) {
			LOG_WARN("Failed to send audio to Diretta");
		}
		
		// Update read pointer in output buffer
		LOCK_O;
		_output_frames_written(frames);
		UNLOCK_O;
	}
	
	LOG_INFO("Diretta output thread stopped");
	
	return NULL;
}

bool output_open_diretta(const char *device, unsigned *rate, unsigned channels) {
	LOG_INFO("opening Diretta output");
	LOG_INFO("  rate: %u Hz", *rate);
	LOG_INFO("  channels: %u", channels);
	
	// Prepare format info for Diretta
	AudioFormat format;
	format.sampleRate = *rate;
	format.bitDepth = 24;  // Default to 24-bit, will be adjusted by squeezelite
	format.channels = channels;
	format.isDSD = false;  // Will be set by squeezelite if DSD
	
	diretta_state.current_format = format;
	
	// Open Diretta connection
	if (!diretta_state.diretta->open(format, diretta_state.buffer_seconds)) {
		LOG_ERROR("Failed to open Diretta output");
		return false;
	}
	
	// Start output thread
	diretta_state.running = true;
	
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
	pthread_create(&diretta_state.thread, &attr, output_thread_diretta, NULL);
	pthread_attr_destroy(&attr);
	
	LOG_INFO("Diretta output opened successfully");
	return true;
}

void output_close_diretta(void) {
	LOG_INFO("closing Diretta output");
	
	// Stop output thread
	diretta_state.running = false;
	
	// Wait for thread to finish
	if (diretta_state.thread) {
		pthread_join(diretta_state.thread, NULL);
		diretta_state.thread = 0;
	}
	
	// Close Diretta connection
	if (diretta_state.diretta) {
		diretta_state.diretta->close();
	}
	
	LOG_INFO("Diretta output closed");
}

void output_flush_diretta(void) {
	LOG_INFO("flushing Diretta output");
	
	LOCK_O;
	
	// Clear output buffer
	outputbuf->readp = outputbuf->buf;
	outputbuf->writep = outputbuf->buf;
	output.frames_played = 0;
	
	UNLOCK_O;
}

bool output_start_diretta(void) {
	LOG_INFO("starting Diretta output");
	
	if (diretta_state.diretta) {
		diretta_state.diretta->resume();
	}
	
	return true;
}

bool output_stop_diretta(void) {
	LOG_INFO("stopping Diretta output");
	
	if (diretta_state.diretta) {
		diretta_state.diretta->pause();
	}
	
	return true;
}

void output_close_common_diretta(void) {
	LOG_INFO("cleaning up Diretta output");
	
	if (diretta_state.diretta) {
		delete diretta_state.diretta;
		diretta_state.diretta = NULL;
	}
}

// Rate change support
bool output_rate_change_diretta(unsigned *rate) {
	LOG_INFO("rate change requested: %u Hz", *rate);
	
	// Update format
	AudioFormat new_format = diretta_state.current_format;
	new_format.sampleRate = *rate;
	
	// Change format in Diretta
	if (!diretta_state.diretta->changeFormat(new_format)) {
		LOG_ERROR("Failed to change rate");
		return false;
	}
	
	diretta_state.current_format = new_format;
	
	LOG_INFO("rate changed successfully to %u Hz", *rate);
	return true;
}

// Volume control (software only for now)
void output_volume_diretta(unsigned left, unsigned right) {
	// Diretta doesn't support hardware volume control
	// Squeezelite will handle software volume in the decode path
	// This is a no-op
}

#ifdef __cplusplus
}
#endif

#endif // DIRETTA
