#pragma once

#include "types.hpp"
#include <windows.h>

namespace nativecall {

// Runs the native Win32 interface. All media and connection operations are queued
// by CallController; this function owns only windows and the message loop.
// smokeTest creates the actual UI/controller and closes it after two seconds.
// It performs no call, microphone, or capture operations and never shows dialogs.
int runNativeApplication(HINSTANCE instance, int showCommand, ConnectionSettings settings,
                         bool smokeTest = false);

} // namespace nativecall
