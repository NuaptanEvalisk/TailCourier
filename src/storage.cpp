// SPDX-FileCopyrightText: 2026 TailCourier contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "storage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace Storage {
bool validName(const QString &n) {
    return !n.isEmpty() && n != "." && n != ".." && !n.contains('/') && !n.contains('\\') &&
           !n.contains(QChar(0)) && !n.contains('\n') && !n.contains('\r');
}
int directory(const QString &path, QString &error) {
    if (!QDir::isAbsolutePath(path) || path.contains(QChar(0))) {
        error = "The folder must have an absolute path without null characters";
        return -1;
    }
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const auto &part : path.split('/', Qt::SkipEmptyParts)) {
        if (part == "." || part == "..") {
            ::close(fd);
            error = "The folder path cannot contain . or .. components";
            return -1;
        }
        auto b = QFile::encodeName(part);
        int next = ::openat(fd, b.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (::mkdirat(fd, b.constData(), 0700) == 0 || errno == EEXIST)
                next = ::openat(fd, b.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) {
            error = QString::fromLocal8Bit(strerror(errno));
            ::close(fd);
            return -1;
        }
        ::close(fd);
        fd = next;
    }
    return fd;
}
QString publish(const QString &source, const QString &destination, QString &error,
                const std::atomic_bool *cancel) {
    const auto name = QFileInfo(source).fileName();
    if (!validName(name)) {
        error = "Unsafe received filename; file kept in staging";
        return {};
    }
    int dir = directory(destination, error);
    if (dir < 0)
        return {};
    int in = ::open(QFile::encodeName(source).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat st{};
    if (in < 0 || ::fstat(in, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() || st.st_nlink != 1) {
        error = "The staged file must be a regular file owned by the current user, with no other hard links";
        if (in >= 0)
            ::close(in);
        ::close(dir);
        return {};
    }
    QString temporary =
        ".tailcourier-" + QString::number(QRandomGenerator::global()->generate64(), 16) + ".part";
    int out = ::openat(dir, QFile::encodeName(temporary).constData(),
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    bool ok = out >= 0;
    char buf[128 * 1024];
    while (ok) {
        if (cancel && cancel->load()) {
            errno = ECANCELED;
            ok = false;
            break;
        }
        ssize_t n = ::read(in, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        ssize_t pos = 0;
        while (pos < n) {
            auto w = ::write(out, buf + pos, n - pos);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                ok = false;
                break;
            }
            pos += w;
        }
    }
    if (ok)
        ok = ::fsync(out) == 0;
    int saved = errno;
    if (out >= 0 && ::close(out) != 0) {
        saved = errno;
        ok = false;
    }
    ::close(in);
    QString result;
    if (ok) {
        for (int i = 0; i < 10000; ++i) {
            QString candidate =
                i == 0
                    ? name
                    : QFileInfo(name).completeBaseName() + QString(" (%1)").arg(i) +
                          (QFileInfo(name).suffix().isEmpty() ? QString() : "." + QFileInfo(name).suffix());
            if (::linkat(dir, QFile::encodeName(temporary).constData(), dir,
                         QFile::encodeName(candidate).constData(), 0) == 0) {
                result = QDir(destination).filePath(candidate);
                break;
            }
            saved = errno;
            if (errno != EEXIST)
                break;
        }
    }
    if (out >= 0)
        ::unlinkat(dir, QFile::encodeName(temporary).constData(), 0);
    if (::fsync(dir) != 0 && !result.isEmpty()) {
        saved = errno;
        result.clear(); // Keep the staging source if directory durability is uncertain.
    }
    ::close(dir);
    if (result.isEmpty())
        error = QString::fromLocal8Bit(strerror(saved));
    else
        QFile::remove(source);
    return result;
}
} // namespace Storage
