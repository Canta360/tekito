// TekitoAI.exe: runs the optional AI models (the japanese-ai-* packs) for
// the input method in a process of its own, so a model's memory is held
// once and not in every app that takes text. Input method instances ask it
// over a named pipe only this user can open (japanese::aihelper); it
// answers one at a time and exits after a while without a request. What it
// is sent stays in memory and is never written anywhere.
//
// It runs them with ONNX Runtime as Windows ML ships it (onnxruntime.dll,
// DirectML.dll and Microsoft.Windows.AI.MachineLearning.dll next to it): on
// the processor, or on the GPU through DirectML.
//
//   TekitoAI.exe                                serve the pipe
//   TekitoAI.exe --convert <model> <device> <file>
//                       for each line "reading<TAB>sentence...": the model's
//                       scores and writings, and how long they took
#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/AiJudge.h"

#include "ByteBpe.h"

#include <WinMLEpCatalog.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <sddl.h>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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

// A tensor of the model's state kept on the CPU: its shape and floats.
struct Tensor {
    std::vector<std::int64_t> shape;
    std::vector<float> data;

    [[nodiscard]] std::size_t Row() const {
        std::size_t n = 1;
        for (std::size_t i = 1; i < shape.size(); ++i) n *= static_cast<std::size_t>(shape[i]);
        return n;
    }
    // An attention state ([batch, heads, past, dim]) as `n` places, its own
    // at the end (a fixed graph's window); any other state as it is.
    [[nodiscard]] Tensor PadTo(std::size_t n) const {
        if (shape.size() != 4 || static_cast<std::size_t>(shape[2]) >= n) return *this;
        Tensor out{shape, {}};
        out.shape[2] = static_cast<std::int64_t>(n);
        const auto places = static_cast<std::size_t>(shape[2]), dim = static_cast<std::size_t>(shape[3]);
        const std::size_t rows = static_cast<std::size_t>(shape[0] * shape[1]);
        out.data.assign(rows * n * dim, 0.0f);
        for (std::size_t r = 0; r < rows; ++r) {
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(r * places * dim), places * dim,
                        out.data.begin() + static_cast<std::ptrdiff_t>((r * n + n - places) * dim));
        }
        return out;
    }
    // Rows `from` (indices into this one's batch), in that order.
    [[nodiscard]] Tensor Rows(const std::vector<std::size_t>& from) const {
        Tensor out{shape, {}};
        out.shape[0] = static_cast<std::int64_t>(from.size());
        const std::size_t row = Row();
        out.data.resize(from.size() * row);
        for (std::size_t i = 0; i < from.size(); ++i) {
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(from[i] * row), row,
                        out.data.begin() + static_cast<std::ptrdiff_t>(i * row));
        }
        return out;
    }
};

// The DXGI adapter (and DirectML device) of the GPU with the most memory of
// The DirectML device (DXGI's adapter index) of the GPU `gpu` (GpuId), or
// with 0 or that GPU not there, of the one with the most memory of its own.
std::optional<int> PickGpu(std::uint32_t gpu) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return std::nullopt;
    std::optional<int> best, asked;
    SIZE_T most = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        adapter->Release();
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (gpu != 0 && !asked && tekito::japanese::GpuId(desc.VendorId, desc.DeviceId) == gpu) asked = static_cast<int>(i);
        if (!best || desc.DedicatedVideoMemory > most) {
            most = desc.DedicatedVideoMemory;
            best = static_cast<int>(i);
        }
    }
    factory->Release();
    return asked ? asked : best;
}

// Whether Windows ML came with this copy (the ai-runtime pack for GPUs and
// NPUs: its ONNX Runtime with DirectML, and its catalog of NPU providers);
// the processor's pack has ONNX Runtime alone. Its DLL is loaded only then
// (delay-loaded).
bool HasWindowsMl() {
    static const bool has = [] {
        std::wstring file(MAX_PATH, L'\0');
        const DWORD size = GetModuleFileNameW(nullptr, file.data(), static_cast<DWORD>(file.size()));
        if (size == 0 || size >= file.size()) return false;
        file.resize(size);
        const auto path = std::filesystem::path(file).parent_path() / L"Microsoft.Windows.AI.MachineLearning.dll";
        std::error_code error;
        return std::filesystem::is_regular_file(path, error);
    }();
    return has;
}

