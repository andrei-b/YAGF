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

#include <QMainWindow>
#include <QString>
#include "ui_mainform.h" 

class QComboBox;
class QLabel;
class QCloseEvent;
class QPixmap;
class QSettings;
class QProcess;
class FileChannel;
class QByteArray;

class MainForm : public QMainWindow, public Ui::MainWindow
{
    Q_OBJECT
public:
	MainForm(QWidget *parent = 0);
private slots:
	void loadImage();
	void rotateCWButtonClicked();
	void rotateCCWButtonClicked();
	void rotate180ButtonClicked();
	void enlargeButtonClicked();
	void decreaseButtonClicked();
	void singleColumnButtonClicked();
	void newLanguageSelected(int index);
	void scanImage();
	void loadNextPage();
	void loadPreviousPage();
	void recognize();
	void saveText();
	void showAboutDlg();
	void showHelp();
        void copyClipboard();
        void copyAvailable(bool yes);
        void textChanged();
private:
	virtual void closeEvent(QCloseEvent * event);
	void rotateImage(int deg);
	void scaleImage(double sf);
	void initSettings();
	void readSettings();
	void writeSettings();
	void fillLanguagesBox();
	void loadFile(const QString &fn);
	void delTmpFiles();
	void loadNext(int number);
	bool imageLoaded;
        bool hasCopy;
	QComboBox * selectLangsBox;
	QComboBox * selectFormatBox;
	QPixmap * pixmap;
	double scaleFactor;
	bool singleColumn;
	QString language;
	QString outputFormat;
	QString lastDir;
	QString lastOutputDir;
	QString workingDir;
	QString fileName;
	QSettings * settings;
	bool useXSane;
	bool textSaved;
        QProcess * process;
        FileChannel * fileChannel;
        QByteArray * ba;
//	QLabel * displayLabel;
};
