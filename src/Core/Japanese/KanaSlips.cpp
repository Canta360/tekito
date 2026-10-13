#include "Core/Japanese/KanaSlips.h"

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/NeuralMath.h"
#include "Core/Japanese/RomajiTable.h"
#include "Core/Japanese/SortedTsv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace tekito::japanese {
namespace {

constexpr char kMagic[4] = {'T', 'K', 'K', 'S'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint8_t kStart = 1, kEnd = 2, kUnknownId = 3;
// Longer input is left alone: the edits grow with its length.
constexpr std::size_t kLongestKeys = 160;
// What an edit costs, in nats against the kana model's ln p.
constexpr double kNearCost = 3.0, kOtherCost = 6.0, kInsertCost = 4.0, kDropCost = 3.5, kSwapCost = 3.0;
// Single edits whose pairs are tried, and readings the LSTM scores.
constexpr std::size_t kPairedSingles = 40;
constexpr std::size_t kRescored = 12;
// The LSTM is asked only when some reading beats the typed one by this
// much by the n-gram model (most input typed right stops here).
constexpr double kNgramGate = 3.0;

float Half(std::uint16_t h) noexcept {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    std::uint32_t exponent = (h >> 10) & 0x1F;
    std::uint32_t mantissa = h & 0x3FF;
    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x400) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3FF;
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof out);
    return out;
}

const char* kQwertyRows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};

bool Neighbors(wchar_t a, wchar_t b) noexcept {
    int ra = -1, ca = 0, rb = -1, cb = 0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; kQwertyRows[r][c]; ++c) {
            if (kQwertyRows[r][c] == a) ra = r, ca = c;
            if (kQwertyRows[r][c] == b) rb = r, cb = c;
        }
    }
    return ra >= 0 && rb >= 0 && a != b && std::abs(ra - rb) <= 1 && std::abs(ca - cb) <= 1;
}

bool IsLetter(wchar_t c) noexcept { return c >= L'a' && c <= L'z'; }

struct Span {
    std::size_t begin, end;
    std::wstring with;
    double cost;
};

// Every single edit of `keys` (as JapaneseComposer would take them back).
std::vector<Span> Edits(std::wstring_view keys) {
    std::vector<Span> out;
    for (std::size_t i = 0; i <= keys.size(); ++i) {
        for (wchar_t c = L'a'; c <= L'z'; ++c) out.push_back({i, i, std::wstring(1, c), kInsertCost});
        if (i == keys.size()) break;
        if (!IsLetter(keys[i])) continue;
        out.push_back({i, i + 1, L"", kDropCost});
        for (wchar_t c = L'a'; c <= L'z'; ++c) {
            if (c != keys[i]) out.push_back({i, i + 1, std::wstring(1, c), Neighbors(keys[i], c) ? kNearCost : kOtherCost});
        }
        if (i + 1 < keys.size() && IsLetter(keys[i + 1]) && keys[i] != keys[i + 1]) {
            out.push_back({i, i + 2, std::wstring{keys[i + 1], keys[i]}, kSwapCost});
        }
    }
    return out;
}

std::wstring Applied(std::wstring_view keys, std::vector<const Span*> spans) {
    std::sort(spans.begin(), spans.end(), [](const Span* a, const Span* b) { return a->begin < b->begin; });
    std::wstring out;
    std::size_t at = 0;
    for (const Span* span : spans) {
        out.append(keys.substr(at, span->begin - at));
        out += span->with;
        at = span->end;
    }
    out.append(keys.substr(at));
    return out;
}

