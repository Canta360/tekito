#include "Tsf/CatSounds.h"

#include "Tsf/Globals.h"
#include "assets/tekito_resource.h"

#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tekito::tsf {
namespace {

// How far each play's pitch may move, in cents (100 a semitone): enough to
// hear, not so much that it stops sounding like the same cat.
constexpr int kMeowCents = 150;
constexpr int kHoldCents = 80;

template <typename T>
T Read(const std::uint8_t* at) {
    T value;
    std::memcpy(&value, at, sizeof value);
    return value;
}

void Put(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

}  // namespace

void CatSounds::OnKey(WPARAM key, bool repeat) noexcept {
    if (repeat) {
        // Held: the kitten once, until the key goes up.
        if (heldKey_ == key) return;
        heldKey_ = key;
        Play(hold_, IDR_CAT_HOLD_SOUND);
        return;
    }
    heldKey_ = 0;
    Play(meow_, IDR_CAT_SOUND);
}

void CatSounds::Play(Sound& sound, int resource) noexcept {
    try {
        if (!sound.loaded) {
            sound.loaded = true;
            const HRSRC found = FindResourceW(g_moduleInstance, MAKEINTRESOURCEW(resource), L"WAVE");
            const HGLOBAL handle = found ? LoadResource(g_moduleInstance, found) : nullptr;
            const auto* data = handle ? static_cast<const std::uint8_t*>(LockResource(handle)) : nullptr;
            const DWORD size = found ? SizeofResource(g_moduleInstance, found) : 0;
            if (!data || size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) return;
            bool pcm16 = false;
            for (DWORD at = 12; at + 8 <= size;) {
                const DWORD length = Read<DWORD>(data + at + 4);
                if (at + 8 + length > size) break;
                if (std::memcmp(data + at, "fmt ", 4) == 0 && length >= 16) {
                    pcm16 = Read<WORD>(data + at + 8) == WAVE_FORMAT_PCM && Read<WORD>(data + at + 22) == 16;
                    sound.channels = Read<WORD>(data + at + 10);
                    sound.rate = Read<DWORD>(data + at + 12);
                } else if (std::memcmp(data + at, "data", 4) == 0 && pcm16) {
                    sound.samples.resize(length / 2);
                    std::memcpy(sound.samples.data(), data + at + 8, sound.samples.size() * 2);
                }
                at += 8 + length + (length & 1);
            }
        }
        if (sound.samples.empty() || sound.channels == 0) return;
        // A pitch of its own: read faster (higher, shorter) or slower.
        const int range = &sound == &meow_ ? kMeowCents : kHoldCents;
        const int cents = std::uniform_int_distribution<int>(-range, range)(random_);
        const double step = std::pow(2.0, cents / 1200.0);
        const std::size_t channels = sound.channels;
        const std::size_t frames = sound.samples.size() / channels;
        const auto outFrames = static_cast<std::size_t>(static_cast<double>(frames) / step);
        auto& wav = played_[next_];
        next_ = (next_ + 1) % played_.size();
        wav.clear();
        const DWORD dataSize = static_cast<DWORD>(outFrames * channels * 2);
        const DWORD riffSize = 36 + dataSize;
        const WORD format = WAVE_FORMAT_PCM, bits = 16, align = static_cast<WORD>(channels * 2);
        const WORD channelCount = sound.channels;
        const DWORD fmtSize = 16, byteRate = sound.rate * align;
        Put(wav, "RIFF", 4);
        Put(wav, &riffSize, 4);
        Put(wav, "WAVEfmt ", 8);
        Put(wav, &fmtSize, 4);
        Put(wav, &format, 2);
        Put(wav, &channelCount, 2);
        Put(wav, &sound.rate, 4);
        Put(wav, &byteRate, 4);
        Put(wav, &align, 2);
        Put(wav, &bits, 2);
        Put(wav, "data", 4);
        Put(wav, &dataSize, 4);
        for (std::size_t i = 0; i < outFrames; ++i) {
            const double at = static_cast<double>(i) * step;
            const auto first = std::min(static_cast<std::size_t>(at), frames - 1);
            const auto second = std::min(first + 1, frames - 1);
            const double mix = at - static_cast<double>(first);
            for (std::size_t c = 0; c < channels; ++c) {
                const double value = sound.samples[first * channels + c] * (1.0 - mix) + sound.samples[second * channels + c] * mix;
                const auto sample = static_cast<std::int16_t>(std::clamp(std::lround(value), -32768L, 32767L));
                Put(wav, &sample, 2);
            }
        }
        PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    } catch (...) {
    }
}

}  // namespace tekito::tsf
