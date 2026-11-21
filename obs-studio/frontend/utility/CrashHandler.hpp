/******************************************************************************
    Copyright (C) 2025 by Patrick Heyer <PatTheMav@users.noreply.github.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#ifndef __ANDROID__
#include <utility/RemoteTextThread.hpp>
#endif

#include <util/base.h>

#include <QObject>
#include <QThread>
#include <QUuid>

#include <chrono>
#include <filesystem>

namespace OBS {

enum class PlatformType { InvalidPlatform, Windows, macOS, Linux, FreeBSD, Android };

using Clock = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<Clock>;

class CrashHandler : public QObject {
	Q_OBJECT

private:
	QUuid appLaunchUUID_;
	std::filesystem::path crashSentinelFile_;

	bool isSentinelPresent_ = false;
	bool isActiveCrashHandler_ = false;

#ifndef __ANDROID__
	TimePoint lastCrashUploadTime_;
	std::filesystem::path lastCrashLogFile_;
	std::string lastCrashLogFileName_;
	std::string lastCrashLogURL_;
	std::unique_ptr<RemoteTextThread> crashLogUploadThread_;
#endif

public:
	CrashHandler() = default;
	CrashHandler(QUuid appLaunchUUID);
	CrashHandler(const CrashHandler &other) = delete;
	CrashHandler &operator=(const CrashHandler &other) = delete;
	CrashHandler(CrashHandler &&other) noexcept;
	CrashHandler &operator=(CrashHandler &&other) noexcept;

	~CrashHandler();

	bool hasUncleanShutdown();
	std::filesystem::path getCrashLogDirectory() const;

#ifndef __ANDROID__
	bool hasNewCrashLog();
	void uploadLastCrashLog();

	enum class CrashLogUpdateResult { InvalidResult, NotAvailable, NotUpdated, Updated };
#endif

private:
	void checkCrashState();
	void setupSentinel();
	std::filesystem::path findLastCrashLog() const;
	PlatformType getPlatformType() const;

#ifndef __ANDROID__
	CrashLogUpdateResult updateLocalCrashLogState();
	void updateCrashLogFromConfig();
	void saveCrashLogToConfig();
	void uploadCrashLogToServer();
	void handleExistingCrashLogUpload();

private slots:
	void crashLogUploadResultHandler(const std::string &uploadResult, const std::string &error);

signals:
	void crashLogUploadStarted() const;
	void crashLogUploadFailed(const QString &errorMessage) const;
	void crashLogUploadFinished(const QString &crashLogURL) const;
#endif

	// FIXME: Turn into private slot once OBSBasic does not handle application shutdown duties anymore.
public slots:
	void applicationShutdownHandler() noexcept;
};
} // namespace OBS
