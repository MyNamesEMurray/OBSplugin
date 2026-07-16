/*
 * Frontend UI: a live health readout in OBS's status bar (fps · Mb/s ·
 * latency, next to OBS's own stats) plus a "LensLink" dock with one row
 * per source, fed by the snapshots ios-camera-source.c keeps anyway.
 *
 * Two deliberate choices keep this portable and optional:
 *
 * - The obs-frontend-api symbols are resolved AT RUNTIME from the running
 *   process (dlsym / GetProcAddress). The plugin never links the frontend
 *   library — the same binary loads in OBS builds without a frontend
 *   (headless, older) and the UI just stays off. Only the header is
 *   needed at compile time, for the types.
 * - No Q_OBJECT: the timer binds a lambda, so no moc, and the plugin's
 *   plain CMake stays plain.
 *
 * Everything here runs on the Qt UI thread (frontend event callbacks and
 * the QTimer both live there); the only cross-thread traffic is
 * lenslink_health_enum, which locks internally.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>

#include <string>
#include <unordered_map>

extern "C" {
#include "health.h"
void lenslink_frontend_init(void);
void lenslink_frontend_shutdown(void);
}

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr int MAX_SOURCES = 16;
constexpr char DOCK_ID[] = "lenslink_health";

/* Runtime-resolved frontend API (see file comment). */
struct FrontendApi {
	void (*add_event_callback)(obs_frontend_event_cb, void *);
	void (*remove_event_callback)(obs_frontend_event_cb, void *);
	void *(*get_main_window)(void);
	bool (*add_dock_by_id)(const char *, const char *, void *);
	void (*remove_dock)(const char *);
};

FrontendApi api = {};
bool callback_installed = false;

template<typename T> void resolve(T &fn, const char *name)
{
#ifdef _WIN32
	HMODULE module = GetModuleHandleA("obs-frontend-api.dll");
	fn = module ? reinterpret_cast<T>(
			      GetProcAddress(module, name))
		    : nullptr;
#else
	fn = reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
#endif
}

QLabel *status_label = nullptr;
QLabel *dock_label = nullptr; /* owned by OBS once the dock is added */
QTimer *timer = nullptr;

/* Rate derivation: previous cumulative counters per source name. */
struct PrevSample {
	uint64_t frames;
	uint64_t bytes;
	uint64_t time_ns;
};
std::unordered_map<std::string, PrevSample> previous;

QString rates_text(const lenslink_health &h, const PrevSample *prev,
		   uint64_t now_ns)
{
	if (!prev || now_ns <= prev->time_ns || h.frames < prev->frames)
		return QStringLiteral("…");

	double seconds = double(now_ns - prev->time_ns) / 1e9;
	double fps = double(h.frames - prev->frames) / seconds;
	double mbps = double(h.bytes - prev->bytes) * 8.0 / seconds / 1e6;

	QString text = QStringLiteral("%1 fps · %2 Mb/s")
			       .arg(qRound(fps))
			       .arg(mbps, 0, 'f', 1);
	if (h.latency_ms > 0)
		text += QStringLiteral(" · %1 ms").arg(h.latency_ms);
	return text;
}

