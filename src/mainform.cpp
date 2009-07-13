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
#include <QStringList>
#include <QFile>
#include <QByteArray>
#include <QRect>
#include <QStatusBar>
#include <QMessageBox>
#include <QImage>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include "mainform.h"
#include "QSelectionLabel.h"
#include "utils.h"
#include "FileChannel.h"

const QString version = "0.6";

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
	frame->show();
	selectFormatBox = new QComboBox();
	toolBar->addWidget(label);
	selectLangsBox->setFrame(true);
	toolBar->addWidget(selectLangsBox);
	toolBar->addWidget(label1);
	toolBar->addWidget(selectFormatBox);
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

	fillLanguagesBox();
	initSettings();
	delTmpFiles();

        scanProcess = new QProcess(this);
        fileChannel = new FileChannel("/var/tmp/yagf.fifo");
        fileChannel->open(QIODevice::ReadOnly);
        ba = new QByteArray();
        connect(fileChannel, SIGNAL(readyRead()), this, SLOT(readyRead()));
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
	}
}

void MainForm::rotateCWButtonClicked()
{
	rotateImage(90);
}

void MainForm::rotateCCWButtonClicked()
{
	rotateImage(270);
}
void MainForm::rotate180ButtonClicked()
{
	rotateImage(180);
}

void MainForm::enlargeButtonClicked()
{
	scaleImage(2);
}

void MainForm::decreaseButtonClicked() 
{
	scaleImage(0.5);
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
}

void MainForm::writeSettings()
{
	settings->setValue("mainwindow/size", size());
	settings->setValue("mainwindow/pos", pos());
	settings->setValue("mainwindow/fullScreen", isFullScreen());
	settings->setValue("mainwindow/lastDir", lastDir);
	settings->setValue("mainwindow/lastOutputDir", lastOutputDir);
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
	  selectFormatBox->addItem("TEXT", QVariant("txt"));
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
                lib.setFile("/usr/local/lib/libyagfpreload.so");
                if (!lib.exists())
                    lib.setFile("/usr/lib/libyagfpreload.so");
                if (!lib.exists()) {
                    QMessageBox::warning(this, trUtf8("Error"), trUtf8("libyagfpreload.so not found"));
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
		displayLabel->setPixmap(*pixmap);
		displayLabel->setSelectionMode(true);
		displayLabel->resetSelection();
		scaleFactor = 1;
                if (pixmap->width() > 4000)
                        scaleImage(0.25);
                else
                if (pixmap->width() > 2000)
                        scaleImage(0.5);
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
	textEdit->append(QString::fromUtf8(text.data()));
	textSaved = FALSE;
	//QImage img = pix.toImage();
}

void MainForm::saveText()
{
	QFileDialog dialog(this,
		trUtf8("Save Text"), lastOutputDir, trUtf8("Text files (*.txt *.html)"));
	dialog.setAcceptMode(QFileDialog::AcceptSave);
	if (dialog.exec()) {
		QStringList fileNames;
		fileNames = dialog.selectedFiles();
		lastOutputDir = dialog.directory().path();
		QFile textFile(fileNames.at(0));
		textFile.open(QIODevice::ReadWrite|QIODevice::Truncate);
		textFile.write(textEdit->toPlainText().toUtf8());
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