// DirectML on the GPU `adapter`, when this ONNX Runtime has it.
bool AppendDirectMl(Ort::SessionOptions& options, int adapter) {
    using Append = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
    const HMODULE runtime = GetModuleHandleW(L"onnxruntime.dll");
    const auto append =
        runtime ? reinterpret_cast<Append>(GetProcAddress(runtime, "OrtSessionOptionsAppendExecutionProvider_DML")) : nullptr;
    if (!append) return false;
    Ort::ThrowOnError(append(options, adapter));
    return true;
}

Ort::Env& Environment() {
    static Ort::Env env = [] {
        Ort::Env e(ORT_LOGGING_LEVEL_ERROR, "TekitoAI");
        // Nothing about the use of the model is reported anywhere.
        e.DisableTelemetryEvents();
        return e;
    }();
    return env;
}

// The execution providers Windows ML has on this PC (the catalog's that are
// installed, made ready for this app: none is downloaded here), registered
// with ONNX Runtime; each call registers those not registered yet.
void RegisterReadyProviders() {
    static std::mutex lock;
    static std::set<std::string> registered;
    std::lock_guard hold(lock);
    if (HasWindowsMl()) {
        WinMLEpCatalogHandle catalog = nullptr;
        if (FAILED(WinMLEpCatalogCreate(&catalog))) return;
        std::vector<std::pair<std::string, std::string>> ready;
        WinMLEpCatalogEnumProviders(
            catalog,
            [](WinMLEpHandle ep, const WinMLEpInfo* info, void* data) -> BOOL {
                if (!info || !info->name || info->readyState == WinMLEpReadyState_NotPresent) return TRUE;
                if (info->readyState == WinMLEpReadyState_NotReady && FAILED(WinMLEpEnsureReady(ep))) return TRUE;
                std::size_t size = 0;
                if (FAILED(WinMLEpGetLibraryPathSize(ep, &size)) || size == 0) return TRUE;
                std::string path(size, '\0');
                std::size_t used = 0;
                if (FAILED(WinMLEpGetLibraryPath(ep, size, path.data(), &used)) || used == 0) return TRUE;
                path.resize(used - 1);
                static_cast<std::vector<std::pair<std::string, std::string>>*>(data)->emplace_back(info->name, path);
                return TRUE;
            },
            &ready);
        WinMLEpCatalogRelease(catalog);
        for (const auto& [name, path] : ready) {
            if (registered.contains(name)) continue;
            try {
                Environment().RegisterExecutionProviderLibrary(name.c_str(), std::filesystem::path(path).wstring());
                registered.insert(name);
            } catch (const Ort::Exception&) {
            }
        }
    }
}

// The NPU devices of the providers registered (OpenVINO, QNN, VitisAI...).
std::vector<Ort::ConstEpDevice> NpuDevices() {
    RegisterReadyProviders();
    std::vector<Ort::ConstEpDevice> npus;
    for (const auto& device : Environment().GetEpDevices()) {
        const std::string_view name = device.EpName();
        // The provider's own device, not its automatic choice among devices.
        if (device.Device().Type() == OrtHardwareDeviceType_NPU && name.find('.') == std::string_view::npos) {
            npus.push_back(device);
            break;
        }
    }
    return npus;
}

// One graph of a pack: its session, input and output names, and the
// state's shapes ([batch, hidden, 2] and [batch, heads, past, dim]; -1 where
// any size goes). A body (body-<b>x<t>.onnx) gives the last hidden state,
// and its head ln p of each target from it.
struct Graph {
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputs, outputs;
    std::vector<std::vector<std::int64_t>> stateShapes;
    std::int64_t batch{-1}, tokens{-1}, window{0};
    Graph* targetHead{nullptr};

