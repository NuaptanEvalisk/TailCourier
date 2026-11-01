// SPDX-FileCopyrightText: 2026 Nuaptan Evalisk
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <QString>
#include <atomic>
namespace Storage {
bool validName(const QString &name);
// Walk every component without following symlinks. Returned fd belongs to caller.
int directory(const QString &path, QString &error);
QString publish(const QString &source, const QString &destination, QString &error,
                const std::atomic_bool *cancel = nullptr);
} // namespace Storage
