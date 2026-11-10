#include "Core/Japanese/AiJudge.h"

#include "Core/ExternalLexiconProvider.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>

namespace tekito::japanese {
namespace {

using Clock = std::chrono::steady_clock;

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

void Put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof value);
}

// Reads what `message` holds at `at`, moving past it; false at the end.
template <typename T>
bool Take(std::span<const std::uint8_t> message, std::size_t& at, T& value) {
    if (message.size() - at < sizeof value) return false;
    std::memcpy(&value, message.data() + at, sizeof value);
    at += sizeof value;
    return true;
}

std::wstring Lower(std::wstring text) {
    for (auto& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

// Whether the pipe's server is the helper next to this module, so text
// is never handed to a program that took the pipe's name first.
bool ServerIsHelper(HANDLE pipe) {
    ULONG id = 0;
    if (!GetNamedPipeServerProcessId(pipe, &id)) return false;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id)};
    if (!process.value) {
        process.value = INVALID_HANDLE_VALUE;
        return false;
    }
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(path));
    if (!QueryFullProcessImageNameW(process.value, 0, path, &size)) return false;
    return Lower(std::wstring(path, size)) == Lower(aihelper::HelperPath());
}

// Starts the helper, at most every few seconds, and not at all when it is
// not installed next to this module.
void StartHelper() {
    static std::atomic<std::int64_t> lastStart{0};
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count();
    constexpr std::int64_t kEvery = 10;
    auto last = lastStart.load();
    if (last && now - last < kEvery) return;
    if (!lastStart.compare_exchange_strong(last, now)) return;
    std::wstring path = aihelper::HelperPath();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::wstring command = L"\"" + path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION started{};
    // Out of the app's job where it may, so closing the app leaves it to
    // the other apps that use it.
    for (const DWORD flags : {static_cast<DWORD>(CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB),
                              static_cast<DWORD>(CREATE_NO_WINDOW)}) {
        if (CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup,
                           &started)) {
            CloseHandle(started.hThread);
            CloseHandle(started.hProcess);
            return;
        }
    }
}

// Waits for an overlapped read or write on `pipe` until `until`.
bool Finish(HANDLE pipe, OVERLAPPED& overlapped, BOOL started, Clock::time_point until, DWORD& done) {
    if (!started && GetLastError() != ERROR_IO_PENDING && GetLastError() != ERROR_MORE_DATA) return false;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
    if (WaitForSingleObject(overlapped.hEvent, static_cast<DWORD>(std::max<long long>(left, 0))) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, &done, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &overlapped, &done, FALSE) != FALSE;
}

// Sends `request` and returns the reply, or nothing when the helper is not
// running (it is started for next time), is someone else, or is late.
std::optional<std::vector<std::uint8_t>> Exchange(const std::vector<std::uint8_t>& request, Clock::time_point until) {
    const std::wstring name = aihelper::PipeName();
    if (name.empty()) return std::nullopt;
    Handle pipe;
    for (;;) {
        pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) StartHelper();
        if (error != ERROR_PIPE_BUSY) return std::nullopt;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        if (left <= 0 || !WaitNamedPipeW(name.c_str(), static_cast<DWORD>(left))) return std::nullopt;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.value, &mode, nullptr, nullptr) || !ServerIsHelper(pipe.value)) {
        return std::nullopt;
    }
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) {
        event.value = INVALID_HANDLE_VALUE;
        return std::nullopt;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.value;
    DWORD done = 0;
    const BOOL wrote = WriteFile(pipe.value, request.data(), static_cast<DWORD>(request.size()), nullptr, &overlapped);
    if (!Finish(pipe.value, overlapped, wrote, until, done) || done != request.size()) return std::nullopt;
    std::vector<std::uint8_t> answer(aihelper::kMaxMessage);
    ResetEvent(event.value);
    overlapped = {};
    overlapped.hEvent = event.value;
    const BOOL read = ReadFile(pipe.value, answer.data(), static_cast<DWORD>(answer.size()), nullptr, &overlapped);
    if (!Finish(pipe.value, overlapped, read, until, done)) return std::nullopt;
    answer.resize(done);
    return answer;
}

}  // namespace

