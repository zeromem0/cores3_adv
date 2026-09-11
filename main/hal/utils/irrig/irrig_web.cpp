/*
 * SPDX-License-Identifier: MIT
 */
#include "irrig_web.h"

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <hal/utils/remoted/remoted.h>
#include <mooncake_log.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "irrig.h"

namespace irrig_web {

namespace {

const std::string _tag = "irrig_web";

/* What the browser last sent, waiting for the main loop to take it. */
SemaphoreHandle_t _pending_lock;
std::string _pending;
bool _has_pending;

esp_err_t send(httpd_req_t* req, const char* text)
{
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

/* -------------------------------------------------------------------------- */
/*                                   The page                                  */
/* -------------------------------------------------------------------------- */

/* Delimited: the page is full of )" pairs -- every onclick with an empty
 * argument list ends one -- and a bare raw string would stop at the
 * first of them. */
const char kHead[] = R"HTML(<!DOCTYPE html><html lang="ro"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Irigare</title><style>
:root{--primary:#6e5ce7;--gray:#999}
body{font-family:'Segoe UI',sans-serif;background:#f0f2f5;margin:0;padding:20px}
h2{text-align:center;margin:10px 0 15px}
.tabs{display:flex;gap:8px;justify-content:center;flex-wrap:wrap;max-width:560px;margin:0 auto}
.tab{padding:12px 24px;background:#fff;border:none;border-radius:12px 12px 0 0;cursor:pointer;
font-weight:bold;color:#555;box-shadow:0 2px 6px rgba(0,0,0,.1)}
.tab.active{background:var(--primary);color:#fff}
.container{display:none;flex-direction:column;gap:15px;align-items:center}
.container.active{display:flex}
.card{background:#fff;border-radius:20px;padding:18px 25px;width:100%;max-width:560px;display:flex;
justify-content:space-between;align-items:center;box-shadow:0 4px 15px rgba(0,0,0,.06);
margin:0 auto;box-sizing:border-box}
.card.disabled{opacity:.5}
.time-display{display:flex;align-items:center;font-size:28px;color:#333}
.time-group{display:flex;align-items:center}
.time-group input{border:none;width:50px;font-size:28px;text-align:center;outline:none;
background:transparent;font-family:inherit}
.dash{margin:0 12px;color:var(--gray);font-weight:300}
.controls{display:flex;align-items:center;gap:15px}
.days{display:flex;gap:7px;color:var(--primary);font-weight:bold;font-size:14px}
.day{cursor:pointer;width:20px;text-align:center}
.day.inactive{color:#ddd}
.switch{width:42px;height:24px;position:relative;display:inline-block;flex-shrink:0}
.switch input{opacity:0;width:0;height:0}
.slider{position:absolute;cursor:pointer;inset:0;background:#e0e0e0;border-radius:24px;transition:.3s}
.slider:before{position:absolute;content:"";height:18px;width:18px;left:3px;bottom:3px;background:#fff;
border-radius:50%;transition:.3s}
input:checked+.slider{background:var(--primary)}
input:checked+.slider:before{transform:translateX(18px)}
button{padding:16px 40px;background:var(--primary);color:#fff;border:none;border-radius:12px;
font-size:17px;cursor:pointer;margin:25px auto;display:block}
#status{text-align:center;color:#555;min-height:20px}
#screen{display:block;margin:0 auto 20px;border:3px solid #333;border-radius:8px;background:#000;
image-rendering:pixelated;width:100%;max-width:480px}
#screenmsg{text-align:center;color:#999;font-size:13px;min-height:18px;margin-bottom:10px}
</style></head><body><h2>Program irigare</h2>
<canvas id="screen" width="240" height="135"></canvas><div id="screenmsg"></div>
<div class="tabs" id="tabs"></div><div id="zones"></div>
<button onclick="saveAll()">SAVE</button><div id="status"></div><script>
const schedules=[)HTML";

const char kTail[] = R"HTML(];
function createZone(idx,dataStr){
 const slots=dataStr.split('|');let h=`<div class="container" id="zone${idx}">`;
 slots.forEach(s=>{const p=s.split(',');const a=p[0].split(':');const b=p[1].split(':');
 const days=p[2];const on=p[3]==='A';
 h+=`<div class="card ${on?'':'disabled'}">
 <div class="time-display">
 <div class="time-group"><input class="h1" value="${a[0]}" maxlength="2">:<input class="m1" value="${a[1]}" maxlength="2"></div>
 <span class="dash">-</span>
 <div class="time-group"><input class="h2" value="${b[0]}" maxlength="2">:<input class="m2" value="${b[1]}" maxlength="2"></div>
 </div><div class="controls"><div class="days">
 ${['L','M','M','J','V','S','D'].map((d,k)=>`<span class="day ${days[k]==='-'?'inactive':''}" data-v="${d}">${d}</span>`).join('')}
 </div><label class="switch"><input type="checkbox" class="toggle" ${on?'checked':''}><span class="slider"></span></label>
 </div></div>`;});
 return h+'</div>';}
const letters=['A','B','C','D'];let t='';
for(let i=0;i<schedules.length;i++)t+=`<button class="tab ${i?'':'active'}" data-tab="${i}">Zone ${letters[i]}</button>`;
document.getElementById('tabs').innerHTML=t;
let z='';for(let i=0;i<schedules.length;i++)z+=createZone(i,schedules[i]);
document.getElementById('zones').innerHTML=z;
document.querySelectorAll('.tab').forEach(tab=>tab.addEventListener('click',()=>{
 document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));tab.classList.add('active');
 document.querySelectorAll('.container').forEach(c=>c.classList.remove('active'));
 document.getElementById('zone'+tab.dataset.tab).classList.add('active');}));
document.getElementById('zone0').classList.add('active');
document.querySelectorAll('.day').forEach(d=>d.addEventListener('click',()=>d.classList.toggle('inactive')));
document.querySelectorAll('.toggle').forEach(t=>t.addEventListener('change',()=>
 t.closest('.card').classList.toggle('disabled',!t.checked)));
function collect(){let out=[];
 document.querySelectorAll('.container').forEach(zone=>{
  zone.querySelectorAll('.card').forEach(c=>{
   const p=n=>c.querySelector(n).value.padStart(2,'0');
   let d='';c.querySelectorAll('.day').forEach(x=>d+=x.classList.contains('inactive')?'-':x.dataset.v);
   out.push(`${p('.h1')}:${p('.m1')},${p('.h2')}:${p('.m2')},${d},${c.querySelector('.toggle').checked?'A':'-'}`);});});
 return out.join('|');}
function saveAll(){document.getElementById('status').textContent='se salveaza...';
 fetch('/irrig/save?data='+encodeURIComponent(collect()))
  .then(r=>r.text()).then(t=>document.getElementById('status').textContent=t)
  .catch(e=>document.getElementById('status').textContent='eroare: '+e);}

/* The panel, from the same /frame the remote page reads. Slower here:
 * this page is for editing a schedule, and a picture that keeps up with
 * a clock ticking is enough. */
const cv=document.getElementById('screen'),cx=cv.getContext('2d');
const msg=document.getElementById('screenmsg');
let busy=false,fails=0;
/* A quarter of the panel may arrive at a time; each piece is painted
   where the header says it goes, over whatever came before. */
function frame(){
 if(busy)return;busy=true;
 fetch('/frame').then(r=>r.arrayBuffer()).then(b=>{
  const v=new DataView(b);
  if(v.getUint32(0,true)!==0x3246354d)throw new Error('bad frame');
  const fw=v.getUint16(4,true),fh=v.getUint16(6,true);
  const x=v.getUint16(8,true),y=v.getUint16(10,true);
  const w=v.getUint16(12,true),h=v.getUint16(14,true);
  if(fw!==cv.width||fh!==cv.height){cv.width=fw;cv.height=fh;}
  const img=cx.createImageData(w,h),d=img.data;
  for(let i=0;i<w*h;i++){
   /* Big-endian on purpose: M5GFX stores 16bpp sprites byte-swapped
      and the firmware sends that out untouched. */
   const p=v.getUint16(16+i*2,false);
   d[i*4]=(p>>8)&0xf8;d[i*4+1]=(p>>3)&0xfc;d[i*4+2]=(p<<3)&0xf8;d[i*4+3]=255;}
  cx.putImageData(img,x,y);fails=0;msg.textContent='';
 }).catch(()=>{if(++fails>2)msg.textContent='ecranul nu raspunde';})
  .then(()=>{busy=false;});}
setInterval(frame,600);frame();
</script></body></html>)HTML";

esp_err_t handle_page(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t ret = send(req, kHead);

    /* The schedules go into the page as the same strings NVS holds, so
     * what the browser edits is exactly what the engine reads. */
    for (int zone = 0; ret == ESP_OK && zone < irrig::zone_count(); zone++) {
        const std::string entry = (zone > 0 ? ",\"" : "\"") + irrig::zone_text(zone) + "\"";
        ret = send(req, entry.c_str());
    }

    if (ret == ESP_OK) {
        ret = send(req, kTail);
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return ret;
}

esp_err_t handle_save(httpd_req_t* req)
{
    /* Four slots per zone, up to four zones, each entry 21 characters
     * plus a separator, and percent escapes roughly triple that. */
    char query[1536];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
    }

    char encoded[1024];
    if (httpd_query_key_value(query, "data", encoded, sizeof(encoded)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing data");
    }

    std::string decoded;
    decoded.reserve(std::strlen(encoded));
    for (const char* p = encoded; *p != '\0'; p++) {
        char ch = *p;
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%' && p[1] != '\0' && p[2] != '\0') {
            const char hex[3] = {p[1], p[2], '\0'};
            ch = (char)std::strtol(hex, nullptr, 16);
            p += 2;
        }
        decoded += ch;
    }

    /* Handed to the main loop rather than applied here: this runs on the
     * server's task, and the engine assumes it has the machine to
     * itself. */
    if (_pending_lock != nullptr && xSemaphoreTake(_pending_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        _pending     = decoded;
        _has_pending = true;
        xSemaphoreGive(_pending_lock);
    } else {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "salvat", HTTPD_RESP_USE_STRLEN);
}

}  // namespace

void start()
{
    if (_pending_lock == nullptr) {
        _pending_lock = xSemaphoreCreateMutex();
        if (_pending_lock == nullptr) {
            mclog::tagError(_tag, "lock allocation failed");
            return;
        }
    }

    static const httpd_uri_t page = {
        .uri = "/irrig", .method = HTTP_GET, .handler = handle_page, .user_ctx = nullptr};
    static const httpd_uri_t save = {
        .uri = "/irrig/save", .method = HTTP_GET, .handler = handle_save, .user_ctx = nullptr};

    remoted::add_route(page);
    remoted::add_route(save);
    remoted::add_link("/irrig", "irrigation schedule");
    mclog::tagInfo(_tag, "editor at /irrig");
}

void stop()
{
    /* The routes stay attached: the server they belong to comes and goes
     * with the network, and a page that answers with the current
     * schedule costs nothing while the daemon is stopped. */
    if (_pending_lock != nullptr && xSemaphoreTake(_pending_lock, 0) == pdTRUE) {
        _has_pending = false;
        _pending.clear();
        xSemaphoreGive(_pending_lock);
    }
}

void tick()
{
    if (_pending_lock == nullptr || !_has_pending) {
        return;
    }
    if (xSemaphoreTake(_pending_lock, 0) != pdTRUE) {
        return;
    }

    const std::string data = _pending;
    _has_pending           = false;
    _pending.clear();
    xSemaphoreGive(_pending_lock);

    /* The browser sends every slot of every zone in order, so they are
     * split back into groups of four. */
    std::string zone_text;
    int slot = 0;
    int zone = 0;
    std::size_t pos = 0;

    while (pos <= data.size() && zone < irrig::zone_count()) {
        const std::size_t bar = data.find('|', pos);
        const std::string entry =
            (bar == std::string::npos) ? data.substr(pos) : data.substr(pos, bar - pos);

        if (!entry.empty()) {
            if (slot > 0) {
                zone_text += '|';
            }
            zone_text += entry;
            slot++;
        }

        if (slot == irrig::kSlotsPerZone) {
            irrig::set_zone_text(zone, zone_text);
            mclog::tagInfo(_tag, "zone {} saved from browser", (char)('A' + zone));
            zone_text.clear();
            slot = 0;
            zone++;
        }

        if (bar == std::string::npos) {
            break;
        }
        pos = bar + 1;
    }

    irrig::update();
}

}  // namespace irrig_web
