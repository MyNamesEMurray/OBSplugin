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
#include "plugin-settings.h"

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
	/* color-scheme makes the browser's own chrome — most visibly the
	 * native dropdown popup of a <select> — render dark; without it the
	 * popup was a light list inheriting white text (invisible until
	 * hovered). The explicit option colors cover browsers that style
	 * options directly instead. */
	":root{color-scheme:dark;--accent:#3D7BFF;--live:#30D158;--amber:#FF9F0A;"
	"--red:#FF453A;--grey:#8E8E93;--bg:#0E0F13;--glass:rgba(28,30,38,0.72);"
	"--hair:rgba(255,255,255,0.08);--txt:#fff;--txt2:rgba(235,235,245,0.6)}"
	"option{background:#1c1e26;color:#fff}"
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
	/* Two-button action rows; the auto-start toggle reads like a chip:
	 * grey when off, accent when on. */
	".btnrow{display:flex;gap:10px;margin-top:14px}"
	".btnrow:first-child{margin-top:0}"
	".primary.toggle{background:rgba(255,255,255,.12)}"
	".primary.toggle.on{background:var(--accent)}"
	".lbl{font-size:13px;color:var(--txt2);width:48px;flex:none}"
	"</style></head><body>"
	"<header><h1>LensLink</h1>"
	"<div class='pill'><span class='dot' id='dot'></span>"
	"<span id='status'>connecting&hellip;</span></div></header>"
	/* Source tabs: hidden until more than one camera source is live.
	 * Every API call carries the selected source's ?src= id. */
	"<div class='seg' id='srctabs' "
	"style='display:none;margin-bottom:12px;flex-wrap:wrap'></div>"
	/* Shown instead of the controls for a screen-mirror source. */
	"<div class='panel' id='screennote' style='display:none'>"
	"Screen mirroring &mdash; camera controls don&rsquo;t apply.</div>"
	/* Remote start: the app is open but idle; one tap starts the camera. */
	"<div class='panel' id='startpanel' style='display:none'>"
	"<div class='btnrow'>"
	"<button class='primary' id='startbtn'>Start camera</button>"
	"<button class='primary toggle' id='asbtn1' "
	"title='Start the camera automatically whenever the app is ready'>"
	"Auto-start</button></div>"
	"<div class='hint' style='margin-top:12px'>The LensLink app is open "
	"and ready &mdash; the camera hasn&rsquo;t been started yet.</div></div>"
	/* Hidden until the first poll confirms a live camera connection —
	 * a default-visible panel flashed dead sliders and a Stop button
	 * before any state was known. */
	"<div class='panel' id='panel' style='display:none'>"
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
	/* AE/Manual toggle: shown when the app reports manual-exposure support. */
	"<div class='row' id='emoderow' style='display:none'>"
	"<div class='seg'><button id='ae' class='on'>AE</button>"
	"<button id='me'>Manual</button></div>"
	"<span class='hint'>Exposure</span></div>"
	"<div class='row' id='biasrow'>"
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
	/* Manual exposure (ISO + shutter) and white-balance rows; hidden until
	 * the app's STATE says the camera supports them. */
	"<div class='row' id='isorow' style='display:none'>"
	"<span class='lbl'>ISO</span>"
	"<input id='iso' type='range' min='34' max='3072' step='1'>"
	"<span class='ro' id='isov'>100</span></div>"
	"<div class='row' id='shutrow' style='display:none'>"
	"<span class='lbl'>Shutter</span>"
	"<input id='shut' type='range' min='0' max='1' step='0.01'>"
	"<span class='ro' id='shutv'>1/60</span></div>"
	"<div class='row' id='wbrow' style='display:none'>"
	"<div class='seg'><button id='awb' class='on'>AWB</button>"
	"<button id='wbl'>Lock</button></div>"
	"<input id='wbtemp' type='range' min='2500' max='8000' step='100' "
	"style='display:none'>"
	"<span class='ro' id='wbv' style='display:none'>5000K</span>"
	"<span class='hint' id='wbhint'>Auto white balance</span></div>"
	/* Mic picker: shown while the app streams its mic as the source's
	 * audio (STATE micEnabled) — mirrors the app's mic row. */
	"<div class='row' id='microw' style='display:none'>"
	"<span class='lbl'>Mic</span>"
	"<select id='micsel' title='Phone microphone'></select>"
	"<span class='hint'>Phone microphone</span></div>"
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
	/* Format row (resolution / fps / codec): shown once the app's STATE
	 * advertises its capability lists; older apps just never show it. */
	"<div class='row' id='fmtrow' style='display:none'>"
	"<select id='fmtres' title='Resolution'></select>"
	"<select id='fmtfps' title='Frame rate'></select>"
	"<select id='fmtcodec' title='Codec'></select>"
	"</div>"
	/* Remote stop: mirrors the app's red Stop. The phone drops back to
	 * standby, so this panel swaps to the Start button afterwards. */
	"<div class='btnrow'>"
	"<button class='primary danger' id='stopbtn'>Stop camera</button>"
	"<button class='primary toggle' id='asbtn2' "
	"title='Start the camera automatically whenever the app is ready'>"
	"Auto-start</button></div>"
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
	"fmtrowEl=$('fmtrow'),fmtresEl=$('fmtres'),fmtfpsEl=$('fmtfps'),"
	"fmtcodecEl=$('fmtcodec'),stopbtnEl=$('stopbtn'),"
	"asbtn1El=$('asbtn1'),asbtn2El=$('asbtn2'),"
	"emoderowEl=$('emoderow'),aeEl=$('ae'),meEl=$('me'),biasrowEl=$('biasrow'),"
	"isorowEl=$('isorow'),isoEl=$('iso'),isovEl=$('isov'),"
	"shutrowEl=$('shutrow'),shutEl=$('shut'),shutvEl=$('shutv'),"
	"wbrowEl=$('wbrow'),awbEl=$('awb'),wblEl=$('wbl'),wbtempEl=$('wbtemp'),"
	"wbvEl=$('wbv'),wbhintEl=$('wbhint'),"
	"microwEl=$('microw'),micselEl=$('micsel'),srctabsEl=$('srctabs');"
	"const COL={live:'#30D158',amber:'#FF9F0A',red:'#FF453A',grey:'#8E8E93'};"
	/* Selected source id (from /api/sources); every request carries it. */
	"let src=null;const q=()=>src==null?'':('?src='+src);"
	"let lastTouch=0;const touch=()=>lastTouch=Date.now();"
	"const send=o=>{touch();"
	"fetch('/api/control'+q(),{method:'POST',body:JSON.stringify(o)})};"
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
	/* Auto-start toggle: writes the source's own auto-start property
	 * (the checkbox in the source properties), not a phone control. */
	"let autoStart=false;"
	"function asUI(on){autoStart=on;const c=on?'primary toggle on':'primary toggle';"
	"asbtn1El.className=c;asbtn2El.className=c}"
	"const toggleAS=()=>{touch();asUI(!autoStart);"
	"fetch('/api/autostart'+q(),{method:'POST',body:JSON.stringify({on:autoStart})})};"
	"asbtn1El.onclick=toggleAS;asbtn2El.onclick=toggleAS;"
	/* Rebuild a select only when its option list changes (same pattern as
	 * the lens picker); values stay raw, labels get a formatter. */
	"function fillSel(el,opts,val,fmt){const want=opts.join('|');"
	"if(el.dataset.opts!==want){el.dataset.opts=want;"
	"el.replaceChildren(...opts.map(o=>{const x=document.createElement('option');"
	"x.value=o;x.textContent=fmt?fmt(o):o;return x}))}"
	"if(val!=null)el.value=val}"
	"const CODEC_NAMES={h264:'H.264',hevc:'HEVC'};"
	"fmtresEl.onchange=()=>send({cmd:'set_format',resolution:fmtresEl.value});"
	"fmtfpsEl.onchange=()=>send({cmd:'set_format',fps:+fmtfpsEl.value});"
	"fmtcodecEl.onchange=()=>send({cmd:'set_format',codec:fmtcodecEl.value});"
	/* Manual exposure. The shutter slider is 0..1 on a log scale (left =
	 * long/slow, right = short/fast), mapped with the app's formula. */
	"let smin=1/8000,smax=1/30,eman=false,wblocked=false;"
	"const shutSecs=()=>Math.exp(Math.log(smax)-(+shutEl.value)*"
	"(Math.log(smax)-Math.log(smin)));"
	"const shutPos=s=>{s=Math.min(Math.max(s,smin),smax);"
	"return 1-(Math.log(s)-Math.log(smin))/(Math.log(smax)-Math.log(smin))};"
	"const shutLabel=s=>s>=1?Math.round(s)+'s':'1/'+Math.round(1/s);"
	"function emodeUI(m){eman=m;aeEl.className=m?'':'on';"
	"meEl.className=m?'on':'';biasrowEl.style.display=m?'none':'';"
	"isorowEl.style.display=m?'':'none';shutrowEl.style.display=m?'':'none'}"
	"aeEl.onclick=()=>{touch();emodeUI(false);send({cmd:'exposure',mode:'auto'})};"
	"meEl.onclick=()=>{touch();emodeUI(true);send({cmd:'exposure',"
	"mode:'manual',iso:+isoEl.value,shutterSeconds:shutSecs()})};"
	"const dIso=deb(()=>send({cmd:'exposure',mode:'manual',iso:+isoEl.value}),60);"
	"isoEl.oninput=()=>{touch();isovEl.textContent=isoEl.value;dIso()};"
	"const dShut=deb(()=>send({cmd:'exposure',mode:'manual',"
	"shutterSeconds:shutSecs()}),60);"
	"shutEl.oninput=()=>{touch();shutvEl.textContent=shutLabel(shutSecs());dShut()};"
	/* White balance. */
	"function wbUI(locked){wblocked=locked;awbEl.className=locked?'':'on';"
	"wblEl.className=locked?'on':'';wbtempEl.style.display=locked?'':'none';"
	"wbvEl.style.display=locked?'':'none';wbhintEl.style.display=locked?'none':''}"
	"awbEl.onclick=()=>{touch();wbUI(false);send({cmd:'white_balance',mode:'auto'})};"
	"wblEl.onclick=()=>{touch();wbUI(true);send({cmd:'white_balance',"
	"mode:'locked',temperature:+wbtempEl.value})};"
	"const dWb=deb(()=>send({cmd:'white_balance',mode:'locked',"
	"temperature:+wbtempEl.value}),60);"
	"wbtempEl.oninput=()=>{touch();wbvEl.textContent=wbtempEl.value+'K';dWb()};"
	"micselEl.onchange=()=>send({cmd:'mic',id:micselEl.value});"
	"function statusColor(t){t=(t||'').toLowerCase();"
	/* Standby/starting are amber (ready, not live) — test before the
	 * generic 'connected' match, which their wording also contains. */
	"if(t.includes('idle')||t.includes('starting'))return COL.amber;"
	"if(t.includes('connected'))return COL.live;"
	"if(t.includes('disconnect')||t.includes('could not')||t.includes('error'))return COL.red;"
	"if(t.includes('wait')||t.includes('trying')||t.includes('dial'))return COL.amber;"
	"return COL.grey}"
	"async function poll(){try{"
	/* Source list first: pick/keep a selection, tabs when >1. */
	"const sj=await(await fetch('/api/sources')).json();"
	"const list=sj.sources||[];"
	"if(!list.length){statusEl.textContent='no LensLink sources';"
	"dotEl.style.background=COL.grey;srctabsEl.style.display='none';"
	"panelEl.style.display='none';startpanelEl.style.display='none';"
	"screennoteEl.style.display='none';return}"
	"if(src==null||!list.some(x=>x.id===src))src=list[0].id;"
	"srctabsEl.style.display=list.length>1?'':'none';"
	"const sk=list.map(x=>x.id+':'+x.name).join('|')+'@'+src;"
	/* textContent, not innerHTML: source names are user data. */
	"if(srctabsEl.dataset.k!==sk){srctabsEl.dataset.k=sk;"
	"srctabsEl.replaceChildren(...list.map(x=>{"
	"const b=document.createElement('button');b.textContent=x.name;"
	"b.className=x.id===src?'on':'';"
	"b.onclick=()=>{src=x.id;lastTouch=0;poll()};return b}))}"
	"const s=await(await fetch('/api/status'+q())).json();"
	"statusEl.textContent=s.status||'idle';dotEl.style.background=statusColor(s.status);"
	/* Live controls only when a camera stream is actually connected;
	 * standby gets the Start panel; screen mirror gets the note; not
	 * connected shows just the status pill. */
	"panelEl.style.display=(s.connected&&!s.screen&&!s.standby)?'':'none';"
	"screennoteEl.style.display=s.screen?'':'none';"
	"startpanelEl.style.display=(s.standby&&!s.screen)?'':'none';"
	"if(typeof s.autoStart==='boolean'&&Date.now()-lastTouch>2000)"
	"asUI(s.autoStart);"
	"if(s.screen||s.standby||!s.connected)return;"
	"const st=await(await fetch('/api/state'+q())).json();"
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
	"lensselEl.style.display=(st.lenses&&st.lenses.length>1)?'':'none';"
	/* Manual exposure + white balance, mirrored from the app. */
	"emoderowEl.style.display=st.supportsManualExposure?'':'none';"
	"if(st.supportsManualExposure){emodeUI(st.exposureMode==='manual');"
	"if(typeof st.minISO==='number'){isoEl.min=Math.round(st.minISO);"
	"isoEl.max=Math.round(st.maxISO)}"
	"if(typeof st.iso==='number'){isoEl.value=Math.round(st.iso);"
	"isovEl.textContent=Math.round(st.iso)}"
	"if(typeof st.minShutterSeconds==='number')"
	"smin=Math.max(st.minShutterSeconds,1/8000);"
	"if(typeof st.maxShutterSeconds==='number')"
	"smax=Math.max(st.maxShutterSeconds,smin*2);"
	"if(typeof st.shutterSeconds==='number'){"
	"shutEl.value=shutPos(st.shutterSeconds);"
	"shutvEl.textContent=shutLabel(st.shutterSeconds)}}"
	"else emodeUI(false);"
	"wbrowEl.style.display=st.supportsWhiteBalanceLock?'':'none';"
	"if(st.supportsWhiteBalanceLock){wbUI(st.whiteBalanceMode==='locked');"
	"if(typeof st.whiteBalanceTemperature==='number'){"
	"wbtempEl.value=Math.round(st.whiteBalanceTemperature);"
	"wbvEl.textContent=Math.round(st.whiteBalanceTemperature)+'K'}}"
	/* Mic picker, only while the phone mic is live as source audio.
	 * Options are {id,name} pairs: ids round-trip, names display. */
	"microwEl.style.display=st.micEnabled?'':'none';"
	"if(st.micEnabled&&Array.isArray(st.mics)){const mn={};"
	"st.mics.forEach(m=>mn[m.id]=m.name);"
	"fillSel(micselEl,st.mics.map(m=>m.id),st.mic,i=>mn[i]||i)}"
	/* Format pickers, populated from the app's capability lists. */
	"if(Array.isArray(st.resolutions)&&Array.isArray(st.frameRates)){"
	"fmtrowEl.style.display='';"
	"fillSel(fmtresEl,st.resolutions,st.resolution);"
	"fillSel(fmtfpsEl,st.frameRates.map(String),String(st.fps),f=>f+' fps');"
	"const codecs=Array.isArray(st.codecs)?st.codecs:[];"
	"fmtcodecEl.style.display=codecs.length>1?'':'none';"
	"fillSel(fmtcodecEl,codecs,st.codec,c=>CODEC_NAMES[c]||c)}}"
	"}catch(e){statusEl.textContent='plugin unreachable';dotEl.style.background=COL.grey}}"
	"setInterval(poll,1000);poll();"
	"</script></body></html>";

