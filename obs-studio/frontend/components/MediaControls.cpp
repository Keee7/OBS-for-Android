#include "MediaControls.hpp"

#include <QFontMetrics>
#include <algorithm>
#include <cstdlib>
#include "ui_media-controls.h"

#include <OBSApp.hpp>

#include <QHBoxLayout>
#include <QToolTip>

#include "moc_MediaControls.cpp"

void MediaControls::OBSMediaStopped(void *data, calldata_t *)
{
	MediaControls *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::SetRestartState);
}

void MediaControls::OBSMediaPlay(void *data, calldata_t *)
{
	MediaControls *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::SetPlayingState);
}

void MediaControls::OBSMediaPause(void *data, calldata_t *)
{
	MediaControls *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::SetPausedState);
}

void MediaControls::OBSMediaStarted(void *data, calldata_t *)
{
	MediaControls *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::SetPlayingState);
}

void MediaControls::OBSMediaNext(void *data, calldata_t *)
{
	MediaControls *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::UpdateSlideCounter);
}

void MediaControls::OBSMediaPrevious(void *data, calldata_t *)
{
	MediaControls *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::UpdateSlideCounter);
}

MediaControls::MediaControls(QWidget *parent, bool compact)
	: QWidget(parent),
	  compact(compact),
	  ui(new Ui::MediaControls)
{
	ui->setupUi(this);
	setFocusPolicy(Qt::StrongFocus);
#ifdef __ANDROID__
	ui->slider->setLightweightMode(true);
#endif
	if (compact) {
		auto *transport = new QHBoxLayout;
		for (auto *button : {ui->playPauseButton, ui->previousButton, ui->stopButton, ui->nextButton}) {
			ui->horizontalLayout->removeWidget(button);
			transport->addWidget(button);
		}
		transport->addStretch();

		auto *position = new QHBoxLayout;
		ui->horizontalLayout->removeWidget(ui->slider);
		ui->horizontalLayout->removeWidget(ui->timerLabel);
		ui->slider->setMinimumHeight(28);
		position->addWidget(ui->slider, 1);
		position->addWidget(ui->timerLabel);

		ui->horizontalLayout->setDirection(QBoxLayout::TopToBottom);
		ui->horizontalLayout->insertLayout(0, transport);
		ui->horizontalLayout->insertLayout(1, position);
		ui->label->hide();
		ui->durationLabel->hide();
	}

	connect(&mediaTimer, &QTimer::timeout, this, &MediaControls::SetSliderPosition);
	connect(&seekTimer, &QTimer::timeout, this, &MediaControls::SeekTimerCallback);
	connect(ui->slider, &AbsoluteSlider::sliderPressed, this, &MediaControls::AbsoluteSliderClicked);
	connect(ui->slider, &AbsoluteSlider::absoluteSliderHovered, this, &MediaControls::AbsoluteSliderHovered);
	connect(ui->slider, &AbsoluteSlider::sliderReleased, this, &MediaControls::AbsoluteSliderReleased);
	connect(ui->slider, &AbsoluteSlider::sliderMoved, this, &MediaControls::AbsoluteSliderMoved);

	countDownTimer = config_get_bool(App()->GetUserConfig(), "BasicWindow", "MediaControlsCountdownTimer");

	QAction *restartAction = new QAction(this);
	restartAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	restartAction->setShortcut({Qt::Key_R});
	connect(restartAction, &QAction::triggered, this, &MediaControls::RestartMedia);
	addAction(restartAction);

	QAction *sliderFoward = new QAction(this);
	sliderFoward->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(sliderFoward, &QAction::triggered, this, &MediaControls::MoveSliderFoward);
	sliderFoward->setShortcut({Qt::Key_Right});
	addAction(sliderFoward);

	QAction *sliderBack = new QAction(this);
	sliderBack->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(sliderBack, &QAction::triggered, this, &MediaControls::MoveSliderBackwards);
	sliderBack->setShortcut({Qt::Key_Left});
	addAction(sliderBack);

	QAction *playPause = new QAction(this);
	playPause->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(playPause, &QAction::triggered, this, &MediaControls::on_playPauseButton_clicked);
	playPause->setShortcut({Qt::Key_Space});
	addAction(playPause);
}

MediaControls::~MediaControls() {}

bool MediaControls::MediaPaused()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return false;
	}

	obs_media_state state = obs_source_media_get_state(source);
	return state == OBS_MEDIA_STATE_PAUSED;
}

int64_t MediaControls::GetSliderTime(int val)
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return 0;
	}

	float percent = (float)val / (float)ui->slider->maximum();
	float duration = (float)obs_source_media_get_duration(source);
	int64_t seekTo = (int64_t)(percent * duration);

	return seekTo;
}