    void Open(const std::filesystem::path& file, const Ort::SessionOptions& options) {
        session = std::make_unique<Ort::Session>(Environment(), file.c_str(), options);
        Ort::AllocatorWithDefaultOptions allocator;
        for (std::size_t i = 0; i < session->GetInputCount(); ++i) inputs.push_back(session->GetInputNameAllocated(i, allocator).get());
        for (std::size_t i = 0; i < session->GetOutputCount(); ++i) outputs.push_back(session->GetOutputNameAllocated(i, allocator).get());
        const auto ids = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        batch = ids[0];
        tokens = ids[1];
        for (std::size_t i = 0; i < session->GetInputCount(); ++i) {
            if (!inputs[i].starts_with("past_")) continue;
            stateShapes.push_back(session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
            if (stateShapes.back().size() == 4) window = std::max<std::int64_t>(window, stateShapes.back()[2]);
        }
    }

    // A state of nothing read yet, for `rows` rows.
    [[nodiscard]] std::vector<Tensor> Empty(std::size_t rows) const {
        std::vector<Tensor> state;
        for (auto shape : stateShapes) {
            shape[0] = static_cast<std::int64_t>(rows);
            std::size_t n = 1;
            for (auto& d : shape) {
                if (d < 0) d = 0;  // no past tokens yet
                n *= static_cast<std::size_t>(d);
            }
            state.push_back(Tensor{shape, std::vector<float>(n, 0.0f)});
        }
        return state;
    }
};

// What one run of a graph gives.
struct Step {
    std::vector<float> targetLogprobs;  // [batch, tokens]
    std::vector<float> lastLogprobs;    // [batch, vocab]
    std::size_t vocab{0};
    std::vector<Tensor> state;
};

// The first output of a head graph for `rows` hidden states (and their
// targets, when it takes them).
std::vector<float> RunHead(Graph& head, const float* hidden, std::size_t rows, std::size_t width,
                           const std::int64_t* targets, std::vector<std::int64_t>* shapeOut = nullptr) {
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::int64_t hiddenShape[] = {static_cast<std::int64_t>(rows), static_cast<std::int64_t>(width)};
    const std::int64_t targetShape[] = {static_cast<std::int64_t>(rows)};
    std::vector<Ort::Value> values;
    std::vector<const char*> in;
    for (const auto& n : head.inputs) {
        in.push_back(n.c_str());
        if (n == "targets") {
            values.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, const_cast<std::int64_t*>(targets), rows, targetShape, 1));
        } else {
            values.push_back(Ort::Value::CreateTensor<float>(memory, const_cast<float*>(hidden), rows * width, hiddenShape, 2));
        }
    }
    const char* out = head.outputs[0].c_str();
    auto results = head.session->Run(Ort::RunOptions{nullptr}, in.data(), values.data(), values.size(), &out, 1);
    const auto info = results[0].GetTensorTypeAndShapeInfo();
    const float* data = results[0].GetTensorData<float>();
    if (shapeOut) *shapeOut = info.GetShape();
    return {data, data + info.GetElementCount()};
}

// Reads `ids` ([batch, tokens]) at `positions` after `state`, with `mask`
// ([batch, past + tokens]: 1 where a token counts).
Step Run(Graph& graph, std::size_t batch, std::size_t tokens, const std::vector<std::int64_t>& ids,
         const std::vector<std::int64_t>& positions, const std::vector<std::int64_t>& targets,
         const std::vector<std::int64_t>& mask, std::vector<Tensor>& state) {
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::size_t all = mask.size() / batch;
    const std::int64_t shape[] = {static_cast<std::int64_t>(batch), static_cast<std::int64_t>(tokens)};
    const std::int64_t maskShape[] = {static_cast<std::int64_t>(batch), static_cast<std::int64_t>(all)};
    auto cast = [](const std::vector<std::int64_t>& v) { return const_cast<std::int64_t*>(v.data()); };
    // By name: a body takes no targets.
    std::vector<Ort::Value> values;
    std::size_t next = 0;
    for (const auto& n : graph.inputs) {
        if (n == "input_ids") {
            values.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, cast(ids), ids.size(), shape, 2));
        } else if (n == "position_ids") {
            values.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, cast(positions), positions.size(), shape, 2));
        } else if (n == "attention_mask") {
            values.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, cast(mask), mask.size(), maskShape, 2));
        } else if (n == "targets") {
            values.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, cast(targets), targets.size(), shape, 2));
        } else {
            auto& t = state.at(next++);
            values.push_back(Ort::Value::CreateTensor<float>(memory, t.data.data(), t.data.size(), t.shape.data(), t.shape.size()));
        }
    }
    std::vector<const char*> in, out;
    for (const auto& n : graph.inputs) in.push_back(n.c_str());
    for (const auto& n : graph.outputs) out.push_back(n.c_str());
    auto results = graph.session->Run(Ort::RunOptions{nullptr}, in.data(), values.data(), values.size(), out.data(), out.size());
    Step step;
    const auto copy = [](Ort::Value& v, std::vector<float>& to, std::vector<std::int64_t>* shapeOut) {
        const auto info = v.GetTensorTypeAndShapeInfo();
        const float* data = v.GetTensorData<float>();
        to.assign(data, data + info.GetElementCount());
        if (shapeOut) *shapeOut = info.GetShape();
    };
    std::size_t first = 2;  // the first output of the state
    if (graph.outputs[0] == "hidden") {
        first = 1;
        std::vector<float> hidden;
        std::vector<std::int64_t> hiddenShape;
        copy(results[0], hidden, &hiddenShape);
        const auto width = static_cast<std::size_t>(hiddenShape.back());
        if (graph.targetHead) {
            step.targetLogprobs = RunHead(*graph.targetHead, hidden.data(), batch * tokens, width, targets.data());
        }
    } else {
        copy(results[0], step.targetLogprobs, nullptr);
        std::vector<std::int64_t> lastShape;
        copy(results[1], step.lastLogprobs, &lastShape);
        step.vocab = static_cast<std::size_t>(lastShape.back());
    }
    for (std::size_t i = first; i < results.size(); ++i) {
        Tensor t;
        copy(results[i], t.data, &t.shape);
        step.state.push_back(std::move(t));
    }
    return step;
}

