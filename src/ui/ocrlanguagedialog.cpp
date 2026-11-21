#include "ocrlanguagedialog.h"
#include "tesseractengine.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <stop_token>

namespace yagf::ui {
namespace {
using processing::TesseractLanguages;
using CommandResult = std::expected<QByteArray,QString>;
QString tr(const char *s) { return QCoreApplication::translate("OcrLanguageDialog",s); }

CommandResult run(const QString &program, const QStringList &args, const QString &cwd = {},
                  std::stop_token stop = {})
{
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL","C");
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(cwd);
    process.start(program,args,QIODevice::ReadOnly);
    if (!process.waitForStarted(3000)) return std::unexpected(process.errorString());
    while (!process.waitForFinished(100) && process.state() != QProcess::NotRunning) {
        if (stop.stop_requested()) {
            process.kill();
            process.waitForFinished();
            return std::unexpected(tr("Download cancelled."));
        }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const auto error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        return std::unexpected(error.isEmpty() ? tr("%1 failed.").arg(program) : error.right(1000));
    }
    return process.readAllStandardOutput();
}

struct Pack {
    QString name, code, package;
};
struct Catalog {
    QVector<Pack> packs;
    TesseractLanguages installed;
    QString note;
};

Catalog catalog()
{
    Catalog result;
    const auto installed = processing::installedTesseractLanguages();
    if (installed) result.installed = *installed;
    else result.note = tr("Could not list Tesseract languages. %1").arg(installed.error());
    QSet<QString> listed;
    // Read names from this distribution. Exclude script bundles, orientation
    // data and the all-languages metapackage from the individual language list.
    const auto available = run("apt-cache",{"search","--names-only","^tesseract-ocr-"});
    if (available) {
        static const QRegularExpression entry(
            "^((?:tesseract-ocr-)([a-z]{3}(?:-[a-z0-9]+)*)) - .*language files for (.+)$");
        for (const auto &line : QString::fromUtf8(*available).split('\n')) {
            const auto match = entry.match(line);
            if (!match.hasMatch()) continue;
            const auto code = match.captured(2).replace('-','_');
            if (code == "osd" || code == "equ" || code == "all") continue;
            result.packs.append({match.captured(3),code,match.captured(1)});
            listed.insert(code);
        }
    }
    if (result.packs.isEmpty() && installed)
        result.note = tr("Install a language pack with your system package manager, then click Refresh.");
    for (const auto &code : result.installed.codes) {
        if (listed.contains(code)) continue;
        QString name = code;
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
        const auto lang = QLocale::codeToLanguage(code);
        if (lang != QLocale::AnyLanguage && lang != QLocale::C) name = QLocale::languageToString(lang);
#endif
        result.packs.append({name,code,{}});
    }
    std::ranges::sort(result.packs,{},&Pack::name);
    return result;
}

enum class InstallMode { Manual, Writable, System };

InstallMode installMode(const QString &directory)
{
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory)) return InstallMode::Manual;
    const QFileInfo info(directory);
    if (info.canonicalFilePath().startsWith("/usr/share/tesseract-ocr/") &&
        !QStandardPaths::findExecutable("pkexec").isEmpty() && QFileInfo("/usr/bin/apt-get").isExecutable())
        return InstallMode::System;
    if (info.isDir() && info.isWritable()) return InstallMode::Writable;
    return InstallMode::Manual;
}

