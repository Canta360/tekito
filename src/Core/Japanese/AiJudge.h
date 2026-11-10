#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tekito::japanese {

// The optional AI models (the japanese-ai-light and japanese-ai-strong
// packs): a language model that reads whole sentences, run by TEKITO's AI
// helper (TekitoAI.exe, apps/tekito-ai) in a process of its own, so its
// memory is held once and not in every app that takes text. Converting asks
// it after the sentence check and shows the sentence check's choice; the
// AI's replaces it once the answer comes, if nothing was changed meanwhile.
//
// A model trained for conversion (scripts/ja/train-japanese-ai-model.py)
// also reads the reading: it scores each sentence as the writing of that
// reading, and writes its own likeliest few.
struct AiConversion {
    // ln p of each sentence asked about, in order.
    std::vector<double> scores;
    // The model's own writings of the reading, likeliest first, with ln p
    // (only from a model trained for conversion).
    std::vector<std::pair<std::wstring, double>> written;
};

class AiJudge {
public:
    virtual ~AiJudge() = default;
    // How many of the converter's sentences it reads.
    [[nodiscard]] virtual std::size_t Top() const noexcept = 0;
    // How much better (ln p) a sentence must read to replace the one the
    // sentence check chose.
    [[nodiscard]] virtual double Margin() const noexcept = 0;
    // Whether the model writes conversions itself (and reads the reading).
    [[nodiscard]] virtual bool Writes() const noexcept { return false; }
    // `sentences` as ways to write `reading` (hiragana) after `context`
    // (what was written just before, maybe nothing); nothing when the
    // helper is not there or late. A model that does not read readings
    // scores the sentences alone. `write`: also the model's own writings
    // (without them the answer comes sooner).
    [[nodiscard]] virtual std::optional<AiConversion> Convert(std::wstring_view reading,
                                                              std::span<const std::wstring> sentences,
                                                              std::wstring_view context, bool write = true) const = 0;
    // Convert without waiting: the answer when this same question was the
    // last answered, else nothing, and it is asked in the background (a
    // newer question replaces one still waiting to be sent).
    [[nodiscard]] virtual std::optional<AiConversion> Ask(std::wstring_view reading,
                                                          std::span<const std::wstring> sentences,
                                                          std::wstring_view context, bool write = true) const {
        return Convert(reading, sentences, context, write);
    }
    // Whether a question Ask sent is still being answered.
    [[nodiscard]] virtual bool Asking() const { return false; }
    // Typing started: the model is wanted soon. Returns at once.
    virtual void Warm() const {}
};

// The AI models Settings offers (UserSettings::japaneseSentenceModel).
enum class AiModel : int { None = 0, Light = 1, Strong = 2 };
// Where the helper runs a model (UserSettings::japaneseAiDevice): Auto is
// the processor; the GPU or the NPU take over once their graphs are loaded
// (an NPU compiles them the first time).
enum class AiDevice : int { Auto = 0, Cpu = 1, Gpu = 2, Npu = 3 };
// Which GPU (UserSettings::japaneseAiGpu): its PCI vendor and device IDs as
// vendor << 16 | device; 0 for the one with the most memory of its own.
[[nodiscard]] constexpr std::uint32_t GpuId(std::uint32_t vendor, std::uint32_t device) noexcept {
    return (vendor & 0xFFFF) << 16 | (device & 0xFFFF);
}

// The pack each model is in.
[[nodiscard]] std::wstring_view AiModelPack(AiModel model) noexcept;

// How a pack's model is to be used, from its manifest.json.
struct AiModelKind {
    bool writes{false};  // trained for conversion: reads the reading, writes
    double margin{6.0};  // AiJudge::Margin
};
[[nodiscard]] AiModelKind ReadAiModelKind(AiModel model);

struct AiAsking;

// Asks TekitoAI.exe, starting it (once in a while) when it is not running.
// What it is sent stays in memory: neither side writes it anywhere.
class AiHelperClient final : public AiJudge {
public:
    AiHelperClient(AiModel model, AiDevice device, std::uint32_t gpu, std::chrono::milliseconds wait, std::size_t top);

    [[nodiscard]] std::size_t Top() const noexcept override { return top_; }
    [[nodiscard]] double Margin() const noexcept override { return kind_.margin; }
    [[nodiscard]] bool Writes() const noexcept override { return kind_.writes; }
    [[nodiscard]] std::optional<AiConversion> Convert(std::wstring_view reading, std::span<const std::wstring> sentences,
                                                      std::wstring_view context, bool write = true) const override;
    [[nodiscard]] std::optional<AiConversion> Ask(std::wstring_view reading, std::span<const std::wstring> sentences,
                                                  std::wstring_view context, bool write = true) const override;
    [[nodiscard]] bool Asking() const override;
    // Starts the helper and has it load the model, so the next conversion
    // need not wait for it.
    void Warm() const override;

private:
    AiModel model_;
    AiDevice device_;
    std::uint32_t gpu_;
    std::chrono::milliseconds wait_;
    std::size_t top_;
    AiModelKind kind_;
    // Ask's questions and the last answer, shared with the thread that asks.
    std::shared_ptr<AiAsking> asking_;
};

// The pipe between the input method and the helper.
namespace aihelper {

// Has a running helper exit, letting go of its model files (Settings
// replacing or removing a model); waits up to two seconds for it.
void Stop();
// The pipe's name: one per signed-in user.
[[nodiscard]] std::wstring PipeName();
// The Data Pack TekitoAI.exe and ONNX Runtime come in, downloaded with the
// first model (the same for every model).
constexpr std::wstring_view kRuntimePack = L"ai-runtime";
// TekitoAI.exe, as the client starts it and checks the pipe's server is it:
// next to the module this code is in when it is there (a build), else in
// the ai-runtime pack.
[[nodiscard]] std::wstring HelperPath();

constexpr std::uint32_t kMagic = 0x3341'4B54;  // "TKA3"
// The protocol as the ai-runtime pack's manifest names it: a pack that
// speaks another is replaced.
constexpr std::string_view kProtocol = "TKA3";
// Score: Convert without the model's own writings.
enum class Request : std::uint32_t { Convert = 0, Warm = 1, Stop = 2, Score = 3 };
enum class Status : std::uint32_t { Ok = 0, NoModel = 1, TooLong = 2, Failed = 3 };
constexpr std::size_t kMaxSentences = 32;
constexpr std::size_t kMaxCharacters = 512;
constexpr std::size_t kMaxMessage = 64 * 1024;
// How many writings a model trained for conversion gives.
constexpr std::size_t kWritten = 5;

// Request: magic, Request, model (AiModel), device (AiDevice), the GPU
// (GpuId, 0 for any), the context, the reading, count, then each sentence; each text its length and
// UTF-16 code units. Reply: magic, Status, count, each ln p as a double,
// then the writings' count and each writing (text, then its ln p).
struct Asked {
    Request request{};
    AiModel model{};
    AiDevice device{};
    std::wstring context;
    std::wstring reading;
    std::vector<std::wstring> sentences;
    std::uint32_t gpu{0};
};
[[nodiscard]] std::vector<std::uint8_t> EncodeRequest(const Asked& asked);
[[nodiscard]] std::optional<Asked> DecodeRequest(std::span<const std::uint8_t> message);
[[nodiscard]] std::vector<std::uint8_t> EncodeReply(Status status, const AiConversion& answer = {});
[[nodiscard]] std::optional<AiConversion> DecodeReply(std::span<const std::uint8_t> message);

}  // namespace aihelper

}  // namespace tekito::japanese
