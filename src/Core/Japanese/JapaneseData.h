#pragma once

#include <memory>
#include <string_view>
#include <vector>

namespace tekito::japanese {

class JapaneseComposer;
class MeaningDictionary;
class PostalCodes;
class RomajiTable;
class UserPartsOfSpeech;

// The Japanese Data Packs, opened once per process from the Data Pack folder
// and shared by every composer in it: mapping takes well under a
// millisecond, and the pages are shared with every other process. The text
// service and the testbed both type through these.
//
// Without japanese-core, Japanese still types kana, and Space switches kana;
// without the language model, conversion goes by the parts of speech alone;
// without the loanwords, katakana has no English candidates.

// The romaji table (japanese-romaji); null when it is missing.
[[nodiscard]] const RomajiTable* ProcessRomajiTable();
// The parts of speech the user's words take (japanese-core/pos.tsv); null
// without japanese-core.
[[nodiscard]] const UserPartsOfSpeech* ProcessUserParts();
// The japanese-zipcode pack, which the user adds in Settings: opened again
// whenever its file changed (added, replaced or removed). Callers keep the
// copy they were given until they ask again.
[[nodiscard]] std::shared_ptr<const PostalCodes> ProcessPostalCodes();
// The meaning packs, mapped on first use; lookups read them in place.
[[nodiscard]] const MeaningDictionary& ProcessMeanings();

// Gives `composer` the converter, slip correction and the loanwords, as far
// as they are installed (set its table first: SetTable clears it).
void AttachJapaneseData(JapaneseComposer& composer);

// What is missing, for diagnostics ("dictionary", "language model", ...).
[[nodiscard]] std::vector<std::wstring_view> MissingJapaneseData();

}  // namespace tekito::japanese