// Keys read as JapaneseComposer reads them (Feed, FlushOnce, FlushAll),
// kana appended as the models know them: full-width letters, digits and
// marks half-width (NFKC does that and nothing else to the table's kana).
class Reader {
public:
    explicit Reader(const RomajiTable& table) : table_(table) {}
    void Feed(wchar_t key) {
        for (int guard = 0; guard < 64; ++guard) {
            pending_ += key;
            if (table_.HasLongerInput(pending_)) return;
            pending_.pop_back();
            std::wstring keys = pending_ + key;
            if (const RomajiRule* rule = table_.Find(keys)) {
                Emit(rule->output);
                pending_ = rule->pending;
                return;
            }
            if (pending_.empty()) {
                Emit(std::wstring_view(&key, 1));
                return;
            }
            FlushOnce();
        }
        Emit(std::wstring_view(&key, 1));
    }
    std::wstring& Finish() {
        for (int guard = 0; guard < 64 && !pending_.empty(); ++guard) FlushOnce();
        Emit(pending_);
        pending_.clear();
        return out_;
    }

private:
    void Emit(std::wstring_view kana) {
        for (wchar_t c : kana) out_ += c >= 0xFF01 && c <= 0xFF5E ? static_cast<wchar_t>(c - 0xFEE0) : c;
    }
    void FlushOnce() {
        if (pending_.empty()) return;
        const std::wstring pending = std::move(pending_);
        pending_.clear();
        for (std::size_t length = pending.size(); length > 0; --length) {
            const RomajiRule* rule = table_.Find(std::wstring_view(pending.data(), length));
            if (!rule || !rule->pending.empty()) continue;
            Emit(rule->output);
            for (wchar_t rest : std::wstring_view(pending).substr(length)) Feed(rest);
            return;
        }
        Emit(std::wstring_view(pending.data(), 1));
        for (wchar_t rest : std::wstring_view(pending).substr(1)) Feed(rest);
    }

    const RomajiTable& table_;
    std::wstring out_;
    std::wstring pending_;
};

std::wstring KanaOf(const RomajiTable& table, std::wstring_view keys) {
    Reader reader(table);
    for (const wchar_t key : keys) reader.Feed(key);
    return std::move(reader.Finish());
}

}  // namespace

bool KanaSlips::Open(const std::filesystem::path& packDirectory) {
    chars_ = 0;
    if (!file_.Open(packDirectory / L"slips.bin")) return false;
    const std::uint8_t* data = file_.Data();
    const std::size_t size = file_.Size();
    std::size_t at = 0;
    const auto read = [&](void* out, std::size_t bytes) {
        if (at + bytes > size) return false;
        std::memcpy(out, data + at, bytes);
        at += bytes;
        return true;
    };
    const auto section = [&](std::size_t bytes) -> const std::uint8_t* {
        at = (at + 63) / 64 * 64;
        if (at + bytes > size) return nullptr;
        const std::uint8_t* start = data + at;
        at += bytes;
        return start;
    };
    char magic[4]{};
    std::uint32_t header[5]{};
    if (!read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !read(header, sizeof header) || header[0] != kVersion) {
        return false;
    }
    const std::uint32_t chars = header[1], order = header[2], embed = header[3], hidden = header[4];
    if (chars == 0 || chars > 128 || order == 0 || order > 4 || embed % 16 != 0 || hidden % 16 != 0) return false;
    orders_.assign(order, {});
    for (auto& o : orders_) {
        std::uint32_t count = 0;
        if (!read(&count, sizeof count)) return false;
        o.count = count;
        o.codes = reinterpret_cast<const std::uint32_t*>(section(count * sizeof(std::uint32_t)));
        o.logProb = reinterpret_cast<const std::uint16_t*>(section(count * sizeof(std::uint16_t)));
        o.logBackoff = reinterpret_cast<const std::uint16_t*>(section(count * sizeof(std::uint16_t)));
        if (!o.logBackoff) return false;
    }
    const std::size_t gates = 4 * std::size_t{hidden};
    embedding_ = reinterpret_cast<const float*>(section(std::size_t{chars} * embed * sizeof(float)));
    inputWeights_ = reinterpret_cast<const std::int8_t*>(section(gates * embed));
    inputScale_ = reinterpret_cast<const float*>(section(gates * sizeof(float)));
    hiddenWeights_ = reinterpret_cast<const std::int8_t*>(section(gates * hidden));
    hiddenScale_ = reinterpret_cast<const float*>(section(gates * sizeof(float)));
    bias_ = reinterpret_cast<const float*>(section(gates * sizeof(float)));
    output_ = reinterpret_cast<const std::int8_t*>(section(std::size_t{chars} * hidden));
    outputScale_ = reinterpret_cast<const float*>(section(chars * sizeof(float)));
    outputBias_ = reinterpret_cast<const float*>(section(chars * sizeof(float)));
    if (!outputBias_ || at != size) return false;

    std::ifstream list(packDirectory / L"chars.tsv", std::ios::binary);
    if (!list) return false;
    charId_.fill(kUnknownId);
    std::uint32_t id = 0;
    for (std::string line; std::getline(list, line); ++id) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::wstring c = FromUtf8(line);
        if (id >= 4 && c.size() == 1) charId_[static_cast<std::uint16_t>(c[0])] = static_cast<std::uint8_t>(id);
    }
    if (id != chars) return false;
    chars_ = chars;
    order_ = order;
    embed_ = embed;
    hidden_ = hidden;
    return true;
}

