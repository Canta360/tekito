#include "Core/RunTogether.h"

#include <algorithm>
#include <cstdint>

namespace tekito {

std::optional<std::vector<std::wstring>> SplitRunTogether(std::wstring_view keys,
                                                          const std::function<bool(std::wstring_view)>& known,
                                                          const std::function<double(std::wstring_view)>& score) {
    // A word no longer than this.
    constexpr std::size_t kLongestWord = 20;
    const std::size_t n = keys.size();
    if (n == 0 || !std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; })) {
        return std::nullopt;
    }
    // Fewest words to each end, then the better scored.
    struct Best {
        std::size_t words{SIZE_MAX};
        double score{0};
        std::size_t from{0};
    };
    std::vector<Best> best(n + 1);
    best[0].words = 0;
    for (std::size_t end = 1; end <= n; ++end) {
        for (std::size_t begin = end > kLongestWord ? end - kLongestWord : 0; begin < end; ++begin) {
            if (best[begin].words == SIZE_MAX) continue;
            const auto word = keys.substr(begin, end - begin);
            if (!known(word)) continue;
            const std::size_t words = best[begin].words + 1;
            const double total = best[begin].score + score(word);
            if (words < best[end].words || (words == best[end].words && total > best[end].score)) {
                best[end] = {words, total, begin};
            }
        }
    }
    if (best[n].words == SIZE_MAX) return std::nullopt;
    std::vector<std::wstring> words;
    for (std::size_t end = n; end > 0; end = best[end].from) {
        words.emplace_back(keys.substr(best[end].from, end - best[end].from));
    }
    std::reverse(words.begin(), words.end());
    return words;
}

}  // namespace tekito
