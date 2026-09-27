// Phase 0 prototype for Japanese conversion: maps the japanese-core pack
// (ja-dict-v1 / ja-matrix-v1, see scripts/build-japanese-packs.py), runs a
// plain Viterbi over the dictionary and reports size, speed and a first
// accuracy figure on Mozc's evaluation.tsv. It has no segmenter, rewriters or
// learning, so the accuracy is a floor, not what TEKITO will ship.
//
// tekito_ja_benchmark --pack <dir> [--eval <evaluation.tsv>]

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::size_t g_lookupSink = 0;

double Microseconds(Clock::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
}

std::size_t WorkingSetKiB() {
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return counters.WorkingSetSize / 1024;
}

class MappedFile {
public:
    explicit MappedFile(const std::wstring& path) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size)) return;
        size_ = static_cast<std::size_t>(size.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) return;
        data_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    }
    ~MappedFile() {
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const std::uint8_t* Data() const noexcept { return data_; }
    std::size_t Size() const noexcept { return size_; }

private:
    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE mapping_{nullptr};
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
};

template <typename T>
T Read(const std::uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

struct Token {
    std::uint16_t left;
    std::uint16_t right;
    std::uint16_t cost;
    std::uint32_t surface;
};

class Dictionary {
public:
    bool Open(const MappedFile& file) {
        const std::uint8_t* p = file.Data();
        if (!p || file.Size() < 60 || std::memcmp(p, "TKJD", 4) != 0 || Read<std::uint32_t>(p + 4) != 1) {
            return false;
        }
        const auto field = [&](int i) { return Read<std::uint32_t>(p + 8 + 4 * i); };
        keyCount_ = field(0);
        tokenCount_ = field(1);
        charCount_ = field(2);
        const std::uint32_t surfaceUnits = field(3);
        if (field(11) != file.Size()) return false;
        const auto inRange = [&](std::uint32_t offset, std::uint64_t bytes) {
            return offset <= file.Size() && bytes <= file.Size() - offset;
        };
        if (!inRange(field(4), 65536) || !inRange(field(5), 2ull * charCount_) ||
            !inRange(field(6), 4ull * (keyCount_ + 1)) || !inRange(field(8), 4ull * (keyCount_ + 1)) ||
            !inRange(field(9), 10ull * tokenCount_) || !inRange(field(10), 2ull * surfaceUnits)) {
            return false;
        }
        reverse_ = p + field(4);
        chars_ = p + field(5);
        keyOffsets_ = p + field(6);
        keyBlob_ = p + field(7);
        tokenStarts_ = p + field(8);
        tokens_ = p + field(9);
        surfaces_ = p + field(10);
        if (!inRange(field(7), KeyOffset(keyCount_)) || TokenStart(keyCount_) != tokenCount_) return false;
        return true;
    }

    std::uint8_t Code(wchar_t c) const noexcept { return reverse_[static_cast<std::uint16_t>(c)]; }

    // Calls found(length, firstToken, endToken) for every key that is a
    // prefix of codes. The range of candidate keys narrows one character at a
    // time, so a lookup costs two binary searches per character.
    template <typename Found>
    void CommonPrefixSearch(std::basic_string_view<std::uint8_t> codes, Found&& found) const {
        std::uint32_t lo = 0, hi = keyCount_;
        for (std::size_t depth = 0; depth < codes.size() && lo < hi; ++depth) {
            const std::uint8_t c = codes[depth];
            if (c == 0) break;
            const auto charAt = [&](std::uint32_t key) -> int {
                const std::uint32_t begin = KeyOffset(key), end = KeyOffset(key + 1);
                return depth < end - begin ? keyBlob_[begin + depth] : -1;
            };
            // Keys in [lo, hi) share the first `depth` codes; find those whose
            // next code is c.
            std::uint32_t a = lo, b = hi;
            while (a < b) {
                const std::uint32_t m = a + (b - a) / 2;
                if (charAt(m) < c) a = m + 1; else b = m;
            }
            std::uint32_t e = a, f = hi;
            while (e < f) {
                const std::uint32_t m = e + (f - e) / 2;
                if (charAt(m) <= c) e = m + 1; else f = m;
            }
            lo = a;
            hi = e;
            if (lo < hi && KeyOffset(lo + 1) - KeyOffset(lo) == depth + 1) {
                found(depth + 1, TokenStart(lo), TokenStart(lo + 1));
            }
        }
    }

    Token TokenAt(std::uint32_t i) const {
        const std::uint8_t* p = tokens_ + 10ull * i;
        return {Read<std::uint16_t>(p), Read<std::uint16_t>(p + 2), Read<std::uint16_t>(p + 4),
                Read<std::uint32_t>(p + 6)};
    }

    std::wstring Surface(const Token& token, std::wstring_view reading) const {
        switch (token.surface >> 30) {
        case 1:
            return std::wstring(reading);
        case 2: {
            std::wstring out(reading);
            for (auto& c : out) {
                if (c >= L'ぁ' && c <= L'ゖ') c = static_cast<wchar_t>(c + 0x60);
            }
            return out;
        }
        default: {
            const std::uint32_t offset = token.surface & ((1u << 29) - 1);
            const std::uint16_t length = Read<std::uint16_t>(surfaces_ + 2ull * offset);
            std::wstring out(length, L'\0');
            std::memcpy(out.data(), surfaces_ + 2ull * offset + 2, 2ull * length);
            return out;
        }
        }
    }

private:
    std::uint32_t KeyOffset(std::uint32_t i) const { return Read<std::uint32_t>(keyOffsets_ + 4ull * i); }
    std::uint32_t TokenStart(std::uint32_t i) const { return Read<std::uint32_t>(tokenStarts_ + 4ull * i); }

    std::uint32_t keyCount_{0}, tokenCount_{0}, charCount_{0};
    const std::uint8_t *reverse_{}, *chars_{}, *keyOffsets_{}, *keyBlob_{}, *tokenStarts_{}, *tokens_{},
        *surfaces_{};
};

class Matrix {
public:
    bool Open(const MappedFile& file) {
        const std::uint8_t* p = file.Data();
        if (!p || file.Size() < 20 || std::memcmp(p, "TKJM", 4) != 0) return false;
        size_ = Read<std::uint32_t>(p + 8);
        format_ = Read<std::uint32_t>(p + 12);
        scale_ = Read<std::uint32_t>(p + 16);
        const std::uint64_t cells = 1ull * size_ * size_;
        if (file.Size() != 20 + cells * (format_ == 0 ? 2 : 1)) return false;
        costs_ = p + 20;
        return true;
    }
    int Cost(std::uint16_t previousRight, std::uint16_t nextLeft) const {
        const std::size_t i = 1ull * previousRight * size_ + nextLeft;
        return format_ == 0 ? Read<std::int16_t>(costs_ + 2 * i) : costs_[i] * static_cast<int>(scale_);
    }
    std::uint32_t Size() const noexcept { return size_; }

private:
    std::uint32_t size_{0}, format_{0}, scale_{1};
    const std::uint8_t* costs_{nullptr};
};

// A reading character with no dictionary word starting there still needs a
// path through the lattice: it becomes a one-character common noun with a
// high cost (名詞,一般 in Mozc's id.def).
constexpr std::uint16_t kUnknownId = 1851;
constexpr int kUnknownCost = 12000;

struct Node {
    std::uint32_t begin, end;
    std::uint16_t left, right;
    int wordCost;
    std::uint32_t token;  // UINT32_MAX for an unknown character
    long long best{std::numeric_limits<long long>::max()};
    int previous{-1};
};

struct ConversionStats {
    std::size_t nodes{0};
};

std::wstring Convert(const Dictionary& dictionary, const Matrix& matrix, std::wstring_view reading,
                     ConversionStats* stats = nullptr) {
    const std::size_t n = reading.size();
    std::basic_string<std::uint8_t> codes(n, 0);
    for (std::size_t i = 0; i < n; ++i) codes[i] = dictionary.Code(reading[i]);

    std::vector<Node> nodes;
    std::vector<std::vector<int>> endingAt(n + 1);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (i > 0 && endingAt[i].empty()) continue;
        dictionary.CommonPrefixSearch(std::basic_string_view<std::uint8_t>(codes).substr(i),
                                      [&](std::size_t length, std::uint32_t first, std::uint32_t end) {
            for (std::uint32_t t = first; t < end; ++t) {
                const Token token = dictionary.TokenAt(t);
                endingAt[i + length].push_back(static_cast<int>(nodes.size()));
                nodes.push_back({i, static_cast<std::uint32_t>(i + length), token.left, token.right,
                                 token.cost, t});
            }
        });
        endingAt[i + 1].push_back(static_cast<int>(nodes.size()));
        nodes.push_back({i, i + 1, kUnknownId, kUnknownId, kUnknownCost, UINT32_MAX});
    }
    if (stats) stats->nodes = nodes.size();

    // Nodes were added in order of their begin position, so every node
    // ending at a node's begin is final by the time we reach it.
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        Node& node = nodes[k];
        if (node.begin == 0) {
            node.best = node.wordCost + matrix.Cost(0, node.left);
            continue;
        }
        for (int p : endingAt[node.begin]) {
            if (nodes[p].best == std::numeric_limits<long long>::max()) continue;
            const long long cost = nodes[p].best + matrix.Cost(nodes[p].right, node.left) + node.wordCost;
            if (cost < node.best) {
                node.best = cost;
                node.previous = p;
            }
        }
    }
    int last = -1;
    long long bestTotal = std::numeric_limits<long long>::max();
    for (int p : endingAt[n]) {
        if (nodes[p].best == std::numeric_limits<long long>::max()) continue;
        const long long total = nodes[p].best + matrix.Cost(nodes[p].right, 0);
        if (total < bestTotal) {
            bestTotal = total;
            last = p;
        }
    }
    std::vector<int> path;
    for (int p = last; p >= 0; p = nodes[p].previous) path.push_back(p);
    std::wstring out;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const Node& node = nodes[*it];
        const std::wstring_view part = reading.substr(node.begin, node.end - node.begin);
        out += node.token == UINT32_MAX ? std::wstring(part)
                                        : dictionary.Surface(dictionary.TokenAt(node.token), part);
    }
    return out;
}

