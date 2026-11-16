#include "artwork.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"

#define ITUNES_COUNTRY "FR"  // boutique Apple Music utilisée pour la recherche
#define ART_SIZE       "170" // côté de la pochette en pixels (hauteur de l'écran)
#define DEBOUNCE_MS    600   // AMS envoie artiste, album et titre séparément
#define MAX_IMAGE      120000

volatile bool artworkChanged = false;

static SemaphoreHandle_t  mtx;
static SemaphoreHandle_t  netMtx;
static volatile ArtStatus status = ArtStatus::Idle;

// Demande en attente
static String   reqArtist, reqAlbum, reqTitle;
static bool     reqPending = false;
static uint32_t reqAt      = 0;

// Image courante
static uint8_t* img    = nullptr;
static size_t   imgLen = 0;
static String   imgKey; // "artiste\nalbum" de l'image affichée

static void setStatus(ArtStatus s) {
    if (status != s) {
        status         = s;
        artworkChanged = true;
    }
}

static String urlEncode(const String& s) {
    static const char hex[] = "0123456789ABCDEF";
    String            out;
    for (size_t i = 0; i < s.length(); i++) {
        const uint8_t c = (uint8_t)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += '+';
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Pas de vérification du certificat : on ne récupère que des pochettes publiques.
static bool httpGetText(const String& url, String& out) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(client, url)) return false;
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[ART] HTTP %d sur %s\n", code, url.c_str());
        http.end();
        return false;
    }
    out = http.getString();
    http.end();
    return out.length() > 0;
}

static uint8_t* httpGetBinary(const String& url, size_t& len) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);
    http.useHTTP10(true); // évite le transfert "chunked"
    if (!http.begin(client, url)) return nullptr;
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[ART] HTTP %d sur %s\n", code, url.c_str());
        http.end();
        return nullptr;
    }
    const int declared = http.getSize();
    if (declared > MAX_IMAGE) {
        http.end();
        return nullptr;
    }
    const size_t cap = declared > 0 ? (size_t)declared : MAX_IMAGE;
    uint8_t*     buf = (uint8_t*)ps_malloc(cap);
    if (!buf) buf = (uint8_t*)malloc(cap);
    if (!buf) {
        http.end();
        return nullptr;
    }
    WiFiClient*    stream = http.getStreamPtr();
    size_t         got    = 0;
    const uint32_t t0     = millis();
    while (got < cap && millis() - t0 < 10000) {
        const int avail = stream->available();
        if (avail > 0) {
            got += stream->readBytes(buf + got, min((size_t)avail, cap - got));
        } else if (!http.connected()) {
            break;
        } else {
            delay(5);
        }
    }
    http.end();
    if (got == 0 || (declared > 0 && got != (size_t)declared)) {
        Serial.printf("[ART] image incomplete (%u / %d octets)\n", (unsigned)got, declared);
        free(buf);
        return nullptr;
    }
    len = got;
    return buf;
}

// Interroge l'API de recherche iTunes et renvoie l'URL de la meilleure pochette.
static String searchArtworkUrl(const String& entity, const String& term, const String& artist,
                               const String& album, const String& title) {
    const String url = "https://itunes.apple.com/search?media=music&entity=" + entity +
                       "&limit=10&country=" ITUNES_COUNTRY "&term=" + urlEncode(term);
    String body;
    if (!httpGetText(url, body)) return "";

    JsonDocument filter;
    filter["results"][0]["artistName"]     = true;
    filter["results"][0]["trackName"]      = true;
    filter["results"][0]["collectionName"] = true;
    filter["results"][0]["artworkUrl100"]  = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        Serial.println("[ART] reponse JSON illisible");
        return "";
    }

    String best;
    int    bestScore = -1;
    for (JsonObject r : doc["results"].as<JsonArray>()) {
        const char* art = r["artworkUrl100"] | "";
        if (!*art) continue;
        int score = 0;
        if (album.equalsIgnoreCase(r["collectionName"] | "")) score += 4;
        if (artist.equalsIgnoreCase(r["artistName"] | "")) score += 2;
        if (title.equalsIgnoreCase(r["trackName"] | "")) score += 1;
        if (score > bestScore) {
            bestScore = score;
            best      = art;
        }
    }
    if (best.length()) {
        best.replace("100x100bb", ART_SIZE "x" ART_SIZE "bb");
        Serial.printf("[ART] pochette trouvee (score %d)\n", bestScore);
    }
    return best;
}