struct web_control {
	pthread_t thread;
	volatile bool stop;
	socket_t listener;
	uint16_t port;
};

/* One server, many sources. The registry maps small stable ids (what
 * ?src= carries) to live sources; handlers resolve a source and use it
 * entirely under the mutex, so once unregister returns, no request can
 * touch that source again. Registration happens on the UI thread;
 * lookups on the (single) web thread. */
#define WC_MAX_SOURCES 16

static struct {
	pthread_mutex_t mutex;
	struct web_control *server;
	struct {
		int id;
		struct ios_camera_source *src;
	} entries[WC_MAX_SOURCES];
	size_t count;
	int next_id;
} g_reg = {.mutex = PTHREAD_MUTEX_INITIALIZER, .next_id = 1};

/* ?src=<id> on the request line picks a source; no (or an unknown) id
 * falls back to the first registered one, so single-source setups and
 * scripts written before multi-source keep working unchanged. Caller
 * holds g_reg.mutex. */
static struct ios_camera_source *locked_pick_source(const char *request)
{
	const char *nl = strpbrk(request, "\r\n");
	const char *q = strstr(request, "src=");
	if (q && nl && q < nl) {
		int id = atoi(q + 4);
		for (size_t i = 0; i < g_reg.count; i++)
			if (g_reg.entries[i].id == id)
				return g_reg.entries[i].src;
	}
	return g_reg.count ? g_reg.entries[0].src : NULL;
}

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

