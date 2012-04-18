/*
    YAGF - cuneiform and tesseract OCR graphical front-ends
    Copyright (C) 2009-2012 Andrei Borovsky <anb@symmetrica.net>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "sidebar.h"
#include "droplabel.h"
#include "popplerdialog.h"
#include "pdfextractor.h"
#include "pdf2ppt.h"
#include "ghostscr.h"
#include "configdialog.h"
#include "advancedconfigdialog.h"
#include "mainform.h"
#include <signal.h>
#include <QComboBox>
#include <QLabel>
#include <QPixmap>
#include <QPixmapCache>
#include <QFileDialog>
#include <QCloseEvent>
#include <QPainter>
#include <QSize>
#include <QStringList>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QFile>
#include <QByteArray>
#include <QRect>
#include <QRectF>
#include <QStatusBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QImage>
#include <QDesktopServices>
#include <QUrl>
#include <QRegExp>
#include <QClipboard>
#include <QTransform>
#include <QMap>
#include "qgraphicsinput.h"
#include "utils.h"
#include "qxtunixsignalcatcher.h"
#include "spellchecker.h"
#include "PageAnalysis.h"
#include <QTextCodec>
#include <QCheckBox>
#include <QEvent>
#include <QCursor>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFont>
#include <QImageReader>

const QString outputBase = "output";
const QString outputExt = ".txt";

MainForm::MainForm(QWidget *parent): QMainWindow(parent)
{
    setupUi(this);

    ///!!!!!
    //alignButton->hide();
    //unalignButton->hide();


    setWindowTitle("YAGF");
    spellChecker = new SpellChecker(textEdit);
    spellChecker->enumerateDicts();
    selectLangsBox = new QComboBox();
    QLabel *label = new QLabel();
    label->setMargin(4);
    label->setText(trUtf8("Recognition language"));
    frame->show();
    toolBar->addWidget(label);
    selectLangsBox->setFrame(true);
    toolBar->addWidget(selectLangsBox);
    graphicsInput = new QGraphicsInput(QRectF(0, 0, 2000, 2000), graphicsView) ;
    graphicsInput->addToolBarAction(actionHideShowTolbar);
    graphicsInput->addToolBarAction(this->actionTBLV);
    graphicsInput->addToolBarAction(this->actionSmaller_view);
    graphicsInput->addToolBarSeparator();
    graphicsInput->addToolBarAction(actionRotate_90_CCW);
    graphicsInput->addToolBarAction(actionRotate_180);
    graphicsInput->addToolBarAction(actionRotate_90_CW);
    graphicsInput->addToolBarAction(actionDeskew);
    graphicsInput->addToolBarSeparator();
    graphicsInput->addToolBarAction(actionSelect_Text_Area);
    graphicsInput->addToolBarAction(actionSelect_multiple_blocks);
    graphicsInput->addToolBarAction(ActionClearAllBlocks);

    statusBar()->show();
    imageLoaded = false;
    useXSane = TRUE;
    textSaved = TRUE;
    hasCopy = false;
    //rotation = 0;
    m_menu = new QMenu(graphicsView);
    ifCounter = 0;

    connect(actionOpen, SIGNAL(triggered()), this, SLOT(loadImage()));
    connect(actionQuit, SIGNAL(triggered()), this, SLOT(close()));
    connect(this, SIGNAL(windowShown()), this, SLOT(onShowWindow()), Qt::QueuedConnection);
    connect(actionScan, SIGNAL(triggered()), this, SLOT(scanImage()));
    connect(actionPreviousPage, SIGNAL(triggered()), this, SLOT(loadPreviousPage()));
    connect(actionNextPage, SIGNAL(triggered()), this, SLOT(loadNextPage()));
    connect(actionRecognize, SIGNAL(triggered()), this, SLOT(recognize()));
    connect(action_Save, SIGNAL(triggered()), this, SLOT(saveText()));
    connect(actionAbout, SIGNAL(triggered()), this, SLOT(showAboutDlg()));
    connect(actionOnlineHelp, SIGNAL(triggered()), this, SLOT(showHelp()));
    connect(actionCopyToClipboard, SIGNAL(triggered()), this, SLOT(copyClipboard()));
    textEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(textEdit, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextMenuRequested(QPoint)));
    connect(textEdit, SIGNAL(copyAvailable(bool)), this, SLOT(copyAvailable(bool)));
    connect(textEdit, SIGNAL(textChanged()), this, SLOT(textChanged()));
    connect(graphicsInput, SIGNAL(rightMouseClicked(int, int, bool)), this, SLOT(rightMouseClicked(int, int, bool)));


    tesMap = new TesMap();
    fillLanguagesBox();
    initSettings();
    delTmpFiles();
    scanProcess = new QProcess(this);
    QXtUnixSignalCatcher::connectUnixSignal(SIGUSR2);
    ba = new QByteArray();
    connect(QXtUnixSignalCatcher::catcher(), SIGNAL(unixSignal(int)), this, SLOT(readyRead(int)));

    connect(textEdit->document(), SIGNAL(cursorPositionChanged(const QTextCursor &)), this, SLOT(updateSP()));

    //displayLabel->installEventFilter(this);
    textEdit->installEventFilter(this);
    QPixmap l_cursor;
    l_cursor.load(":/resize.png");
    resizeCursor = new QCursor(l_cursor);
    graphicsInput->setMagnifierCursor(resizeCursor);
    l_cursor.load(":/resize_block.png");
    resizeBlockCursor = new QCursor(l_cursor);
   // textEdit->setContextMenuPolicy(Qt::ActionsContextMenu);

    this->sideBar->show();
    connect(sideBar, SIGNAL(fileSelected(const QString &)), this, SLOT(fileSelected(const QString &)));

    connect(actionRecognize_All_Pages, SIGNAL(triggered()), this, SLOT(recognizeAll()));

    graphicsInput->setSideBar(sideBar);

    QPixmap pm;
    pm.load(":/align.png");
    //alignButton->setIcon(pm);
    pm.load(":/undo.png");
    //unalignButton->setIcon(pm);
    //connect(unalignButton, SIGNAL(clicked()), this, SLOT(unalignButtonClicked()));

    //clearBlocksButton->setDefaultAction(ActionClearAllBlocks);
    loadFromCommandLine();
    emit windowShown();

    pdfx = NULL;
    if (findProgram("pdftoppm")) {
        pdfx = new PDF2PPT();
    } else
    if (findProgram("gs")) {
         pdfx = new GhostScr();
    }

    if (pdfx) {
        connect(pdfx, SIGNAL(addPage(QString)), this, SLOT(addPDFPage(QString)), Qt::QueuedConnection);
        connect (pdfx, SIGNAL(finished()), this, SLOT(finishedPDF()));
    }

    pdfPD.setWindowTitle("YAGF");
    pdfPD.setLabelText(trUtf8("Importing pages from the PDF document..."));
    pdfPD.setCancelButtonText(trUtf8("Cancel"));
    pdfPD.setMinimum(-1);
    pdfPD.setMaximum(-1);
    pdfPD.setWindowIcon(QIcon(":/yagf.png"));
    if (pdfx)
        connect(&pdfPD, SIGNAL(canceled()), pdfx, SLOT(cancel()));

}

void MainForm::onShowWindow()
{
    // actionCheck_spelling->setCheckable(true);
    connect(selectLangsBox, SIGNAL(currentIndexChanged(int)), this, SLOT(newLanguageSelected(int)));
    selectLangsBox->setCurrentIndex(selectLangsBox->findData(QVariant(settings.getLanguage())));
    //spellChecker->setLanguage(language);
    //actionCheck_spelling->setEnabled(spellChecker->spellCheck());
}

void MainForm::loadFromCommandLine()
{
    QStringList sl = QApplication::arguments();
    if (sl.count() > 1) {
        if (QFile::exists(sl.at(1)))
            loadFile(sl.at(1));
        for (int i = 2; i < sl.count(); i++)
            if (QFile::exists(sl.at(i)))
                loadFile(sl.at(i));
        if (QFile::exists(sl.at(1)))
            sideBar->select(sl.at(1));

    }
}

void MainForm::showConfigDlg()
{
    ConfigDialog dialog(this);
    if (settings.getSelectedEngine() == UseCuneiform)
        dialog.setSelectedEngine(0);
    else
        dialog.setSelectedEngine(1);
    dialog.setTessDataPath(settings.getTessdataPath());
    if (dialog.exec()) {
        settings.setSelectedEngine(dialog.selectedEngine() == 0 ? UseCuneiform : UseTesseract);
        settings.setTessdataPath(dialog.tessdataPath());
    }
}

void MainForm::importPDF()
{
    if (!pdfx) {
        QMessageBox::critical(this, trUtf8("No PDF converter installed"), trUtf8("No compatible PDF converter software could be found. Please install either the pdftoppm utility or the GhostScript package (from this the gs command will be required)."));
        return;
    }
    PopplerDialog dialog(this);
    if (dialog.exec()) {
        pdfx->setSourcePDF(dialog.getPDFFile());
        if (pdfx->getSourcePDF().isEmpty()) {
            QMessageBox::information(this, trUtf8("Error"), trUtf8("PDF file name may not be empty"));
            return;
        }
        pdfx->setStartPage(dialog.getStartPage());
        pdfx->setStopPage(dialog.getStopPage());
        bool doit = true;
        QString outputDir;
        while (doit) {
            outputDir = QFileDialog::getExistingDirectory(this, trUtf8("Select an existing directory for output or create some new one")); //, QString(""), QString(), (QString*) NULL, QFileDialog::ShowDirsOnly);
            if (outputDir.isEmpty())
                return;
            QDir dir(outputDir);
            if (dir.count() > 2)
                QMessageBox::warning(this, trUtf8("Selecting Directory"), trUtf8("The selected directory is not empty"));
            else doit = false;
        }
        pdfx->setOutputDir(outputDir);
        QApplication::processEvents();
        pdfPD.setWindowFlags(Qt::Dialog|Qt::WindowStaysOnTopHint);
        pdfPD.show();
        pdfPD.setMinimum(0);
        pdfPD.setMaximum(100);
        QApplication::processEvents();
        pdfx->exec();
    }
}

void MainForm::addPDFPage(QString pageName)
{
    sideBar->addFile(pageName);
    pdfPD.setValue(pdfPD.value()+1);
}

void MainForm::finishedPDF()
{
    pdfPD.hide();
}

void MainForm::loadImage()
{
    QFileDialog dialog(this,
                       trUtf8("Open Image"), settings.getLastDir(), trUtf8("Image Files (*.png *.jpg *.jpeg *.bmp *.tiff *.tif *.gif *.pnm *.pgm *.pbm *.ppm)"));
    dialog.setFileMode(QFileDialog::ExistingFiles);
    if (dialog.exec()) {
        QStringList fileNames;
        fileNames = dialog.selectedFiles();
        settings.setLastDir(dialog.directory().path());
        if (fileNames.count() > 0)
         loadFile(fileNames.at(0));
        if (!imageLoaded)
            return;
        for (int i = 1; i < fileNames.count(); i++) {
            loadFile(fileNames.at(i), false);
        }
        if (fileNames.count() > 0)
            sideBar->select(fileNames.at(0));
    }
}

void MainForm::singleColumnButtonClicked()
{
    //singleColumn = singleColumnButton->isChecked();
}

void MainForm::closeEvent(QCloseEvent *event)
{
    if (!textSaved) {
        QPixmap icon;
        icon.load(":/info.png");

        QMessageBox messageBox(QMessageBox::NoIcon, "YAGF", trUtf8("There is an unsaved text in the editor window. Do you want to save it?"),
                               QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
        messageBox.setIconPixmap(icon);
        int result = messageBox.exec();
        if (result == QMessageBox::Save)
            saveText();
        else if (result == QMessageBox::Cancel) {
            event->ignore();
            return;
        }

    }
    scanProcess->terminate();
    settings.setFontSize(textEdit->font().pointSize());
    settings.setSize(size());
    settings.setPosition(pos());
    settings.setFullScreen(isFullScreen());
    settings.writeSettings();
    delTmpFiles();
    event->accept();
    QXtUnixSignalCatcher::catcher()->disconnectUnixSugnals();
}

void MainForm::rotateCWButtonClicked()
{
    QCursor oldCursor = cursor();
    setCursor(Qt::WaitCursor);
    graphicsInput->rotateImage(90);
    setCursor(oldCursor);
}

void MainForm::rotateCCWButtonClicked()
{
    QCursor oldCursor = cursor();
    setCursor(Qt::WaitCursor);
    graphicsInput->rotateImage(-90);
    setCursor(oldCursor);
}
void MainForm::rotate180ButtonClicked()
{
    QCursor oldCursor = cursor();
    setCursor(Qt::WaitCursor);
    graphicsInput->rotateImage(180);
    setCursor(oldCursor);
}

void MainForm::enlargeButtonClicked()
{
    scaleImage(2.0);
}

void MainForm::decreaseButtonClicked()
{
    scaleImage(0.5);
}

void MainForm::scaleImage(double sf)
{
    if (!imageLoaded)
        return;
    if (sideBar->getScale()*sf > 0.5)
        return;
    graphicsInput->setViewScale(sideBar->getScale()*sf, 0);
    sideBar->setScale(sideBar->getScale()*sf);
}

void MainForm::initSettings()
{
    workingDir = QDir::homePath();
    if (!workingDir.endsWith("/"))
        workingDir += '/';
    QDir d(workingDir + ".config");
    if (d.exists()) workingDir += ".config/";
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    workingDir = env.value("XDG_CONFIG_HOME", workingDir);
    if (!workingDir.endsWith("/"))
        workingDir += '/';
    workingDir += "yagf/";
    QDir dir(workingDir);
    if (!dir.exists())
        dir.mkdir(workingDir);
    settings.readSettings(workingDir);
    settings.writeSettings();
    if (settings.getFullScreen())
        showFullScreen();
    else {
        move(settings.getPosition());
        resize(settings.getSize());
    }
    QFont f(textEdit->font());
    f.setPointSize(settings.getFontSize());
    textEdit->setFont(f);
    actionCheck_spelling->setChecked(settings.getCheckSpelling());
    actionSelect_HTML_format->setChecked(settings.getOutputFormat() != "text");

    QList<int> li;
    li.append(1);
    li.append(1);
    splitter->setSizes(li);
    toolBar->setIconSize(settings.getIconSize());
}

void MainForm::fillLanguagesBox()
{
    selectLangsBox->addItem(trUtf8("Russian"), QVariant("rus"));
    selectLangsBox->addItem(trUtf8("Russian-English"), QVariant("ruseng"));
    selectLangsBox->addItem(trUtf8("Bulgarian"), QVariant("bul"));
    selectLangsBox->addItem(trUtf8("Croatian"), QVariant("hrv"));
    selectLangsBox->addItem(trUtf8("Czech"), QVariant("cze"));
    selectLangsBox->addItem(trUtf8("Danish"), QVariant("dan"));
    selectLangsBox->addItem(trUtf8("Dutch"), QVariant("dut"));
    selectLangsBox->addItem(trUtf8("English"), QVariant("eng"));
    selectLangsBox->addItem(trUtf8("Estonian"), QVariant("est"));
    selectLangsBox->addItem(trUtf8("Finnish (tesseract only)"), QVariant("fin"));
    selectLangsBox->addItem(trUtf8("French"), QVariant("fra"));
    selectLangsBox->addItem(trUtf8("German"), QVariant("ger"));
    selectLangsBox->addItem(trUtf8("German Gothic (tesseract 3+)"), QVariant("gerf"));
    selectLangsBox->addItem(trUtf8("Greek (tesseract only)"), QVariant("ell"));
    selectLangsBox->addItem(trUtf8("Hebrew (tesseract 3.0.1+)"), QVariant("heb"));
    selectLangsBox->addItem(trUtf8("Hungarian"), QVariant("hun"));
    selectLangsBox->addItem(trUtf8("Italian"), QVariant("ita"));
    selectLangsBox->addItem(trUtf8("Latvian"), QVariant("lav"));
    selectLangsBox->addItem(trUtf8("Lithuanian"), QVariant("lit"));
    selectLangsBox->addItem(trUtf8("Norwegian (tesseract only)"), QVariant("nor"));
    selectLangsBox->addItem(trUtf8("Polish"), QVariant("pol"));
    selectLangsBox->addItem(trUtf8("Portuguese"), QVariant("por"));
    selectLangsBox->addItem(trUtf8("Romanian"), QVariant("rum"));
    selectLangsBox->addItem(trUtf8("Spanish"), QVariant("spa"));
    selectLangsBox->addItem(trUtf8("Swedish"), QVariant("swe"));
    selectLangsBox->addItem(trUtf8("Swedish Gothic (tesseract 3+)"), QVariant("swef"));
    selectLangsBox->addItem(trUtf8("Serbian"), QVariant("srp"));
    selectLangsBox->addItem(trUtf8("Slovenian"), QVariant("slo"));
    selectLangsBox->addItem(trUtf8("Slovakian (tesseract only)"), QVariant("slk"));
    selectLangsBox->addItem(trUtf8("Turkish (tesseract only)"), QVariant("tur"));
    selectLangsBox->addItem(trUtf8("Ukrainian"), QVariant("ukr"));
    //selectLangsBox->addItem(trUtf8("Russian-French"), QVariant("rus_fra"));
    //selectLangsBox->addItem(trUtf8("Russian-German"), QVariant("rus_ger"));
    //selectLangsBox->addItem(trUtf8("Russian-Spanish"), QVariant("rus_spa"));
    //selectFormatBox->addItem("TEXT", QVariant("text"));
    //selectFormatBox->addItem("HTML", QVariant("html"));

    tesMap->insert("rus", "rus");
    tesMap->insert("eng", "eng");
    tesMap->insert("ger", "deu");
    tesMap->insert("gerf", "deu-frak");
    tesMap->insert("fra", "fra");
    tesMap->insert("heb", "heb");
    tesMap->insert("swe", "swe");
    tesMap->insert("swef", "swe-frak");
    tesMap->insert("slo", "slv");
    tesMap->insert("spa", "spa");
    tesMap->insert("por", "por");
    tesMap->insert("pol", "pol");
    tesMap->insert("ukr", "ukr");
    tesMap->insert("bul", "bul");
    tesMap->insert("lav", "lav");
    tesMap->insert("lit", "lit");
    tesMap->insert("ita", "ita");
    tesMap->insert("hun", "hun");
    tesMap->insert("rum", "ron");
    tesMap->insert("dan", "dan");
    tesMap->insert("srp", "srp");
    tesMap->insert("dut", "nld");
    tesMap->insert("cze", "ces");
    tesMap->insert("fin", "fin");
    tesMap->insert("nor", "nor");
    tesMap->insert("tur", "tur");
    tesMap->insert("ell", "ell");
    tesMap->insert("slk", "slk");
}

void MainForm::newLanguageSelected(int index)
{
    settings.setLanguage(selectLangsBox->itemData(index).toString());
    actionCheck_spelling->setEnabled(spellChecker->hasDict(settings.getLanguage()));
    if (settings.getCheckSpelling()) {
        spellChecker->setLanguage(settings.getLanguage());
        settings.setCheckSpelling(spellChecker->spellCheck());
        //actionCheck_spelling->setEnabled(checkSpelling);
        actionCheck_spelling->setChecked(settings.getCheckSpelling());
    }

}

void MainForm::scanImage()
{
    scanProcess->terminate();
    scanProcess->waitForFinished(10000);
    if (useXSane) {
        if (!findProgram("xsane")) {
            QMessageBox::warning(this, trUtf8("Warning"), trUtf8("xsane not found"));
            return;
        }
        QStringList sl;
        sl.append("-s");
        sl.append("-n");
        sl.append("-N");
        sl.append(workingDir + "input.jpg");
        QStringList env = QProcess::systemEnvironment();
        QFileInfo lib;
        lib.setFile("/usr/local/lib/yagf/libxspreload.so");
        if (!lib.exists())
            lib.setFile("/usr/lib/yagf/libxspreload.so");
        if (!lib.exists())
            lib.setFile("/usr/lib64/yagf/libxspreload.so");
        if (!lib.exists())
            lib.setFile("/usr/local/lib64/yagf/libxspreload.so");
        if (!lib.exists()) {
            QMessageBox::warning(this, trUtf8("Error"), trUtf8("libxspreload.so not found"));
            return;
        }
        env.append("LD_PRELOAD=" + lib.filePath());
        scanProcess->setEnvironment(env);
        scanProcess->start("xsane", sl);
//      proc.waitForFinished(-1);
    }
}

void MainForm::loadFile(const QString &fn, bool loadIntoView)
{
    QCursor oldCursor = cursor();
    setCursor(Qt::WaitCursor);
    if ((fileName != "") && (sideBar->getFileNames().contains(fileName))) {
        sideBar->select(fileName);
        sideBar->clearBlocks();
        for (int i = 0; i < graphicsInput->blocksCount(); i++)
            sideBar->addBlock(graphicsInput->getBlockRectByIndex(i).toRect());
    }
    qreal xrotation = 0;
    QRect crop1(0, 0, 0, 0);
    double scaleFactor = 0.5;
    if (sideBar->fileLoaded(fn)) {
        sideBar->select(fn);
        xrotation = sideBar->getRotation();
        scaleFactor = sideBar->getScale();
        crop1 = sideBar->getCrop1();
    } else {
        //xrotation = sideBar->getRotation();
        //scaleFactor = sideBar->getScale();
    }

    QImage image;
    if ((imageLoaded = image.load(fn))) {
        //pixmap.detach();
        //sideBar->addFile(fn , &image);
        if (settings.getCropLoaded()) {
            if (crop1.height() == 0) {
                CCBuilder * cb = new CCBuilder(image);
                cb->setGeneralBrightness(360);
                cb->setMaximumColorComponent(100);
                QRect r = cb->crop();
                delete cb;
                image = image.copy(r);
                //sideBar->setCrop1(r);
                crop1 = r;
            } else {
                image = image.copy(crop1);
            }
        }

    } else {
        setCursor(oldCursor);
        QMessageBox::critical(this, trUtf8("Image loading error"), trUtf8("Image %1 could not be loaded").arg(fn));
        return;
    }

    if (!loadIntoView) {
        setCursor(oldCursor);
        return;
    }
    if (imageLoaded) {
        fileName = fn;
        setWindowTitle("YAGF - " + extractFileName(fileName));
        graphicsInput->loadImage(image);
        sideBar->addFile(fn , graphicsInput->getImageBy16());
        sideBar->setCrop1(crop1);
        sideBar->setScale(scaleFactor);
        graphicsInput->setViewScale(0.5, xrotation);
        for (int i = 0; i < sideBar->getBlocksCount(); i++)
            graphicsInput->addBlock(sideBar->getBlockByHalf(i));
        graphicsInput->setViewScale(scaleFactor, 0);
        // ((FileToolBar *) m_toolBar)->setRotation(xrotation);
        //  ((FileToolBar *) m_toolBar)->setScale(graphicsInput->getRealScale());
        if (scaleFactor == 1) {
            if (image.width() > 4000)
                scaleImage(0.25);
            else if (image.width() > 2000)
                scaleImage(0.5);
        }
        graphicsInput->setFocus();
    } else {
       // sideBar->addFile(fn);
    }
    scaleFactor = 1;
    setCursor(oldCursor);
}

void MainForm::delTmpFiles()
{

    QDir dir(workingDir);
    dir.setFilter(QDir::Files | QDir::NoSymLinks);
    for (uint i = 0; i < dir.count(); i++) {
        if (dir[i].endsWith("jpg") || dir[i].endsWith("bmp") || dir[i].endsWith("png") || dir[i].endsWith("txt"))
            dir.remove(dir[i]);
    }
    delTmpDir();
}

void MainForm::loadNext(int number)
{

    QStringList files = sideBar->getFileNames();
    if (files.count() == 0)
        return;
    QString name = fileName;
    if (imageLoaded) {
        if (number > 0) {
            if (files.indexOf(name) < files.count() - 1)
                name = files.at(files.indexOf(name) + 1);
        } else if (number < 0) {
            if (files.indexOf(name) > 0)
                name = files.at(files.indexOf(name) - 1);
        }
        sideBar->select(name);
        //QString path = extractFilePath(name);
        /*QString path = extractFilePath(fileName);
            QString digits = extractDigits(name);
            bool result;
            int d = digits.toInt(&result);
            if (!result) return;
            d +=number;
            if (d < 0) d = 0;
            QString newDigits = QString::number(d);
            while (newDigits.size() < digits.size())
                newDigits = '0' + newDigits;
                name = name.replace(digits, newDigits);*/
        loadFile(name);
    }
}