// What a prompt left to go on from: its state (as the processor's graph
// keeps it), its tokens, and ln p of the next token.
struct Prompt {
    std::vector<Tensor> state;
    std::size_t tokens{0};
    std::vector<float> last;
};

// One model loaded, the one asked for last, where it was asked to run: the
// graph of any shapes on the processor, and with a GPU or an NPU the
// fixed-shape graphs there (an NPU compiles them the first time, which may
// take minutes: until then the processor answers).
class Model {
public:
    ~Model() { Unload(); }

    bool Load(AiModel which, AiDevice device, std::uint32_t gpu = 0) {
        if (which == loaded_ && device == device_ && gpu == gpu_ && cpu_.session) return true;
        Unload();
        const auto directory = tekito::ExternalLexiconProvider::PackDirectory(AiModelPack(which));
        std::error_code error;
        if (directory.empty() || !std::filesystem::is_regular_file(directory / L"model.onnx", error) ||
            !tokenizer_.Load(directory)) {
            return false;
        }
        try {
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads(Threads());
            options.SetInterOpNumThreads(1);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            cpu_.Open(directory / L"model.onnx", options);
        } catch (const Ort::Exception&) {
            Unload();
            return false;
        }
        loaded_ = which;
        device_ = device;
        gpu_ = gpu;
        writes_ = ReadAiModelKind(which).writes;
        if (device == AiDevice::Gpu || device == AiDevice::Npu) {
            loading_ = std::thread([this, directory, device, gpu] { LoadFixed(directory, device, gpu); });
        }
        return true;
    }

