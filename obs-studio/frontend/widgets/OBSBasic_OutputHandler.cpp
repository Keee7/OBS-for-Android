/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

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

#include "OBSBasic.hpp"

#include <qt-wrappers.hpp>

#include <QDir>
#ifdef __ANDROID__
#include "OBSAndroidPath.hpp"
#include <utility/AndroidCodecCapabilities.hpp>
#endif

#ifdef __ANDROID__
extern const char *get_simple_output_encoder(const char *encoder);

static const char *androidFallbackStatusKey(OBSAndroidSoftwareFallbackReason reason)
{
	return reason == OBSAndroidSoftwareFallbackReason::FullRangeHardwareUnavailable
		       ? "Output.AndroidFullRangeSoftwareFallback.Status"
		       : "Output.AndroidHevcSoftwareFallback.Status";
}

void OBSBasic::NotifyAndroidSoftwareEncoderFallback(bool forStreaming, bool forRecording, bool configOnly)
{
	if (!outputHandler)
		return;
	auto reason = OBSAndroidSoftwareFallbackReason::None;
	if (!configOnly) {
		if (forStreaming && outputHandler->androidStreamingSoftwareFallback)
			reason = outputHandler->androidStreamingFallbackReason;
		else if (forRecording && outputHandler->androidRecordingSoftwareFallback)
			reason = outputHandler->androidRecordingFallbackReason;
	} else {
		const auto space = OBSAndroidVideoColorSpace();
		const auto range = OBSAndroidVideoColorRange();
		const bool advanced = strcmp(config_get_string(activeConfiguration, "Output", "Mode"), "Advanced") == 0;
		if (advanced) {
			const char *streamEnc = config_get_string(activeConfiguration, "AdvOut", "Encoder");
			const char *recEnc = config_get_string(activeConfiguration, "AdvOut", "RecEncoder");
			const bool useStream = strcmp(recEnc, "none") == 0;
			const bool custom = strcmp(config_get_string(activeConfiguration, "AdvOut", "RecType"), "FFmpeg") == 0;
			if (forStreaming)
				reason = OBSAndroidSoftwareFallbackForVideo(streamEnc, space, range, false);
			if (reason == OBSAndroidSoftwareFallbackReason::None && forRecording && !custom)
				reason = OBSAndroidSoftwareFallbackForVideo(useStream ? streamEnc : recEnc, space, range,
									    false);
		} else {
			const char *streamEnc =
				get_simple_output_encoder(config_get_string(activeConfiguration, "SimpleOutput", "StreamEncoder"));
			const char *recQuality = config_get_string(activeConfiguration, "SimpleOutput", "RecQuality");
			const bool recUsesStream = strcmp(recQuality, "Stream") == 0;
			const bool lossless = strcmp(recQuality, "Lossless") == 0;
			const char *recEnc =
				get_simple_output_encoder(config_get_string(activeConfiguration, "SimpleOutput", "RecEncoder"));
			if (forStreaming)
				reason = OBSAndroidSoftwareFallbackForVideo(streamEnc, space, range, true);
			if (reason == OBSAndroidSoftwareFallbackReason::None && forRecording && !lossless)
				reason = OBSAndroidSoftwareFallbackForVideo(recUsesStream ? streamEnc : recEnc, space,
									    range, true);
		}
	}
	if (reason == OBSAndroidSoftwareFallbackReason::None)
		return;
	ShowStatusBarMessage(QTStr(androidFallbackStatusKey(reason)));
	if (outputHandler->androidSoftwareFallbackNotified)
		return;
	outputHandler->androidSoftwareFallbackNotified = true;
	const bool fullRange = reason == OBSAndroidSoftwareFallbackReason::FullRangeHardwareUnavailable;
	OBSMessageBox::warning(this,
			       QTStr(fullRange ? "Output.AndroidFullRangeSoftwareFallback.Title"
					       : "Output.AndroidHevcSoftwareFallback.Title"),
			       QTStr(fullRange ? "Output.AndroidFullRangeSoftwareFallback.Text"
					       : "Output.AndroidHevcSoftwareFallback.Text"));
}
#endif