void MainForm::loadNextPage()
{
    loadNext(1);
}

void MainForm::loadPreviousPage()
{
    loadNext(-1);
}

// TODO: think on blocks/page recognition

bool MainForm::useTesseract(const QString &inputFile)
{
    QProcess proc;
    proc.setWorkingDirectory(workingDir);
    QStringList sl;
    sl.append(inputFile);
    sl.append(outputBase);
    sl.append("-l");
    sl.append(tesMap->value(settings.getLanguage()));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("TESSDATA_PREFIX", settings.getTessdataPath());
    proc.setProcessEnvironment(env);
    proc.start("tesseract", sl);
    proc.waitForFinished(-1);
    if (proc.exitCode()) {
        QByteArray stdout = proc.readAllStandardOutput();
        QByteArray stderr = proc.readAllStandardError();
        QString output = QString(stdout) + QString(stderr);
        QMessageBox::critical(this, trUtf8("Starting tesseract failed"), trUtf8("The system said: ") + (output != "" ? output : trUtf8("program not found")));
        return false;
    }
    return true;
}

bool MainForm::useCuneiform(const QString &inputFile, const QString &outputFile)
{
    QProcess proc;
    proc.setWorkingDirectory(workingDir);
    QStringList sl;
    sl.append("-l");
    sl.append(settings.getLanguage());
    sl.append("-f");
    if (settings.getOutputFormat() == "text")
        sl.append("text");
    else
        sl.append("html");
    sl.append("-o");
    sl.append(workingDir + outputFile);
    sl.append(workingDir + inputFile);
    proc.start("cuneiform", sl);
    proc.waitForFinished(-1);
    if (proc.exitCode()) {
        QByteArray stdout = proc.readAllStandardOutput();
        QByteArray stderr = proc.readAllStandardError();
        QString output = QString(stdout) + QString(stderr);
        QMessageBox::critical(this, trUtf8("Starting cuneiform failed"), trUtf8("The system said: ") + (output != "" ? output : trUtf8("program not found")));
        return false;
    }
    return true;
}