    std::optional<AiConversion> Convert(const aihelper::Asked& asked) {
        try {
            if (!writes_) {
                // A model that reads sentences alone: after the context; the
                // first token not counted without one, as the margins were chosen.
                auto scores = ScoreAfter(Utf8(asked.context), asked.sentences, !asked.context.empty(), false);
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
            auto scores = ScoreAfter(Utf8(prompt), asked.sentences, true, true);
            if (!scores) return std::nullopt;
            AiConversion answer{std::move(*scores), {}};
            if (asked.request != aihelper::Request::Score) answer.written = Write(asked.reading.size());
            return answer;
        } catch (const Ort::Exception&) {
            return std::nullopt;
        }
    }

    // For checking a pack: waits until the GPU or NPU graphs are loaded (or
    // could not be).
    bool WaitFixed() {
        if (loading_.joinable()) loading_.join();
        return fixedReady_;
    }

    void Unload() {
        stop_ = true;
        if (loading_.joinable()) loading_.join();
        stop_ = false;
        fixedReady_ = false;
        score_ = Graph{};
        scoreHead_ = Graph{};
        cpu_ = Graph{};
        loaded_ = AiModel::None;
    }

private:
    // The scoring body and its head on the GPU (DirectML) or the NPU (the
    // prompt is read on the processor, a few dozen tokens).
    void LoadFixed(std::filesystem::path directory, AiDevice device, std::uint32_t gpu) {
        try {
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            if (device == AiDevice::Gpu) {
                const auto adapter = PickGpu(gpu);
                if (!adapter) return;
                options.DisableMemPattern();
                options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                if (!AppendDirectMl(options, *adapter)) return;
            } else {
                const auto npus = NpuDevices();
                if (npus.empty()) return;
                // What OpenVINO compiles for the NPU is kept in the pack
                // (npu-cache), so it goes with the model, not in the
                // driver's own folder.
                std::unordered_map<std::string, std::string> epOptions;
                if (std::string_view(npus.front().EpName()) == "OpenVINOExecutionProvider") {
                    const auto cache = NpuCache(directory);
                    std::string path = Utf8(cache.wstring());
                    std::replace(path.begin(), path.end(), '\\', '/');
                    std::string escaped;
                    for (const char c : path) {
                        if (c == '"') escaped += '\\';
                        escaped += c;
                    }
                    epOptions["load_config"] = R"({"NPU":{"CACHE_DIR":")" + escaped + R"("}})";
                }
                options.AppendExecutionProvider_V2(Environment(), npus, epOptions);
            }
            Graph score, scoreHead;
            for (auto [graph, name] : {std::pair{&score, L"body-10x32.onnx"}, std::pair{&scoreHead, L"head-320.onnx"}}) {
                if (stop_) return;
                graph->Open(directory / name, options);
            }
            std::lock_guard lock(fixedLock_);
            score_ = std::move(score);
            scoreHead_ = std::move(scoreHead);
            score_.targetHead = &scoreHead_;
            fixedReady_ = true;
        } catch (const Ort::Exception&) {
        }
    }

    // Whether the fixed-shape graphs answer this time.
    bool Fixed() const { return fixedReady_; }

    // Where OpenVINO keeps what it compiles for the NPU: npu-cache/<layout>
    // in the pack, so it goes with the model; what an earlier layout of the
    // graphs left there is removed (read-only, as the compiler writes it).
    static std::filesystem::path NpuCache(const std::filesystem::path& pack) {
        constexpr wchar_t kLayout[] = L"score-10x32";
        const auto root = pack / L"npu-cache";
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (entry.path().filename() == kLayout) continue;
            for (auto it = std::filesystem::recursive_directory_iterator(entry.path(), error);
                 !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
                SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
            }
            error.clear();
            SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            std::filesystem::remove_all(entry.path(), error);
        }
        error.clear();
        std::filesystem::create_directories(root / kLayout, error);
        return root / kLayout;
    }

    // Reads the prompt (BOS and `start`) on the processor.
    std::optional<Prompt> ReadPrompt(const std::string& start) {
        const auto tokens = tokenizer_.Encode(start, true);
        if (tokens.empty() || tokens.size() > kBatch) return std::nullopt;
        Prompt p;
        p.tokens = tokens.size();
        std::vector<std::int64_t> ids(tokens.begin(), tokens.end()), zeros(ids.size(), 0), ones(ids.size(), 1), at(ids.size());
        for (std::size_t i = 0; i < at.size(); ++i) at[i] = static_cast<std::int64_t>(i);
        auto empty = cpu_.Empty(1);
        auto read = Run(cpu_, 1, ids.size(), ids, at, zeros, ones, empty);
        p.state = std::move(read.state);
        p.last = std::move(read.lastLogprobs);
        return p;
    }

