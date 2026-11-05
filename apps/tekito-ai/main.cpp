// TekitoAI.exe: runs the optional AI models (the japanese-ai-* packs) for
// the input method in a process of its own, so a model's memory is held
// once and not in every app that takes text. Input method instances ask it
// over a named pipe only this user can open (japanese::aihelper); it
// answers one at a time and exits after a while without a request. What it
// is sent stays in memory and is never written anywhere.
//
// The backends are DLLs next to it, loaded at start: the processor's (the
// build for its generation) and, where the PC has a Vulkan driver, the
// GPU's.
//
//   TekitoAI.exe                                serve the pipe
//   TekitoAI.exe --convert <model> <device> <file>
//                       for each line "reading<TAB>sentence...": the model's
//                       scores and writings, and how long they took
#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/AiJudge.h"

#include "ggml-backend.h"
#include "llama.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace tekito::japanese;

// With no request for this long the helper exits and frees the model.
constexpr DWORD kIdleExit = 10 * 60 * 1000;
// Tokens the context holds: what was written before, then the text.
constexpr std::uint32_t kContext = 2048;
// Tokens read at once: the context, or one sentence, is at most this long.
constexpr std::uint32_t kBatch = 256;
// The left context a model trained for conversion reads, in characters,
// as it was trained.
constexpr std::size_t kContextChars = 40;
// The tags of that training's prompt (train-japanese-ai-model.py).
constexpr wchar_t kContextTag[] = L"";
constexpr wchar_t kInputTag[] = L"";
constexpr wchar_t kOutputTag[] = L"";

void Quiet(ggml_log_level, const char*, void*) {}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// Threads for the model: half the logical processors (the physical
// cores, mostly), 2 to 8.
int Threads() {
    const unsigned logical = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(logical / 2, 2u, 8u));
}

std::filesystem::path HereDirectory() {
    std::wstring file(MAX_PATH, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, file.data(), static_cast<DWORD>(file.size()));
    file.resize(size);
    return std::filesystem::path(file).parent_path();
}

// Whether the PC has a GPU of its own (not one built into the processor).
bool HasOwnGpu() {
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        if (ggml_backend_dev_type(ggml_backend_dev_get(i)) == GGML_BACKEND_DEVICE_TYPE_GPU) return true;
    }
    return false;
}

// One model loaded, the one asked for last, where it was asked to run.
class Model {
public:
    ~Model() { Unload(); }

    bool Load(AiModel which, AiDevice device) {
        const bool gpu = device == AiDevice::Gpu || (device == AiDevice::Auto && HasOwnGpu());
        if (which == loaded_ && gpu == gpu_ && context_) return true;
        Unload();
        const auto directory = tekito::ExternalLexiconProvider::PackDirectory(AiModelPack(which));
        const auto file = directory / L"model.gguf";
        std::error_code error;
        if (directory.empty() || !std::filesystem::is_regular_file(file, error)) return false;
        auto modelParams = llama_model_default_params();
        // On the processor no device at all, so nothing goes to a GPU.
        static ggml_backend_dev_t none[] = {nullptr};
        if (!gpu) modelParams.devices = none;
        modelParams.n_gpu_layers = gpu ? 999 : 0;
        // Weights stay mapped from the file: repacking them for speed would
        // hold a second copy in memory.
        modelParams.use_extra_bufts = false;
        model_ = llama_model_load_from_file(Utf8(file.wstring()).c_str(), modelParams);
        if (!model_) return false;
        auto contextParams = llama_context_default_params();
        contextParams.n_ctx = kContext;
        // Logits for this many tokens at a time are kept (each a row of the
        // whole vocabulary), so it stays small.
        contextParams.n_batch = kBatch;
        contextParams.n_ubatch = kBatch;
        // The start, a copy of it each sentence goes on from, and two sets
        // of writings (one growing from the other).
        contextParams.n_seq_max = static_cast<std::uint32_t>(2 * aihelper::kWritten + 2);
        contextParams.kv_unified = true;
        contextParams.n_threads = Threads();
        contextParams.n_threads_batch = Threads();
        contextParams.offload_kqv = gpu;
        contextParams.op_offload = gpu;
        context_ = llama_init_from_model(model_, contextParams);
        if (!context_) {
            Unload();
            return false;
        }
        loaded_ = which;
        gpu_ = gpu;
        writes_ = ReadAiModelKind(which).writes;
        return true;
    }

