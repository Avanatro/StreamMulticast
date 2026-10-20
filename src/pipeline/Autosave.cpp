/*
StreamMulticast — OBS Multi-RTMP Plugin with Per-Output Re-Encode
Copyright (C) 2026 Avanatro <contact@avanatro.com>

GPLv2 — see LICENSE for full text.
*/

#include "Autosave.hpp"

#include "../core/AutosavePolicy.hpp"
#include "../plugin-support.h"

#include <obs-frontend-api.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <algorithm>
#include <memory>
#include <string>

namespace smulti {

namespace {

/* Lives on the UI thread; the timer is its child, so it dies with it. */
class Autosave : public QObject {
public:
	Autosave() : m_timer(this)
	{
		m_clock.start();
		m_policy.set_blocked(true); // until FINISHED_LOADING
		m_timer.setSingleShot(true);
		connect(&m_timer, &QTimer::timeout, this, [this] { on_timeout(); });
	}

	void set_blocked(bool blocked)
	{
		m_policy.set_blocked(blocked);
		if (blocked)
			m_timer.stop();
	}

	void request(const char *reason)
	{
		if (m_policy.blocked())
			return;
		m_reason = reason ? reason : "edit";
		m_policy.request(m_clock.elapsed());
		arm();
	}

private:
	void arm()
	{
		if (!m_policy.pending())
			return;
		const qint64 wait = std::max<qint64>(0, m_policy.deadline_ms() - m_clock.elapsed());
		m_timer.start(static_cast<int>(wait));
	}

	void on_timeout()
	{
		if (m_policy.due(m_clock.elapsed())) {
			obs_log(LOG_DEBUG, "autosave (%s)", m_reason.c_str());
			obs_frontend_save();
		} else {
			arm(); // timer fired slightly early
		}
	}

	AutosavePolicy m_policy;
	QElapsedTimer m_clock;
	QTimer m_timer;
	std::string m_reason;
};

std::unique_ptr<Autosave> g_autosave;

bool on_ui_thread()
{
	QCoreApplication *app = QCoreApplication::instance();
	return app && QThread::currentThread() == app->thread();
}

} // namespace

void autosave_start()
{
	if (!g_autosave)
		g_autosave = std::make_unique<Autosave>();
}

void autosave_stop()
{
	g_autosave.reset();
}

void autosave_set_blocked(bool blocked)
{
	if (g_autosave)
		g_autosave->set_blocked(blocked);
}

void request_autosave(const char *reason)
{
	if (!on_ui_thread()) {
		obs_log(LOG_WARNING, "autosave (%s) refused: not on the UI thread", reason ? reason : "edit");
		Q_ASSERT(false);
		return;
	}
	if (g_autosave)
		g_autosave->request(reason);
}

} // namespace smulti
