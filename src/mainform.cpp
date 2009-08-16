/*
    YAGF - cuneiform OCR graphical front-end 
    Copyright (C) 2009 Andrei Borovsky <anb@symmetrica.net>

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

#include <QComboBox>
#include <QLabel>
#include <QPixmap>
#include <QPixmapCache>
#include <QFileDialog>
#include <QCloseEvent>
#include <QPainter>
#include <QSize>
#include <QStringList>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QFile>
#include <QByteArray>
#include <QRect>
#include <QStatusBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QImage>
#include <QDesktopServices>
#include <QUrl>
#include <QRegExp>
#include <QClipboard>
#include "mainform.h"
#include "QSelectionLabel.h"
#include "utils.h"
#include "FileChannel.h"
#include "spellchecker.h"
#include <QTextCodec>
#include <QCheckBox>
#include <QEvent>
#include <QCursor>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFont>
#include "FileToolBar.h"

const QString version = "0.8.1";

MainForm::MainForm(QWidget *parent):QMainWindow(parent)
{
	setupUi(this);
	setWindowTitle("YAGF");
	selectLangsBox = new QComboBox();
	QLabel * label = new QLabel();
	label->setMargin(4);
	label->setText(trUtf8("Recognition language"));
        QLabel * label1 = new QLabel();
	label1->setMargin(4);
	label1->setText(trUtf8("Output format"));
        spellCheckBox = new QCheckBox(trUtf8("Check spelling"), 0);
	frame->show();
	selectFormatBox = new QComboBox();
	toolBar->addWidget(label);
	selectLangsBox->setFrame(true);
	toolBar->addWidget(selectLangsBox);
	toolBar->addWidget(label1);
	toolBar->addWidget(selectFormatBox);
        toolBar->addWidget(spellCheckBox);
	pixmap = new QPixmap();
	QSelectionLabel * displayLabel = new QSelectionLabel();
	scrollArea->setWidget(displayLabel);
	scrollArea->ensureVisible(0, 0);

	statusBar()->show();
	imageLoaded = false;
	lastDir = QDir::homePath();
	lastOutputDir = QDir::homePath();
	useXSane = TRUE;
	textSaved = TRUE;
        hasCopy = false;
        scaleFactor = 1;
        rotation = 0;

        connect(actionOpen, SIGNAL(triggered()), this, SLOT(loadImage()));
	connect(actionQuit, SIGNAL(triggered()), this, SLOT(close()));
	connect(actionScan, SIGNAL(triggered()), this, SLOT(scanImage()));
	connect(actionPreviousPage, SIGNAL(triggered()), this, SLOT(loadPreviousPage()));
	connect(actionNextPage, SIGNAL(triggered()), this, SLOT(loadNextPage()));
	connect(actionRecognize, SIGNAL(triggered()), this, SLOT(recognize()));
	connect(action_Save, SIGNAL(triggered()), this, SLOT(saveText()));
	connect(actionAbout, SIGNAL(triggered()), this, SLOT(showAboutDlg()));
	connect(actionOnlineHelp, SIGNAL(triggered()), this, SLOT(showHelp()));
        connect(actionCopyToClipboard, SIGNAL(triggered()), this, SLOT(copyClipboard()));
	connect(rotateCWButton, SIGNAL(clicked()), this, SLOT(rotateCWButtonClicked()));
	connect(rotateCCWButton, SIGNAL(clicked()), this, SLOT(rotateCCWButtonClicked()));
	connect(rotate180Button, SIGNAL(clicked()), this, SLOT(rotate180ButtonClicked()));
	connect(enlargeButton, SIGNAL(clicked()), this, SLOT(enlargeButtonClicked()));
	connect(decreaseButton, SIGNAL(clicked()), this, SLOT(decreaseButtonClicked()));
	connect(singleColumnButton, SIGNAL(clicked()), this, SLOT(singleColumnButtonClicked()));
	connect(selectLangsBox, SIGNAL(currentIndexChanged(int)), this, SLOT(newLanguageSelected(int)));
        connect(textEdit, SIGNAL(copyAvailable (bool)), this, SLOT(copyAvailable (bool)));
        connect(textEdit, SIGNAL(textChanged()), this, SLOT(textChanged()));
        connect (displayLabel, SIGNAL(selectionResized()), this, SLOT(setResizingCusor()));
        connect (displayLabel, SIGNAL(selectionUnresized()), this, SLOT(setUnresizingCusor()));
        QAction * action;
        action = new QAction(trUtf8("Undo\tCtrl+Z"), this);
        action->setShortcut(QKeySequence("Ctrl+Z"));
        connect(action, SIGNAL(triggered()), textEdit, SLOT(undo()));
        textEdit->addAction(action);
        action = new QAction(trUtf8("Redo\tCtrl+Shift+Z"), this);
        action->setShortcut(QKeySequence("Ctrl+Shift+Z"));
        connect(action, SIGNAL(triggered()), textEdit, SLOT(redo()));
        textEdit->addAction(action);
        action = new QAction("separator", this);
        action->setSeparator(true);
        textEdit->addAction(action);
        action = new QAction(trUtf8("Select All\tCtrl+A"), this);
        action->setShortcut(QKeySequence("Ctrl+A"));
        connect(action, SIGNAL(triggered()), textEdit, SLOT(selectAll()));
        textEdit->addAction(action);
        action = new QAction(trUtf8("Cut\tCtrl+X"), this);
        action->setShortcut(QKeySequence("Ctrl+X"));
        connect(action, SIGNAL(triggered()), textEdit, SLOT(cut()));
        textEdit->addAction(action);
        action = new QAction(trUtf8("Copy\tCtrl+C"), this);
        action->setShortcut(QKeySequence("Ctrl+C"));
        connect(action, SIGNAL(triggered()), textEdit, SLOT(copy()));
        textEdit->addAction(action);
        action = new QAction(trUtf8("Paste\tCtrl+V"), this);
        action->setShortcut(QKeySequence("Ctrl+V"));
        connect(action, SIGNAL(triggered()), textEdit, SLOT(paste()));
        textEdit->addAction(action);
        action = new QAction("separator", this);
        action->setSeparator(true);
        textEdit->addAction(action);
        action = new QAction(trUtf8("Larger Font\tCtrl++"), this);
        connect(action, SIGNAL(triggered()), this, SLOT(enlargeFont()));
        textEdit->addAction(action);
        action = new QAction(trUtf8("Smaller Font\tCtrl+-"), this);
        connect(action, SIGNAL(triggered()), this, SLOT(decreaseFont()));
        textEdit->addAction(action);


	fillLanguagesBox();
	initSettings();
	delTmpFiles();

        scanProcess = new QProcess(this);
        fileChannel = new FileChannel("/var/tmp/yagf.fifo");
        fileChannel->open(QIODevice::ReadOnly);
        ba = new QByteArray();
        connect(fileChannel, SIGNAL(readyRead()), this, SLOT(readyRead()));

        spellChecker = new SpellChecker(textEdit);

        connect(textEdit->document(), SIGNAL(cursorPositionChanged ( const QTextCursor &)), this, SLOT(updateSP()));

        displayLabel->installEventFilter(this);
        textEdit->installEventFilter(this);
        QPixmap l_cursor;
        l_cursor.load(":/resize.png");
        resizeCursor = new QCursor(l_cursor);
        l_cursor.load(":/resize_block.png");
        resizeBlockCursor = new QCursor(l_cursor);
        textEdit->setContextMenuPolicy(Qt::ActionsContextMenu);
        m_toolBar = new FileToolBar(this);
        addToolBar(Qt::LeftToolBarArea, m_toolBar);
        m_toolBar->show();
        connect(m_toolBar, SIGNAL(fileSelected(const QString &)), this, SLOT(fileSelected(const QString &)));

        connect(actionRecognize_All_Pages, SIGNAL(triggered()), this, SLOT(recognizeAll()));
 }

void MainForm::loadImage()
{
	QFileDialog dialog(this,
     trUtf8("Open Image"), lastDir, trUtf8("Image Files (*.png *.jpg *.jpeg *.bmp *.tiff *.tif *.gif *.pnm *.pgm *.pbm *.ppm)"));
	if (dialog.exec()) {
		QStringList fileNames;
		fileNames = dialog.selectedFiles();
		lastDir = dialog.directory().path();
		loadFile(fileNames.at(0));
	}
}

void MainForm::singleColumnButtonClicked()
{
	singleColumn = singleColumnButton->isChecked();
}

void MainForm::closeEvent(QCloseEvent *event)
{
	if (!textSaved) {
                QPixmap icon;
                icon.load(":/info.png");

                QMessageBox messageBox(QMessageBox::NoIcon, "YAGF", trUtf8("There is an unsaved text in the editor window. Do you want to save it?"),
			QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel, this);
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
        writeSettings();
	delTmpFiles();
	event->accept();
}

void MainForm::rotateImage(int deg)
{
        rotation %=360;
        if (imageLoaded) {
                QMatrix matrix;
                matrix.rotate(deg);
		((QSelectionLabel*)(scrollArea->widget()))->resetSelection(true);
		QPixmap pix = ((QLabel*)(scrollArea->widget()))->pixmap()->transformed(matrix);
		//pix.transformed(matrix);
		((QLabel*)(scrollArea->widget()))->setPixmap(pix);
		pix = pixmap->transformed(matrix);
		delete pixmap;
		pixmap = new QPixmap(pix);
                rotation += deg;
                ((FileToolBar *) m_toolBar)->setRotation(rotation);
	}
}

void MainForm::rotateCWButtonClicked()
{
        rotateImage(90);
}

void MainForm::rotateCCWButtonClicked()
{
        rotateImage(-90);
}
void MainForm::rotate180ButtonClicked()
{
        rotateImage(180);
}

void MainForm::enlargeButtonClicked()
{
        if (scaleFactor < 0.25 )
            scaleImage(0.25/scaleFactor);
        else
        if (scaleFactor < 0.33 )
            scaleImage(0.33/scaleFactor);
        else
        if (scaleFactor < 0.5 )
            scaleImage(0.5/scaleFactor);
        else
        if (scaleFactor < 0.75 )
            scaleImage(0.75/scaleFactor);
        else
        if (scaleFactor < 1)
            scaleImage(1/scaleFactor);
        else
            scaleImage(2);
}

void MainForm::decreaseButtonClicked() 
{
        if (scaleFactor > 1 )
            scaleImage(0.5);
        else
        if (scaleFactor > 0.75 )
            scaleImage(0.75/scaleFactor);
        else
        if (scaleFactor > 0.5 )
            scaleImage(0.5/scaleFactor);
        else
        if (scaleFactor > 0.33 )
            scaleImage(0.33/scaleFactor);
        else
        if (scaleFactor > 0.25 )
            scaleImage(0.25/scaleFactor);
        else
            scaleImage(0.2/scaleFactor);
}

void MainForm::scaleImage(double sf) 
{
	if (!imageLoaded)
		return;
	if ((scaleFactor < 0.2) && (sf < 1))
		return;
	if ((scaleFactor > 4) && (sf > 1))
		return;
	scaleFactor *= sf;
	QPixmap pix = pixmap->scaled(QSize(pixmap->width()*scaleFactor, pixmap->height()*scaleFactor));
	((QLabel*)(scrollArea->widget()))->setPixmap(pix);
        ((QSelectionLabel*)(scrollArea->widget()))->resetSelection();
}

void MainForm::initSettings()
{
	workingDir = QDir::homePath();
	if (!workingDir.endsWith("/"))
		workingDir += '/';
	workingDir += ".yagf/";
	QDir dir(workingDir);
	if (!dir.exists())
		dir.mkdir(workingDir);
	QString iniFile = workingDir + "yagf.ini";
	settings = new QSettings(iniFile, QSettings::IniFormat);
	QFileInfo iniFileInfo(iniFile);
	if (iniFileInfo.exists())
		readSettings();
	else {
		writeSettings();
	}
	QList<int> li;
	li.append(1);
	li.append(1);
	splitter->setSizes(li);
}

void MainForm::readSettings()
{
	resize(settings->value("mainwindow/size", QSize(400, 400)).toSize());
	move(settings->value("mainwindow/pos", QPoint(200, 200)).toPoint());
	if (settings->value("mainwindow/fullScreen").toBool())
	      showFullScreen();
        singleColumn = settings->value("ocr/singleColumn", bool(false)).toBool();
        singleColumnButton->setChecked(singleColumn);
	lastDir = settings->value("mainwindow/lastDir").toString();
	lastOutputDir = settings->value("mainwindow/lastOutputDir", lastOutputDir).toString();
	language = settings->value("ocr/language", QString("rus")).toString();
	selectLangsBox->setCurrentIndex(selectLangsBox->findData(QVariant(language)));
        outputFormat = settings->value("ocr/outputFormat", QString("text")).toString();
        if (outputFormat == "") outputFormat = "text";
        selectFormatBox->setCurrentIndex(selectFormatBox->findData(QVariant(outputFormat)));
        spellCheckBox->setChecked(settings->value("mainWindow/checkSpelling", bool(true)).toBool());
        bool ok;
        QFont f(textEdit->font());
        f.setPointSize(settings->value("mainWindow/fontSize", int(12)).toInt(&ok));
        textEdit->setFont(f);
}

void MainForm::writeSettings()
{
	settings->setValue("mainwindow/size", size());
	settings->setValue("mainwindow/pos", pos());
	settings->setValue("mainwindow/fullScreen", isFullScreen());
	settings->setValue("mainwindow/lastDir", lastDir);
        settings->setValue("mainWindow/checkSpelling", spellCheckBox->isChecked());
	settings->setValue("mainwindow/lastOutputDir", lastOutputDir);
        settings->setValue("mainWindow/fontSize", textEdit->font().pointSize());
	settings->setValue("ocr/language", language);
	settings->setValue("ocr/singleColumn", singleColumn);
	settings->setValue("ocr/outputFormat", outputFormat);
	settings->sync();
}


void MainForm::fillLanguagesBox()
{
	  language = "rus";
	  selectLangsBox->addItem(trUtf8("Russian"), QVariant("rus"));
          selectLangsBox->addItem(trUtf8("Russian-English"), QVariant("ruseng"));
          selectLangsBox->addItem(trUtf8("Czech"), QVariant("cze"));
          selectLangsBox->addItem(trUtf8("Danish"), QVariant("dan"));
          selectLangsBox->addItem(trUtf8("Dutch"), QVariant("dut"));
          selectLangsBox->addItem(trUtf8("English"), QVariant("eng"));
          selectLangsBox->addItem(trUtf8("Estonian"), QVariant("est"));
          selectLangsBox->addItem(trUtf8("French"), QVariant("fra"));
	  selectLangsBox->addItem(trUtf8("German"), QVariant("ger"));
          selectLangsBox->addItem(trUtf8("Hungarian"), QVariant("hun"));
          selectLangsBox->addItem(trUtf8("Italian"), QVariant("ita"));
          selectLangsBox->addItem(trUtf8("Latvian"), QVariant("lav"));
          selectLangsBox->addItem(trUtf8("Lithuanian"), QVariant("lit"));
          selectLangsBox->addItem(trUtf8("Polish"), QVariant("pol"));
          selectLangsBox->addItem(trUtf8("Portugueze"), QVariant("por"));
          selectLangsBox->addItem(trUtf8("Roman"), QVariant("rum"));
          selectLangsBox->addItem(trUtf8("Spanish"), QVariant("spa"));
          selectLangsBox->addItem(trUtf8("Swedish"), QVariant("swe"));
          selectLangsBox->addItem(trUtf8("Ukrainian"), QVariant("ukr"));
	  selectLangsBox->addItem(trUtf8("Russian-French"), QVariant("rus_fra"));
	  selectLangsBox->addItem(trUtf8("Russian-German"), QVariant("rus_ger"));
	  selectLangsBox->addItem(trUtf8("Russian-Spanish"), QVariant("rus_spa"));
          selectFormatBox->addItem("TEXT", QVariant("text"));
	  selectFormatBox->addItem("HTML", QVariant("html"));
}

void MainForm::newLanguageSelected(int index)
{
	language = selectLangsBox->itemData(index).toString();
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
                sl.append ("-N");
                sl.append (workingDir + "input.jpg");
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
//		proc.waitForFinished(-1);
	}
}

void MainForm::loadFile(const QString &fn)
{
	imageLoaded = pixmap->load(fn);
	fileName = fn;
	setWindowTitle("YAGF - " + extractFileName(fileName));
	QSelectionLabel * displayLabel = (QSelectionLabel *) scrollArea->widget();
	displayLabel->setPixmap(QPixmap());
	displayLabel->setSelectionMode(false);
	if (imageLoaded) {
                ((FileToolBar *) m_toolBar)->addFile(*pixmap, fn);
                displayLabel->setPixmap(*pixmap);
		displayLabel->setSelectionMode(true);
		displayLabel->resetSelection();
                if (scaleFactor == 1) {
                    scaleFactor = 1;
                    if (pixmap->width() > 4000)
                            scaleImage(0.25);
                    else
                    if (pixmap->width() > 2000)
                            scaleImage(0.5);
                } else {
                    double tmp = scaleFactor;
                    scaleFactor = 1;
                    scaleImage(tmp);
                }
                int deg = rotation;
                rotation = 0;
                rotateImage(deg);
                ((FileToolBar *) m_toolBar)->setRotation(rotation);
                displayLabel->setFocus();
	}
}

void MainForm::delTmpFiles()
{
	
	QDir dir(workingDir);
	dir.setFilter(QDir::Files | QDir::NoSymLinks);
	for (uint i = 0; i < dir.count(); i++) {
		if (dir[i].endsWith("jpg") || dir[i].endsWith("bmp"))
			dir.remove(dir[i]);
	}
        delTmpDir();
}

void MainForm::loadNext(int number)
{
	QString name = extractFileName(fileName);
	QString path = extractFilePath(fileName);
	QString digits = extractDigits(name);
	bool result; 
	int d = digits.toInt(&result);
	if (!result) return;
	d +=number;
	if (d < 0) d = 0;
	QString newDigits = QString::number(d);
	while (newDigits.size() < digits.size())
	    newDigits = '0' + newDigits;
	name = name.replace(digits, newDigits);
	loadFile(path + name);
}

void MainForm::loadNextPage()
{
	loadNext(1);
}

void MainForm::loadPreviousPage()
{
	loadNext(-1);
}

void MainForm::recognize()
{
        const QString inputFile = "input.bmp";
	const QString outputFile = "output.txt";
        outputFormat = selectFormatBox->itemData(selectFormatBox->currentIndex()).toString();
	if (!imageLoaded) {
		QMessageBox::critical(this, trUtf8("Error"), trUtf8("No image loaded"));
		return;
	}
	if (!findProgram("cuneiform")) {
		QMessageBox::warning(this, trUtf8("Warning"), trUtf8("cuneiform not found"));
		return;
	}
	QRect rect = ((QSelectionLabel *) scrollArea->widget())->getSelectedRect();
	QPixmap pix = pixmap->copy(rect.x()/scaleFactor, rect.y()/scaleFactor, rect.width()/scaleFactor, rect.height()/scaleFactor);
	QPixmapCache::clear();
	pix.save(workingDir + inputFile, "BMP");
	QProcess proc;
	proc.setWorkingDirectory(workingDir);
	QStringList sl;	
	sl.append("-l");
	sl.append(language);
        sl.append("-f");
        if (outputFormat == "text")
          sl.append("text");
        else
        sl.append("html");
	if (singleColumn)
		sl.append("-c1");
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
		return;
	}
	QFile textFile(workingDir + outputFile);
	textFile.open(QIODevice::ReadOnly);
	QByteArray text = textFile.readAll();
        textFile.close();
        QString textData;
        QTextCodec *codec = QTextCodec::codecForName("UTF-8");
        textData = codec->toUnicode(text); //QString::fromUtf8(text.data());
        if (outputFormat == "text")
                textData.prepend("<meta content=\"text/html; charset=utf-8\" http-equiv=\"content-type\" />");
        textData.replace("<img src=output_files", "");
        textData.replace(".bmp\">", "\"--");
        textData.replace(".bmp>", "");
 //       textData.replace("-</p><p>", "");
//        textData.replace("-<br>", "");
        textEdit->append(textData);
        textSaved = FALSE;
        if (spellCheckBox->isChecked()) {
            spellChecker->setLanguage(language);
            spellChecker->spellCheck();
        }
	//QImage img = pix.toImage();
}

void MainForm::saveText()
{
        outputFormat = selectFormatBox->itemData(selectFormatBox->currentIndex()).toString();
        QString filter;
        if (outputFormat == "text")
            filter = trUtf8("Text Files (*.txt)");
        else
            filter = trUtf8("HTML Files (*.html)");
        QFileDialog dialog(this,
                trUtf8("Save Text"), lastOutputDir, filter);
        if (outputFormat == "text")
            dialog.setDefaultSuffix("txt");
        else
            dialog.setDefaultSuffix("html");
	dialog.setAcceptMode(QFileDialog::AcceptSave);
	if (dialog.exec()) {
		QStringList fileNames;
		fileNames = dialog.selectedFiles();
		lastOutputDir = dialog.directory().path();
		QFile textFile(fileNames.at(0));
		textFile.open(QIODevice::ReadWrite|QIODevice::Truncate);
                if (outputFormat == "text")
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
        QMessageBox aboutBox(QMessageBox::NoIcon, trUtf8("About YAGF"), trUtf8("<p align=\"center\"><b>YAGF - Yet Another Graphical Front-end for cuneiform</b></p> <p align=\"center\">Version %1</p> This is a free software. Visit <a href=\"http://symmetrica.net/cuneiform-linux/yagf-en.html\">http://symmetrica.net/cuneiform-linux/yagf-en.html</a> for more details.").arg(version), QMessageBox::Ok);
	aboutBox.setIconPixmap(icon);
        QList<QLabel *> labels = aboutBox.findChildren<QLabel*>();
        for (int i = 0; i < labels.count(); i++) {
            QLabel * lab = labels.at(i);
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
    if(!hasCopy) {
        QClipboard *clipboard = QApplication::clipboard();
        clipboard->setText(textEdit->toPlainText(), QClipboard::Clipboard);
    }
    else
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

void MainForm::readyRead() {
   char * endMarker = "PIPETZ";
   QByteArray tmp = fileChannel->read(0xFFFFFF);
   while (tmp.count() != 0) {
       ba->append(tmp);
        if(tmp.contains(endMarker)) {
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
         QFile f(fi.absoluteFilePath());
         f.open(QIODevice::WriteOnly);
         f.write(*ba);
         f.close();
         ba->clear();
         tmp = fileChannel->read(0xFFFFFF);
         loadFile(fi.absoluteFilePath());
         break;
        }
        tmp = fileChannel->read(0xFFFFFF);
    }
   tmp = fileChannel->read(0xFFFFFF);
   while (tmp.count() != 0)
   tmp = fileChannel->read(0xFFFFFF);
}

void MainForm::saveHtml(QFile * file) {
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
    if (spellCheckBox->isChecked())
        spellChecker->checkWord();
}

bool MainForm::eventFilter(QObject *object, QEvent *event)
{
    if (object ==  scrollArea->widget()) {
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
    } else
    if (object == textEdit) {
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent * e = (QKeyEvent *) event;
            if (e->modifiers() & Qt::ControlModifier) {
                if ((e->key() == Qt::Key_Plus)||(e->key() == Qt::Key_Equal)) {
                    enlargeFont();
                    return true;
                }
                else
                if (e->key() == Qt::Key_Minus) {
                    decreaseFont();
                    return true;
                }
            }
        } else
        if (event->type() == QEvent::Wheel) {
            QWheelEvent * e = (QWheelEvent *) event;
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
        scrollArea->widget()->setCursor(*resizeBlockCursor);
}

void MainForm::setUnresizingCusor()
{
    scrollArea->widget()->setCursor(QCursor(Qt::ArrowCursor));
}

void MainForm::fileSelected(const QString &path)
{
    rotation = ((FileToolBar *) m_toolBar)->getRotation();
    loadFile(path);
}

void MainForm::recognizeAll()
{
    QStringList files = ((FileToolBar *)m_toolBar)->getFileNames();
    if (files.empty())
        recognize();
    else {
            QProgressDialog progress(trUtf8("Recognizing pages..."), trUtf8("Abort"), 0, files.count(), this);
            //progress.setWindowModality(Qt::WindowModal);
            progress.setWindowTitle("YAGF");
            progress.show();
            progress.setValue(0);
            for (int i=0; i < files.count(); i++) {
                progress.setValue(i);
                if(progress.wasCanceled())
                    break;
                rotation = ((FileToolBar *) m_toolBar)->getRotation(files.at(i));
                loadFile(files.at(i));
                recognize();
            }
    }
}
