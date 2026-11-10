#pragma once

// The tokenizer of an AI model pack: byte-level BPE as Hugging Face's
// tokenizers runs it for the LFM2 models (the GPT-4-style split, bytes as
// printable characters, merges by rank). The pack holds it as two lists
// written from tokenizer.json (build-japanese-ai-packs.py): vocab.txt, one
// token per line in id order, and merges.txt, one "left right" pair per
// line in rank order.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekito::ai {

class ByteBpe {
public:
    bool Load(const std::filesystem::path& directory);
    [[nodiscard]] bool IsOpen() const noexcept { return !vocab_.empty(); }

    // Token ids of UTF-8 text; `start`: the start-of-text token first.
    [[nodiscard]] std::vector<std::int32_t> Encode(std::string_view text, bool start) const;
    // The UTF-8 text of token ids (special tokens left out).
    [[nodiscard]] std::string Decode(const std::vector<std::int32_t>& ids) const;

    [[nodiscard]] std::int32_t Start() const noexcept { return start_; }
    [[nodiscard]] std::int32_t End() const noexcept { return end_; }

private:
    void EncodeWord(std::string_view word, std::vector<std::int32_t>& out) const;

    std::vector<std::string> vocab_;  // id -> token as byte-level characters
    std::unordered_map<std::string, std::int32_t> ids_;
    std::unordered_map<std::string, std::int32_t> ranks_;  // "left\x01right" -> rank
    std::string byteChar_[256];       // byte -> its printable character, UTF-8
    std::unordered_map<std::string, std::uint8_t> charByte_;
    std::int32_t start_{1};
    std::int32_t end_{7};
    mutable std::unordered_map<std::string, std::vector<std::int32_t>> cache_;
};

}  // namespace tekito::ai
