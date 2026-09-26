#include "Core/BuiltinLexicon.h"

#include <array>

namespace tekito {
namespace {

constexpr std::array<BuiltinLexiconEntry, 58> kBuiltinLexicon{{
    {L"name", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"neume", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"meme", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"mneme", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"nemesis", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"hello", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"he", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"world", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"meet", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"met", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"the", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"there", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"their", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"they're", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"think", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"thank", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"going", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"home", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"receive", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"because", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"definitely", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"environment", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"necessary", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"probably", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"happy", L"\U0001F600", SemanticLabel::Original, SemanticLabel::Emoji, true, true},
    {L"hippy", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"suspicious", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"charisma", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"average", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"exhausted", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"seriously", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"laughing", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"loud", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"want", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"kind", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"of", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"to", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"i", L"I", SemanticLabel::Original, SemanticLabel::None, false, false},
    {L"im", L"I'm", SemanticLabel::Original, SemanticLabel::None, false, false},
    {L"i'm", L"I'm", SemanticLabel::Original, SemanticLabel::None, false, false},
    {L"ive", L"I've", SemanticLabel::Original, SemanticLabel::None, false, false},
    {L"i've", L"I've", SemanticLabel::Original, SemanticLabel::None, false, false},
    {L"gonna", L"going to", SemanticLabel::Phrase, SemanticLabel::Standard, true, false},
    {L"wanna", L"want to", SemanticLabel::Phrase, SemanticLabel::Standard, true, false},
    {L"kinda", L"kind of", SemanticLabel::Phrase, SemanticLabel::Standard, true, false},
    {L"sus", L"suspicious", SemanticLabel::Slang, SemanticLabel::Standard, true, false},
    {L"rizz", L"charisma", SemanticLabel::Slang, SemanticLabel::Standard, true, false},
    {L"mid", L"average", SemanticLabel::Slang, SemanticLabel::Standard, true, false},
    {L"delulu", L"delusional", SemanticLabel::Slang, SemanticLabel::Standard, true, false},
    {L"cooked", L"exhausted", SemanticLabel::Slang, SemanticLabel::Standard, true, false},
    {L"lol", L"laughing out loud", SemanticLabel::Slang, SemanticLabel::Standard, true, false},
    {L"bruh", L"bro", SemanticLabel::Slang, SemanticLabel::Standard, true, false},
    {L"lowkey", L"quietly", SemanticLabel::Slang, SemanticLabel::Standard, true, false},
    {L"nocap", L"no cap", SemanticLabel::Phrase, SemanticLabel::Standard, true, false},
    {L"idk", L"I don't know", SemanticLabel::Abbreviation, SemanticLabel::Standard, true, false},
    {L"delusional", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"bro", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
    {L"quietly", {}, SemanticLabel::Original, SemanticLabel::None, true, true},
}};

}  // namespace

std::span<const LexiconEntry> BuiltinLexicon() noexcept {
    return kBuiltinLexicon;
}

}  // namespace tekito
