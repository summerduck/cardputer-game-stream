#include "upload.h"

#include <WebServer.h>
#include <WiFi.h>

#include "storage.h"

namespace upload {

namespace {

WebServer *server = nullptr;
File uploadFile;
String uploadName;
bool uploadFailed = false;
String lastUploaded;

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Книги на Cardputer</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee;max-width:560px}
h1{font-size:22px;margin:0 0 4px}p{color:#aaa;margin:4px 0 16px}
label.pick{display:block;padding:28px 12px;border:2px dashed #555;border-radius:12px;text-align:center;font-size:18px;cursor:pointer}
input[type=file]{display:none}
.row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid #333;gap:8px}
.row span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
button{background:#333;color:#eee;border:0;border-radius:8px;padding:8px 12px;font-size:15px}
progress{width:100%;height:14px}#status{margin:12px 0;min-height:20px}.err{color:#f66}.ok{color:#6d6}
</style></head><body>
<h1>Книги на Cardputer</h1>
<p>Форматы: .txt, .epub, .fb2 · свободно: <b id="free">…</b></p>
<label class="pick">📚 Выбрать книги<input id="f" type="file" multiple accept=".txt,.epub,.fb2"></label>
<div id="status"></div><progress id="bar" value="0" max="100" hidden></progress>
<h1 style="margin-top:24px">На устройстве</h1><div id="list"></div>
<script>
const $=id=>document.getElementById(id);
async function refresh(){
  const r=await fetch('/list');const d=await r.json();
  $('free').textContent=(d.free/1048576).toFixed(1)+' МБ';
  $('list').innerHTML=d.books.length?'':'<p>Пока пусто</p>';
  for(const b of d.books){
    const row=document.createElement('div');row.className='row';
    const s=document.createElement('span');s.textContent=b;row.appendChild(s);
    const del=document.createElement('button');del.textContent='Удалить';
    del.onclick=async()=>{if(confirm('Удалить «'+b+'»?')){await fetch('/delete?name='+encodeURIComponent(b),{method:'POST'});refresh();}};
    row.appendChild(del);$('list').appendChild(row);
  }
}
function send(file){return new Promise(res=>{
  const x=new XMLHttpRequest();const fd=new FormData();fd.append('book',file,file.name);
  x.upload.onprogress=e=>{$('bar').value=e.loaded/e.total*100};
  x.onload=()=>res(x.status==200?null:x.responseText||('ошибка '+x.status));
  x.onerror=()=>res('связь прервалась');
  x.open('POST','/upload');x.send(fd);})}
$('f').onchange=async e=>{
  const files=[...e.target.files];$('bar').hidden=false;
  for(const [i,f] of files.entries()){
    $('status').className='';$('status').textContent='Загружаю '+(i+1)+' из '+files.length+': '+f.name;
    const err=await send(f);
    if(err){$('status').className='err';$('status').textContent=f.name+': '+err;$('bar').hidden=true;refresh();return;}
  }
  $('bar').hidden=true;$('status').className='ok';$('status').textContent='Готово! Книги уже в списке на Cardputer.';
  e.target.value='';refresh();
};
refresh();
</script></body></html>)HTML";

void handleList() {
    String json = "{\"free\":" + String(static_cast<unsigned long>(storage::freeBytes())) + ",\"books\":[";
    File dir = storage::fs().open("/books");
    bool first = true;
    for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
        String name = f.name();
        bool isDir = f.isDirectory();
        f.close();
        if (isDir || name.startsWith(".")) continue;
        name.replace("\\", "\\\\");
        name.replace("\"", "\\\"");
        json += (first ? "\"" : ",\"") + name + "\"";
        first = false;
    }
    if (dir) dir.close();
    json += "]}";
    server->send(200, "application/json", json);
}

void handleDelete() {
    String name = storage::safeName(server->arg("name"));
    for (auto &b : storage::listBooks()) {
        if (b.file == name) storage::removeBook(b);
    }
    server->send(200, "text/plain", "ok");
}

void handleUploadData() {
    HTTPUpload &up = server->upload();
    if (up.status == UPLOAD_FILE_START) {
        uploadFailed = false;
        uploadName = storage::safeName(up.filename);
        if (rsvp::formatFromName(uploadName.c_str()) == rsvp::Format::Unknown) {
            uploadFailed = true;
            return;
        }
        uploadFile = storage::fs().open("/books/" + uploadName, "w");
        if (!uploadFile) uploadFailed = true;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!uploadFailed && uploadFile.write(up.buf, up.currentSize) != up.currentSize) uploadFailed = true;
    } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile) uploadFile.close();
        if (up.status == UPLOAD_FILE_ABORTED) uploadFailed = true;
        if (uploadFailed && uploadName.length()) storage::fs().remove("/books/" + uploadName);
    }
}

void handleUploadDone() {
    if (uploadFailed) {
        bool badType = rsvp::formatFromName(uploadName.c_str()) == rsvp::Format::Unknown;
        server->send(badType ? 415 : 507, "text/plain",
                     badType ? "нужен файл .txt, .epub или .fb2" : "не хватило места на устройстве");
        return;
    }
    lastUploaded = uploadName;
    server->send(200, "text/plain", "ok");
}

}  // namespace

void start() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(SSID);  // open network: nothing to type on the phone
    server = new WebServer(80);
    server->on("/", HTTP_GET, [] { server->send_P(200, "text/html; charset=utf-8", PAGE); });
    server->on("/list", HTTP_GET, handleList);
    server->on("/delete", HTTP_POST, handleDelete);
    server->on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
    server->onNotFound([] {  // phones probing for captive portals etc.
        server->sendHeader("Location", "http://192.168.4.1/");
        server->send(302, "text/plain", "");
    });
    server->begin();
}

void stop() {
    if (server) {
        server->stop();
        delete server;
        server = nullptr;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

void loop() {
    if (server) server->handleClient();
}

String takeLastUploaded() {
    String s = lastUploaded;
    lastUploaded = "";
    return s;
}

int clientCount() { return WiFi.softAPgetStationNum(); }

}  // namespace upload