void MainForm::recognizeInternal(const QImage &img)
{
    const QString inputFile = "input.bmp";
    const QString outputFile = "output.txt";

    QFile f(workingDir+inputFile);
    f.remove();
    f.setFileName(workingDir+outputFile);
    f.remove();

    QPixmapCache::clear();
    img.save(workingDir + inputFile, "BMP");
    if (settings.getSelectedEngine() == UseCuneiform) {
        if (!useCuneiform(inputFile, outputFile))
            return;
    }
    if (settings.getSelectedEngine() == UseTesseract) {
        if (!useTesseract(inputFile))
           return;
    }
    QFile textFile(workingDir + outputFile);
    textFile.open(QIODevice::ReadOnly);
    QByteArray text = textFile.readAll();
    textFile.close();
    QString textData;
    QTextCodec *codec = QTextCodec::codecForName("UTF-8");
    textData = codec->toUnicode(text); //QString::fromUtf8(text.data());
    if (settings.getOutputFormat() == "text")
        textData.prepend("<meta content=\"text/html; charset=utf-8\" http-equiv=\"content-type\" />");
    textData.replace("<img src=output_files", "");
    textData.replace(".bmp\">", "\"--");
    textData.replace(".bmp>", "");
//       textData.replace("-</p><p>", "");
//        textData.replace("-<br>", "");
    textEdit->append(textData);
    textEdit->append(QString(" "));
    textSaved = FALSE;
    if (settings.getCheckSpelling()) {
        spellChecker->setLanguage(settings.getLanguage());
        actionCheck_spelling->setChecked(spellChecker->spellCheck());
    }

}