std::wstring_view AiModelPack(AiModel model) noexcept {
    switch (model) {
    case AiModel::Light:
        return L"japanese-ai-light";
    case AiModel::Strong:
        return L"japanese-ai-strong";
    default:
        return {};
    }
}

AiModelKind ReadAiModelKind(AiModel model) {
    AiModelKind kind;
    const auto directory = ExternalLexiconProvider::PackDirectory(AiModelPack(model));
    std::ifstream in(directory / L"manifest.json", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // "prompt": "tekito-reading-v1" marks a model trained for conversion;
    // "margin": its own (ln p).
    kind.writes = manifest.find("\"tekito-reading-v1\"") != std::string::npos;
    if (const auto at = manifest.find("\"margin\""); at != std::string::npos) {
        const auto colon = manifest.find(':', at);
        if (colon != std::string::npos) kind.margin = std::strtod(manifest.c_str() + colon + 1, nullptr);
    }
    return kind;
}

struct AiAsking {
    std::mutex mutex;
    // How long a question asked in the background may take: the helper may
    // still be loading the model.
    std::chrono::milliseconds wait{10'000};
    std::vector<std::uint8_t> sending;            // being answered
    std::optional<std::vector<std::uint8_t>> next;  // to send after it
    std::vector<std::uint8_t> answered;           // the last answered
    std::optional<AiConversion> answer;           // its answer
    bool running{false};
};

AiHelperClient::AiHelperClient(AiModel model, AiDevice device, std::uint32_t gpu, std::chrono::milliseconds wait,
                               std::size_t top)
    : model_(model), device_(device), gpu_(gpu), wait_(wait), top_(top), kind_(ReadAiModelKind(model)),
      asking_(std::make_shared<AiAsking>()) {}

std::optional<AiConversion> AiHelperClient::Convert(std::wstring_view reading, std::span<const std::wstring> sentences,
                                                    std::wstring_view context, bool write) const {
    if (sentences.size() > aihelper::kMaxSentences || reading.size() > aihelper::kMaxCharacters) return std::nullopt;
    if (context.size() > aihelper::kMaxCharacters) context = context.substr(context.size() - aihelper::kMaxCharacters);
    aihelper::Asked asked{write ? aihelper::Request::Convert : aihelper::Request::Score, model_, device_,
                          std::wstring(context), std::wstring(reading),
                          std::vector<std::wstring>(sentences.begin(), sentences.end()), gpu_};
    const auto reply = Exchange(aihelper::EncodeRequest(asked), Clock::now() + wait_);
    if (!reply) return std::nullopt;
    auto answer = aihelper::DecodeReply(*reply);
    if (!answer || answer->scores.size() != sentences.size()) return std::nullopt;
    return answer;
}

namespace {

// This module, kept loaded while the thread pool runs its code.
HMODULE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module;
}

// Sends the questions in turn until none is waiting.
void CALLBACK AskInBackground(PTP_CALLBACK_INSTANCE, void* context) {
    std::unique_ptr<std::shared_ptr<AiAsking>> owned(static_cast<std::shared_ptr<AiAsking>*>(context));
    AiAsking& asking = **owned;
    std::unique_lock lock(asking.mutex);
    for (;;) {
        const auto request = asking.sending;
        const auto wait = asking.wait;
        lock.unlock();
        const auto reply = Exchange(request, Clock::now() + wait);
        auto answer = reply ? aihelper::DecodeReply(*reply) : std::nullopt;
        lock.lock();
        asking.answered = request;
        asking.answer = std::move(answer);
        if (!asking.next) break;
        asking.sending = std::move(*asking.next);
        asking.next.reset();
    }
    asking.running = false;
}

}  // namespace

std::optional<AiConversion> AiHelperClient::Ask(std::wstring_view reading, std::span<const std::wstring> sentences,
                                                std::wstring_view context, bool write) const {
    if (sentences.size() > aihelper::kMaxSentences || reading.size() > aihelper::kMaxCharacters) return std::nullopt;
    if (context.size() > aihelper::kMaxCharacters) context = context.substr(context.size() - aihelper::kMaxCharacters);
    aihelper::Asked asked{write ? aihelper::Request::Convert : aihelper::Request::Score, model_, device_,
                          std::wstring(context), std::wstring(reading),
                          std::vector<std::wstring>(sentences.begin(), sentences.end()), gpu_};
    auto request = aihelper::EncodeRequest(asked);
    std::lock_guard lock(asking_->mutex);
    if (asking_->answered == request) {
        if (asking_->answer && asking_->answer->scores.size() == sentences.size()) return asking_->answer;
        return std::nullopt;
    }
    if (asking_->running) {
        if (asking_->sending != request) asking_->next = std::move(request);
        return std::nullopt;
    }
    TP_CALLBACK_ENVIRON environment;
    InitializeThreadpoolEnvironment(&environment);
    if (HMODULE module = ThisModule()) SetThreadpoolCallbackLibrary(&environment, module);
    auto* shared = new std::shared_ptr<AiAsking>(asking_);
    asking_->sending = std::move(request);
    asking_->running = true;
    if (!TrySubmitThreadpoolCallback(&AskInBackground, shared, &environment)) {
        asking_->running = false;
        delete shared;
    }
    DestroyThreadpoolEnvironment(&environment);
    return std::nullopt;
}

bool AiHelperClient::Asking() const {
    std::lock_guard lock(asking_->mutex);
    return asking_->running;
}

void AiHelperClient::Warm() const {
    constexpr auto kWait = std::chrono::milliseconds(20);
    aihelper::Asked asked{aihelper::Request::Warm, model_, device_, {}, {}, {}, gpu_};
    (void)Exchange(aihelper::EncodeRequest(asked), Clock::now() + kWait);
}

namespace aihelper {

void Stop() {
    const auto request = EncodeRequest({Request::Stop, AiModel::Light, AiDevice::Auto, {}, {}, {}});
    const std::wstring name = PipeName();
    if (name.empty()) return;
    // Not started for this: only a helper that is there is told to go.
    if (!WaitNamedPipeW(name.c_str(), 1) && GetLastError() == ERROR_FILE_NOT_FOUND) return;
    (void)Exchange(request, Clock::now() + std::chrono::seconds(2));
}

std::wstring PipeName() {
    static const std::wstring name = [] {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return std::wstring();
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<std::uint8_t> buffer(size);
        std::wstring result;
        if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
                DWORD session = 0;
                ProcessIdToSessionId(GetCurrentProcessId(), &session);
                result = L"\\\\.\\pipe\\TEKITO-AI-" + std::wstring(sid) + L"-" + std::to_wstring(session);
                LocalFree(sid);
            }
        }
        CloseHandle(token);
        return result;
    }();
    return name;
}