std::wstring Widen(std::string_view utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::string Narrow(std::wstring_view text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr,
                        nullptr);
    return out;
}

struct Percentiles {
    double p50, p95, p99, max;
};

Percentiles Summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) {
        return samples[std::min(samples.size() - 1, static_cast<std::size_t>(q * samples.size()))];
    };
    return {at(0.50), at(0.95), at(0.99), samples.back()};
}

void Print(const char* name, const Percentiles& p) {
    std::cout << name << "_us p50=" << p.p50 << " p95=" << p.p95 << " p99=" << p.p99 << " max=" << p.max << "\n";
}

struct EvalRow {
    std::wstring input, argument;
    std::string command;
};

std::vector<EvalRow> LoadEval(const std::wstring& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<EvalRow> rows;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        std::stringstream parts(line);
        for (std::string field; std::getline(parts, field, '\t');) fields.push_back(field);
        if (fields.size() < 5) continue;
        rows.push_back({Widen(fields[1]), Widen(fields[4]), fields[3]});
    }
    return rows;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring pack, eval;
    bool showMisses = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--pack" && i + 1 < argc) pack = argv[++i];
        else if (arg == L"--eval" && i + 1 < argc) eval = argv[++i];
        else if (arg == L"--show-misses") showMisses = true;
    }
    if (pack.empty()) {
        std::cerr << "usage: tekito_ja_benchmark --pack <dir> [--eval <evaluation.tsv>] [--show-misses]\n";
        return 2;
    }
    SetConsoleOutputCP(CP_UTF8);

    const std::size_t baseKiB = WorkingSetKiB();
    const auto openStart = Clock::now();
    MappedFile dictionaryFile(pack + L"\\dictionary.bin");
    MappedFile matrixFile(pack + L"\\connection.bin");
    Dictionary dictionary;
    Matrix matrix;
    if (!dictionary.Open(dictionaryFile) || !matrix.Open(matrixFile)) {
        std::cerr << "could not open the japanese-core pack in the given directory\n";
        return 1;
    }
    std::cout << "open_us " << Microseconds(Clock::now() - openStart) << "\n";
    std::cout << "file_bytes dictionary=" << dictionaryFile.Size() << " connection=" << matrixFile.Size() << "\n";

    if (eval.empty()) return 0;
    const auto rows = LoadEval(eval);

    // Accuracy on the rows whose command is about the first conversion.
    std::size_t exactTotal = 0, exactHit = 0, matchTotal = 0, matchHit = 0;
    for (const auto& row : rows) {
        const std::wstring output = Convert(dictionary, matrix, row.input);
        if (row.command == "Conversion Expected") {
            ++exactTotal;
            if (output == row.argument) ++exactHit;
            else if (showMisses) std::cout << "miss " << Narrow(row.input) << " -> " << Narrow(output) << " (want " << Narrow(row.argument) << ")\n";
        } else if (row.command == "Conversion Match" || row.command == "Conversion Not Match") {
            ++matchTotal;
            const bool contains = output.find(row.argument) != std::wstring::npos;
            if (contains == (row.command == "Conversion Match")) ++matchHit;
        }
    }
    std::cout << "top1_exact " << exactHit << "/" << exactTotal << "\n";
    std::cout << "match_checks " << matchHit << "/" << matchTotal << "\n";

    // Latency: every conversion input, then every prefix of it (what a
    // prediction lookup or a conversion after each key would cost), then long
    // inputs made by joining rows.
    std::vector<double> full, perKeyConvert, perKeyLookup;
    std::size_t maxNodes = 0;
    for (int round = 0; round < 3; ++round) {
        for (const auto& row : rows) {
            ConversionStats stats;
            auto start = Clock::now();
            volatile std::size_t sink = Convert(dictionary, matrix, row.input, &stats).size();
            (void)sink;
            full.push_back(Microseconds(Clock::now() - start));
            maxNodes = std::max(maxNodes, stats.nodes);
            if (round > 0) continue;
            for (std::size_t k = 1; k <= row.input.size(); ++k) {
                const std::wstring_view prefix = std::wstring_view(row.input).substr(0, k);
                start = Clock::now();
                volatile std::size_t sink2 = Convert(dictionary, matrix, prefix).size();
                (void)sink2;
                perKeyConvert.push_back(Microseconds(Clock::now() - start));

                std::basic_string<std::uint8_t> codes;
                for (wchar_t c : prefix) codes.push_back(dictionary.Code(c));
                std::size_t found = 0;
                start = Clock::now();
                for (std::size_t i = 0; i < codes.size(); ++i) {
                    dictionary.CommonPrefixSearch(std::basic_string_view<std::uint8_t>(codes).substr(i),
                                                  [&](std::size_t, std::uint32_t a, std::uint32_t b) { found += b - a; });
                }
                perKeyLookup.push_back(Microseconds(Clock::now() - start));
                g_lookupSink += found;
            }
        }
    }
    Print("convert_full", Summarize(full));
    Print("convert_every_prefix", Summarize(perKeyConvert));
    Print("lookup_every_prefix", Summarize(perKeyLookup));
    std::cout << "max_lattice_nodes " << maxNodes << "\n";

    for (std::size_t target : {40u, 100u}) {
        std::vector<double> samples;
        std::wstring joined;
        for (const auto& row : rows) {
            if (row.input.empty()) continue;
            joined += row.input;
            if (joined.size() < target) continue;
            joined.resize(target);
            const auto start = Clock::now();
            volatile std::size_t sink = Convert(dictionary, matrix, joined).size();
            (void)sink;
            samples.push_back(Microseconds(Clock::now() - start));
            joined.clear();
        }
        const std::string name = "convert_" + std::to_string(target) + "_chars";
        Print(name.c_str(), Summarize(samples));
    }
    std::cout << "working_set_kib base=" << baseKiB << " after=" << WorkingSetKiB() << "\n";
    return 0;
}
