#pragma once

#include <windows.h>

#include <optional>

// The desktop companion (Settings > General > Desktop companion, off at
// first): Home's companion in a corner of the desktop, run as
// "TEKITO.exe --companion" and started with Windows while it is on. It
// watches the pointer, blinks, hops when the mode changes or when clicked,
// closes its eyes in secret mode, can be dragged anywhere, and steps aside
// for full-screen apps. It leaves when the setting is turned off.

// Runs the companion when the command line asks for it; its exit code then.
[[nodiscard]] std::optional<int> RunCompanionFromCommandLine();
// Starts the companion when it is not running.
void StartCompanion();
// Starts the companion with Windows, or no longer.
void SetCompanionAtStartup(bool on);
