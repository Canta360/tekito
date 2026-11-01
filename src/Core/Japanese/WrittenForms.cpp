#include "Core/Japanese/WrittenForms.h"

namespace tekito::japanese {

bool WrittenForms::Open(const std::filesystem::path& packDirectory) noexcept {
    return table_.Open(packDirectory / L"entries.tsv");
}

std::optional<WrittenForms::Form> WrittenForms::Find(std::wstring_view lower) const {
    if (!IsOpen() || lower.empty()) return std::nullopt;
    std::optional<Form> found;
    table_.ForEachRow(ToUtf8(lower), [&](const std::vector<std::string_view>& fields) {
        if (fields.size() < 3) return true;
        found = Form{FromUtf8(fields[1]), fields[2] == "japanese"};
        return false;
    });
    return found;
}

}  // namespace tekito::japanese
