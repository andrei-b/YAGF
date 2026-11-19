#include "spellchecker.h"
#include <QLibrary>
#include <QLocale>

namespace yagf::core {
namespace {
QString normalizedWord(const QString &word)
{
    return word.normalized(QString::NormalizationForm_C).replace(QChar(0x2019), QLatin1Char('\''));
}

QString dictionaryCode(QString code)
{
    code.replace('-', '_');
    static const QHash<QString, QString> ocrCodes{{"eng", "en"}, {"deu", "de"}, {"ger", "de"},
        {"fra", "fr"}, {"fre", "fr"}, {"spa", "es"}, {"ita", "it"}, {"por", "pt"},
        {"rus", "ru"}, {"ukr", "uk"}, {"jpn", "ja"}, {"kor", "ko"}, {"ara", "ar"},
        {"heb", "he"}, {"chi_sim", "zh_CN"}, {"chi_tra", "zh_TW"}, {"nld", "nl"}, {"pol", "pl"}};
    return ocrCodes.value(code.toLower(), code);
}
}

struct SpellChecker::Dictionaries {
    // Enchant 2's stable C ABI; spelling remains an optional runtime dependency.
    struct Broker;
    struct Dictionary;
    using Describe = void (*)(const char *, const char *, const char *, const char *, void *);
    QLibrary library{QStringLiteral("enchant-2"), 2};
    Broker *broker = nullptr;
    Broker *(*init)() = nullptr;
    void (*freeBroker)(Broker *) = nullptr;
    void (*list)(Broker *, Describe, void *) = nullptr;
    Dictionary *(*request)(Broker *, const char *) = nullptr;
    void (*freeDictionary)(Broker *, Dictionary *) = nullptr;
    int (*check)(Dictionary *, const char *, qptrdiff) = nullptr;
    char **(*suggest)(Dictionary *, const char *, qptrdiff, size_t *) = nullptr;
    void (*freeSuggestions)(Dictionary *, char **) = nullptr;
    QStringList available, active;
    QVector<Dictionary *> loaded;
    QString status;

    Dictionaries()
    {
        init = reinterpret_cast<decltype(init)>(library.resolve("enchant_broker_init"));
        freeBroker = reinterpret_cast<decltype(freeBroker)>(library.resolve("enchant_broker_free"));
        list = reinterpret_cast<decltype(list)>(library.resolve("enchant_broker_list_dicts"));
        request = reinterpret_cast<decltype(request)>(library.resolve("enchant_broker_request_dict"));
        freeDictionary = reinterpret_cast<decltype(freeDictionary)>(library.resolve("enchant_broker_free_dict"));
        check = reinterpret_cast<decltype(check)>(library.resolve("enchant_dict_check"));
        suggest = reinterpret_cast<decltype(suggest)>(library.resolve("enchant_dict_suggest"));
        freeSuggestions = reinterpret_cast<decltype(freeSuggestions)>(library.resolve("enchant_dict_free_string_list"));
        if (!init || !freeBroker || !list || !request || !freeDictionary || !check) return;
        broker = init();
        if (!broker) return;
        list(broker, [](const char *code, const char *, const char *, const char *, void *data) {
            static_cast<QStringList *>(data)->append(QString::fromUtf8(code));
        }, &available);
        available.removeDuplicates();
        available.sort();
    }

    void clear()
    {
        for (auto *dictionary : loaded) freeDictionary(broker, dictionary);
        loaded.clear();
        active.clear();
    }
    ~Dictionaries() { clear(); if (broker) freeBroker(broker); }