std::vector<std::uint8_t> KanaSlips::Ids(std::wstring_view kana) const {
    std::vector<std::uint8_t> ids{kStart};
    for (const wchar_t c : kana) ids.push_back(charId_[static_cast<std::uint16_t>(c)]);
    ids.push_back(kEnd);
    return ids;
}

std::size_t KanaSlips::Find(const Order& order, std::uint32_t code) const {
    const std::uint32_t* end = order.codes + order.count;
    const std::uint32_t* found = std::lower_bound(order.codes, end, code);
    return found != end && *found == code ? static_cast<std::size_t>(found - order.codes) : SIZE_MAX;
}

double KanaSlips::Contribution(const std::uint8_t* ids, std::size_t at) const {
    double backoff = 0;
    for (std::size_t n = std::min<std::size_t>(order_, at + 1); n >= 1; --n) {
        std::uint32_t code = 0;
        for (std::size_t j = at + 1 - n; j <= at; ++j) code = code * 128 + ids[j];
        if (const std::size_t found = Find(orders_[n - 1], code); found != SIZE_MAX) {
            return backoff + Half(orders_[n - 1].logProb[found]);
        }
        if (n == 1) break;
        // The history's weight for what it was not seen followed by.
        if (const std::size_t history = Find(orders_[n - 2], code / 128); history != SIZE_MAX) {
            backoff += Half(orders_[n - 2].logBackoff[history]);
        }
    }
    return backoff - 20.0;  // a character the model never saw
}

double KanaSlips::NgramLogProbability(std::wstring_view kana) const {
    if (!IsOpen()) return 0;
    const auto ids = Ids(kana);
    double total = 0;
    for (std::size_t at = 1; at < ids.size(); ++at) total += Contribution(ids.data(), at);
    return total;
}

std::vector<double> KanaSlips::NeuralLogProbabilities(std::span<const std::wstring> kana) const {
    std::vector<double> result(kana.size(), 0.0);
    if (!IsOpen() || kana.empty()) return result;
    // The readings as a tree of ids: what they share at the start is read once.
    struct Node {
        std::uint8_t id{0};
        std::vector<std::size_t> children;
        double logProbability{0};
    };
    std::vector<Node> nodes(1);
    std::vector<std::size_t> ends(kana.size());
    for (std::size_t s = 0; s < kana.size(); ++s) {
        const auto ids = Ids(kana[s]);
        std::size_t at = 0;
        for (std::size_t i = 1; i < ids.size(); ++i) {
            std::size_t next = 0;
            for (const std::size_t child : nodes[at].children) {
                if (nodes[child].id == ids[i]) next = child;
            }
            if (next == 0) {
                next = nodes.size();
                nodes[at].children.push_back(next);
                nodes.push_back({ids[i], {}, 0});
            }
            at = next;
        }
        ends[s] = at;
    }
    const std::size_t gates = 4 * std::size_t{hidden_};
    struct State {
        std::vector<float> h, c;
    };
    const auto step = [&](const State& state, std::uint8_t id) {
        const float* row = embedding_ + std::size_t{id} * embed_;
        std::vector<float> x(row, row + embed_);
        std::vector<std::int8_t> qx, qh;
        const float sx = Quantize(x, qx);
        const float sh = Quantize(state.h, qh);
        std::vector<std::int32_t> fromInput(gates), fromHidden(gates);
        DotRows(inputWeights_, gates, embed_, qx.data(), fromInput.data());
        DotRows(hiddenWeights_, gates, hidden_, qh.data(), fromHidden.data());
        State next{std::vector<float>(hidden_), std::vector<float>(hidden_)};
        for (std::uint32_t k = 0; k < hidden_; ++k) {
            const auto g = [&](std::size_t j) {
                return bias_[j] + inputScale_[j] * sx * static_cast<float>(fromInput[j]) +
                       hiddenScale_[j] * sh * static_cast<float>(fromHidden[j]);
            };
            const float input = Sigmoid(g(k)), forget = Sigmoid(g(hidden_ + k));
            const float cell = std::tanh(g(2 * hidden_ + k)), output = Sigmoid(g(3 * hidden_ + k));
            next.c[k] = forget * state.c[k] + input * cell;
            next.h[k] = output * std::tanh(next.c[k]);
        }
        return next;
    };
    std::vector<std::pair<std::size_t, State>> stack;
    stack.emplace_back(0, step(State{std::vector<float>(hidden_, 0.0f), std::vector<float>(hidden_, 0.0f)}, kStart));
    std::vector<std::int8_t> qh;
    std::vector<std::int32_t> dots(chars_);
    std::vector<float> logits(chars_);
    while (!stack.empty()) {
        auto [at, state] = std::move(stack.back());
        stack.pop_back();
        const float sh = Quantize(state.h, qh);
        DotRows(output_, chars_, hidden_, qh.data(), dots.data());
        float largest = -std::numeric_limits<float>::infinity();
        for (std::uint32_t r = 0; r < chars_; ++r) {
            logits[r] = outputBias_[r] + outputScale_[r] * sh * static_cast<float>(dots[r]);
            largest = std::max(largest, logits[r]);
        }
        const float logSum = largest + std::log(SumExp(logits.data(), chars_, largest));
        for (const std::size_t child : nodes[at].children) {
            nodes[child].logProbability = nodes[at].logProbability + logits[nodes[child].id] - logSum;
            if (!nodes[child].children.empty()) stack.emplace_back(child, step(state, nodes[child].id));
        }
    }
    for (std::size_t s = 0; s < kana.size(); ++s) result[s] = nodes[ends[s]].logProbability;
    return result;
}


