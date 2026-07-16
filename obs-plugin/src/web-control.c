#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/bmem.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "net-compat.h"
#include "web-control.h"

#define MAX_REQUEST (16 * 1024)
#define MAX_CONTROL_BODY 512

/*
 * Browser control panel, styled to docs/UI_DESIGN.md (shared palette,
 * control metaphors and ordering with the app's Live panel). Single-quoted
 * HTML/SVG attributes keep the C escaping sane.
 */
static const char control_page[] =
	"<!doctype html><html><head><meta charset='utf-8'>"
	"<meta name='viewport' content='width=device-width,initial-scale=1'>"
	"<title>LensLink Control</title><style>"
	":root{--accent:#3D7BFF;--live:#30D158;--amber:#FF9F0A;--red:#FF453A;"
	"--grey:#8E8E93;--bg:#0E0F13;--glass:rgba(28,30,38,0.72);"
	"--hair:rgba(255,255,255,0.08);--txt:#fff;--txt2:rgba(235,235,245,0.6)}"
	"*{box-sizing:border-box}"
	"body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);"
	"color:var(--txt);max-width:440px;margin:0 auto;padding:20px 16px}"
	"header{display:flex;align-items:center;justify-content:space-between;"
	"margin-bottom:16px}h1{font-size:20px;font-weight:600;margin:0}"
	".pill{display:inline-flex;align-items:center;gap:8px;background:var(--glass);"
	"border:1px solid var(--hair);border-radius:999px;padding:6px 12px;"
	"font-size:13px;font-weight:600}"
	".dot{width:10px;height:10px;border-radius:50%;background:var(--grey);"
	"transition:background .2s}"
	".panel{background:var(--glass);border:1px solid var(--hair);"
	"border-radius:16px;padding:16px;backdrop-filter:blur(20px)}"
	".row{display:flex;align-items:center;gap:12px;margin:14px 0}"
	".row:first-child{margin-top:0}"
	".ro{font-variant-numeric:tabular-nums;font-family:ui-monospace,monospace;"
	"width:48px;text-align:right;font-size:13px;flex:none}"
	"input[type=range]{flex:1;accent-color:var(--accent);height:4px}"
	".ic{width:18px;height:18px;flex:none;color:var(--txt2)}"
	".seg{display:inline-flex;background:rgba(255,255,255,.1);border-radius:12px;"
	"padding:2px;flex:none}.seg button{background:none;border:0;color:var(--txt);"
	"padding:6px 14px;border-radius:10px;font-size:13px;cursor:pointer}"
	".seg button.on{background:var(--accent)}"
	".hint{color:var(--txt2);font-size:13px;flex:1}"
	".chips{display:flex;gap:10px;align-items:center;margin-top:14px}"
	".chip{width:44px;height:44px;border:0;border-radius:50%;"
	"background:rgba(255,255,255,.12);color:var(--txt);cursor:pointer;"
	"display:inline-flex;align-items:center;justify-content:center}"
	".chip.on{background:var(--accent)}.chip .ic{color:var(--txt);width:20px;height:20px}"
	"select{flex:1;background:rgba(255,255,255,.12);color:var(--txt);border:0;"
	"border-radius:12px;padding:0 12px;height:44px;font-size:14px}"
	".primary{width:100%;height:44px;border:0;border-radius:12px;"
	"background:var(--accent);color:#fff;font-size:15px;font-weight:600;"
	"cursor:pointer}"
	/* Stop is the one destructive control (docs/UI_DESIGN.md §1). */
	".primary.danger{background:var(--red)}"
	"</style></head><body>"
	"<header><h1>LensLink</h1>"
	"<div class='pill'><span class='dot' id='dot'></span>"
	"<span id='status'>connecting&hellip;</span></div></header>"
	/* Shown instead of the controls for a screen-mirror source. */
	"<div class='panel' id='screennote' style='display:none'>"
	"Screen mirroring &mdash; camera controls don&rsquo;t apply.</div>"
	/* Remote start: the app is open but idle; one tap starts the camera. */
	"<div class='panel' id='startpanel' style='display:none'>"
	"<button class='primary' id='startbtn'>Start camera</button>"
	"<div class='hint' style='margin-top:12px'>The LensLink app is open "
	"and ready &mdash; the camera hasn&rsquo;t been started yet.</div></div>"
	"<div class='panel' id='panel'>"
	"<div class='row'>"
	"<svg class='ic' viewBox='0 0 24 24' fill='none' stroke='currentColor' "
	"stroke-width='2' stroke-linecap='round'><circle cx='11' cy='11' r='7'/>"
	"<line x1='21' y1='21' x2='16.65' y2='16.65'/><line x1='8' y1='11' x2='14' y2='11'/></svg>"
	"<input id='zoom' type='range' min='1' max='10' step='0.1' value='1'>"
	"<svg class='ic' viewBox='0 0 24 24' fill='none' stroke='currentColor' "
	"stroke-width='2' stroke-linecap='round'><circle cx='11' cy='11' r='7'/>"
	"<line x1='21' y1='21' x2='16.65' y2='16.65'/><line x1='8' y1='11' x2='14' y2='11'/>"
	"<line x1='11' y1='8' x2='11' y2='14'/></svg>"
	"<span class='ro' id='zv'>1.0&times;</span></div>"
	"<div class='row'>"
	"<svg class='ic' viewBox='0 0 24 24' fill='none' stroke='currentColor' "
	"stroke-width='2'><circle cx='12' cy='12' r='3'/></svg>"
	"<input id='exposure' type='range' min='-2' max='2' step='0.1' value='0'>"
	"<svg class='ic' viewBox='0 0 24 24' fill='none' stroke='currentColor' "
	"stroke-width='2' stroke-linecap='round'><circle cx='12' cy='12' r='4'/>"
	"<line x1='12' y1='1' x2='12' y2='4'/><line x1='12' y1='20' x2='12' y2='23'/>"
	"<line x1='1' y1='12' x2='4' y2='12'/><line x1='20' y1='12' x2='23' y2='12'/>"
	"<line x1='4.6' y1='4.6' x2='6.7' y2='6.7'/><line x1='17.3' y1='17.3' x2='19.4' y2='19.4'/>"
	"<line x1='4.6' y1='19.4' x2='6.7' y2='17.3'/><line x1='17.3' y1='6.7' x2='19.4' y2='4.6'/></svg>"
	"<span class='ro' id='ev'>0.0</span></div>"
	"<div class='row'>"
	"<div class='seg'><button id='af' class='on'>AF</button>"
	"<button id='mf'>Lock</button></div>"
	"<input id='lens' type='range' min='0' max='1' step='0.01' value='0.5' "
	"style='display:none'>"
	"<span class='hint' id='fhint'>Tap the phone to focus</span></div>"
	"<div class='chips'>"
	"<button class='chip' id='flashlight' title='Flashlight'>"
	"<svg class='ic' viewBox='0 0 24 24' fill='currentColor'>"
	"<path d='M13 2 L4 14 h6 l-1 8 9-12 h-6 z'/></svg></button>"
	"<select id='lenssel'></select>"
	"<button class='chip' id='flip' title='Flip camera'>"
	"<svg class='ic' viewBox='0 0 24 24' fill='none' stroke='currentColor' "
	"stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
	"<path d='M15 4 h5 v5'/><path d='M20 9 A8 8 0 0 0 6 6'/>"
	"<path d='M9 20 H4 v-5'/><path d='M4 15 A8 8 0 0 0 18 18'/></svg></button>"
	"</div>"
	/* Remote stop: mirrors the app's red Stop. The phone drops back to
	 * standby, so this panel swaps to the Start button afterwards. */
	"<div class='row' style='margin-bottom:0'>"
	"<button class='primary danger' id='stopbtn'>Stop camera</button></div>"
	"</div>"
	"<script>"
	/* NB: elements are looked up explicitly — a bare `status` would
	 * resolve to window.status, not the element. */
	"const $=id=>document.getElementById(id);"
	"const dotEl=$('dot'),statusEl=$('status'),zoomEl=$('zoom'),zvEl=$('zv'),"
	"expEl=$('exposure'),evEl=$('ev'),afEl=$('af'),mfEl=$('mf'),lensEl=$('lens'),"
	"fhintEl=$('fhint'),flashlightEl=$('flashlight'),flipEl=$('flip'),lensselEl=$('lenssel'),"
	"panelEl=$('panel'),screennoteEl=$('screennote'),"
	"startpanelEl=$('startpanel'),startbtnEl=$('startbtn'),"
	"stopbtnEl=$('stopbtn');"
	"const COL={live:'#30D158',amber:'#FF9F0A',red:'#FF453A',grey:'#8E8E93'};"
	"let lastTouch=0;const touch=()=>lastTouch=Date.now();"
	"const send=o=>{touch();"
	"fetch('/api/control',{method:'POST',body:JSON.stringify(o)})};"
	"const deb=(f,ms)=>{let t;return(...a)=>{clearTimeout(t);"
	"t=setTimeout(()=>f(...a),ms)}};"
	"const dz=deb(()=>send({cmd:'zoom',value:+zoomEl.value}),60);"
	"zoomEl.oninput=()=>{touch();"
	"zvEl.textContent=(+zoomEl.value).toFixed(1)+'\\u00d7';dz()};"
	"const de=deb(()=>send({cmd:'exposure_bias',value:+expEl.value}),60);"
	"expEl.oninput=()=>{touch();evEl.textContent=(+expEl.value).toFixed(1);de()};"
	"function focusUI(locked){afEl.className=locked?'':'on';"
	"mfEl.className=locked?'on':'';lensEl.style.display=locked?'':'none';"
	"fhintEl.style.display=locked?'none':''}"
	"afEl.onclick=()=>{focusUI(false);send({cmd:'focus',mode:'auto'})};"
	"mfEl.onclick=()=>{focusUI(true);"
	"send({cmd:'focus',mode:'locked',lensPosition:+lensEl.value})};"
	"lensEl.oninput=deb(()=>send({cmd:'focus',mode:'locked',"
	"lensPosition:+lensEl.value}),60);"
	"let fon=false;"
	"function flashlightUI(on){fon=on;flashlightEl.className=on?'chip on':'chip'}"
	"flashlightEl.onclick=()=>{touch();flashlightUI(!fon);send({cmd:'flashlight',on:fon})};"
	"flipEl.onclick=()=>send({cmd:'flip'});"
	"lensselEl.onchange=()=>send({cmd:'selectLens',label:lensselEl.value});"
	"startbtnEl.onclick=()=>send({cmd:'start_stream'});"
	"stopbtnEl.onclick=()=>send({cmd:'stop_stream'});"
	"function statusColor(t){t=(t||'').toLowerCase();"
	/* Standby/starting are amber (ready, not live) — test before the
	 * generic 'connected' match, which their wording also contains. */
	"if(t.includes('idle')||t.includes('starting'))return COL.amber;"
	"if(t.includes('connected'))return COL.live;"
	"if(t.includes('disconnect')||t.includes('could not')||t.includes('error'))return COL.red;"
	"if(t.includes('wait')||t.includes('trying')||t.includes('dial'))return COL.amber;"
	"return COL.grey}"
	"async function poll(){try{"
	"const s=await(await fetch('/api/status')).json();"
	"statusEl.textContent=s.status||'idle';dotEl.style.background=statusColor(s.status);"
	/* Screen mirror has no camera controls: show the note instead.
	 * Standby (app idle): show the Start button instead of dead sliders. */
	"panelEl.style.display=(s.screen||s.standby)?'none':'';"
	"screennoteEl.style.display=s.screen?'':'none';"
	"startpanelEl.style.display=(s.standby&&!s.screen)?'':'none';"
	"if(s.screen||s.standby)return;"
	"const st=await(await fetch('/api/state')).json();"
	/* Don't fight the operator's hand: only mirror app state when the panel
	 * hasn't been touched for a couple of seconds. */
	"if(Date.now()-lastTouch>2000&&typeof st.zoom==='number'){"
	"if(st.maxZoom)zoomEl.max=st.maxZoom;"
	"zoomEl.value=st.zoom;zvEl.textContent=(+st.zoom).toFixed(1)+'\\u00d7';"
	"if(typeof st.exposureBias==='number'){expEl.value=st.exposureBias;"
	"evEl.textContent=(+st.exposureBias).toFixed(1)}"
	"const locked=st.focusMode==='locked';focusUI(locked);"
	"if(locked&&typeof st.lensPosition==='number')lensEl.value=st.lensPosition;"
	"flashlightUI(!!st.flashlight);flashlightEl.style.display=st.hasFlashlight===false?'none':'';"
	"if(Array.isArray(st.lenses)){"
	"const want=st.lenses.join('|');"
	/* textContent, not innerHTML: lens labels come from the device. */
	"if(lensselEl.dataset.opts!==want){lensselEl.dataset.opts=want;"
	"lensselEl.replaceChildren(...st.lenses.map(l=>{"
	"const o=document.createElement('option');o.textContent=l;return o}))}"
	"if(st.lens)lensselEl.value=st.lens}"
	"lensselEl.style.display=(st.lenses&&st.lenses.length>1)?'':'none'}"
	"}catch(e){statusEl.textContent='plugin unreachable';dotEl.style.background=COL.grey}}"
	"setInterval(poll,1000);poll();"
	"</script></body></html>";