    QString resolve(const QString &requested) const
    {
        for (const auto &code : available)
            if (code.compare(requested, Qt::CaseInsensitive) == 0) return code;
        // An explicit regional variant should never silently use another region's spelling rules.
        if (requested.contains('_')) return {};
        const QString regional = QLocale(requested).name();
        if (available.contains(regional)) return regional;
        for (const auto &code : available)
            if (code.startsWith(requested + '_', Qt::CaseInsensitive)) return code;
        return {};
    }
};

SpellChecker::SpellChecker() : dictionaries(std::make_unique<Dictionaries>()) { setLanguages({QStringLiteral("auto")}); }
SpellChecker::~SpellChecker() = default;
QStringList SpellChecker::availableDictionaries() const { return dictionaries->available; }
QStringList SpellChecker::activeDictionaries() const { return dictionaries->active; }
QString SpellChecker::statusText() const { return dictionaries->status; }

void SpellChecker::setLanguages(const QStringList &languages)
{
    QStringList codes;
    for (const auto &language : languages)
        for (const auto &part : language.split('+', Qt::SkipEmptyParts))
            if (part != "auto" && !codes.contains(dictionaryCode(part))) codes.append(dictionaryCode(part));
    if (codes.isEmpty()) {
        const QString system = dictionaries->resolve(QLocale::system().name());
        codes.append(system.isEmpty() ? QStringLiteral("en") : system);
    }
    if (requested == codes) return;
    requested = codes;
    dictionaries->clear();
    cache.clear();
    QStringList missing;
    for (const auto &code : codes) {
        const QString resolved = dictionaries->resolve(code);
        if (resolved.isEmpty()) { missing.append(code); continue; }
        if (dictionaries->active.contains(resolved)) continue;
        auto *dictionary = dictionaries->request(dictionaries->broker, resolved.toUtf8().constData());
        if (!dictionary) { missing.append(code); continue; }
        dictionaries->loaded.append(dictionary);
        dictionaries->active.append(resolved);
    }
    if (!missing.isEmpty()) dictionaries->clear();
    dictionaries->status = dictionaries->available.isEmpty()
        ? QStringLiteral("Spell check needs Enchant 2 and an installed spelling dictionary.")
        : !missing.isEmpty() ? QStringLiteral("No spelling dictionary for %1.").arg(missing.join(", "))
        : QStringLiteral("Spelling dictionaries: %1").arg(dictionaries->active.join(", "));
}

bool SpellChecker::isMisspelled(const QString &word)
{
    if (dictionaries->loaded.isEmpty() || word.size() <= 1) return false;
    const QString normalized = normalizedWord(word);
    if (cache.contains(normalized)) return cache.value(normalized);
    const QByteArray utf8 = normalized.toUtf8();
    bool misspelled = true;
    for (auto *dictionary : dictionaries->loaded)
        if (dictionaries->check(dictionary, utf8.constData(), utf8.size()) <= 0) { misspelled = false; break; }
    if (cache.size() >= 32768) cache.clear();
    cache.insert(normalized, misspelled);
    return misspelled;
}

QStringList SpellChecker::suggestions(const QString &word) const
{
    constexpr int limit = 8;
    if (!dictionaries->suggest || !dictionaries->freeSuggestions) return {};
    const auto utf8 = normalizedWord(word).toUtf8();
    QVector<QStringList> candidates;
    for (auto *dictionary : dictionaries->loaded) {
        size_t count = 0;
        char **list = dictionaries->suggest(dictionary, utf8.constData(), utf8.size(), &count);
        if (!list) continue;
        QStringList ranked;
        for (size_t i = 0; i < count && ranked.size() < limit; ++i) {
            const auto candidate = QString::fromUtf8(list[i]);
            if (!candidate.isEmpty() && candidate != word && !ranked.contains(candidate))
                ranked.append(candidate);
        }
        dictionaries->freeSuggestions(dictionary, list);
        candidates.append(ranked);
    }
    QStringList result;
    for (int rank = 0; rank < limit; ++rank) {
        for (const auto &list : candidates) {
            if (rank < list.size() && !result.contains(list[rank])) result.append(list[rank]);
            if (result.size() == limit) return result;
        }
    }
    return result;
}

}