static void handle_client(socket_t client)
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

	/* For POSTs, pull the whole body in before touching the source
	 * registry — a slow client must never stall register/unregister
	 * (the OBS UI thread) on our socket reads. */
	size_t body_offset = (size_t)(body - request);
	size_t content_length = 0;
	if (strncmp(request, "POST ", 5) == 0) {
		const char *cl = find_header(request, "Content-Length");
		content_length = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
		if (content_length == 0 || content_length > MAX_CONTROL_BODY) {
			respond(client, "400 Bad Request", "text/plain",
				"bad length");
			return;
		}
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
	}

	if (!request_is_local(request)) {
		respond(client, "403 Forbidden", "text/plain", "forbidden");
		return;
	}

	if (strncmp(request, "GET / ", 6) == 0) {
		respond(client, "200 OK", "text/html; charset=utf-8",
			control_page);
		return;
	}

	if (strncmp(request, "GET /api/sources", 16) == 0) {
		char json[4096];
		size_t o = (size_t)snprintf(json, sizeof(json),
					    "{\"sources\":[");
		pthread_mutex_lock(&g_reg.mutex);
		for (size_t i = 0; i < g_reg.count && o + 384 < sizeof(json);
		     i++) {
			struct ios_camera_source *s = g_reg.entries[i].src;
			char name[128], esc[280];
			ios_camera_copy_name(s, name, sizeof(name));
			json_escape(name, esc, sizeof(esc));
			o += (size_t)snprintf(
				json + o, sizeof(json) - o,
				"%s{\"id\":%d,\"name\":\"%s\","
				"\"connected\":%s,\"standby\":%s,"
				"\"screen\":%s}",
				i ? "," : "", g_reg.entries[i].id, esc,
				ios_camera_is_connected(s) ? "true" : "false",
				ios_camera_is_standby(s) ? "true" : "false",
				ios_camera_is_screen(s) ? "true" : "false");
		}
		pthread_mutex_unlock(&g_reg.mutex);
		snprintf(json + o, sizeof(json) - o, "]}");
		respond(client, "200 OK", "application/json", json);
		return;
	}

	if (strncmp(request, "GET /api/state", 14) == 0) {
		char state[1024] = {0};
		pthread_mutex_lock(&g_reg.mutex);
		struct ios_camera_source *s = locked_pick_source(request);
		if (s)
			ios_camera_copy_state(s, state, sizeof(state));
		pthread_mutex_unlock(&g_reg.mutex);
		if (!s) {
			respond(client, "503 Service Unavailable",
				"text/plain", "no sources");
			return;
		}
		respond(client, "200 OK", "application/json", state);
		return;
	}

	if (strncmp(request, "GET /api/status", 15) == 0) {
		char status[256] = {0};
		char escaped[512] = {0};
		char json[768];
		bool screen = false, standby = false, connected = false;
		bool auto_start = false;

		pthread_mutex_lock(&g_reg.mutex);
		struct ios_camera_source *s = locked_pick_source(request);
		if (s) {
			ios_camera_copy_status(s, status, sizeof(status));
			screen = ios_camera_is_screen(s);
			standby = ios_camera_is_standby(s);
			connected = ios_camera_is_connected(s);
			auto_start = ios_camera_auto_start(s);
		}
		pthread_mutex_unlock(&g_reg.mutex);
		if (!s) {
			respond(client, "503 Service Unavailable",
				"text/plain", "no sources");
			return;
		}
		json_escape(status, escaped, sizeof(escaped));
		snprintf(json, sizeof(json),
			 "{\"status\":\"%s\",\"screen\":%s,\"standby\":%s,"
			 "\"connected\":%s,\"autoStart\":%s}",
			 escaped, screen ? "true" : "false",
			 standby ? "true" : "false",
			 connected ? "true" : "false",
			 auto_start ? "true" : "false");
		respond(client, "200 OK", "application/json", json);
		return;
	}

	if (strncmp(request, "POST /api/autostart", 19) == 0) {
		/* Body: {"on":true|false}. Toggles the source's auto-start
		 * property (same value the properties checkbox edits). */
		pthread_mutex_lock(&g_reg.mutex);
		struct ios_camera_source *s = locked_pick_source(request);
		if (s)
			ios_camera_set_auto_start(
				s,
				strstr(request + body_offset, "true") != NULL);
		pthread_mutex_unlock(&g_reg.mutex);
		respond(client, s ? "204 No Content" : "503 Service Unavailable",
			"text/plain", s ? NULL : "no sources");
		return;
	}

	if (strncmp(request, "POST /api/control", 17) == 0) {
		pthread_mutex_lock(&g_reg.mutex);
		struct ios_camera_source *s = locked_pick_source(request);
		if (s)
			ios_camera_enqueue_control(s, request + body_offset,
						   content_length);
		pthread_mutex_unlock(&g_reg.mutex);
		respond(client, s ? "204 No Content" : "503 Service Unavailable",
			"text/plain", s ? NULL : "no sources");
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

		handle_client(client);
		net_close(client);
	}

	return NULL;
}