void OBSBasic::ResetOutputs()
{
	ProfileScope("OBSBasic::ResetOutputs");

	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	bool advOut = astrcmpi(mode, "Advanced") == 0;

	if ((!outputHandler || !outputHandler->Active()) &&
	    (!setupStreamingGuard.valid() ||
	     setupStreamingGuard.wait_for(std::chrono::seconds{0}) == std::future_status::ready)) {
		outputHandler.reset();
		outputHandler.reset(advOut ? CreateAdvancedOutputHandler(this) : CreateSimpleOutputHandler(this));

		emit ReplayBufEnabled(outputHandler->replayBuffer);

		if (sysTrayReplayBuffer) {
			sysTrayReplayBuffer->setEnabled(!!outputHandler->replayBuffer);
		}

		UpdateIsRecordingPausable();
	} else {
		outputHandler->Update();
	}
}

bool OBSBasic::Active() const
{
	if (!outputHandler) {
		return false;
	}
	return outputHandler->Active();
}

void OBSBasic::ResizeOutputSizeOfSource()
{
	if (obs_video_active()) {
		return;
	}

	QMessageBox resize_output(this);
	resize_output.setText(QTStr("ResizeOutputSizeOfSource.Text") + "\n\n" +
			      QTStr("ResizeOutputSizeOfSource.Continue"));
	QAbstractButton *Yes = resize_output.addButton(QTStr("Yes"), QMessageBox::YesRole);
	resize_output.addButton(QTStr("No"), QMessageBox::NoRole);
	resize_output.setIcon(QMessageBox::Warning);
	resize_output.setWindowTitle(QTStr("ResizeOutputSizeOfSource"));
	resize_output.exec();

	if (resize_output.clickedButton() != Yes) {
		return;
	}

	OBSSource source = obs_sceneitem_get_source(GetCurrentSceneItem());

	int width = obs_source_get_width(source);
	int height = obs_source_get_height(source);

	width = ((width + 3) / 4) * 4;   // Round width up to the nearest multiple of 4
	height = ((height + 1) / 2) * 2; // Round height up to the nearest multiple of 2

	config_set_uint(activeConfiguration, "Video", "BaseCX", width);
	config_set_uint(activeConfiguration, "Video", "BaseCY", height);
	config_set_uint(activeConfiguration, "Video", "OutputCX", width);
	config_set_uint(activeConfiguration, "Video", "OutputCY", height);

	ResetVideo();
	ResetOutputs();
	activeConfiguration.SaveSafe("tmp");
	on_actionFitToScreen_triggered();
}

const char *OBSBasic::GetCurrentOutputPath()
{
	const char *path = nullptr;
	const char *mode = config_get_string(Config(), "Output", "Mode");

	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");

		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			path = config_get_string(Config(), "AdvOut", "FFFilePath");
		} else {
			path = config_get_string(Config(), "AdvOut", "RecFilePath");
		}
	} else {
		path = config_get_string(Config(), "SimpleOutput", "FilePath");
	}

	return path;
}

void OBSBasic::OutputPathInvalidMessage()
{
	blog(LOG_ERROR, "Recording stopped because of bad output path");

#ifdef __ANDROID__
	OBSMessageBox::critical(this, QTStr("Output.BadPath.Title"),
		QStringLiteral("无法访问录像目录。请在设置中重新选择文件夹并授予写入权限，"
			       "或输入 /storage/emulated/0/Download/obs。请勿选择 Android/data 等系统受限目录。"));
#else
	OBSMessageBox::critical(this, QTStr("Output.BadPath.Title"), QTStr("Output.BadPath.Text"));
#endif
}

bool OBSBasic::IsFFmpegOutputToURL() const
{
	const char *mode = config_get_string(Config(), "Output", "Mode");
	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");
		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			bool is_local = config_get_bool(Config(), "AdvOut", "FFOutputToFile");
			if (!is_local) {
				return true;
			}
		}
	}

	return false;
}

bool OBSBasic::OutputPathValid()
{
	if (IsFFmpegOutputToURL()) {
		return true;
	}

	const char *path = GetCurrentOutputPath();
#ifdef __ANDROID__
	if (path && obsAndroidStorageManaged(QString::fromUtf8(path)))
		return obsAndroidStorageWritable(QString::fromUtf8(path));
#endif
	return path && *path && QDir(path).exists();
}