std::vector<KanaSlips::Reading> KanaSlips::Find(std::wstring_view keys, const RomajiTable& table,
                                                std::size_t count) const {
    std::vector<Reading> out;
    if (!IsOpen() || keys.empty() || keys.size() > kLongestKeys) return out;
    // The typed keys as romaji units: where reading again can start and stop.
    struct Unit {
        std::size_t keyBegin, kanaBegin;
        bool vowelLast;  // its keys end in a vowel or a mark: what follows cannot change it
    };
    std::vector<Unit> units;
    std::wstring typedKana;
    {
        std::size_t key = 0;
        for (const auto& token : JapaneseComposer::ParseRomaji(table, keys)) {
            const wchar_t last = token.keys ? keys[key + token.keys - 1] : L'a';
            units.push_back({key, typedKana.size(), !IsLetter(last) || std::wstring_view(L"aiueo").find(last) != std::wstring_view::npos});
            for (wchar_t c : token.kana) typedKana += c >= 0xFF01 && c <= 0xFF5E ? static_cast<wchar_t>(c - 0xFEE0) : c;
            key += token.keys;
        }
    }
    const auto typedIds = Ids(typedKana);
    std::vector<double> typedContribution(typedIds.size(), 0.0);
    double typedNgram = 0;
    for (std::size_t at = 1; at < typedIds.size(); ++at) {
        typedContribution[at] = Contribution(typedIds.data(), at);
        typedNgram += typedContribution[at];
    }

    // A change to the keys, read again only from a safe unit before it to a
    // safe unit after it, and scored only where its kana and their next
    // few differ from the typed reading's.
    struct Change {
        std::vector<const Span*> spans;
        double cost{0};
        double ngram{0};
        std::size_t kanaBegin{0}, kanaEnd{0};
        std::wstring kana;  // what replaces typedKana[kanaBegin, kanaEnd)
    };
    const std::size_t reach = order_ - 1;
    std::vector<std::uint8_t> local;
    const auto make = [&](std::vector<const Span*> edits, Change& change) {
        std::sort(edits.begin(), edits.end(), [](const Span* x, const Span* y) { return x->begin < y->begin; });
        const std::size_t begin = edits.front()->begin, end = edits.back()->end;
        std::size_t at = 0;
        while (at + 1 < units.size() && units[at + 1].keyBegin <= begin) ++at;
        std::size_t from = at > 0 ? at - 1 : 0;
        while (from > 0 && !units[from - 1].vowelLast) --from;
        std::size_t to = at;
        while (to < units.size() && units[to].keyBegin < end + 2) ++to;
        while (to < units.size() && !units[to - 1].vowelLast) ++to;
        const std::size_t keyFrom = units.empty() ? 0 : units[from].keyBegin;
        const std::size_t keyTo = to < units.size() ? units[to].keyBegin : keys.size();
        std::wstring window;
        std::size_t key = keyFrom;
        for (const Span* edit : edits) {
            window.append(keys.substr(key, edit->begin - key));
            window += edit->with;
            key = edit->end;
        }
        window.append(keys.substr(key, keyTo - key));
        change.kanaBegin = units.empty() ? 0 : units[from].kanaBegin;
        change.kanaEnd = to < units.size() ? units[to].kanaBegin : typedKana.size();
        change.kana = KanaOf(table, window);
        if (std::wstring_view(typedKana).substr(change.kanaBegin, change.kanaEnd - change.kanaBegin) == change.kana) return false;
        // ids: up to `reach` typed ones before, the new ones, then typed ones after.
        const std::size_t first = change.kanaBegin + 1;  // typedIds has <s> first
        const std::size_t before = std::min(first, reach);
        local.assign(typedIds.begin() + static_cast<std::ptrdiff_t>(first - before), typedIds.begin() + static_cast<std::ptrdiff_t>(first));
        for (const wchar_t c : change.kana) local.push_back(charId_[static_cast<std::uint16_t>(c)]);
        const std::size_t resume = change.kanaEnd + 1;
        const std::size_t after = std::min(reach, typedIds.size() - resume);
        local.insert(local.end(), typedIds.begin() + static_cast<std::ptrdiff_t>(resume),
                     typedIds.begin() + static_cast<std::ptrdiff_t>(resume + after));
        double total = typedNgram;
        for (std::size_t p = first; p < resume + after; ++p) total -= typedContribution[p];
        for (std::size_t p = before; p < local.size(); ++p) total += Contribution(local.data(), p);
        change.ngram = total;
        change.spans = std::move(edits);
        for (const Span* edit : change.spans) change.cost += edit->cost;
        return true;
    };

    const auto spans = Edits(keys);
    std::vector<Change> changes;
    changes.reserve(spans.size() + kPairedSingles * kPairedSingles / 2);
    for (const Span& span : spans) {
        Change change;
        if (make({&span}, change)) changes.push_back(std::move(change));
    }
    const auto byScore = [&](std::size_t x, std::size_t y) {
        return changes[x].ngram - changes[x].cost > changes[y].ngram - changes[y].cost;
    };
    std::vector<std::size_t> ranked(changes.size());
    for (std::size_t i = 0; i < ranked.size(); ++i) ranked[i] = i;
    std::sort(ranked.begin(), ranked.end(), byScore);
    // Two slips: pairs of the best single edits, apart from each other.
    std::vector<const Span*> best;
    for (std::size_t i = 0; i < ranked.size() && best.size() < kPairedSingles; ++i) best.push_back(changes[ranked[i]].spans.front());
    const std::size_t singles = changes.size();
    for (std::size_t i = 0; i < best.size(); ++i) {
        for (std::size_t j = i + 1; j < best.size(); ++j) {
            const Span *x = best[i], *y = best[j];
            if (x->begin == y->begin || (x->end > y->begin && y->end > x->begin)) continue;
            Change change;
            if (make({x, y}, change)) changes.push_back(std::move(change));
        }
    }
    for (std::size_t i = singles; i < changes.size(); ++i) ranked.push_back(i);
    std::sort(ranked.begin(), ranked.end(), byScore);
    if (ranked.empty() || changes[ranked.front()].ngram - changes[ranked.front()].cost - typedNgram < kNgramGate) {
        out.push_back({std::wstring(keys), typedKana, 0, {}});
        return out;
    }
    // The best few, and the typed reading, by the LSTM; each reading once.
    std::vector<std::wstring> pool{typedKana};
    std::vector<const Change*> founds{nullptr};
    for (std::size_t i = 0; i < ranked.size() && pool.size() <= kRescored; ++i) {
        const Change& change = changes[ranked[i]];
        std::wstring kana = typedKana.substr(0, change.kanaBegin) + change.kana + typedKana.substr(change.kanaEnd);
        if (std::find(pool.begin(), pool.end(), kana) != pool.end()) continue;
        founds.push_back(&change);
        pool.push_back(std::move(kana));
    }
    const auto neural = NeuralLogProbabilities(pool);
    out.push_back({std::wstring(keys), typedKana, neural[0], {}});
    std::vector<std::size_t> order(pool.size() - 1);
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i + 1;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return neural[a] - founds[a]->cost > neural[b] - founds[b]->cost;
    });
    for (std::size_t k = 0; k < order.size() && out.size() <= count; ++k) {
        const std::size_t i = order[k];
        Reading reading{Applied(keys, founds[i]->spans), pool[i], neural[i] - founds[i]->cost, {}};
        for (const Span* span : founds[i]->spans) reading.edits.push_back({span->begin, span->end, span->with});
        std::sort(reading.edits.begin(), reading.edits.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
        out.push_back(std::move(reading));
    }
    return out;
}

}  // namespace tekito::japanese
