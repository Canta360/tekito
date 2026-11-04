#include "Core/Japanese/JapaneseData.h"

#include "Core/ExternalLexiconProvider.h"
#include "Core/ExternalRankingProviders.h"
#include "Core/Japanese/AiJudge.h"
#include "Core/Japanese/ChunkJudge.h"
#include "Core/Japanese/LetterOdds.h"
#include "Core/Japanese/WrittenForms.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KanaSlips.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/PostalCodes.h"
#include "Core/Japanese/RomajiTable.h"
#include "Core/Japanese/SentenceReranker.h"
#include "Core/Japanese/TopicVectors.h"

#include <filesystem>
#include <mutex>

namespace tekito::japanese {
namespace {

// Letters read by their names (ぴーしー: PC) cost this much over a proper
// noun: offered beside the katakana (ピーシー), not over it.
constexpr std::int32_t kLetterWordCost = 2000;

struct JapaneseData {
    JapaneseDictionary dictionary;
    ConnectionMatrix matrix;
    LanguageModel model;
    TopicVectors topics;
    SentenceReranker reranker;
    KanaSlips slips;
    Loanwords loanwords;
    KanjiSets kanjiSets;
    UserPartsOfSpeech userParts;
    std::unique_ptr<JapaneseConverter> converter;
    std::unique_ptr<KeyConverter> keys;
    // Mixed typing: English word scores, the judge, and the part of speech
    // Japanese after an English word reads on from.
    std::unique_ptr<ExternalFrequencyProvider> english;
    std::unique_ptr<ChunkJudge> chunks;
    WrittenForms writtenForms;
    LetterOdds letterOdds;
    std::uint16_t nounContext{0};
};

const JapaneseData* ProcessJapaneseData() {
    static const std::unique_ptr<JapaneseData> data = [] {
        auto loaded = std::make_unique<JapaneseData>();
        const auto core = ExternalLexiconProvider::PackDirectory(L"japanese-core");
        if (!loaded->dictionary.Open(core / L"dictionary.bin") || !loaded->matrix.Open(core / L"connection.bin")) {
            return std::unique_ptr<JapaneseData>{};
        }
        // Words of Wikipedia article titles, a pack of their own (CC BY-SA).
        (void)loaded->dictionary.AddWords(ExternalLexiconProvider::PackDirectory(L"japanese-wikimedia") / L"dictionary.bin");
        // Proper nouns from Wikidata (CC0), set phrases and names from
        // JMdict and JMnedict (CC BY-SA), SudachiDict's nouns (Apache-2.0)
        // and Japan Post's place names: packs of their own too.
        for (const wchar_t* pack : {L"japanese-names", L"japanese-jmdict", L"japanese-sudachi", L"japanese-places"}) {
            (void)loaded->dictionary.AddWords(ExternalLexiconProvider::PackDirectory(pack) / L"dictionary.bin");
        }
        loaded->converter = std::make_unique<JapaneseConverter>(loaded->dictionary, loaded->matrix);
        if (loaded->model.Open(ExternalLexiconProvider::PackDirectory(L"japanese-lm"))) {
            loaded->converter->SetLanguageModel(&loaded->model);
        }
        if (loaded->topics.Open(ExternalLexiconProvider::PackDirectory(L"japanese-topics"))) {
            loaded->converter->SetTopicVectors(&loaded->topics);
        }
        if (const auto* table = ProcessRomajiTable()) {
            loaded->keys = std::make_unique<KeyConverter>(loaded->dictionary, loaded->matrix, *loaded->converter,
                                                          *table);
        }
        (void)loaded->reranker.Open(ExternalLexiconProvider::PackDirectory(L"japanese-reranker"));
        (void)loaded->slips.Open(ExternalLexiconProvider::PackDirectory(L"japanese-slips"));
        (void)loaded->loanwords.Open(ExternalLexiconProvider::PackDirectory(L"japanese-loanwords"));
        (void)loaded->loanwords.OpenEnglish(ExternalLexiconProvider::PackDirectory(L"japanese-english"));
        (void)loaded->kanjiSets.Load(ExternalLexiconProvider::PackDirectory(L"kanji-sets") / L"kanji.tsv");
        (void)loaded->kanjiSets.LoadVariants(ExternalLexiconProvider::PackDirectory(L"japanese-variants") / L"variants.tsv");
        loaded->userParts = UserPartsOfSpeech::Load(core / L"pos.tsv");
        if (const auto number = loaded->userParts.Number()) {
            loaded->converter->SetNumberWord(JapaneseConverter::NumberWord{number->left, number->right, number->cost});
        }
        if (const auto noun = loaded->userParts.For(UserWordKind::ProperNoun)) {
            loaded->nounContext = noun->right;
            loaded->converter->SetLetterWord(JapaneseConverter::NumberWord{noun->left, noun->right, noun->cost + kLetterWordCost});
        }
        loaded->english = std::make_unique<ExternalFrequencyProvider>(ExternalFrequencyProvider::DefaultPath());
        if (const auto* table = ProcessRomajiTable(); table && loaded->english->IsLoaded()) {
            loaded->chunks = std::make_unique<ChunkJudge>(*table, *loaded->converter, loaded->keys.get(),
                                                          *loaded->english,
                                                          loaded->loanwords.IsOpen() ? &loaded->loanwords : nullptr);
            if (loaded->writtenForms.Open(ExternalLexiconProvider::PackDirectory(L"written-forms"))) {
                loaded->chunks->SetWrittenForms(&loaded->writtenForms);
            }
            if (loaded->letterOdds.Open(ExternalLexiconProvider::PackDirectory(L"japanese-letter-odds"))) {
                loaded->chunks->SetLetterOdds(&loaded->letterOdds);
            }
        }
        return loaded;
    }();
    return data.get();
}

}  // namespace

namespace {

RomajiTable* SharedRomajiTable() {
    // A few kilobytes.
    static const std::unique_ptr<RomajiTable> table = [] {
        auto loaded = RomajiTable::Load(ExternalLexiconProvider::PackDirectory(L"japanese-romaji"));
        if (loaded) loaded->ApplyChanges(RomajiTable::ReadChanges(UserRomajiFile()));
        return loaded;
    }();
    return table.get();
}

}  // namespace

const RomajiTable* ProcessRomajiTable() {
    return SharedRomajiTable();
}

std::filesystem::path UserRomajiFile() {
    return ExternalLexiconProvider::PackDirectory(L"user-romaji") / L"romaji.tsv";
}

void ReloadUserRomaji() {
    if (auto* table = SharedRomajiTable()) table->ApplyChanges(RomajiTable::ReadChanges(UserRomajiFile()));
}

const UserPartsOfSpeech* ProcessUserParts() {
    const auto* data = ProcessJapaneseData();
    return data ? &data->userParts : nullptr;
}

std::shared_ptr<const PostalCodes> ProcessPostalCodes() {
    static std::mutex mutex;
    static std::shared_ptr<const PostalCodes> cached;
    static std::filesystem::file_time_type stamp;
    std::lock_guard lock(mutex);
    const auto directory = ExternalLexiconProvider::PackDirectory(L"japanese-zipcode");
    std::error_code error;
    const auto written = std::filesystem::last_write_time(directory / L"zipcodes.tsv", error);
    if (error || !std::filesystem::exists(directory / L"manifest.json", error)) {
        cached.reset();
        return cached;
    }
    if (!cached || written != stamp) {
        auto opened = std::make_shared<PostalCodes>();
        cached = opened->Open(directory) ? std::move(opened) : nullptr;
        stamp = written;
    }
    return cached;
}

const MeaningDictionary& ProcessMeanings() {
    static const std::unique_ptr<MeaningDictionary> meanings = [] {
        auto opened = std::make_unique<MeaningDictionary>();
        (void)opened->Open(ExternalLexiconProvider::DataPackRoot());
        return opened;
    }();
    return *meanings;
}

const AiJudge* ProcessAiJudge(AiModel model, AiDevice device) {
    if (model != AiModel::Light && model != AiModel::Strong) return nullptr;
    std::error_code error;
    const auto directory = ExternalLexiconProvider::PackDirectory(AiModelPack(model));
    if (directory.empty() || !std::filesystem::is_regular_file(directory / L"model.gguf", error)) return nullptr;
    // One client per model and device, made once its pack is there (it
    // reads how to use the model from the pack). How long a conversion
    // waits for it: the light model reads ten sentences and writes five in
    // about 0.15 s on a GPU, 1 s on a processor; the strong one about four
    // times that.
    static std::mutex lock;
    static std::unique_ptr<AiHelperClient> clients[3][3];
    const auto m = static_cast<std::size_t>(model);
    const auto d = static_cast<std::size_t>(std::clamp(static_cast<int>(device), 0, 2));
    std::lock_guard<std::mutex> hold(lock);
    if (!clients[m][d]) {
        using std::chrono::milliseconds;
        const auto wait = model == AiModel::Light ? milliseconds(device == AiDevice::Cpu ? 1500 : 600)
                                                  : milliseconds(device == AiDevice::Cpu ? 4000 : 1500);
        clients[m][d] = std::make_unique<AiHelperClient>(model, device, wait, 10);
    }
    return clients[m][d].get();
}

void AttachJapaneseData(JapaneseComposer& composer) {
    const auto* data = ProcessJapaneseData();
    composer.SetConverter(data ? data->converter.get() : nullptr);
    composer.SetKeyConverter(data ? data->keys.get() : nullptr);
    composer.SetLoanwords(data && data->loanwords.IsOpen() ? &data->loanwords : nullptr);
    composer.SetRunTogether(data ? data->chunks.get() : nullptr);
    composer.SetSentenceReranker(data && data->reranker.IsOpen() ? &data->reranker : nullptr);
    composer.SetKanaSlips(data && data->slips.IsOpen() ? &data->slips : nullptr);
    composer.SetKanjiSets(data && (!data->kanjiSets.Empty() || data->kanjiSets.HasVariants()) ? &data->kanjiSets : nullptr);
}

void AttachMixedTyping(JapaneseComposer& composer, bool on) {
    const auto* data = ProcessJapaneseData();
    if (on && data && data->chunks) {
        composer.SetMixedTyping(data->chunks.get(), data->nounContext);
    } else {
        composer.SetMixedTyping(nullptr);
    }
}

std::vector<std::wstring_view> MissingJapaneseData() {
    std::vector<std::wstring_view> missing;
    if (!ProcessRomajiTable()) missing.emplace_back(L"romaji table");
    const auto* data = ProcessJapaneseData();
    if (!data) {
        missing.emplace_back(L"dictionary");
        return missing;
    }
    if (!data->model.IsOpen()) missing.emplace_back(L"language model");
    if (!data->loanwords.IsOpen()) missing.emplace_back(L"loanwords");
    if (!data->reranker.IsOpen()) missing.emplace_back(L"sentence check");
    if (!data->slips.IsOpen()) missing.emplace_back(L"slip finder");
    if (data->userParts.Empty()) missing.emplace_back(L"user word parts of speech");
    return missing;
}

}  // namespace tekito::japanese