void MainForm::recognize()
{
    QFile::remove(workingDir + "input*.bmp");
    if (!imageLoaded) {
        QMessageBox::critical(this, trUtf8("Error"), trUtf8("No image loaded"));
        return;
    }
    if (settings.getSelectedEngine() == UseCuneiform) {
        if (!findProgram("cuneiform")) {
            if (findProgram("tesseract")) {
                QMessageBox::warning(this, trUtf8("Warning"), trUtf8("cuneiform not found, switching to tesseract"));
                settings.setSelectedEngine(UseTesseract);
            } else {
                QMessageBox::warning(this, trUtf8("Warning"), trUtf8("No recognition engine found.\nPlease install either cuneiform or tesseract"));
                return;
            }
        }
     }
    if (settings.getSelectedEngine() == UseTesseract) {
        if (!findProgram("tesseract")) {
            if (findProgram("cuneiform")) {
                QMessageBox::warning(this, trUtf8("Warning"), trUtf8("tesseract not found, switching to cuneiform"));
                settings.setSelectedEngine(UseCuneiform);
            } else {
                QMessageBox::warning(this, trUtf8("Warning"), trUtf8("No recognition engine found.\nPlease install either cuneiform or tesseract"));
                return;
            }
        }
     }
    if (graphicsInput->blocksCount() > 0) {
        for (int i = graphicsInput->blocksCount(); i >= 0; i--)
            if (!graphicsInput->getBlockByIndex(i).isNull())
                recognizeInternal(graphicsInput->getBlockByIndex(i));
    } else {
        recognizeInternal(graphicsInput->getAdaptedImage());
    }
}

