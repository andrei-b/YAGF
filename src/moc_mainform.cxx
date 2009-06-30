/****************************************************************************
** Meta object code from reading C++ file 'mainform.h'
**
** Created
**      by: The Qt Meta Object Compiler version 59 (Qt 4.4.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "mainform.h"
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'mainform.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 59
#error "This file was generated using the moc from 4.4.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
static const uint qt_meta_data_MainForm[] = {

 // content:
       1,       // revision
       0,       // classname
       0,    0, // classinfo
      18,   10, // methods
       0,    0, // properties
       0,    0, // enums/sets

 // slots: signature, parameters, type, tag, flags
      10,    9,    9,    9, 0x08,
      22,    9,    9,    9, 0x08,
      46,    9,    9,    9, 0x08,
      71,    9,    9,    9, 0x08,
      96,    9,    9,    9, 0x08,
     119,    9,    9,    9, 0x08,
     143,    9,    9,    9, 0x08,
     177,  171,    9,    9, 0x08,
     202,    9,    9,    9, 0x08,
     214,    9,    9,    9, 0x08,
     229,    9,    9,    9, 0x08,
     248,    9,    9,    9, 0x08,
     260,    9,    9,    9, 0x08,
     271,    9,    9,    9, 0x08,
     286,    9,    9,    9, 0x08,
     297,    9,    9,    9, 0x08,
     317,  313,    9,    9, 0x08,
     337,    9,    9,    9, 0x08,

       0        // eod
};

static const char qt_meta_stringdata_MainForm[] = {
    "MainForm\0\0loadImage()\0rotateCWButtonClicked()\0"
    "rotateCCWButtonClicked()\0"
    "rotate180ButtonClicked()\0"
    "enlargeButtonClicked()\0decreaseButtonClicked()\0"
    "singleColumnButtonClicked()\0index\0"
    "newLanguageSelected(int)\0scanImage()\0"
    "loadNextPage()\0loadPreviousPage()\0"
    "recognize()\0saveText()\0showAboutDlg()\0"
    "showHelp()\0copyClipboard()\0yes\0"
    "copyAvailable(bool)\0textChanged()\0"
};

const QMetaObject MainForm::staticMetaObject = {
    { &QMainWindow::staticMetaObject, qt_meta_stringdata_MainForm,
      qt_meta_data_MainForm, 0 }
};

const QMetaObject *MainForm::metaObject() const
{
    return &staticMetaObject;
}

void *MainForm::qt_metacast(const char *_clname)
{
    if (!_clname) return 0;
    if (!strcmp(_clname, qt_meta_stringdata_MainForm))
        return static_cast<void*>(const_cast< MainForm*>(this));
    if (!strcmp(_clname, "Ui::MainWindow"))
        return static_cast< Ui::MainWindow*>(const_cast< MainForm*>(this));
    return QMainWindow::qt_metacast(_clname);
}

int MainForm::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: loadImage(); break;
        case 1: rotateCWButtonClicked(); break;
        case 2: rotateCCWButtonClicked(); break;
        case 3: rotate180ButtonClicked(); break;
        case 4: enlargeButtonClicked(); break;
        case 5: decreaseButtonClicked(); break;
        case 6: singleColumnButtonClicked(); break;
        case 7: newLanguageSelected((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 8: scanImage(); break;
        case 9: loadNextPage(); break;
        case 10: loadPreviousPage(); break;
        case 11: recognize(); break;
        case 12: saveText(); break;
        case 13: showAboutDlg(); break;
        case 14: showHelp(); break;
        case 15: copyClipboard(); break;
        case 16: copyAvailable((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 17: textChanged(); break;
        }
        _id -= 18;
    }
    return _id;
}
QT_END_MOC_NAMESPACE