void MediaControls::AbsoluteSliderClicked()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	obs_media_state state = obs_source_media_get_state(source);

	if (state == OBS_MEDIA_STATE_PAUSED) {
		prevPaused = true;
	} else if (state == OBS_MEDIA_STATE_PLAYING) {
		prevPaused = false;
		PauseMedia();
		StopMediaTimer();
	}

	seek = ui->slider->effectiveValue();
	seekTimer.start(100);
}

void MediaControls::AbsoluteSliderReleased()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	if (seekTimer.isActive()) {
		seekTimer.stop();
		if (lastSeek != seek) {
			obs_source_media_set_time(source, GetSliderTime(seek));
		}

		UpdateLabels(seek);
		seek = lastSeek = -1;
	}

	if (!prevPaused) {
		PlayMedia();
		StartMediaTimer();
	}
}

void MediaControls::AbsoluteSliderHovered(int val)
{
	if (compact)
		return;
	float seconds = ((float)GetSliderTime(val) / 1000.0f);
	QToolTip::showText(QCursor::pos(), FormatSeconds((int)seconds), this);
}

void MediaControls::AbsoluteSliderMoved(int val)
{
	if (seekTimer.isActive()) {
		seek = val;
		UpdateLabels(seek);
	}
}

void MediaControls::SeekTimerCallback()
{
	if (lastSeek != seek) {
		OBSSource source = OBSGetStrongRef(weakSource);
		if (source) {
			obs_source_media_set_time(source, GetSliderTime(seek));
		}
		lastSeek = seek;
	}
}

void MediaControls::StartMediaTimer()
{
	if (isSlideshow) {
		return;
	}

	if (!mediaTimer.isActive()) {
#ifdef __ANDROID__
		mediaTimer.setTimerType(Qt::CoarseTimer);
		mediaTimer.start(250);
#else
		mediaTimer.start(compact ? 50 : 16);
#endif
	}
}

void MediaControls::StopMediaTimer()
{
	if (mediaTimer.isActive()) {
		mediaTimer.stop();
	}
}

void MediaControls::SetPlayingState()
{
	ui->slider->setEnabled(true);
	ui->playPauseButton->setProperty("class", "icon-media-pause");
	ui->playPauseButton->style()->unpolish(ui->playPauseButton);
	ui->playPauseButton->style()->polish(ui->playPauseButton);
	ui->playPauseButton->setToolTip(QTStr("ContextBar.MediaControls.PauseMedia"));

	prevPaused = false;

	UpdateSlideCounter();
	StartMediaTimer();
}

void MediaControls::SetPausedState()
{
	ui->playPauseButton->setProperty("class", "icon-media-play");
	ui->playPauseButton->style()->unpolish(ui->playPauseButton);
	ui->playPauseButton->style()->polish(ui->playPauseButton);
	ui->playPauseButton->setToolTip(QTStr("ContextBar.MediaControls.PlayMedia"));

	StopMediaTimer();
}

void MediaControls::SetRestartState()
{
	ui->playPauseButton->setProperty("class", "icon-media-restart");
	ui->playPauseButton->style()->unpolish(ui->playPauseButton);
	ui->playPauseButton->style()->polish(ui->playPauseButton);
	ui->playPauseButton->setToolTip(QTStr("ContextBar.MediaControls.RestartMedia"));

	ui->slider->setProgressValue(0);

	if (!isSlideshow) {
		ui->timerLabel->setText("--:--:--");
		ui->durationLabel->setText("--:--:--");
	} else {
		ui->timerLabel->setText("-");
		ui->durationLabel->setText("-");
	}

	ui->slider->setEnabled(false);

	StopMediaTimer();
}

void MediaControls::RefreshControls()
{
	OBSSource source;
	source = OBSGetStrongRef(weakSource);

	uint32_t flags = 0;
	const char *id = nullptr;

	if (source) {
		flags = obs_source_get_output_flags(source);
		id = obs_source_get_unversioned_id(source);
	}

	if (!source || !(flags & OBS_SOURCE_CONTROLLABLE_MEDIA)) {
		SetRestartState();
		setEnabled(false);
		hide();
		return;
	} else {
		setEnabled(true);
		show();
	}

	isSlideshow = strcmp(id, "slideshow") == 0;
	const bool has_playlist = isSlideshow || (!compact && strcmp(id, "ffmpeg_source") != 0);
	ui->previousButton->setVisible(has_playlist);
	ui->nextButton->setVisible(has_playlist);

	ui->slider->setVisible(!isSlideshow);
	ui->emptySpaceAgain->setVisible(!compact && isSlideshow);

	obs_media_state state = obs_source_media_get_state(source);

	switch (state) {
	case OBS_MEDIA_STATE_STOPPED:
	case OBS_MEDIA_STATE_ENDED:
	case OBS_MEDIA_STATE_NONE:
		SetRestartState();
		break;
	case OBS_MEDIA_STATE_PLAYING:
		SetPlayingState();
		break;
	case OBS_MEDIA_STATE_PAUSED:
		SetPausedState();
		break;
	default:
		break;
	}

	if (isSlideshow) {
		UpdateSlideCounter();
	} else {
		SetSliderPosition();
	}
}

