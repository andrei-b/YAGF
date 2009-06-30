#ifndef FILECHANNEL_H
#define SFILECHANNEL_H

#include <QFile>

class QString;
class QSocketNotifier;

class FileChannel : public QFile
{
Q_OBJECT
public:
        FileChannel(const QString & name, bool enableWriteBuffer = false);
        FileChannel(QObject * parent, bool enableWriteBuffer = false);
        FileChannel(const QString & name, QObject * parent, bool enableWriteBuffer = false);
        virtual bool open(OpenMode mode, bool clearGarbage = true);
	virtual void close();
	bool waitForReadyRead(int msecs);
	bool waitForBytesWritten(int msecs);
        bool isSequential() const;
	bool seek(qint64 pos);
        static qint64 getBufferSize();
        bool lock();
        void unlock();
        bool flush();
        QString errorString();
        QFile::FileError error() const;
signals:
	void exception();

protected:
	qint64 writeData(const char *data, qint64 len);
        qint64 readData(char *data, qint64 maxlen);
private:
        int write2Buffer(const char * data, int len);
        bool create();
        bool openRead(int * fd);
        bool openWrite(int * fd);
        void setNotify(OpenMode mode);
        int fd2;
        char * writeBuffer;
        int bufSize;
        int offset;
        bool fd2Open;
        bool buffered;
	void init();
	bool waitForFop(int msecs, bool read);
	QSocketNotifier * rwNotifier;
	QSocketNotifier * exceptionNotifier;
        char lastErrorString[256];
        QFile::FileError _error;
private slots:
	void readActivated();
	void writeActivated();
	void exceptionActivated();
};

#endif