void refresh()
{
	if (!status_label)
		return;

	lenslink_health list[MAX_SOURCES];
	size_t count = lenslink_health_enum(list, MAX_SOURCES);
	uint64_t now = os_gettime_ns();

	QStringList bar_parts;
	QStringList dock_rows;
	std::unordered_map<std::string, PrevSample> next;
	int standby_count = 0;

	for (size_t i = 0; i < count; i++) {
		const lenslink_health &h = list[i];
		std::string key = h.source_name;
		const PrevSample *prev = nullptr;
		auto it = previous.find(key);
		if (it != previous.end())
			prev = &it->second;
		next[key] = {h.frames, h.bytes, now};

		QString who = QString::fromUtf8(
			h.device[0] ? h.device : h.source_name);
		if (h.connected && !h.standby) {
			bar_parts << QStringLiteral("%1 %2").arg(
				who, rates_text(h, prev, now));
		} else if (h.connected && h.standby) {
			standby_count++;
		}

		/* Dock: every source gets a row, live or not. */
		QString detail;
		if (h.connected && !h.standby)
			detail = rates_text(h, prev, now);
		else
			detail = QString::fromUtf8(h.status).toHtmlEscaped();
		dock_rows << QStringLiteral("<b>%1</b>%2<br/>%3")
				     .arg(QString::fromUtf8(h.source_name)
						  .toHtmlEscaped(),
					  h.device[0] ? QStringLiteral(
							      " — %1")
							      .arg(QString::fromUtf8(
									   h.device)
									   .toHtmlEscaped())
						      : QString(),
					  detail);
	}
	previous = std::move(next);

	/* Status bar: live streams get numbers; standby-only gets a word;
	 * nothing LensLink → no label at all (don't crowd OBS's bar). */
	if (!bar_parts.isEmpty()) {
		status_label->setText(
			QStringLiteral("LensLink: %1")
				.arg(bar_parts.join(QStringLiteral("  |  "))));
		status_label->setVisible(true);
	} else if (standby_count > 0) {
		status_label->setText(
			QStringLiteral("LensLink: ready (camera idle)"));
		status_label->setVisible(true);
	} else {
		status_label->setVisible(false);
	}

	if (dock_label) {
		dock_label->setText(
			dock_rows.isEmpty()
				? QStringLiteral("<i>%1</i>")
					  .arg(QString::fromUtf8(obs_module_text(
							       "HealthDock.Empty"))
						       .toHtmlEscaped())
				: dock_rows.join(
					  QStringLiteral("<br/><br/>")));
	}
}

void create_widgets()
{
	auto *window =
		static_cast<QMainWindow *>(api.get_main_window());
	if (!window || status_label)
		return;

	status_label = new QLabel(window);
	status_label->setVisible(false);
	window->statusBar()->addPermanentWidget(status_label);

	if (api.add_dock_by_id) {
		dock_label = new QLabel;
		dock_label->setTextFormat(Qt::RichText);
		dock_label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
		dock_label->setWordWrap(true);
		dock_label->setContentsMargins(10, 8, 10, 8);
		if (!api.add_dock_by_id(DOCK_ID,
					obs_module_text("HealthDock"),
					dock_label)) {
			delete dock_label;
			dock_label = nullptr;
		}
	}

	timer = new QTimer(window);
	QObject::connect(timer, &QTimer::timeout, [] { refresh(); });
	timer->start(1000);

	blog(LOG_INFO, "[lenslink] frontend UI ready (status bar%s)",
	     dock_label ? " + dock" : "");
}

void destroy_widgets()
{
	if (timer) {
		timer->stop();
		timer->deleteLater();
		timer = nullptr;
	}
	if (status_label) {
		auto *window = static_cast<QMainWindow *>(
			api.get_main_window());
		if (window)
			window->statusBar()->removeWidget(status_label);
		status_label->deleteLater();
		status_label = nullptr;
	}
	if (dock_label) {
		/* OBS owns the widget; removing the dock destroys it. */
		if (api.remove_dock)
			api.remove_dock(DOCK_ID);
		dock_label = nullptr;
	}
	previous.clear();
}

void frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		create_widgets();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		destroy_widgets();
		break;
	default:
		break;
	}
}

} // namespace

void lenslink_frontend_init(void)
{
	resolve(api.add_event_callback, "obs_frontend_add_event_callback");
	resolve(api.remove_event_callback,
		"obs_frontend_remove_event_callback");
	resolve(api.get_main_window, "obs_frontend_get_main_window");
	resolve(api.add_dock_by_id, "obs_frontend_add_dock_by_id");
	resolve(api.remove_dock, "obs_frontend_remove_dock");

	if (!api.add_event_callback || !api.remove_event_callback ||
	    !api.get_main_window) {
		blog(LOG_INFO,
		     "[lenslink] no OBS frontend in this process — "
		     "status-bar/dock UI disabled");
		return;
	}

	api.add_event_callback(frontend_event, nullptr);
	callback_installed = true;
}

void lenslink_frontend_shutdown(void)
{
	if (callback_installed) {
		api.remove_event_callback(frontend_event, nullptr);
		callback_installed = false;
	}
	/* Widgets are torn down on OBS_FRONTEND_EVENT_EXIT, which precedes
	 * module unload; nothing further to do here. */
}