OBSSource MediaControls::GetSource()
{
	return OBSGetStrongRef(weakSource);
}

void MediaControls::SetSource(OBSSource source)
{
	sigs.clear();

	if (source) {
		weakSource = OBSGetWeakRef(source);
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		sigs.emplace_back(sh, "media_play", OBSMediaPlay, this);
		sigs.emplace_back(sh, "media_pause", OBSMediaPause, this);
		sigs.emplace_back(sh, "media_restart", OBSMediaPlay, this);
		sigs.emplace_back(sh, "media_stopped", OBSMediaStopped, this);
		sigs.emplace_back(sh, "media_started", OBSMediaStarted, this);
		sigs.emplace_back(sh, "media_ended", OBSMediaStopped, this);
		sigs.emplace_back(sh, "media_next", OBSMediaNext, this);
		sigs.emplace_back(sh, "media_previous", OBSMediaPrevious, this);
	} else {
		weakSource = nullptr;
	}

	RefreshControls();
}

namespace {
constexpr char kLastSecProp[] = "_obs_last_sec";
constexpr char kLastCountDownProp[] = "_obs_last_countdown";
constexpr char kLastFontKeyProp[] = "_obs_last_font_key";

void ensureStableLabelWidth(QLabel *label, const QString &sampleText)
{
	const QString fontKey = label->font().key();
	if (label->property(kLastFontKeyProp).toString() == fontKey) {
		return;
	}

	const int minW = QFontMetrics(label->font()).horizontalAdvance(sampleText) + 4;
	if (label->minimumWidth() < minW) {
		label->setMinimumWidth(minW);
	}
	label->setProperty(kLastFontKeyProp, fontKey);
}

void updateTimeLabelIfChanged(QLabel *label, int seconds, bool negativePrefix, const QString &formatted)
{
	const QVariant prevSec = label->property(kLastSecProp);
	const QVariant prevNeg = label->property(kLastCountDownProp);
	if (prevSec.isValid() && prevSec.toInt() == seconds && prevNeg.isValid() && prevNeg.toBool() == negativePrefix) {
		return;
	}

	label->setText(negativePrefix ? (QLatin1Char('-') + formatted) : formatted);
	label->setProperty(kLastSecProp, seconds);
	label->setProperty(kLastCountDownProp, negativePrefix);
}

template<typename FormatFn>
void syncMediaTimeLabels(QLabel *timerLabel, QLabel *durationLabel, bool compact, bool countDownTimer,
			 int64_t timeMs, int64_t durationMs, FormatFn &&formatFn)
{
	ensureStableLabelWidth(timerLabel, QStringLiteral("88:88:88"));
	const int curSec = (int)(timeMs / 1000);
	const QVariant prevTimer = timerLabel->property(kLastSecProp);
	if (!prevTimer.isValid() || prevTimer.toInt() != curSec) {
		updateTimeLabelIfChanged(timerLabel, curSec, false, formatFn(curSec));
	}

	if (compact) {
		return;
	}

	ensureStableLabelWidth(durationLabel, QStringLiteral("-88:88:88"));
	const int64_t durMs = countDownTimer ? std::max<int64_t>(0, durationMs - timeMs) : durationMs;
	const int durSec = (int)(durMs / 1000);
	const QVariant prevDur = durationLabel->property(kLastSecProp);
	const QVariant prevNeg = durationLabel->property(kLastCountDownProp);
	if (!prevDur.isValid() || prevDur.toInt() != durSec || !prevNeg.isValid() ||
	    prevNeg.toBool() != countDownTimer) {
		updateTimeLabelIfChanged(durationLabel, durSec, countDownTimer, formatFn(durSec));
	}
}
} // namespace