std::expected<void,QString> install(const Pack &pack, const QString &directory, InstallMode mode,
                                    std::stop_token stop)
{
    if (mode == InstallMode::System) {
        // Let the system complete its package transaction even if the window
        // closes. The program and package arguments never pass through a shell.
        auto result = run("pkexec",{"/usr/bin/apt-get","--no-remove","--assume-yes","install",pack.package});
        if (!result) return std::unexpected(tr("%1\nYou can also install %2 with your package manager and click Refresh.")
                                            .arg(result.error(), pack.package));
    } else if (mode == InstallMode::Writable) {
        QTemporaryDir work;
        if (!work.isValid()) return std::unexpected(work.errorString());
        auto download = run("apt-get",{"-o","Acquire::http::Timeout=30","-o","Acquire::https::Timeout=30",
                                      "download",pack.package},work.path(),stop);
        if (!download) return std::unexpected(download.error());
        const auto packages = QDir(work.path()).entryList({"*.deb"},QDir::Files);
        if (packages.size() != 1) return std::unexpected(tr("Could not locate the downloaded language package."));
        const auto extracted = work.filePath("model");
        auto unpack = run("dpkg-deb",{"-x",work.filePath(packages.first()),extracted},work.path(),stop);
        if (!unpack) return std::unexpected(unpack.error());
        const QString filename = pack.code+".traineddata";
        QDirIterator files(extracted,{filename},QDir::Files,QDirIterator::Subdirectories);
        if (!files.hasNext()) return std::unexpected(tr("The package did not contain %1.").arg(filename));
        QFile source(files.next());
        if (!source.open(QIODevice::ReadOnly) || !source.size()) return std::unexpected(tr("Could not read %1.").arg(filename));
        const auto destination = QDir(directory).filePath(filename);
        QTemporaryFile staged(QDir(directory).filePath(".yagf-language-XXXXXX"));
        if (!staged.open()) return std::unexpected(staged.errorString());
        while (!source.atEnd()) {
            if (stop.stop_requested()) return std::unexpected(tr("Download cancelled."));
            const auto chunk = source.read(65536);
            if (chunk.isEmpty() || staged.write(chunk) != chunk.size()) return std::unexpected(tr("Could not copy the language data."));
        }
        if (!staged.flush()) return std::unexpected(staged.errorString());
        staged.close();
        // Concurrent installs may already have published this pack.
        if (!QFileInfo::exists(destination)) {
            if (staged.rename(destination)) staged.setAutoRemove(false);
            else if (!QFileInfo::exists(destination)) return std::unexpected(staged.errorString());
        }
    } else return std::unexpected(tr("Install the language pack using your system package manager."));
    const auto installed = processing::installedTesseractLanguages();
    if (!installed || !installed->codes.contains(pack.code))
        return std::unexpected(tr("Tesseract still cannot find %1. Check its language data folder and click Refresh.").arg(pack.code));
    return {};
}
} // namespace

QList<OcrLanguageChoice> installedOcrLanguageChoices()
{
    const auto data = catalog();
    QList<OcrLanguageChoice> choices;
    for (const auto &pack : data.packs) {
        if (data.installed.codes.contains(pack.code)) choices.append({pack.name, pack.code});
    }
    return choices;
}

