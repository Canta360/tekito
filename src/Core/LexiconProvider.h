#pragma once

#include "Core/BuiltinLexicon.h"

#include <functional>
#include <cstddef>
#include <span>
#include <vector>

namespace tekito {

struct LexiconQuery {
    enum class Kind {
        Exact,
        Prefix,
        Candidate,
    };

    Kind kind{Kind::Exact};
    std::wstring_view text;
    std::size_t maxResults{0};
};

class ILexiconProvider {
public:
    using Visitor = std::function<void(const LexiconEntry&)>;

    virtual ~ILexiconProvider() = default;
    [[nodiscard]] virtual std::span<const LexiconEntry> Entries() const noexcept = 0;
    virtual void Visit(const Visitor& visitor) const {
        for (const auto& entry : Entries()) visitor(entry);
    }
    virtual void Find(const LexiconQuery& query, const Visitor& visitor) const;
};

class BuiltinLexiconProvider final : public ILexiconProvider {
public:
    [[nodiscard]] std::span<const LexiconEntry> Entries() const noexcept override;
};

class CompositeLexiconProvider final : public ILexiconProvider {
public:
    CompositeLexiconProvider(const ILexiconProvider& primary,
                             const ILexiconProvider& fallback);

    void Refresh();
    [[nodiscard]] std::span<const LexiconEntry> Entries() const noexcept override;
    void Visit(const Visitor& visitor) const override;
    void Find(const LexiconQuery& query, const Visitor& visitor) const override;

private:
    const ILexiconProvider& primary_;
    const ILexiconProvider& fallback_;
    std::vector<LexiconEntry> entries_;
};

}  // namespace tekito