void MediaControls::SetSliderPosition()
{
	if (!isVisible()) {
		return;
	}

	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	const int64_t durationMs = std::max<int64_t>(0, obs_source_media_get_duration(source));
	const int64_t timeMs = (durationMs > 0) ? std::clamp<int64_t>(obs_source_media_get_time(source), 0, durationMs)
						: 0;
	const int maxSlider = ui->slider->maximum();

	int sliderPosition = 0;
	if (durationMs > 0 && maxSlider > 0) {
		sliderPosition = (int)std::clamp<int64_t>((timeMs * maxSlider) / durationMs, 0, maxSlider);
	}

	if (!ui->slider->isSliderDown()) {
		const int curPos = ui->slider->effectiveValue();
#ifdef __ANDROID__
		const int trackPixels = ui->slider->width();
		const int minStep = (trackPixels > 0) ? std::max(1, maxSlider / trackPixels) : 1;
		if (sliderPosition != curPos &&
		    (sliderPosition == 0 || sliderPosition == maxSlider ||
		     std::abs(sliderPosition - curPos) >= minStep)) {
			ui->slider->setProgressValue(sliderPosition);
		}
#else
		if (sliderPosition != curPos) {
			ui->slider->setValue(sliderPosition);
		}
#endif
	}

	syncMediaTimeLabels(ui->timerLabel, ui->durationLabel, compact, countDownTimer, timeMs, durationMs,
			    [this](int sec) { return FormatSeconds(sec); });
}

QString MediaControls::FormatSeconds(int totalSeconds)
{
	int seconds = totalSeconds % 60;
	int totalMinutes = totalSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;

	return QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
}

void MediaControls::on_playPauseButton_clicked()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	obs_media_state state = obs_source_media_get_state(source);

	switch (state) {
	case OBS_MEDIA_STATE_STOPPED:
	case OBS_MEDIA_STATE_ENDED:
		RestartMedia();
		break;
	case OBS_MEDIA_STATE_PLAYING:
		PauseMedia();
		break;
	case OBS_MEDIA_STATE_PAUSED:
		PlayMedia();
		break;
	default:
		break;
	}
}

void MediaControls::RestartMedia()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_restart(source);
	}
}

void MediaControls::PlayMedia()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_play_pause(source, false);
	}
}

void MediaControls::PauseMedia()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_play_pause(source, true);
	}
}

void MediaControls::StopMedia()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_stop(source);
	}
}

void MediaControls::PlaylistNext()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_next(source);
	}
}

void MediaControls::PlaylistPrevious()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_previous(source);
	}
}

void MediaControls::on_stopButton_clicked()
{
	StopMedia();
}

void MediaControls::on_nextButton_clicked()
{
	PlaylistNext();
}

void MediaControls::on_previousButton_clicked()
{
	PlaylistPrevious();
}

void MediaControls::on_durationLabel_clicked()
{
	countDownTimer = !countDownTimer;

	config_set_bool(App()->GetUserConfig(), "BasicWindow", "MediaControlsCountdownTimer", countDownTimer);

	if (MediaPaused()) {
		SetSliderPosition();
	}
}

void MediaControls::MoveSliderFoward(int seconds)
{
	OBSSource source = OBSGetStrongRef(weakSource);

	if (!source) {
		return;
	}

	int ms = obs_source_media_get_time(source);
	ms += seconds * 1000;

	obs_source_media_set_time(source, ms);
	SetSliderPosition();
}

void MediaControls::MoveSliderBackwards(int seconds)
{
	OBSSource source = OBSGetStrongRef(weakSource);

	if (!source) {
		return;
	}

	int ms = obs_source_media_get_time(source);
	ms -= seconds * 1000;

	obs_source_media_set_time(source, ms);
	SetSliderPosition();
}

void MediaControls::UpdateSlideCounter()
{
	if (!isSlideshow) {
		return;
	}

	OBSSource source = OBSGetStrongRef(weakSource);

	if (!source) {
		return;
	}

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	calldata_t cd = {};

	proc_handler_call(ph, "current_index", &cd);
	int slide = calldata_int(&cd, "current_index");

	proc_handler_call(ph, "total_files", &cd);
	int total = calldata_int(&cd, "total_files");
	calldata_free(&cd);

	if (total > 0) {
		ui->timerLabel->setText(QString::number(slide + 1));
		ui->durationLabel->setText(QString::number(total));
	} else {
		ui->timerLabel->setText("-");
		ui->durationLabel->setText("-");
	}
}

void MediaControls::UpdateLabels(int val)
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	const int64_t durationMs = std::max<int64_t>(0, obs_source_media_get_duration(source));
	const int maxSlider = ui->slider->maximum();
	const int64_t timeMs = (durationMs > 0 && maxSlider > 0)
				       ? std::clamp<int64_t>((int64_t(val) * durationMs) / maxSlider, 0, durationMs)
				       : 0;

	syncMediaTimeLabels(ui->timerLabel, ui->durationLabel, compact, countDownTimer, timeMs, durationMs,
			    [this](int sec) { return FormatSeconds(sec); });
}
