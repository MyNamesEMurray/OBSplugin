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

/* Single-quoted HTML attributes keep the C escaping sane. */
static const char control_page[] =
	"<!doctype html><html><head><meta charset='utf-8'>"
	"<meta name='viewport' content='width=device-width,initial-scale=1'>"
	"<title>iOS Camera Control</title><style>"
	"body{font-family:system-ui;background:#14161a;color:#e8eaee;"
	"max-width:420px;margin:24px auto;padding:0 16px}"
	"h2{font-weight:600}#status{padding:8px 0 16px;color:#7ee07e;font-size:14px}"
	".row{margin:16px 0}label{display:block;font-size:13px;color:#9aa7bd;margin-bottom:6px}"
	"input[type=range]{width:100%}"
	"button{background:#2a6cf4;color:#fff;border:0;border-radius:8px;"
	"padding:10px 16px;margin:0 8px 0 0;font-size:14px}"
	"button.off{background:#3a3f4a}"
	"</style></head><body>"
	"<h2>iOS Camera</h2><div id='status'>connecting…</div>"
	"<div class='row'><label>Zoom <span id='zv'>1.0×</span></label>"
	"<input id='zoom' type='range' min='1' max='10' step='0.1' value='1'></div>"
	"<div class='row'><label>Exposure <span id='ev'>0.0</span> EV</label>"
	"<input id='exposure' type='range' min='-2' max='2' step='0.1' value='0'></div>"
	"<div class='row'><label>Focus</label>"
	"<button id='af'>Auto</button><button id='mf' class='off'>Locked</button>"
	"<div style='margin-top:8px'><input id='lens' type='range' min='0' max='1' "
	"step='0.01' value='0.5' disabled></div></div>"
	"<div class='row'><button id='torch' class='off'>Torch</button>"
	"<button id='flip'>Flip camera</button></div>"
	"<script>"
	"const send=o=>fetch('/api/control',{method:'POST',body:JSON.stringify(o)});"
	"const deb=(f,ms)=>{let t;return(...a)=>{clearTimeout(t);t=setTimeout(()=>f(...a),ms)}};"
	"const dz=deb(()=>send({cmd:'zoom',value:+zoom.value}),60);"
	"zoom.oninput=()=>{zv.textContent=(+zoom.value).toFixed(1)+'\\u00d7';dz()};"
	"const de=deb(()=>send({cmd:'exposure_bias',value:+exposure.value}),60);"
	"exposure.oninput=()=>{ev.textContent=(+exposure.value).toFixed(1);de()};"
	"af.onclick=()=>{af.className='';mf.className='off';lens.disabled=true;"
	"send({cmd:'focus',mode:'auto'})};"
	"mf.onclick=()=>{mf.className='';af.className='off';lens.disabled=false;"
	"send({cmd:'focus',mode:'locked',lensPosition:+lens.value})};"
	"lens.oninput=deb(()=>send({cmd:'focus',mode:'locked',lensPosition:+lens.value}),60);"
	"let ton=false;"
	"torch.onclick=()=>{ton=!ton;torch.className=ton?'':'off';send({cmd:'torch',on:ton})};"
	"flip.onclick=()=>send({cmd:'flip'});"
	"setInterval(async()=>{try{const r=await fetch('/api/status');"
	"const j=await r.json();status.textContent=j.status}"
	"catch(e){status.textContent='plugin unreachable'}},1000);"
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

	if (strncmp(request, "GET / ", 6) == 0) {
		respond(client, "200 OK", "text/html; charset=utf-8",
			control_page);
		return;
	}

	if (strncmp(request, "GET /api/status ", 16) == 0) {
		char status[256] = {0};
		char escaped[512] = {0};
		ios_camera_copy_status(wc->source, status, sizeof(status));
		json_escape(status, escaped, sizeof(escaped));

		char json[600];
		snprintf(json, sizeof(json), "{\"status\":\"%s\"}", escaped);
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
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(wc->listener, &fds);
		struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
		int ret = select((int)wc->listener + 1, &fds, NULL, NULL, &tv);
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
		     "[ios-camera] web control: port %u unavailable "
		     "(another iOS Camera source running?)",
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
	     "[ios-camera] web control panel at http://localhost:%u/",
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