    std::optional<AiConversion> Convert(const aihelper::Asked& asked) {
        if (!writes_) {
            auto scores = Score(asked.sentences, asked.context);
            if (!scores) return std::nullopt;
            return AiConversion{std::move(*scores), {}};
        }
        std::wstring prompt;
        if (!asked.context.empty()) {
            prompt = kContextTag;
            prompt += asked.context.size() > kContextChars ? asked.context.substr(asked.context.size() - kContextChars)
                                                           : asked.context;
        }
        prompt += kInputTag + asked.reading + kOutputTag;
        auto scores = ScoreAfter(prompt, asked.sentences);
        if (!scores) return std::nullopt;
        AiConversion answer{std::move(*scores), {}};
        if (asked.request != aihelper::Request::Score) answer.written = Write(prompt, asked.reading.size());
        return answer;
    }

    // ln p of each sentence after `context`, for a model that reads
    // sentences alone. The start (BOS and the context) is read once and each
    // sentence goes on from a copy of it. Without a context the first token
    // is not counted, as when the margins were chosen.
    std::optional<std::vector<double>> Score(const std::vector<std::wstring>& sentences, std::wstring_view context) {
        const auto start = Tokenize(Utf8(context), true);
        if (start.empty() || start.size() > kBatch) return std::nullopt;
        llama_memory_clear(Memory(), true);
        if (!Decode(start, 0, 0, false)) return std::nullopt;
        const auto startLogits = LastLogits();
        std::vector<double> scores;
        for (const auto& sentence : sentences) {
            const auto tokens = Tokenize(Utf8(sentence), false);
            if (tokens.empty()) {
                scores.push_back(0);
                continue;
            }
            if (tokens.size() > kBatch || start.size() + tokens.size() > kContext) return std::nullopt;
            Branch(0, 1);
            if (!Decode(tokens, 1, static_cast<int>(start.size()), true)) return std::nullopt;
            double sum = context.empty() ? 0 : LogProbability(startLogits.data(), tokens.front());
            for (std::size_t i = 1; i < tokens.size(); ++i) sum += LogProbability(Logits(static_cast<int>(i) - 1), tokens[i]);
            scores.push_back(sum);
        }
        return scores;
    }

    void Unload() {
        if (context_) llama_free(context_);
        if (model_) llama_model_free(model_);
        context_ = nullptr;
        model_ = nullptr;
        loaded_ = AiModel::None;
    }

private:
    llama_memory_t Memory() const { return llama_get_memory(context_); }
    const llama_vocab* Vocab() const { return llama_model_get_vocab(model_); }
    int VocabSize() const { return llama_vocab_n_tokens(Vocab()); }
    const float* Logits(int i) const { return llama_get_logits_ith(context_, i); }
    std::vector<float> LastLogits() const {
        const float* last = Logits(-1);
        return std::vector<float>(last, last + VocabSize());
    }

    double LogProbability(const float* logits, llama_token token) const {
        const int size = VocabSize();
        const float top = *std::max_element(logits, logits + size);
        double total = 0;
        for (int v = 0; v < size; ++v) total += std::exp(static_cast<double>(logits[v] - top));
        return logits[token] - top - std::log(total);
    }

    // Sequence `to` made a copy of sequence `from`.
    void Branch(llama_seq_id from, llama_seq_id to) {
        llama_memory_seq_rm(Memory(), to, -1, -1);
        llama_memory_seq_cp(Memory(), from, to, -1, -1);
    }

    std::vector<llama_token> Tokenize(const std::string& text, bool start) const {
        std::vector<llama_token> tokens(text.size() + 8);
        const int count = llama_tokenize(Vocab(), text.c_str(), static_cast<int>(text.size()), tokens.data(),
                                         static_cast<int>(tokens.size()), start, false);
        tokens.resize(count > 0 ? static_cast<std::size_t>(count) : 0);
        return tokens;
    }

