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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "FileChannel.h"
#include <QString>
#include <QByteArray>
#include <QSocketNotifier>
#include <limits.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string.h>

FileChannel::FileChannel(const QString &name, QObject *parent, bool enableWriteBuffer) : QFile(name, parent), buffered(enableWriteBuffer)
{
    init();
}

FileChannel::FileChannel(const QString &name, bool enableWriteBuffer) : QFile(name), buffered(enableWriteBuffer)
{
    init();
}

FileChannel::FileChannel(QObject *parent, bool enableWriteBuffer) : QFile(parent), buffered(enableWriteBuffer)
{
    init();
}

int _open(const char *path, int flags)
{
    return open(path, flags);
}

void _close(int fd)
{
    close(fd);
}

ssize_t _write(int fd, const void *buf, size_t count)
{
    return write(fd, buf, count);
}

ssize_t _read(int fd, void *buf, size_t count)
{
    return read(fd, buf, count);
}

void FileChannel::init()
{
    fd2Open = false;
    _error = QFile::NoError;
}

bool FileChannel::open(OpenMode mode, bool clearGarbage)
{
    if (isOpen()) {
        qWarning("FileChannel::open: File already open");
        //strcpy(lastErrorString, "File already open");
        _error = QFile::OpenError;
        errno = EBUSY;
        return false;
    }
    if (mode & QIODevice::Unbuffered) {
        mode &= !QIODevice::Unbuffered;
        buffered = false;
    }
    if (!((mode == QIODevice::ReadOnly) | (mode == QIODevice::WriteOnly))) {
        qWarning("FileChannel::open: Open file either for reading or for writing, not both");
        //strcpy(lastErrorString, "Open file either for reading or for writing, not both");
        _error = QFile::OpenError;
        errno = EINVAL;
        return false;
    }
    int fd;
    bool result = false;
    if (mode == QIODevice::ReadOnly) {
        if (exists(fileName()) && clearGarbage)
            remove(fileName());
        if (!create())
            return false;
        buffered = false;
        if (!openRead(&fd))
            return false;
    }
    if (mode == QIODevice::WriteOnly) {
        if (!openWrite(&fd))
            return false;
        if (buffered) {
            bufSize = getBufferSize();
            writeBuffer = new char[bufSize];
            offset = 0;
        }
    }
    result = QFile::open(fd, mode | QIODevice::Unbuffered);
    if (result)
        setNotify(mode);
    return result;
}

void FileChannel::close()
{
    if (isOpen()) {
        delete rwNotifier;
        delete exceptionNotifier;
    }
    flush();
    if (buffered) delete writeBuffer;
    QFile::close();
    _close(fd2);
}

void FileChannel::readActivated()
{
    emit readyRead();
}

void FileChannel::writeActivated()
{
    rwNotifier->setEnabled(false);
    emit bytesWritten(0);
}

void FileChannel::exceptionActivated()
{
    emit exception();
}

int FileChannel::write2Buffer(const char *data, int len)
{
    memcpy(&writeBuffer[offset], data, len);
    offset += len;
    return len;
}

qint64 FileChannel::writeData(const char *data, qint64 len)
{
    if (buffered) {
        if (offset + len <= bufSize) {
            return write2Buffer(data, len);
        } else {
            flush();
            return write2Buffer(data, bufSize - offset < len ? bufSize - offset : len);
        }

    } else return _write(handle(), data, len);
}

bool FileChannel::waitForReadyRead(int msecs)
{
    return waitForFop(msecs, true);
}

bool FileChannel::waitForBytesWritten(int msecs)
{
    return waitForFop(msecs, false);
}

bool FileChannel::waitForFop(int msecs, bool read)
{
    fd_set rset, wset, eset;
    timeval timeout;
    timeval *ptimeout;
    int maxfd;
    FD_ZERO(&rset);
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    if (read)
        FD_SET(handle(), &rset);
    else
        FD_SET(handle(), &wset);
    timeout.tv_sec = 0;
    timeout.tv_usec = msecs;
    ptimeout = msecs >= 0 ? &timeout : (timeval *) 0;
    maxfd = handle() + 1;
    if ((select(maxfd, &rset, &wset, &eset, ptimeout) > 0) && (!FD_ISSET(handle(), &eset)))
        return true;
    else
        return false;

}

bool FileChannel::isSequential() const
{
    return true;
}

bool FileChannel::seek(qint64 pos)
{
    Q_UNUSED(pos);
    return false;
}

qint64 FileChannel::readData(char *data, qint64 maxlen)
{
    return _read(handle(), data, maxlen);
}

qint64 FileChannel::getBufferSize()
{
    return PIPE_BUF;
}

bool FileChannel::lock()
{
    return flock(handle(), LOCK_EX);
}

void FileChannel::unlock()
{
    flock(handle(), LOCK_UN);
}

bool FileChannel::flush()
{
    if (buffered) {
        int res = _write(handle(), writeBuffer, offset);
        if (res < 0) {
            _error = QFile::WriteError;
            return false;
        }
        offset -= res;
        return offset == 0;
    }
    return true;
}

QString FileChannel::errorString()
{
    strcpy(lastErrorString, strerror_r(errno, lastErrorString, 0));
    return QString(trUtf8(lastErrorString));
}

QFile::FileError FileChannel::error() const
{
    return _error;
}

bool FileChannel::create()
{
    if (mkfifo(fileName().toLatin1().data(), 0666)) {
        if (errno != EEXIST) {
            _error = QFile::FatalError;
            qWarning("FileChannel::open: Failed to create FIFO");
            return false;
        }
    }
    return true;
}

bool FileChannel::openRead(int *fd)
{
    (*fd) = _open(fileName().toLatin1().data(), O_RDONLY | O_NONBLOCK);
    if ((*fd) < 0) {
        qWarning("FileChannel::open: Failed to open FIFO for reading");
        _error = QFile::OpenError;
        return false;
    }
    // Dummy file descriptor used to avoid the "selcet on closed FIFO" problem (see http://fixunix.com/unix/350803-use-select-fifo-after-has-closed.html)
    if ((fd2 = _open(fileName().toLatin1().data(), O_WRONLY | O_NONBLOCK)) > 0)
        fd2Open = true;
    return true;
}

bool FileChannel::openWrite(int *fd)
{
    (*fd) = _open(fileName().toLatin1().data(), O_WRONLY | O_NONBLOCK);
    if ((*fd) < 0) {
        qWarning("FileChannel::open: Failed to open FIFO for writing");
        _error = QFile::OpenError;
        return false;
    }
    return true;
}

void FileChannel::setNotify(OpenMode mode)
{
    if (mode == QIODevice::ReadOnly) {
        rwNotifier = new QSocketNotifier(handle(), QSocketNotifier::Read, this);
        connect(rwNotifier,  SIGNAL(activated(int)), this, SLOT(readActivated()));
    } else {
        rwNotifier = new QSocketNotifier(handle(), QSocketNotifier::Write, this);
        connect(rwNotifier,  SIGNAL(activated(int)), this, SLOT(writeActivated()));
    }
    exceptionNotifier = new QSocketNotifier(handle(), QSocketNotifier::Exception, this);
    connect(exceptionNotifier,  SIGNAL(activated(int)), this, SLOT(exceptionActivated()));
}