void MainForm::saveText()
{
    if (actionSelect_HTML_format->isChecked())
    settings.setOutputFormat("html");
    else
    settings.setOutputFormat("text");
    QString filter;
    if (settings.getOutputFormat() == "text")
        filter = trUtf8("Text Files (*.txt)");
    else
        filter = trUtf8("HTML Files (*.html)");
    QFileDialog dialog(this,
                       trUtf8("Save Text"), settings.getLastOutputDir(), filter);
    if (settings.getOutputFormat() == "text")
        dialog.setDefaultSuffix("txt");
    else
        dialog.setDefaultSuffix("html");
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    if (dialog.exec()) {
        QStringList fileNames;
        fileNames = dialog.selectedFiles();
        settings.setLastOutputDir(dialog.directory().path());
        QFile textFile(fileNames.at(0));
        textFile.open(QIODevice::ReadWrite | QIODevice::Truncate);
        if (settings.getOutputFormat() == "text")
            textFile.write(textEdit->toPlainText().toUtf8());
        else
            saveHtml(&textFile);
        textFile.close();
        textSaved = TRUE;
    }
}

void MainForm::showAboutDlg()
{
    QPixmap icon;
    icon.load(":/yagf.png");
    QMessageBox aboutBox(QMessageBox::NoIcon, trUtf8("About YAGF"), trUtf8("<p align=\"center\"><b>YAGF - Yet Another Graphical Front-end for cuneiform and tesseract OCR engines</b></p><p align=\"center\">Version %1</p> <p align=\"center\">Ⓒ 2009-2012 Andrei Borovsky</p> This is a free software distributed under GPL v3. Visit <a href=\"http://symmetrica.net/cuneiform-linux/yagf-en.html\">http://symmetrica.net/cuneiform-linux/yagf-en.html</a> for more details.").arg(version), QMessageBox::Ok);
    aboutBox.setIconPixmap(icon);
    QList<QLabel *> labels = aboutBox.findChildren<QLabel *>();
    for (int i = 0; i < labels.count(); i++) {
        QLabel *lab = labels.at(i);
        lab->setTextInteractionFlags(Qt::TextBrowserInteraction);
    }
    aboutBox.setTextFormat(Qt::RichText);
    aboutBox.exec();
}