    // Reads `tokens` into sequence `sequence` from position `at`, with
    // logits for every token (`all`) or the last only.
    bool Decode(const std::vector<llama_token>& tokens, llama_seq_id sequence, int at, bool all) {
        const int count = static_cast<int>(tokens.size());
        llama_batch batch = llama_batch_init(count, 0, 1);
        for (int i = 0; i < count; ++i) {
            batch.token[i] = tokens[static_cast<std::size_t>(i)];
            batch.pos[i] = at + i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = sequence;
            batch.logits[i] = all || i == count - 1;
        }
        batch.n_tokens = count;
        const bool ok = llama_decode(context_, batch) == 0;
        llama_batch_free(batch);
        return ok;
    }

    // ln p of each sentence and the end after the prompt (BOS, prompt).
    std::optional<std::vector<double>> ScoreAfter(const std::wstring& prompt, const std::vector<std::wstring>& sentences) {
        const auto start = Tokenize(Utf8(prompt), true);
        if (start.empty() || start.size() > kBatch) return std::nullopt;
        llama_memory_clear(Memory(), true);
        if (!Decode(start, 0, 0, false)) return std::nullopt;
        promptLogits_ = LastLogits();
        promptTokens_ = static_cast<int>(start.size());
        const llama_token end = llama_vocab_eos(Vocab());
        std::vector<double> scores;
        for (const auto& sentence : sentences) {
            auto tokens = Tokenize(Utf8(sentence), false);
            tokens.push_back(end);
            if (tokens.size() > kBatch || start.size() + tokens.size() > kContext) return std::nullopt;
            Branch(0, 1);
            if (!Decode(tokens, 1, promptTokens_, true)) return std::nullopt;
            double sum = LogProbability(promptLogits_.data(), tokens.front());
            for (std::size_t i = 1; i < tokens.size(); ++i) sum += LogProbability(Logits(static_cast<int>(i) - 1), tokens[i]);
            scores.push_back(sum);
        }
        return scores;
    }