    // ln p of each sentence (and the end, `withEnd`) after `start` (BOS and
    // it). The start is read once and each sentence goes on from it, many
    // at a time; `countFirst`: the first token of each counts too.
    std::optional<std::vector<double>> ScoreAfter(const std::string& start, const std::vector<std::wstring>& sentences,
                                                  bool countFirst, bool withEnd) {
        std::vector<std::vector<std::int32_t>> rows;
        std::size_t longest = 0;
        for (const auto& sentence : sentences) {
            auto tokens = tokenizer_.Encode(Utf8(sentence), false);
            if (withEnd) tokens.push_back(tokenizer_.End());
            if (tokens.size() > kBatch) return std::nullopt;
            longest = std::max(longest, tokens.size());
            rows.push_back(std::move(tokens));
        }
        // On the fixed graphs when it fits them, else on the processor.
        auto prompt = ReadPrompt(start);
        if (!prompt) return std::nullopt;
        promptRead_ = *prompt;
        // The fixed graphs read on from the prompt's state as their window:
        // its places at the end, the rest not counted.
        bool fixed = Fixed() && longest <= static_cast<std::size_t>(score_.tokens) &&
                     prompt->tokens <= static_cast<std::size_t>(score_.window);
        std::vector<Tensor> windowState;
        std::vector<std::int64_t> windowMask;
        if (fixed) {
            const auto places = static_cast<std::size_t>(score_.window);
            for (const auto& t : prompt->state) windowState.push_back(t.PadTo(places));
            windowMask.assign(places - prompt->tokens, 0);
            windowMask.resize(places, 1);
        }
        std::vector<double> scores(rows.size(), 0.0);
        if (longest == 0) return scores;
        Graph& graph = fixed ? score_ : cpu_;
        const std::size_t past = prompt->tokens;
        if (!fixed && past + longest > kContext) return std::nullopt;
        const std::size_t width = fixed ? static_cast<std::size_t>(graph.tokens) : longest;
        const std::size_t group = fixed ? static_cast<std::size_t>(graph.batch) : rows.size();
        const std::size_t window = fixed ? windowMask.size() : past;
        for (std::size_t from = 0; from < rows.size(); from += group) {
            const std::size_t n = fixed ? group : rows.size();
            std::vector<std::int64_t> ids(n * width, 0), targets(n * width, 0), at(n * width), mask(n * (window + width), 0);
            for (std::size_t r = 0; r < n; ++r) {
                for (std::size_t k = 0; k < window; ++k) mask[r * (window + width) + k] = fixed ? windowMask[k] : 1;
                for (std::size_t t = 0; t < width; ++t) at[r * width + t] = static_cast<std::int64_t>(past + t);
                if (from + r >= rows.size()) continue;
                const auto& row = rows[from + r];
                for (std::size_t t = 0; t < row.size(); ++t) {
                    ids[r * width + t] = row[t];
                    if (t + 1 < row.size()) targets[r * width + t] = row[t + 1];
                    mask[r * (window + width) + window + t] = 1;
                }
            }
            std::vector<std::size_t> each(n, 0);
            std::vector<Tensor> state;
            for (const auto& t : fixed ? windowState : prompt->state) state.push_back(t.Rows(each));
            const auto read = Run(graph, n, width, ids, at, targets, mask, state);
            for (std::size_t r = 0; r < n && from + r < rows.size(); ++r) {
                const auto& row = rows[from + r];
                if (row.empty()) continue;
                double sum = countFirst ? prompt->last[static_cast<std::size_t>(row[0])] : 0.0;
                for (std::size_t t = 0; t + 1 < row.size(); ++t) sum += read.targetLogprobs[r * width + t];
                scores[from + r] = sum;
            }
            if (!fixed) break;
        }
        return scores;
    }