std::wstring HelperPath() {
    static const std::wstring path = [] {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&HelperPath), &module)) {
            return std::wstring();
        }
        std::wstring file(MAX_PATH, L'\0');
        for (;;) {
            const DWORD size = GetModuleFileNameW(module, file.data(), static_cast<DWORD>(file.size()));
            if (size == 0) return std::wstring();
            if (size < file.size()) {
                file.resize(size);
                break;
            }
            file.resize(file.size() * 2);
        }
        const auto slash = file.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            std::wstring local = file.substr(0, slash + 1) + L"TekitoAI.exe";
            if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
        }
        const auto root = ExternalLexiconProvider::DataPackRoot();
        return root.empty() ? std::wstring() : (root / std::wstring(kRuntimePack) / L"TekitoAI.exe").wstring();
    }();
    return path;
}

namespace {

void PutText(std::vector<std::uint8_t>& out, std::wstring_view text) {
    Put32(out, static_cast<std::uint32_t>(text.size()));
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    out.insert(out.end(), bytes, bytes + text.size() * sizeof(wchar_t));
}

bool TakeText(std::span<const std::uint8_t> message, std::size_t& at, std::wstring& text) {
    std::uint32_t length = 0;
    if (!Take(message, at, length) || length > kMaxCharacters) return false;
    if (message.size() - at < length * sizeof(wchar_t)) return false;
    text.assign(length, L'\0');
    std::memcpy(text.data(), message.data() + at, length * sizeof(wchar_t));
    at += length * sizeof(wchar_t);
    return true;
}

void PutDouble(std::vector<std::uint8_t>& out, double value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof value);
}

}  // namespace