    // The likeliest writings after the prompt read last (ScoreAfter), by
    // beam search: each beam a sequence; a longer one copies its parent's
    // and reads one token more.
    std::vector<std::pair<std::wstring, double>> Write(const std::wstring& prompt, std::size_t readingLength) {
        (void)prompt;
        constexpr int kBeams = static_cast<int>(aihelper::kWritten);
        struct Beam {
            std::vector<llama_token> tokens;
            double score{0};
            bool done{false};
            llama_seq_id seq{0};
            std::vector<std::pair<float, llama_token>> next;
        };
        const int size = VocabSize();
        const auto best = [&](const float* logits) {
            const float top = *std::max_element(logits, logits + size);
            double total = 0;
            for (int v = 0; v < size; ++v) total += std::exp(static_cast<double>(logits[v] - top));
            const float logTotal = static_cast<float>(std::log(total)) + top;
            std::vector<std::pair<float, llama_token>> all;
            all.reserve(static_cast<std::size_t>(size));
            for (int v = 0; v < size; ++v) all.emplace_back(logits[v] - logTotal, v);
            std::partial_sort(all.begin(), all.begin() + kBeams, all.end(),
                              [](const auto& a, const auto& b) { return a.first > b.first; });
            all.resize(kBeams);
            return all;
        };
        std::vector<Beam> beams(1);
        beams[0].next = best(promptLogits_.data());
        // Tokens: plenty for any writing of the reading.
        const int limit = static_cast<int>(readingLength) * 2 + 8;
        for (int step = 0; step < limit; ++step) {
            struct Option {
                double score;
                int from;
                llama_token token;  // -1: the beam as it is (done)
            };
            std::vector<Option> options;
            for (int b = 0; b < static_cast<int>(beams.size()); ++b) {
                if (beams[b].done) {
                    options.push_back({beams[b].score, b, -1});
                    continue;
                }
                for (const auto& [lp, token] : beams[b].next) options.push_back({beams[b].score + lp, b, token});
            }
            std::sort(options.begin(), options.end(), [](const Option& a, const Option& b) { return a.score > b.score; });
            if (static_cast<int>(options.size()) > kBeams) options.resize(kBeams);
            // Children go in the set of sequences the parents are not in.
            const llama_seq_id base = (step % 2 == 0) ? 2 + kBeams : 2;
            std::vector<Beam> children;
            for (int k = 0; k < static_cast<int>(options.size()); ++k) {
                const Beam& parent = beams[options[k].from];
                Beam child{parent.tokens, options[k].score, parent.done, base + k, {}};
                if (options[k].token >= 0) {
                    child.tokens.push_back(options[k].token);
                    child.done = llama_vocab_is_eog(Vocab(), options[k].token);
                    if (!child.done) {
                        Branch(parent.seq, child.seq);
                        const int at = promptTokens_ + static_cast<int>(child.tokens.size()) - 1;
                        if (Decode({options[k].token}, child.seq, at, true)) {
                            child.next = best(Logits(-1));
                        } else {
                            child.done = true;
                        }
                    }
                }
                children.push_back(std::move(child));
            }
            for (const auto& beam : beams) {
                if (beam.seq) llama_memory_seq_rm(Memory(), beam.seq, -1, -1);
            }
            beams = std::move(children);
            if (std::all_of(beams.begin(), beams.end(), [](const Beam& b) { return b.done; })) break;
        }
        std::vector<std::pair<std::wstring, double>> written;
        for (const auto& beam : beams) {
            if (!beam.done) continue;
            std::string text;
            for (const llama_token t : beam.tokens) {
                if (llama_vocab_is_eog(Vocab(), t)) break;
                char piece[256];
                const int n = llama_token_to_piece(Vocab(), t, piece, sizeof(piece), 0, false);
                if (n > 0) text.append(piece, static_cast<std::size_t>(n));
            }
            auto wide = Wide(text);
            const bool seen = std::any_of(written.begin(), written.end(), [&](const auto& w) { return w.first == wide; });
            if (!wide.empty() && !seen) written.emplace_back(std::move(wide), beam.score);
        }
        return written;
    }

    llama_model* model_{nullptr};
    llama_context* context_{nullptr};
    AiModel loaded_{AiModel::None};
    bool gpu_{false};
    bool writes_{false};
    std::vector<float> promptLogits_;
    int promptTokens_{0};
};

std::vector<std::uint8_t> Answer(Model& model, std::span<const std::uint8_t> message, bool& stop) {
    const auto asked = aihelper::DecodeRequest(message);
    if (!asked) return aihelper::EncodeReply(aihelper::Status::Failed);
    if (asked->request == aihelper::Request::Stop) {
        model.Unload();
        stop = true;
        return aihelper::EncodeReply(aihelper::Status::Ok);
    }
    if (!model.Load(asked->model, asked->device)) return aihelper::EncodeReply(aihelper::Status::NoModel);
    if (asked->request == aihelper::Request::Warm) return aihelper::EncodeReply(aihelper::Status::Ok);
    const auto answer = model.Convert(*asked);
    if (!answer) return aihelper::EncodeReply(aihelper::Status::TooLong);
    return aihelper::EncodeReply(aihelper::Status::Ok, *answer);
}

// The pipe, open to this user only and not over the network.
HANDLE CreatePipe(const std::wstring& name, bool first) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return INVALID_HANDLE_VALUE;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buffer(size);
    std::wstring sid;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
            sid = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    if (sid.empty()) return INVALID_HANDLE_VALUE;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES attributes{sizeof attributes, descriptor, FALSE};
    const DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE pipe = CreateNamedPipeW(name.c_str(), open, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                                                          PIPE_REJECT_REMOTE_CLIENTS,
                                   1, static_cast<DWORD>(aihelper::kMaxMessage),
                                   static_cast<DWORD>(aihelper::kMaxMessage), 0, &attributes);
    LocalFree(descriptor);
    return pipe;
}

