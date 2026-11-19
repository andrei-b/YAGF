#pragma once
#include <QHash>
#include <QStringList>
#include <memory>
namespace yagf::core {
// Optional Enchant backend, shared by editor and verification queue.
class SpellChecker final {
public:
    SpellChecker();
    ~SpellChecker();
    QStringList availableDictionaries() const;
    QStringList activeDictionaries() const;
    QString statusText() const;
    void setLanguages(const QStringList &languages);
    bool isMisspelled(const QString &word);
private:
    struct Dictionaries;
    std::unique_ptr<Dictionaries> dictionaries;
    QHash<QString, bool> cache;
    QStringList requested;
};
}