std::optional<QString> chooseAdditionalOcrLanguage(QWidget *parent,const QString &selection)
{
    QDialog dialog(parent);
    dialog.setObjectName("ocrLanguageDialog");
    dialog.setWindowTitle(tr("Add OCR language"));
    dialog.resize(540,465);
    auto *layout = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit;
    search->setObjectName("languagePackSearch");
    search->setPlaceholderText(tr("Search by language name or code"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto *list = new QTreeWidget;
    list->setObjectName("ocrLanguagePacks");
    list->setHeaderLabels({tr("Language"),tr("Code"),tr("Availability")});
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->header()->setStretchLastSection(false);
    list->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    list->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    list->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);
    layout->addWidget(list,1);
    auto *status = new QLabel;
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *refresh = buttons->addButton(tr("Refresh"),QDialogButtonBox::ActionRole);
    auto *add = buttons->addButton(tr("Add language"),QDialogButtonBox::AcceptRole);
    add->setObjectName("confirmOcrLanguage");
    add->setDefault(true);
    layout->addWidget(buttons);

    Catalog data;
    const auto current = selection.trimmed().isEmpty() ? QStringList{}
        : processing::TesseractEngine::modelLanguage(selection).split('+', Qt::SkipEmptyParts);
    std::optional<QString> result;
    std::stop_source cancellation;
    QFutureWatcher<Catalog> loading;
    QFutureWatcher<std::expected<void,QString>> installing;
    bool busy = false;
    auto update = [&] {
        const auto *item = list->currentItem();
        const auto code = item ? item->text(1) : QString{};
        const bool installed = data.installed.codes.contains(code);
        const auto mode = installMode(data.installed.directory);
        const bool alreadySelected = current.contains(code);
        auto combined = current;
        if (!alreadySelected) combined.append(code);
        const bool fits = combined.join('+').size() <= 64;
        add->setText(installed ? tr("Add to selection") : tr("Install and add"));
        add->setEnabled(!busy && item && !item->isHidden() && !(alreadySelected && installed) && fits &&
                        (installed || mode != InstallMode::Manual));
        refresh->setEnabled(!busy);
        list->setEnabled(!busy);
        search->setEnabled(!busy);
        if (busy) return;
        QString message = data.note;
        if (item && !item->isHidden()) {
            if (alreadySelected && installed) message = tr("%1 is already in your selection.").arg(code);
            else if (!fits && !alreadySelected) message = tr("The combined OCR language code exceeds 64 characters.");
            else if (!installed && mode == InstallMode::Manual) {
                if (!data.installed.directory.isEmpty())
                    message = tr("Place %1.traineddata in %2, then click Refresh.").arg(code, data.installed.directory);
                else if (message.isEmpty())
                    message = tr("Install %1 using your system package manager, then click Refresh.").arg(item->data(0,Qt::UserRole).toString());
            } else if (!installed) message = mode == InstallMode::System
                ? tr("Your system will ask for permission to install %1.").arg(item->data(0,Qt::UserRole).toString())
                : tr("Downloads %1 and installs the language data for your local Tesseract.").arg(item->data(0,Qt::UserRole).toString());
            else if (message.isEmpty()) message = tr("%1 is ready to use.").arg(item->text(0));
        } else if (message.isEmpty()) message = tr("No matching languages.");
        status->setText(message);
    };
    auto filter = [&] {
        QTreeWidgetItem *first = nullptr;
        const auto query = search->text().trimmed();
        for (int i=0;i<list->topLevelItemCount();++i) {
            auto *item = list->topLevelItem(i);
            const bool match = item->text(0).contains(query,Qt::CaseInsensitive) || item->text(1).contains(query,Qt::CaseInsensitive);
            item->setHidden(!match);
            if (match && !first) first = item;
        }
        if (!list->currentItem() || list->currentItem()->isHidden()) list->setCurrentItem(first);
        update();
    };
    auto reload = [&] {
        busy = true;
        update();
        status->setText(tr("Looking for installed languages and available packs…"));
        loading.setFuture(QtConcurrent::run(catalog));
    };
    QObject connections;
    QObject::connect(&loading,&QFutureWatcherBase::finished,&connections,[&] {
        data = loading.result();
        list->clear();
        for (const auto &pack : data.packs) {
            auto *item = new QTreeWidgetItem(list,{pack.name,pack.code,data.installed.codes.contains(pack.code)
                ? tr("Installed") : tr("Available to install")});
            item->setData(0,Qt::UserRole,pack.package);
        }
        busy = false;
        filter();
        search->setFocus();
    });
    auto acceptCode = [&](const QString &code) {
        auto codes = current;
        if (!codes.contains(code)) codes.append(code);
        result = codes.join('+');
        dialog.accept();
    };
    QObject::connect(&installing,&QFutureWatcherBase::finished,&connections,[&] {
        busy = false;
        const auto installed = installing.result();
        if (installed) acceptCode(list->currentItem()->text(1));
        else { update(); status->setText(installed.error()); }
    });
    QObject::connect(add,&QPushButton::clicked,&connections,[&] {
        const auto *item = list->currentItem();
        if (busy || !add->isEnabled() || !item) return;
        Pack pack{item->text(0),item->text(1),item->data(0,Qt::UserRole).toString()};
        if (data.installed.codes.contains(pack.code)) { acceptCode(pack.code); return; }
        busy = true;
        update();
        const auto mode = installMode(data.installed.directory);
        status->setText(mode == InstallMode::System
            ? tr("Installing %1… Closing this window lets the system installer finish in the background.").arg(pack.name)
            : tr("Installing %1…").arg(pack.name));
        installing.setFuture(QtConcurrent::run(install,pack,data.installed.directory,mode,cancellation.get_token()));
    });
    QObject::connect(search,&QLineEdit::textChanged,&connections,filter);
    QObject::connect(list,&QTreeWidget::currentItemChanged,&connections,update);
    QObject::connect(list,&QTreeWidget::itemDoubleClicked,&connections,[&]{ if (add->isEnabled()) add->click(); });
    QObject::connect(refresh,&QPushButton::clicked,&connections,reload);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    reload();
    dialog.exec();
    cancellation.request_stop();
    return result;
}
} // namespace yagf::ui
