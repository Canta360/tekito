#pragma once

#include "Core\LexiconProvider.h"
#include "Core\UserDictionary.h"

#include <span>
#include <string>
#include <vector>

namespace tekito {

class UserLexiconProvider final : public ILexiconProvider {
public:
    explicit UserLexiconProvider(const UserDictionary& dictionary);

    void Refresh();
    [[nodiscard]] std::span<const LexiconEntry> Entries() const noexcept override;

private:
    const UserDictionary& dictionary_;
    std::vector<std::wstring> rawTexts_;
    std::vector<std::wstring> candidateTexts_;
    std::vector<LexiconEntry> entries_;
};

}  // namespace tekito