// Waits for an overlapped operation; false on failure or after `wait`.
bool Wait(HANDLE pipe, OVERLAPPED& overlapped, BOOL started, DWORD wait, DWORD& done) {
    if (!started) {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) return true;
        if (error != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(overlapped.hEvent, wait) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &done, TRUE);
            return false;
        }
    }
    return GetOverlappedResult(pipe, &overlapped, &done, FALSE) != FALSE;
}

int Serve() {
    const std::wstring name = aihelper::PipeName();
    if (name.empty()) return 1;
    // One helper per user and session: a second one finds the pipe taken.
    HANDLE pipe = CreatePipe(name, true);
    if (pipe == INVALID_HANDLE_VALUE) return 0;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Model model;
    std::vector<std::uint8_t> message(aihelper::kMaxMessage);
    constexpr DWORD kReadWait = 2000;
    bool stop = false;
    while (!stop) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        ResetEvent(event);
        DWORD done = 0;
        const BOOL connected = ConnectNamedPipe(pipe, &overlapped);
        if (!Wait(pipe, overlapped, connected, kIdleExit, done)) break;
        overlapped = {};
        overlapped.hEvent = event;
        ResetEvent(event);
        const BOOL read = ReadFile(pipe, message.data(), static_cast<DWORD>(message.size()), nullptr, &overlapped);
        if (Wait(pipe, overlapped, read, kReadWait, done)) {
            const auto reply = Answer(model, std::span<const std::uint8_t>(message.data(), done), stop);
            overlapped = {};
            overlapped.hEvent = event;
            ResetEvent(event);
            const BOOL wrote = WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), nullptr, &overlapped);
            Wait(pipe, overlapped, wrote, kReadWait, done);
            FlushFileBuffers(pipe);
        }
        SecureZeroMemory(message.data(), message.size());
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(event);
    CloseHandle(pipe);
    return 0;
}

// For checking a pack: each line "reading<TAB>sentence..." asked as the
// input method asks, the answer and how long it took printed.
int ConvertFile(AiModel which, AiDevice device, const std::filesystem::path& file) {
    Model model;
    if (!model.Load(which, device)) {
        std::fprintf(stderr, "the model is not installed\n");
        return 2;
    }
    std::ifstream in(file, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::wstring> fields;
        std::stringstream ss(line);
        for (std::string field; std::getline(ss, field, '\t');) fields.push_back(Wide(field));
        if (fields.empty()) continue;
        aihelper::Asked asked{aihelper::Request::Convert, which, device, {}, fields[0],
                              std::vector<std::wstring>(fields.begin() + 1, fields.end())};
        const auto start = std::chrono::steady_clock::now();
        const auto answer = model.Convert(asked);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("%s\t%.1f ms\n", Utf8(fields[0]).c_str(), ms);
        if (!answer) continue;
        for (std::size_t i = 0; i < answer->scores.size(); ++i) {
            std::printf("  %.2f\t%s\n", answer->scores[i], Utf8(fields[i + 1]).c_str());
        }
        for (const auto& [text, score] : answer->written) std::printf("  w %.2f\t%s\n", score, Utf8(text).c_str());
    }
    return 0;
}

}  // namespace

// A console program, started without a console (CREATE_NO_WINDOW), so
// --convert prints where it was run from.
int wmain(int argc, wchar_t** argv) {
    // TEKITO_AI_LOG=1: llama.cpp's messages on the console, for --convert.
    if (!_wgetenv(L"TEKITO_AI_LOG")) llama_log_set(Quiet, nullptr);
    ggml_backend_load_all_from_path(Utf8(HereDirectory().wstring()).c_str());
    llama_backend_init();
    int result = 0;
    if (argc == 5 && std::wstring_view(argv[1]) == L"--convert") {
        const AiModel which = std::wstring_view(argv[2]) == L"strong" ? AiModel::Strong : AiModel::Light;
        const std::wstring_view where = argv[3];
        const AiDevice device = where == L"cpu" ? AiDevice::Cpu : where == L"gpu" ? AiDevice::Gpu : AiDevice::Auto;
        result = ConvertFile(which, device, argv[4]);
    } else {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        result = Serve();
    }
    llama_backend_free();
    return result;
}
