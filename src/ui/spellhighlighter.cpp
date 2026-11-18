#include "spellhighlighter.h"

#include <QColor>
#include <QLibrary>
#include <QLocale>
#include <QRegularExpression>
#include <QTextCharFormat>

namespace yagf::ui {
namespace {
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

struct SpellHighlighter::Dictionaries {
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

SpellHighlighter::SpellHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document), dictionaries(std::make_unique<Dictionaries>())
{
    setObjectName(QStringLiteral("spellHighlighter"));
    setLanguages({QStringLiteral("auto")});
}
SpellHighlighter::~SpellHighlighter() = default;
QStringList SpellHighlighter::availableDictionaries() const { return dictionaries->available; }
QStringList SpellHighlighter::activeDictionaries() const { return dictionaries->active; }
QString SpellHighlighter::statusText() const { return enabled ? dictionaries->status : tr("Spell check is off."); }

void SpellHighlighter::setLanguages(const QStringList &languages)
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
        ? tr("Spell check needs Enchant 2 and an installed spelling dictionary.")
        : !missing.isEmpty() ? tr("No spelling dictionary for %1.").arg(missing.join(", "))
        : tr("Spelling dictionaries: %1").arg(dictionaries->active.join(", "));
    rehighlight();
}

void SpellHighlighter::setEnabled(bool value)
{
    if (enabled == value) return;
    enabled = value;
    rehighlight();
}

bool SpellHighlighter::isMisspelled(const QString &word)
{
    if (!enabled || dictionaries->loaded.isEmpty() || word.size() <= 1) return false;
    const QString normalized = word.normalized(QString::NormalizationForm_C).replace(QChar(0x2019), QLatin1Char('\''));
    if (cache.contains(normalized)) return cache.value(normalized);
    const QByteArray utf8 = normalized.toUtf8();
    bool misspelled = true;
    for (auto *dictionary : dictionaries->loaded)
        if (dictionaries->check(dictionary, utf8.constData(), utf8.size()) <= 0) { misspelled = false; break; }
    if (cache.size() >= 32768) cache.clear();
    cache.insert(normalized, misspelled);
    return misspelled;
}

void SpellHighlighter::highlightBlock(const QString &text)
{
    if (!enabled || dictionaries->loaded.isEmpty()) return;
    static const QRegularExpression words(QStringLiteral("[\\p{L}\\p{M}\\p{N}_]+(?:['’][\\p{L}\\p{M}]+)*"));
    static const QRegularExpression identifiers(QStringLiteral("[\\p{N}_]"));
    QTextCharFormat underline;
    underline.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    underline.setUnderlineColor(QColor(210, 45, 55));
    auto matches = words.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!match.captured().contains(identifiers) && isMisspelled(match.captured()))
            setFormat(match.capturedStart(), match.capturedLength(), underline);
    }
}

} // namespace yagf::ui