struct web_control {
	pthread_t thread;
	volatile bool stop;
	socket_t listener;
	uint16_t port;
	struct ios_camera_source *source;
};

static void set_timeouts(socket_t s, int seconds)
{
#ifdef _WIN32
	DWORD ms = (DWORD)seconds * 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
	struct timeval tv = {.tv_sec = seconds, .tv_usec = 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static void send_str(socket_t s, const char *str)
{
	size_t len = strlen(str);
	const char *p = str;
	while (len > 0) {
		int n = (int)send(s, p, (int)len, 0);
		if (n <= 0)
			return;
		p += n;
		len -= (size_t)n;
	}
}

static void respond(socket_t s, const char *status_line,
		    const char *content_type, const char *body)
{
	char header[256];
	snprintf(header, sizeof(header),
		 "HTTP/1.1 %s\r\n"
		 "Content-Type: %s\r\n"
		 "Content-Length: %zu\r\n"
		 "Cache-Control: no-store\r\n"
		 "Connection: close\r\n\r\n",
		 status_line, content_type, body ? strlen(body) : 0);
	send_str(s, header);
	if (body)
		send_str(s, body);
}

/* Case-insensitive header lookup; returns pointer past "name:". */
static const char *find_header(const char *request, const char *name)
{
	size_t name_len = strlen(name);
	for (const char *p = request; (p = strchr(p, '\n')) != NULL;) {
		p++;
		size_t i = 0;
		while (i < name_len && p[i] &&
		       tolower((unsigned char)p[i]) ==
			       tolower((unsigned char)name[i]))
			i++;
		if (i == name_len && p[i] == ':')
			return p + name_len + 1;
	}
	return NULL;
}

static void json_escape(const char *in, char *out, size_t out_size)
{
	size_t o = 0;
	for (size_t i = 0; in[i] && o + 2 < out_size; i++) {
		unsigned char ch = (unsigned char)in[i];
		if (ch == '"' || ch == '\\') {
			out[o++] = '\\';
			out[o++] = (char)ch;
		} else if (ch < 0x20) {
			out[o++] = ' ';
		} else {
			out[o++] = (char)ch;
		}
	}
	out[o] = 0;
}

/* True if the header value (up to CR/LF) names a loopback host, e.g.
 * "localhost:9980" or "127.0.0.1:9980" or "http://localhost:9980". */
static bool value_is_local(const char *v)
{
	while (*v == ' ' || *v == '\t')
		v++;
	if (strncmp(v, "http://", 7) == 0)
		v += 7;
	return strncmp(v, "localhost", 9) == 0 ||
	       strncmp(v, "127.0.0.1", 9) == 0;
}

/* The listener binds to loopback, but that alone doesn't stop DNS
 * rebinding (Host: attacker.com resolving to 127.0.0.1) or cross-site
 * POSTs from web pages the streamer happens to visit (a text/plain POST
 * is a "simple request" — no CORS preflight). Require a loopback Host,
 * and a loopback Origin whenever the browser sends one. */
static bool request_is_local(const char *request)
{
	const char *host = find_header(request, "Host");
	if (!host || !value_is_local(host))
		return false;
	const char *origin = find_header(request, "Origin");
	if (origin && !value_is_local(origin))
		return false;
	return true;
}

static void handle_client(struct web_control *wc, socket_t client)
{
	char request[MAX_REQUEST + 1];
	size_t have = 0;
	const char *body = NULL;

	/* On Windows the accepted socket inherits the listener's
	 * non-blocking mode; this handler needs blocking reads. */
	net_set_blocking(client);
	set_timeouts(client, 2);

	/* Read until end of headers. */
	while (have < MAX_REQUEST) {
		int n = (int)recv(client, request + have,
				  (int)(MAX_REQUEST - have), 0);
		if (n <= 0)
			return;
		have += (size_t)n;
		request[have] = 0;
		const char *end = strstr(request, "\r\n\r\n");
		if (end) {
			body = end + 4;
			break;
		}
	}
	if (!body)
		return;

	if (!request_is_local(request)) {
		respond(client, "403 Forbidden", "text/plain", "forbidden");
		return;
	}

	if (strncmp(request, "GET / ", 6) == 0) {
		respond(client, "200 OK", "text/html; charset=utf-8",
			control_page);
		return;
	}

	if (strncmp(request, "GET /api/state ", 15) == 0) {
		char state[1024] = {0};
		ios_camera_copy_state(wc->source, state, sizeof(state));
		respond(client, "200 OK", "application/json", state);
		return;
	}

	if (strncmp(request, "GET /api/status ", 16) == 0) {
		char status[256] = {0};
		char escaped[512] = {0};
		ios_camera_copy_status(wc->source, status, sizeof(status));
		json_escape(status, escaped, sizeof(escaped));

		char json[600];
		snprintf(json, sizeof(json),
			 "{\"status\":\"%s\",\"screen\":%s,\"standby\":%s}",
			 escaped,
			 ios_camera_is_screen(wc->source) ? "true" : "false",
			 ios_camera_is_standby(wc->source) ? "true" : "false");
		respond(client, "200 OK", "application/json", json);
		return;
	}

	if (strncmp(request, "POST /api/control", 17) == 0) {
		const char *cl = find_header(request, "Content-Length");
		size_t content_length = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
		if (content_length == 0 || content_length > MAX_CONTROL_BODY) {
			respond(client, "400 Bad Request", "text/plain",
				"bad length");
			return;
		}

		size_t body_offset = (size_t)(body - request);
		while (have - body_offset < content_length &&
		       have < MAX_REQUEST) {
			int n = (int)recv(client, request + have,
					  (int)(MAX_REQUEST - have), 0);
			if (n <= 0)
				return;
			have += (size_t)n;
			request[have] = 0;
		}
		/* The refill loop can also exit because the buffer is full
		 * (headers padded near MAX_REQUEST); enqueuing then would
		 * read past the received bytes — off the end of `request`. */
		if (have - body_offset < content_length) {
			respond(client, "400 Bad Request", "text/plain",
				"truncated body");
			return;
		}

		ios_camera_enqueue_control(wc->source, request + body_offset,
					   content_length);
		respond(client, "204 No Content", "text/plain", NULL);
		return;
	}

	respond(client, "404 Not Found", "text/plain", "not found");
}

static void *web_thread(void *data)
{
	struct web_control *wc = data;

	os_set_thread_name("ios-camera-web");

	while (!wc->stop) {
		int ret = net_wait(wc->listener, NET_WAIT_READ, 200);
		if (ret < 0)
			break;
		if (ret == 0)
			continue;

		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		socket_t client = accept(wc->listener,
					 (struct sockaddr *)&from, &from_len);
		if (client == OBSC_INVALID_SOCKET)
			continue;

		handle_client(wc, client);
		net_close(client);
	}

	return NULL;
}

struct web_control *web_control_start(struct ios_camera_source *source,
				      uint16_t port)
{
	socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener == OBSC_INVALID_SOCKET)
		return NULL;

	int yes = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
		   sizeof(yes));

	/* Local machine only — this is a control surface. */
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);

	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(listener, 4) != 0) {
		blog(LOG_WARNING,
		     "[lenslink] web control: port %u unavailable "
		     "(another LensLink Camera source running?)",
		     (unsigned)port);
		net_close(listener);
		return NULL;
	}

	net_set_nonblocking(listener);

	struct web_control *wc = bzalloc(sizeof(*wc));
	wc->listener = listener;
	wc->port = port;
	wc->source = source;

	if (pthread_create(&wc->thread, NULL, web_thread, wc) != 0) {
		net_close(listener);
		bfree(wc);
		return NULL;
	}

	blog(LOG_INFO,
	     "[lenslink] web control panel at http://localhost:%u/",
	     (unsigned)port);
	return wc;
}

void web_control_stop(struct web_control *wc)
{
	if (!wc)
		return;
	wc->stop = true;
	pthread_join(wc->thread, NULL);
	net_close(wc->listener);
	bfree(wc);
}
