// NHL_QUICK_START: presses through the boot screens that only wait for a button (see quick_start.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nhllegacy {

// A game file was opened from the loose tree; the boot sequence is tracked by these.
void QuickStartNoteFile(const char* path);

// What to serve instead of this file during quick start, or null for the file itself.
std::shared_ptr<const std::vector<uint8_t>> QuickStartServe(const std::string& path, const std::filesystem::path& host);

// Whether the guest should get a connected pad now, even without a real one.
bool QuickStartActive();
// True when REX_NHL_QUICK_START is on (the cvar), whether or not it has finished.
bool QuickStartEnabled();

// Buttons to add to user 0's state this poll. real_buttons: what the player is holding.
uint16_t QuickStartButtons(uint16_t real_buttons);

}  // namespace nhllegacy
