/**
 * @file globals.cpp
 * @brief Global variable definitions for squeeze2diretta
 */

#include "globals.h"
#include "DirettaSync.h"

// Global verbose flag - controls logging output
bool g_verbose = false;

// Global log ring for async logging in hot paths
// Allocated in main() if verbose mode is enabled
LogRing* g_logRing = nullptr;