static struct web_control *server_start(uint16_t port)
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
		     "[lenslink] web control: port %u unavailable — is "
		     "another app using it? The port can be changed in "
		     "Tools → LensLink Settings",
		     (unsigned)port);
		net_close(listener);
		return NULL;
	}

	net_set_nonblocking(listener);

	struct web_control *wc = bzalloc(sizeof(*wc));
	wc->listener = listener;
	wc->port = port;

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

static void server_stop(struct web_control *wc)
{
	if (!wc)
		return;
	wc->stop = true;
	pthread_join(wc->thread, NULL);
	net_close(wc->listener);
	bfree(wc);
}

/* Reconcile the singleton server with the plugin-wide settings and the
 * registry: running while (enabled && any source), on the settings'
 * port. The server to stop is detached under the mutex but joined
 * outside it — a request handler blocked on g_reg.mutex must be able to
 * finish, or the join would deadlock. */
void web_control_apply_settings(void)
{
	struct web_control *to_stop = NULL;
	uint16_t port = (uint16_t)lenslink_settings_web_port();

	pthread_mutex_lock(&g_reg.mutex);
	bool want = lenslink_settings_web_enabled() && g_reg.count > 0;
	if (g_reg.server && (!want || g_reg.server->port != port)) {
		to_stop = g_reg.server;
		g_reg.server = NULL;
	}
	bool need_start = want && !g_reg.server;
	pthread_mutex_unlock(&g_reg.mutex);

	if (to_stop)
		server_stop(to_stop);
	if (!need_start)
		return;

	struct web_control *wc = server_start(port);
	if (!wc)
		return;
	pthread_mutex_lock(&g_reg.mutex);
	if (!g_reg.server && g_reg.count > 0) {
		g_reg.server = wc;
		wc = NULL;
	}
	pthread_mutex_unlock(&g_reg.mutex);
	if (wc) /* raced with another apply, or the registry emptied */
		server_stop(wc);
}