void MainForm::showHelp()
{
    QDesktopServices::openUrl(QUrl(trUtf8("http://symmetrica.net/cuneiform-linux/yagf-en.html")));
}

void MainForm::copyClipboard()
{
    if (!hasCopy) {
        QClipboard *clipboard = QApplication::clipboard();
        clipboard->setText(textEdit->toPlainText(), QClipboard::Clipboard);
    } else
        textEdit->copy();
}

void MainForm::copyAvailable(bool yes)
{
    hasCopy = yes;
}

void MainForm::textChanged()
{
    textSaved = !(textEdit->toPlainText().count());
}

void MainForm::readyRead(int sig)
{
    QFile f(workingDir + "/input.jpg");
    QString newName = QString(workingDir + "/scan-input-%1.jpg").arg(ifCounter);
    ifCounter++;
    QFileInfo fi(workingDir + "/" + newName);
    if (fi.exists()) {
        QFile f2(workingDir + "/" + newName);
        f2.remove();
    }
    f.rename(newName);
    loadFile(newName);
}

void MainForm::saveHtml(QFile *file)
{
    QString text = textEdit->document()->toHtml().toUtf8();
    QString newDir = extractFilePath(file->fileName()) + extractFileName(file->fileName()) + ".files";
    text.replace("<meta name=\"qrichtext\" content=\"1\" />", "<meta content=\"text/html; charset=utf-8\" http-equiv=\"content-type\" />");
    /*text.replace(workingDir + "output_files",  newDir);
    text.replace("[img src=", "<img src=");
    text.replace(".bmp\"]", ".bmp\">");
    text.replace(".bmp]", ".bmp>");
    QDir dir(workingDir+"output_files");
    dir.rename(workingDir+"output_files", newDir);*/
    file->write(text.toAscii());
}

void MainForm::delTmpDir()
{
    QDir dir;
    dir.setPath(workingDir + "output_files");
    dir.setFilter(QDir::Files | QDir::NoSymLinks);
    for (uint i = 0; i < dir.count(); i++) {
        if (dir[i].endsWith("jpg") || dir[i].endsWith("bmp"))
            dir.remove(dir[i]);
    }
    dir.rmdir(workingDir + "output_files");

}

void MainForm::updateSP()
{
    if (settings.getCheckSpelling())
        spellChecker->checkWord();
}

bool MainForm::eventFilter(QObject *object, QEvent *event)
{
    /*if (object ==  scrollArea->widget()) {
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent * e = (QKeyEvent *) event;
            if (e->modifiers() & Qt::ControlModifier) {
                scrollArea->widget()->setCursor(*resizeCursor);
                if ((e->key() == Qt::Key_Plus)||(e->key() == Qt::Key_Equal))
                    scaleImage(1.05);
                else
                if (e->key() == Qt::Key_Minus)
                    scaleImage(0.95);

            } else {
                scrollArea->widget()->setCursor(QCursor(Qt::ArrowCursor));
            }
        }
        else
        if (event->type() == QEvent::KeyRelease) {
                scrollArea->widget()->setCursor(QCursor(Qt::ArrowCursor));
        }
        else
        if (event->type() == QEvent::Wheel) {
            QWheelEvent * e = (QWheelEvent *) event;
            if (e->modifiers() & Qt::ControlModifier) {
                scrollArea->widget()->setFocus();
                scrollArea->widget()->setCursor(*resizeCursor);
                if (e->delta() > 0)
                    scaleImage(1.05);
                else
                    scaleImage(0.95);
                return true;
            }
        }
    } else */
    if (object == textEdit) {
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent *e = (QKeyEvent *) event;
            if (e->modifiers() & Qt::ControlModifier) {
                if ((e->key() == Qt::Key_Plus) || (e->key() == Qt::Key_Equal)) {
                    enlargeFont();
                    return true;
                } else if (e->key() == Qt::Key_Minus) {
                    decreaseFont();
                    return true;
                }
            }
        } else if (event->type() == QEvent::Wheel) {
            QWheelEvent *e = (QWheelEvent *) event;
            if (e->modifiers() & Qt::ControlModifier) {
                if (e->delta() > 0)
                    enlargeFont();
                else
                    decreaseFont();
                return true;
            }
        }

    }
    return QMainWindow::eventFilter(object, event);
}

