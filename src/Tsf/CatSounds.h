#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace tekito::tsf {

// Cat (UserSettings::catSounds): a meow for each key pressed, a little
// higher or lower each time, and a kitten's mew once when a key is held.
// The sounds are the DLL's WAVE resources (16-bit PCM); each play is
// resampled to its pitch and played from memory, a new one cutting the last
// short.
class CatSounds {
public:
    // A key went down: `repeat` when it was already down (held).
    void OnKey(WPARAM key, bool repeat) noexcept;

private:
    struct Sound {
        bool loaded{false};
        WORD channels{1};
        DWORD rate{44100};
        std::vector<std::int16_t> samples;
    };
    void Play(Sound& sound, int resource) noexcept;

    Sound meow_, hold_;
    WPARAM heldKey_{0};
    // The WAV files played: the oldest is rewritten for the next play (the
    // one it held has been cut short by the plays after it).
    std::array<std::vector<std::uint8_t>, 3> played_;
    std::size_t next_{0};
    std::minstd_rand random_{std::random_device{}()};
};

}  // namespace tekito::tsf