static void fetch(const String& artist, const String& album, const String& title) {
    const String key = artist + "\n" + album;

    xSemaphoreTake(mtx, portMAX_DELAY);
    const bool same = img && key == imgKey;
    xSemaphoreGive(mtx);
    if (same) { // même album que la pochette déjà affichée
        setStatus(ArtStatus::Ready);
        return;
    }

    setStatus(ArtStatus::Searching);
    String url = searchArtworkUrl("song", artist + " " + title, artist, album, title);
    if (!url.length() && album.length()) url = searchArtworkUrl("album", artist + " " + album, artist, album, title);

    size_t   len  = 0;
    uint8_t* data = url.length() ? httpGetBinary(url, len) : nullptr;

    xSemaphoreTake(mtx, portMAX_DELAY);
    free(img);
    img    = data;
    imgLen = len;
    imgKey = data ? key : "";
    xSemaphoreGive(mtx);
    status         = data ? ArtStatus::Ready : ArtStatus::NotFound;
    artworkChanged = true;
    if (data) Serial.printf("[ART] pochette telechargee : %u octets\n", (unsigned)len);
    else Serial.println("[ART] pas de pochette");
}

static void clearImage() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    free(img);
    img    = nullptr;
    imgLen = 0;
    imgKey = "";
    xSemaphoreGive(mtx);
    status         = ArtStatus::Idle;
    artworkChanged = true;
}

static void artworkTask(void*) {
    bool wasConnected = false;
    for (;;) {
        delay(100);
        const bool connected = WiFi.status() == WL_CONNECTED;
        if (connected != wasConnected) {
            wasConnected = connected;
            if (connected) Serial.printf("[WIFI] connecte, IP %s\n", WiFi.localIP().toString().c_str());
            else Serial.println("[WIFI] deconnecte");
            if (connected) {
                // Relance la recherche pour le morceau en cours.
                xSemaphoreTake(mtx, portMAX_DELAY);
                reqPending = true;
                reqAt      = millis();
                xSemaphoreGive(mtx);
                setStatus(ArtStatus::Idle);
            }
        }
        if (!connected) {
            setStatus(ArtStatus::WifiConnecting);
            continue;
        }

        xSemaphoreTake(mtx, portMAX_DELAY);
        const bool   due    = reqPending && millis() - reqAt >= DEBOUNCE_MS;
        const String artist = reqArtist, album = reqAlbum, title = reqTitle;
        if (due) reqPending = false;
        xSemaphoreGive(mtx);
        if (!due) continue;

        // Sans artiste (rien en lecture, ou titre provisoire du type "Chargement…"),
        // une recherche renverrait une pochette au hasard.
        if (!artist.length() || !title.length()) clearImage();
        else {
            netLock();
            fetch(artist, album, title);
            netUnlock();
        }
    }
}

void artworkBegin() {
    mtx    = xSemaphoreCreateMutex();
    netMtx = xSemaphoreCreateMutex();
    if (!strlen(WIFI_SSID)) {
        Serial.println("[WIFI] non configure (include/secrets.h)");
        status = ArtStatus::NoWifiConfig;
        return;
    }
    status = ArtStatus::WifiConnecting;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD); // le mode économie du Wi-Fi reste actif : requis avec le BLE
    xTaskCreatePinnedToCore(artworkTask, "artwork", 16384, nullptr, 1, nullptr, 1);
}

void artworkRequest(const String& artist, const String& album, const String& title) {
    if (!mtx) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (artist != reqArtist || album != reqAlbum || title != reqTitle) {
        reqArtist  = artist;
        reqAlbum   = album;
        reqTitle   = title;
        reqPending = true;
        reqAt      = millis();
    }
    xSemaphoreGive(mtx);
}

ArtStatus artworkStatus() { return status; }

void netLock() { xSemaphoreTake(netMtx, portMAX_DELAY); }
void netUnlock() { xSemaphoreGive(netMtx); }

bool artworkLock(const uint8_t** data, size_t* len) {
    if (!mtx) return false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (!img) {
        xSemaphoreGive(mtx);
        return false;
    }
    *data = img;
    *len  = imgLen;
    return true;
}

void artworkUnlock() { xSemaphoreGive(mtx); }