void MainForm::enlargeFont()
{
    int fontSize = textEdit->font().pointSize();
    fontSize++;
    QFont f(textEdit->font());
    f.setPointSize(fontSize);
    textEdit->setFont(f);
}

void MainForm::decreaseFont()
{
    int fontSize = textEdit->font().pointSize();
    if (fontSize > 1) fontSize--;
    QFont f(textEdit->font());
    f.setPointSize(fontSize);
    textEdit->setFont(f);
}

void MainForm::setResizingCusor()
{
    //scrollArea->widget()->setCursor(*resizeBlockCursor);
}

void MainForm::setUnresizingCusor()
{
    //scrollArea->widget()->setCursor(QCursor(Qt::ArrowCursor));
}

void MainForm::fileSelected(const QString &path)
{
    if (path == "") {
        graphicsInput->loadImage(QImage(0, 0));
        this->setWindowTitle("YAGF");
        return;
    }
    loadFile(path);
}

void MainForm::recognizeAll()
{
    QStringList files = sideBar->getFileNames();
    if (files.empty())
        recognize();
    else {
        QProgressDialog progress(trUtf8("Recognizing pages..."), trUtf8("Abort"), 0, files.count(), this);
        //progress.setWindowModality(Qt::WindowModal);
        progress.setWindowTitle("YAGF");
        progress.show();
        progress.setValue(0);
        for (int i = 0; i < files.count(); i++) {
            progress.setValue(i);
            if (progress.wasCanceled())
                break;
            //rotation = ((FileToolBar *) m_toolBar)->getRotation(files.at(i));
            loadFile(files.at(i));
            recognize();
        }
    }
}

void MainForm::unalignButtonClicked()
{
    /*if (((QSelectionLabel *) scrollArea->widget())->pixmap()->isNull())
        return;
    int rot = ((FileToolBar *) m_toolBar)->getRotation();
    int rrot = ((rot + 45)/90);
    rrot *=90;
    rotateImage(rrot - rot);
    rotation = rrot;*/
}

void MainForm::hideToolBar()
{
    graphicsInput->setToolBarVisible();
}

void MainForm::on_ActionClearAllBlocks_activated()
{
    graphicsInput->clearBlocks();
}

void MainForm::rightMouseClicked(int x, int y, bool inTheBlock)
{
    m_menu->clear();
    m_menu->addAction(ActionClearAllBlocks);
    if (inTheBlock) {
        m_menu->addAction(ActionDeleteBlock);
        m_menu->addAction(actionRecognize_block);
        m_menu->addAction(actionSave_block);
        m_menu->addAction(actionDeskew_by_Block);
    } else {
        m_menu->addAction(actionSelect_Text_Area);
        m_menu->addAction(actionSelect_multiple_blocks);
    }
    QPoint p = graphicsView->mapToGlobal(QPoint(x, y));
    m_menu->move(p);
    m_menu->show();
}

void MainForm::on_ActionDeleteBlock_activated()
{
    graphicsInput->deleteCurrentBlock();
}

void MainForm::on_actionRecognize_block_activated()
{
    if (graphicsInput->getCurrentBlock().isNull())
        return;
    recognizeInternal(graphicsInput->getCurrentBlock());
}

/*void MainForm::on_actionRecognize_activated()
{

}*/


void MainForm::on_actionCheck_spelling_triggered()
{

}

void MainForm::on_actionSave_current_image_activated()
{
    this->saveImageInternal(graphicsInput->getImage());
}

void MainForm::saveImageInternal(const QPixmap &pix)
{
    QString jpegFilter = QObject::trUtf8("JPEG Files (*.jpg)");
    QString pngFilter = QObject::trUtf8("PNG Files (*.png)");
    QString imageSaveFailed = QObject::trUtf8("Failed to save the image");
    QStringList filters;
    QString format = "JPEG";
    filters << jpegFilter << pngFilter;
    QFileDialog dialog(this,
                       trUtf8("Save Image"), settings.getLastOutputDir());
    dialog.setFilters(filters);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("jpg");
    QCursor oldCursor = cursor();
    if (dialog.exec()) {
        setCursor(Qt::WaitCursor);
        if (dialog.selectedNameFilter() == jpegFilter) {
            format = "JPEG";
            dialog.setDefaultSuffix("jpg");
        } else if (dialog.selectedNameFilter() == pngFilter) {
            format = "PNG";
            dialog.setDefaultSuffix("png");
        }
        QStringList fileNames;
        fileNames = dialog.selectedFiles();
        settings.setLastOutputDir(dialog.directory().path());
        if (!pix.save(fileNames.at(0), format.toAscii().data(), 100))
            QMessageBox::critical(this, trUtf8("Error"), imageSaveFailed);
    }
    setCursor(oldCursor);
}

MainForm::~MainForm()
{
    delete resizeBlockCursor;
    delete resizeCursor;
    delete spellChecker;
    //delete fileChannel;
    delete graphicsInput;
    delete ba;
    delete pdfx;
    delete tesMap;
}

void MainForm::on_actionSave_block_activated()
{
    saveImageInternal(QPixmap::fromImage(graphicsInput->getCurrentBlock()));
}

