#include "OTA.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <Update.h>
#include <freertos/semphr.h>

#ifndef PIXELPAD_WIFI_OTA
#define PIXELPAD_WIFI_OTA 1
#endif

#if PIXELPAD_WIFI_OTA
namespace {
AsyncWebServer server(80);
SemaphoreHandle_t webMutex=nullptr;
AsyncWebServerRequest* activeRequest=nullptr;
bool finished=false;
size_t received=0, expected=0;
uint32_t activity=0, restartAt=0;
struct Lock {
    Lock() { xSemaphoreTakeRecursive(webMutex,portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(webMutex); }
};
bool ownsWeb() { return transferGate.owner()==TransferOwner::WebOta; }
void abortWeb() {
    if (!ownsWeb()) return;
    Update.abort(); activeRequest=nullptr; finished=false; restartAt=0;
    transferGate.release(TransferOwner::WebOta);
}
bool beginWeb(size_t bytes) {
    if (!transferGate.claim(TransferOwner::WebOta)) return false;
    activeRequest=nullptr; finished=false; restartAt=0; received=0; expected=bytes;
    activity=millis();
    if (!Update.begin(bytes?bytes:UPDATE_SIZE_UNKNOWN,U_FLASH)) { abortWeb(); return false; }
    return true;
}
void bind(AsyncWebServerRequest* request) {
    activeRequest=request;
    request->onDisconnect([request]() {
        Lock lock;
        if (activeRequest==request && !finished) abortWeb();
    });
}
bool append(size_t index,uint8_t* data,size_t size) {
    if (index!=received || (expected && (received>expected || size>expected-received)) ||
        (size && Update.write(data,size)!=size)) { abortWeb(); return false; }
    received+=size; activity=millis(); return true;
}
bool finish() {
    if (!received || (expected && received!=expected) || !Update.end(true)) { abortWeb(); return false; }
    finished=true; restartAt=millis(); return true;
}
}
#endif

void setupOTA() {
#if PIXELPAD_WIFI_OTA
    webMutex=xSemaphoreCreateRecursiveMutex();
    if (!webMutex) { Serial.println("Fatal: Web OTA mutex allocation failed"); abort(); }
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Pixel_Screen","12345678");
    server.on("/",HTTP_GET,[](AsyncWebServerRequest* request) {
        request->send(200,"text/plain","PixelPad FM6373C RTOS OTA: /update");
    });
    // Guarded routes precede ElegantOTA's UI routes. All Flash writers share a gate.
    server.on("/ota/start",HTTP_GET,[](AsyncWebServerRequest* request) {
        Lock lock;
        if (request->hasParam("mode") && request->getParam("mode")->value()=="fs") {
            request->send(400,"text/plain","Only application firmware is supported"); return;
        }
        if (!beginWeb(0)) { request->send(409,"text/plain","Device busy or OTA unavailable"); return; }
        if (request->hasParam("hash")) {
            const String hash=request->getParam("hash")->value();
            bool valid=hash.length()==32;
            for (size_t i=0;i<hash.length();++i) valid=valid && isxdigit(hash[i]);
            if (!valid || !Update.setMD5(hash.c_str())) {
                abortWeb(); request->send(400,"text/plain","Invalid MD5"); return;
            }
        }
        request->send(200,"text/plain","OK");
    });
    server.on("/ota/upload",HTTP_POST,[](AsyncWebServerRequest* request) {
        Lock lock;
        const bool ok=ownsWeb() && activeRequest==request && finished;
        request->send(ok?200:400,"text/plain",ok?"OK":"OTA failed");
    },[](AsyncWebServerRequest* request,String,size_t index,uint8_t* data,size_t len,bool final) {
        Lock lock;
        if (!ownsWeb() || finished) return;
        if (!index && !activeRequest) bind(request);
        if (activeRequest!=request || !append(index,data,len)) return;
        if (final) finish();
    });
    server.on("/raw_upload",HTTP_POST,[](AsyncWebServerRequest* request) {
        Lock lock;
        const bool ok=ownsWeb() && activeRequest==request && received==expected && finish();
        request->send(ok?200:400,"text/plain",ok?"OK":"OTA failed");
    },nullptr,[](AsyncWebServerRequest* request,uint8_t* data,size_t len,size_t index,size_t total) {
        Lock lock;
        if (!index) {
            if (!total || !beginWeb(total)) return;
            bind(request);
        }
        if (!ownsWeb() || finished || activeRequest!=request) return;
        append(index,data,len);
    });
    ElegantOTA.begin(&server);
    server.begin();
    Serial.printf("WiFi OTA: http://%s/update\n",WiFi.softAPIP().toString().c_str());
#endif
}

void loopOTA() {
#if PIXELPAD_WIFI_OTA
    ElegantOTA.loop();
    Lock lock;
    if (!ownsWeb()) return;
    if (finished && millis()-restartAt>=2000) ESP.restart();
    else if (!finished && millis()-activity>=30000) abortWeb();
#endif
}