std::vector<std::uint8_t> EncodeRequest(const Asked& asked) {
    std::vector<std::uint8_t> out;
    Put32(out, kMagic);
    Put32(out, static_cast<std::uint32_t>(asked.request));
    Put32(out, static_cast<std::uint32_t>(asked.model));
    Put32(out, static_cast<std::uint32_t>(asked.device));
    Put32(out, asked.gpu);
    PutText(out, asked.context);
    PutText(out, asked.reading);
    Put32(out, static_cast<std::uint32_t>(asked.sentences.size()));
    for (const auto& sentence : asked.sentences) PutText(out, sentence);
    return out;
}

std::optional<Asked> DecodeRequest(std::span<const std::uint8_t> message) {
    std::size_t at = 0;
    std::uint32_t magic = 0, request = 0, model = 0, device = 0, count = 0;
    Asked asked;
    if (!Take(message, at, magic) || magic != kMagic || !Take(message, at, request) || !Take(message, at, model) ||
        !Take(message, at, device) || !Take(message, at, asked.gpu) || !TakeText(message, at, asked.context) || !TakeText(message, at, asked.reading) ||
        !Take(message, at, count)) {
        return std::nullopt;
    }
    if (request > static_cast<std::uint32_t>(Request::Score) || model < 1 ||
        model > static_cast<std::uint32_t>(AiModel::Strong) || device > static_cast<std::uint32_t>(AiDevice::Npu) ||
        count > kMaxSentences) {
        return std::nullopt;
    }
    asked.request = static_cast<Request>(request);
    asked.model = static_cast<AiModel>(model);
    asked.device = static_cast<AiDevice>(device);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!TakeText(message, at, asked.sentences.emplace_back())) return std::nullopt;
    }
    if (at != message.size()) return std::nullopt;
    return asked;
}

std::vector<std::uint8_t> EncodeReply(Status status, const AiConversion& answer) {
    std::vector<std::uint8_t> out;
    Put32(out, kMagic);
    Put32(out, static_cast<std::uint32_t>(status));
    Put32(out, static_cast<std::uint32_t>(answer.scores.size()));
    for (const double score : answer.scores) PutDouble(out, score);
    Put32(out, static_cast<std::uint32_t>(answer.written.size()));
    for (const auto& [text, score] : answer.written) {
        PutText(out, text);
        PutDouble(out, score);
    }
    return out;
}

std::optional<AiConversion> DecodeReply(std::span<const std::uint8_t> message) {
    std::size_t at = 0;
    std::uint32_t magic = 0, status = 0, count = 0, written = 0;
    if (!Take(message, at, magic) || magic != kMagic || !Take(message, at, status) || !Take(message, at, count) ||
        status != static_cast<std::uint32_t>(Status::Ok) || count > kMaxSentences) {
        return std::nullopt;
    }
    AiConversion answer;
    answer.scores.resize(count);
    for (auto& score : answer.scores) {
        if (!Take(message, at, score)) return std::nullopt;
    }
    if (!Take(message, at, written) || written > kMaxSentences) return std::nullopt;
    for (std::uint32_t i = 0; i < written; ++i) {
        std::wstring text;
        double score = 0;
        if (!TakeText(message, at, text) || !Take(message, at, score)) return std::nullopt;
        answer.written.emplace_back(std::move(text), score);
    }
    if (at != message.size()) return std::nullopt;
    return answer;
}

}  // namespace aihelper

}  // namespace tekito::japanese