    // The likeliest writings after the prompt read last (ScoreAfter), by
    // beam search on the processor: the beams read their next tokens
    // together, each going on from its parent's state (one token at a time
    // a GPU or an NPU runs no faster).
    std::vector<std::pair<std::wstring, double>> Write(std::size_t readingLength) {
        constexpr std::size_t kBeams = aihelper::kWritten;
        struct Beam {
            std::vector<std::int32_t> tokens;
            double score{0};
            bool done{false};
            std::size_t row{0};  // its row in the state
        };
        const auto best = [&](const float* logprobs, std::size_t vocab) {
            std::vector<std::pair<float, std::int32_t>> all(vocab);
            for (std::size_t v = 0; v < vocab; ++v) all[v] = {logprobs[v], static_cast<std::int32_t>(v)};
            std::partial_sort(all.begin(), all.begin() + kBeams, all.end(),
                              [](const auto& a, const auto& b) { return a.first > b.first; });
            all.resize(kBeams);
            return all;
        };
        Graph& graph = cpu_;
        const Prompt& prompt = promptRead_;
        const std::size_t vocab = prompt.last.size();
        std::vector<Beam> beams(1);
        std::vector<std::vector<std::pair<float, std::int32_t>>> next{best(prompt.last.data(), vocab)};
        std::vector<Tensor> state;
        for (const auto& t : prompt.state) state.push_back(t);
        std::size_t past = prompt.tokens;
        // Tokens: plenty for any writing of the reading.
        const std::size_t limit = std::min(readingLength * 2 + 8, kContext - prompt.tokens);
        for (std::size_t step = 0; step < limit; ++step) {
            struct Option {
                double score;
                std::size_t from;
                std::int32_t token;  // -1: the beam as it is (done)
            };
            std::vector<Option> options;
            for (std::size_t b = 0; b < beams.size(); ++b) {
                if (beams[b].done) {
                    options.push_back({beams[b].score, b, -1});
                    continue;
                }
                for (const auto& [lp, token] : next[beams[b].row]) options.push_back({beams[b].score + lp, b, token});
            }
            std::stable_sort(options.begin(), options.end(), [](const Option& a, const Option& b) { return a.score > b.score; });
            if (options.size() > kBeams) options.resize(kBeams);
            std::vector<Beam> children;
            std::vector<std::size_t> parents;
            std::vector<std::int64_t> ids;
            for (const auto& option : options) {
                const Beam& parent = beams[option.from];
                Beam child{parent.tokens, option.score, parent.done, 0};
                if (option.token >= 0) {
                    child.tokens.push_back(option.token);
                    child.done = option.token == tokenizer_.End();
                    if (!child.done) {
                        child.row = parents.size();
                        parents.push_back(parent.row);
                        ids.push_back(option.token);
                    }
                }
                children.push_back(std::move(child));
            }
            beams = std::move(children);
            if (parents.empty()) break;
            const std::size_t batch = parents.size();
            std::vector<Tensor> picked;
            for (const auto& t : state) picked.push_back(t.Rows(parents));
            std::vector<std::int64_t> zeros(batch, 0), at(batch, static_cast<std::int64_t>(past)), mask(batch * (past + 1), 1);
            auto read = Run(graph, batch, 1, ids, at, zeros, mask, picked);
            ++past;
            state = std::move(read.state);
            next.assign(batch, {});
            for (std::size_t r = 0; r < batch; ++r) next[r] = best(read.lastLogprobs.data() + r * read.vocab, read.vocab);
        }
        std::vector<std::pair<std::wstring, double>> written;
        for (const auto& beam : beams) {
            if (!beam.done) continue;
            auto wide = Wide(tokenizer_.Decode(beam.tokens));
            const bool seen = std::any_of(written.begin(), written.end(), [&](const auto& w) { return w.first == wide; });
            if (!wide.empty() && !seen) written.emplace_back(std::move(wide), beam.score);
        }
        return written;
    }

    Graph cpu_, score_, scoreHead_;
    std::thread loading_;
    std::mutex fixedLock_;
    std::atomic<bool> fixedReady_{false}, stop_{false};
    tekito::ai::ByteBpe tokenizer_;
    AiModel loaded_{AiModel::None};
    AiDevice device_{AiDevice::Auto};
    std::uint32_t gpu_{0};
    bool writes_{false};
    Prompt promptRead_;
};

