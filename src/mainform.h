/*
    YAGF - cuneiform OCR graphical front-end
    Copyright (C) 2009-2010 Andrei Borovsky <anb@symmetrica.net>

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

#include "settings.h"
#include <QMainWindow>
#include <QString>
#include "ui_mainform.h"
#include <QProgressDialog>
#include <QMap>
//#include "ui_popplerdialog.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QCloseEvent;
class QPixmap;
class QProcess;
class QByteArray;
class QFile;
class SpellChecker;
class QCursor;
class QGraphicsInput;
class QMenu;
class PDFExtractor;
class ccbuilder;

typedef QMap<QString, QString> TesMap;

const QString version = "0.9.1";

class MainForm : public QMainWindow, public Ui::MainWindow
{
    Q_OBJECT
public:
    MainForm(QWidget *parent = 0);
    ~MainForm();
signals:
    void windowShown();
private slots:
    //void on_actionRecognize_activated();
    void on_actionSelect_HTML_format_activated();
    void on_actionDeskew_activated();
    //void on_alignButton_clicked();
    void on_actionCheck_spelling_activated();
    void on_actionSave_block_activated();
    void on_actionSave_current_image_activated();
    void on_actionCheck_spelling_triggered();
    void on_actionRecognize_block_activated();
    void on_ActionDeleteBlock_activated();
    void on_ActionClearAllBlocks_activated();
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
    void recognizeAll();
    void saveText();
    void showAboutDlg();
    void showHelp();
    void copyClipboard();
    void copyAvailable(bool yes);
    void textChanged();
    void enlargeFont();
    void decreaseFont();
    void unalignButtonClicked();
    void hideToolBar();
    void importPDF();
    void showConfigDlg();
    void addPDFPage(QString pageName);
    void finishedPDF();
    void pasteimage();
    void deskewByBlock();
    void selectTextArea();
    void selectBlocks();
    void setSmallIcons();
protected:
    bool eventFilter(QObject *object, QEvent *event);
private:
    virtual void closeEvent(QCloseEvent *event);
    void scaleImage(double sf);
    void initSettings();
    void fillLanguagesBox();
    void loadFile(const QString &fn, bool loadIntoView = true);
    //void loadFileWithPixmap(const QString &fn, const QPixmap &pixmap);
    void delTmpFiles();
    void loadNext(int number);
    void saveHtml(QFile *file);
    void delTmpDir();
    void recognizeInternal(const QImage &img);
    bool useCuneiform(const QString &inputFile, const QString &outputFile);
    bool useTesseract(const QString &inputFile);
    void saveImageInternal(const QPixmap &pix);
    void loadFromCommandLine();
    bool imageLoaded;
    bool hasCopy;
    QComboBox *selectLangsBox;
    QGraphicsInput *graphicsInput;
    QString workingDir;
    QString fileName;
    QCursor *resizeCursor;
    QCursor *resizeBlockCursor;
    bool useXSane;
    bool textSaved;
    QProcess *scanProcess;
    QByteArray *ba;
    SpellChecker *spellChecker;
//        int rotation;
    QMenu *m_menu;
    PDFExtractor * pdfx;
    QProgressDialog pdfPD;
    TesMap * tesMap;
    int ifCounter;
    Settings settings;
//  QLabel * displayLabel;
private slots:
    void readyRead(int sig);
    void updateSP();
    void setResizingCusor();
    void setUnresizingCusor();
    void fileSelected(const QString &path);
    void rightMouseClicked(int x, int y, bool inTheBlock);
    void onShowWindow();
    void showAdvancedSettings();
    void contextMenuRequested(const QPoint& point);
    void replaceWord();
 };