void web_control_register(struct ios_camera_source *source)
{
	pthread_mutex_lock(&g_reg.mutex);
	for (size_t i = 0; i < g_reg.count; i++) {
		if (g_reg.entries[i].src == source) {
			pthread_mutex_unlock(&g_reg.mutex);
			return;
		}
	}
	if (g_reg.count < WC_MAX_SOURCES) {
		g_reg.entries[g_reg.count].id = g_reg.next_id++;
		g_reg.entries[g_reg.count].src = source;
		g_reg.count++;
	} else {
		blog(LOG_WARNING,
		     "[lenslink] web control: more than %d sources; "
		     "extra sources won't appear in the panel",
		     WC_MAX_SOURCES);
	}
	pthread_mutex_unlock(&g_reg.mutex);
}

void web_control_unregister(struct ios_camera_source *source)
{
	pthread_mutex_lock(&g_reg.mutex);
	for (size_t i = 0; i < g_reg.count; i++) {
		if (g_reg.entries[i].src == source) {
			memmove(&g_reg.entries[i], &g_reg.entries[i + 1],
				(g_reg.count - i - 1) *
					sizeof(g_reg.entries[0]));
			g_reg.count--;
			break;
		}
	}
	pthread_mutex_unlock(&g_reg.mutex);
}
