#pragma once

#include "types.hpp"
#include <windows.h>

namespace nativecall {

// Runs the native Win32 interface. All media and connection operations are queued
// by CallController; this function owns only windows and the message loop.
int runNativeApplication(HINSTANCE instance, int showCommand, ConnectionSettings settings);

} // namespace nativecall