std::vector<std::uint8_t> Answer(Model& model, std::span<const std::uint8_t> message, bool& stop) {
    const auto asked = aihelper::DecodeRequest(message);
    if (!asked) return aihelper::EncodeReply(aihelper::Status::Failed);
    if (asked->request == aihelper::Request::Stop) {
        model.Unload();
        stop = true;
        return aihelper::EncodeReply(aihelper::Status::Ok);
    }
    if (!model.Load(asked->model, asked->device, asked->gpu)) return aihelper::EncodeReply(aihelper::Status::NoModel);
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
int ConvertFile(AiModel which, AiDevice device, std::uint32_t gpu, const std::filesystem::path& file) {
    Model model;
    if (!model.Load(which, device, gpu)) {
        std::fprintf(stderr, "the model is not installed\n");
        return 2;
    }
    if (device == AiDevice::Gpu || device == AiDevice::Npu) {
        const auto start = std::chrono::steady_clock::now();
        const bool ready = model.WaitFixed();
        std::printf("%s graphs %s in %.1f s\n", device == AiDevice::Gpu ? "GPU" : "NPU", ready ? "loaded" : "not loaded",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
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

// The execution providers Windows ML offers on this PC (its catalog), and
// the devices ONNX Runtime then sees. `prepare`: a provider not yet on the
// PC downloaded first (Windows ML fetches it from Microsoft).
int ListDevices(std::string_view prepare) {
    struct Context {
        std::string_view prepare;  // the provider to download, if any
        std::vector<std::pair<std::string, std::string>> registered;
    } context{prepare, {}};
    WinMLEpCatalogHandle catalog = nullptr;
    if (!HasWindowsMl()) {
        std::printf("no Windows ML with this copy (the processor alone)\n");
    } else if (FAILED(WinMLEpCatalogCreate(&catalog))) {
        std::printf("no Windows ML catalog (Windows 11 24H2 or later is needed)\n");
    } else {
        WinMLEpCatalogEnumProviders(
            catalog,
            [](WinMLEpHandle ep, const WinMLEpInfo* info, void* data) -> BOOL {
                auto* c = static_cast<Context*>(data);
                if (!info || !info->name) return TRUE;
                const char* states[] = {"ready", "not ready", "not present"};
                std::printf("provider %s %s: %s%s\n", info->name, info->version ? info->version : "",
                            states[std::clamp(static_cast<int>(info->readyState), 0, 2)],
                            info->certification == WinMLEpCertification_Certified ? ", certified" : "");
                // "npu": the providers that run NPUs (Intel's, Qualcomm's, AMD's).
                const std::string_view name = info->name;
                const bool npu = name == "OpenVINOExecutionProvider" || name == "QNNExecutionProvider" ||
                                 name == "VitisAIExecutionProvider";
                const bool wanted = c->prepare == name || (c->prepare == "npu" && npu);
                if (info->readyState != WinMLEpReadyState_Ready && !(wanted && SUCCEEDED(WinMLEpEnsureReady(ep)))) {
                    return TRUE;
                }
                std::size_t size = 0;
                if (FAILED(WinMLEpGetLibraryPathSize(ep, &size)) || size == 0) return TRUE;
                std::string path(size, '\0');
                std::size_t used = 0;
                if (FAILED(WinMLEpGetLibraryPath(ep, size, path.data(), &used)) || used == 0) return TRUE;
                path.resize(used - 1);
                c->registered.emplace_back(info->name, path);
                return TRUE;
            },
            &context);
        WinMLEpCatalogRelease(catalog);
    }
    for (const auto& [name, path] : context.registered) {
        try {
            Environment().RegisterExecutionProviderLibrary(name.c_str(), std::filesystem::path(path).wstring());
            std::printf("registered %s\n", name.c_str());
        } catch (const Ort::Exception& e) {
            std::printf("could not register %s: %s\n", name.c_str(), e.what());
        }
    }
    bool npu = false;
    for (const auto& device : Environment().GetEpDevices()) {
        const auto type = device.Device().Type();
        npu = npu || type == OrtHardwareDeviceType_NPU;
        std::printf("device %s on %s (%s)\n", device.EpName(), type == OrtHardwareDeviceType_NPU ? "NPU"
                                                             : type == OrtHardwareDeviceType_GPU ? "GPU" : "CPU",
                    device.Device().Vendor());
    }
    // Preparing the NPU: whether one is ready now.
    return prepare == "npu" && !npu ? 1 : 0;
}

// A console program, started without a console (CREATE_NO_WINDOW), so
// --convert prints where it was run from.
int wmain(int argc, wchar_t** argv) {
    int result = 0;
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--devices") {
        result = ListDevices(argc >= 4 && std::wstring_view(argv[2]) == L"--prepare" ? Utf8(argv[3]) : std::string{});
    } else if (argc == 5 && std::wstring_view(argv[1]) == L"--convert") {
        const AiModel which = std::wstring_view(argv[2]) == L"strong" ? AiModel::Strong : AiModel::Light;
        // cpu, gpu, gpu=<GpuId in hex>, npu, or auto.
        const std::wstring_view where = argv[3];
        const AiDevice device = where == L"cpu"            ? AiDevice::Cpu
                               : where.starts_with(L"gpu") ? AiDevice::Gpu
                               : where == L"npu"           ? AiDevice::Npu
                                                           : AiDevice::Auto;
        const std::uint32_t gpu = where.starts_with(L"gpu=") ? std::wcstoul(argv[3] + 4, nullptr, 16) : 0;
        result = ConvertFile(which, device, gpu, argv[4]);
    } else {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        RegisterReadyProviders();
        result = Serve();
    }
    return result;
}
