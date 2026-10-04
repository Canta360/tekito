#include "Core/Japanese/JapaneseData.h"

#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/PostalCodes.h"
#include "Core/Japanese/RomajiTable.h"

#include <filesystem>
#include <mutex>

namespace tekito::japanese {
namespace {

struct JapaneseData {
    JapaneseDictionary dictionary;
    ConnectionMatrix matrix;
    LanguageModel model;
    Loanwords loanwords;
    UserPartsOfSpeech userParts;
    std::unique_ptr<JapaneseConverter> converter;
    std::unique_ptr<KeyConverter> keys;
};

const JapaneseData* ProcessJapaneseData() {
    static const std::unique_ptr<JapaneseData> data = [] {
        auto loaded = std::make_unique<JapaneseData>();
        const auto core = ExternalLexiconProvider::PackDirectory(L"japanese-core");
        if (!loaded->dictionary.Open(core / L"dictionary.bin") || !loaded->matrix.Open(core / L"connection.bin")) {
            return std::unique_ptr<JapaneseData>{};
        }
        loaded->converter = std::make_unique<JapaneseConverter>(loaded->dictionary, loaded->matrix);
        if (loaded->model.Open(ExternalLexiconProvider::PackDirectory(L"japanese-lm"))) {
            loaded->converter->SetLanguageModel(&loaded->model);
        }
        if (const auto* table = ProcessRomajiTable()) {
            loaded->keys = std::make_unique<KeyConverter>(loaded->dictionary, loaded->matrix, *loaded->converter,
                                                          *table);
        }
        (void)loaded->loanwords.Open(ExternalLexiconProvider::PackDirectory(L"japanese-loanwords"));
        loaded->userParts = UserPartsOfSpeech::Load(core / L"pos.tsv");
        if (const auto number = loaded->userParts.Number()) {
            loaded->converter->SetNumberWord(JapaneseConverter::NumberWord{number->left, number->right, number->cost});
        }
        return loaded;
    }();
    return data.get();
}

}  // namespace

const RomajiTable* ProcessRomajiTable() {
    // A few kilobytes.
    static const std::unique_ptr<RomajiTable> table =
        RomajiTable::Load(ExternalLexiconProvider::PackDirectory(L"japanese-romaji"));
    return table.get();
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

void AttachJapaneseData(JapaneseComposer& composer) {
    const auto* data = ProcessJapaneseData();
    composer.SetConverter(data ? data->converter.get() : nullptr);
    composer.SetKeyConverter(data ? data->keys.get() : nullptr);
    composer.SetLoanwords(data && data->loanwords.IsOpen() ? &data->loanwords : nullptr);
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
    if (data->userParts.Empty()) missing.emplace_back(L"user word parts of speech");
    return missing;
}

}  // namespace tekito::japanese