void MainForm::on_actionCheck_spelling_activated()
{
    settings.setCheckSpelling(actionCheck_spelling->isChecked());
    if (settings.getCheckSpelling()) {
        spellChecker->setLanguage(settings.getLanguage());
        actionCheck_spelling->setChecked(spellChecker->spellCheck());
    } else
        spellChecker->unSpellCheck();
}

/*void MainForm::on_alignButton_clicked()
{
    this->AnalizePage();
}*/

void MainForm::on_actionDeskew_activated()
{
   // AnalizePage();
    {
        QCursor oldCursor = cursor();
        setCursor(Qt::WaitCursor);
        graphicsInput->deskew(graphicsInput->getSmallImage());
        setCursor(oldCursor);
    }
}

void MainForm::on_actionSelect_HTML_format_activated()
{
        if (actionSelect_HTML_format->isChecked())
            settings.setOutputFormat("html");
        else
            settings.setOutputFormat("text");
}

void MainForm::pasteimage()
{
    QClipboard *clipboard = QApplication::clipboard();
    QPixmap pm = clipboard->pixmap();
    if (pm.isNull()) return;
    QCursor oldCursor = cursor();
    setCursor(Qt::WaitCursor);
    QString tmpFile = "input-01.jpg";
    QFileInfo fi(workingDir + tmpFile);
    while (fi.exists()) {
        QString digits = extractDigits(tmpFile);
        bool result;
        int d = digits.toInt(&result);
        if (!result) return;
        d++;
        if (d < 0) d = 0;
        QString newDigits = QString::number(d);
        while (newDigits.size() < digits.size())
            newDigits = '0' + newDigits;
        tmpFile = tmpFile.replace(digits, newDigits);
        fi.setFile(workingDir, tmpFile);
    }
    pm.save(fi.absoluteFilePath(), "JPEG");
    loadFile(fi.absoluteFilePath());
    setCursor(oldCursor);
}

void MainForm::deskewByBlock()
{
    QCursor oldCursor = cursor();
    setCursor(Qt::WaitCursor);
    graphicsInput->update();
    QApplication::processEvents();
    if (!graphicsInput->getCurrentBlock().isNull()) {
        QImage img = graphicsInput->getCurrentBlock();
        graphicsInput->deskew(&img);
    }
    setCursor(oldCursor);
}

void MainForm::selectTextArea()
{
    graphicsInput->blockAllText();
    //graphicsInput->splitPage();
}

void MainForm::showAdvancedSettings()
{
    AdvancedConfigDialog dlg;
    dlg.setCrop1(settings.getCropLoaded());
    if (dlg.exec()) {
        settings.setCropLoaded(dlg.doCrop1());
    }
}

void MainForm::contextMenuRequested(const QPoint &point)
{
    QAction *action;
    QMenu * menu = new QMenu(textEdit);
    QStringList sl = spellChecker->suggestions();
    //if (sl.count() == 0) {

    action = new QAction(trUtf8("Undo\tCtrl+Z"), this);
    action->setShortcut(QKeySequence("Ctrl+Z"));
    connect(action, SIGNAL(triggered()), textEdit, SLOT(undo()));
    menu->addAction(action);
    action = new QAction(trUtf8("Redo\tCtrl+Shift+Z"), this);
    action->setShortcut(QKeySequence("Ctrl+Shift+Z"));
    connect(action, SIGNAL(triggered()), textEdit, SLOT(redo()));
    menu->addAction(action);
    action = new QAction("separator", this);
    action->setText("");
    action->setSeparator(true);
    menu->addAction(action);
    action = new QAction(trUtf8("Select All\tCtrl+A"), this);
    action->setShortcut(QKeySequence("Ctrl+A"));
    connect(action, SIGNAL(triggered()), textEdit, SLOT(selectAll()));
    menu->addAction(action);
    action = new QAction(trUtf8("Cut\tCtrl+X"), this);
    action->setShortcut(QKeySequence("Ctrl+X"));
    connect(action, SIGNAL(triggered()), textEdit, SLOT(cut()));
    menu->addAction(action);
    action = new QAction(trUtf8("Copy\tCtrl+C"), this);
    action->setShortcut(QKeySequence("Ctrl+C"));
    connect(action, SIGNAL(triggered()), textEdit, SLOT(copy()));
    menu->addAction(action);
    action = new QAction(trUtf8("Paste\tCtrl+V"), this);
    action->setShortcut(QKeySequence("Ctrl+V"));
    connect(action, SIGNAL(triggered()), textEdit, SLOT(paste()));
    menu->addAction(action);
    action = new QAction("separator", this);
    action->setText("");
    action->setSeparator(true);
    menu->addAction(action);
    action = new QAction(trUtf8("Larger Font\tCtrl++"), this);
    connect(action, SIGNAL(triggered()), this, SLOT(enlargeFont()));
    menu->addAction(action);
    action = new QAction(trUtf8("Smaller Font\tCtrl+-"), this);
    connect(action, SIGNAL(triggered()), this, SLOT(decreaseFont()));
    menu->addAction(action);

    //}
    if (sl.count() > 0)
        menu->addSeparator();
    foreach(QString str, sl) {
        QAction * action = menu->addAction(str);
        connect(action, SIGNAL(triggered()), this, SLOT(replaceWord()));
    }
    menu->exec(textEdit->mapToGlobal(point));
    delete menu;
}

void MainForm::replaceWord()
{
    QAction * action =  (QAction *) sender();
    QTextCursor cursor = textEdit->textCursor();
    cursor.select(QTextCursor::WordUnderCursor);
    cursor.removeSelectedText();
    cursor.insertText(action->text());
}

void MainForm::selectBlocks()
{
    graphicsInput->splitPage();
}

void MainForm::setSmallIcons()
{
    QSize s = toolBar->iconSize();
    if (s.height() > 32) {
         s.setHeight(32);
         s.setWidth(32);
    }
    else {
        s.setHeight(48);
        s.setWidth(48);
    }
    toolBar->setIconSize(s);
    settings.setIconSize(s);
}
