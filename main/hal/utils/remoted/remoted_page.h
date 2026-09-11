/*
 * The page remoted serves. Kept apart from the server so the markup stays
 * readable; it is a plain string literal, embedded in flash.
 *
 * The frame endpoint hands back raw RGB565, byte-swapped as M5GFX stores
 * it, which the page converts to ImageData in JavaScript. That avoids
 * putting an image encoder in the firmware, and on a local network the
 * frames are small enough not to matter.
 */
#pragma once

static const char REMOTED_PAGE[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>M5Stack CoreS3</title>
<style>
:root{color-scheme:dark}
body{margin:0;background:#111;color:#ddd;font:14px system-ui,sans-serif;
     display:flex;flex-direction:column;align-items:center;gap:10px;padding:10px}
/* 640 because that is twice the panel's own 320. At 720 the browser was
   scaling by two and a quarter, and a fractional scale on a pixel-exact
   image is what turns crisp text into fringes. */
canvas{width:100%;max-width:640px;image-rendering:pixelated;
       border:1px solid #333;border-radius:6px;background:#000}
#pad{display:grid;grid-template-columns:repeat(5,1fr);gap:6px;width:100%;max-width:640px}
button{background:#222;color:#ddd;border:1px solid #444;border-radius:6px;
       padding:12px 0;font-size:15px;touch-action:manipulation}
button:active{background:#99ff00;color:#000}
form{display:flex;gap:6px;width:100%;max-width:640px}
input{flex:1;min-width:0;background:#222;color:#ddd;border:1px solid #444;border-radius:6px;padding:10px}
form button{flex:0 0 110px}
#st{font-size:12px;color:#888;min-height:1em}
/* Everything else this board serves, filled in as the page is sent:
   the pages belong to modules that register themselves, so the list
   cannot be written out here in advance. */
#nav{display:flex;flex-wrap:wrap;justify-content:center;gap:6px 18px;
     width:100%;max-width:640px;padding:2px 0 6px}
#nav a{color:#99ff00;text-decoration:none;font-size:13px}
#nav a:hover{text-decoration:underline}
#nav span{color:#666;font-size:13px}
</style></head><body>
<canvas id=s width=320 height=240></canvas>
<div id=pad>
  <button data-k="2,0">Fn</button>
  <button data-k="0,0" data-fn=1>Esc</button>
  <button data-k="2,11">&uarr;</button>
  <button data-k="0,13">Del</button>
  <button data-k="3,13">Space</button>
  <button data-k="1,0">Tab</button>
  <button data-k="3,10">&larr;</button>
  <button data-k="3,11">&darr;</button>
  <button data-k="3,12">&rarr;</button>
  <button data-k="2,13">Enter</button>
</div>
<form id=f><input id=t placeholder="just type here" autocomplete=off><button type=submit>Send</button></form>
<div id=st></div>
<nav id=nav><!--LINKS--></nav>
<script>
var cv=document.getElementById('s'),cx=cv.getContext('2d'),st=document.getElementById('st');
var busy=false,fails=0;

/* The device may send a quarter of the panel at a time when it has not
   the memory to hold a whole frame, so what has arrived is kept here and
   each piece is painted into it where the header says it belongs. */
function frame(){
  if(busy){return;}
  busy=true;
  fetch('/frame').then(function(r){return r.arrayBuffer();}).then(function(b){
    var v=new DataView(b);
    if(v.getUint32(0,true)!==0x3246354d){throw new Error('bad frame');}
    var fw=v.getUint16(4,true),fh=v.getUint16(6,true);
    var x=v.getUint16(8,true),y=v.getUint16(10,true);
    var w=v.getUint16(12,true),h=v.getUint16(14,true);
    if(fw!==cv.width||fh!==cv.height){cv.width=fw;cv.height=fh;}
    var img=cx.createImageData(w,h),d=img.data,o=16;
    for(var i=0;i<w*h;i++){
      /* Big-endian on purpose: M5GFX stores 16bpp sprites byte-swapped
         (swap565), and the firmware sends that out untouched rather than
         turning every pixel round on the device. */
      var p=v.getUint16(o+i*2,false);
      d[i*4]=(p>>8)&0xf8;d[i*4+1]=(p>>3)&0xfc;d[i*4+2]=(p<<3)&0xf8;d[i*4+3]=255;
    }
    cx.putImageData(img,x,y);
    fails=0;st.textContent='';
  }).catch(function(e){
    fails++;
    if(fails>2){st.textContent='no connection';}
  }).then(function(){busy=false;});
}
setInterval(frame,250);frame();

function key(rc,fn){
  var p=rc.split(',');
  var u='/key?r='+p[0]+'&c='+p[1]+(fn?'&fn=1':'');
  fetch(u).then(function(){setTimeout(frame,80);});
}
document.querySelectorAll('#pad button').forEach(function(b){
  b.addEventListener('click',function(){key(b.dataset.k,b.dataset.fn);});
});
document.getElementById('f').addEventListener('submit',function(e){
  e.preventDefault();
  var t=document.getElementById('t');
  if(!t.value){return;}
  fetch('/text?s='+encodeURIComponent(t.value)).then(function(){
    t.value='';setTimeout(frame,150);
  });
});
</script></body></html>)HTML";
