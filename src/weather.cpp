#include "weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "artwork.h"

// Lyon, 3e arrondissement
#define LATITUDE   "45.758"
#define LONGITUDE  "4.855"
#define REFRESH_MS (15UL * 60UL * 1000UL)
#define RETRY_MS   (60UL * 1000UL)

static const uint8_t SLOT_HOURS[WEATHER_SLOTS] = {8, 11, 14, 17, 20, 23};

static const char* const URL =
    "https://api.open-meteo.com/v1/forecast?latitude=" LATITUDE "&longitude=" LONGITUDE
    "&current=temperature_2m,apparent_temperature,weather_code,is_day"
    "&hourly=temperature_2m,precipitation_probability,weather_code,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,precipitation_probability_max"
    "&timezone=Europe%2FParis&forecast_days=2";

volatile bool weatherChanged = false;

static SemaphoreHandle_t mtx;
static Weather           data;
static bool              hasData    = false;
static bool              wifiConfig = true;
static volatile bool     failed     = false;
static volatile bool     refreshNow = true;

static int8_t toDeg(JsonVariantConst v) { return (int8_t)lroundf(v | 0.0f); }

static bool fetchOnce() {
    String body;
    {
        netLock();
        WiFiClientSecure client;
        client.setInsecure(); // données publiques, pas de vérification du certificat
        HTTPClient http;
        http.setTimeout(8000);
        int code = -1;
        if (http.begin(client, URL)) {
            code = http.GET();
            if (code == HTTP_CODE_OK) body = http.getString();
            http.end();
        }
        netUnlock();
        if (code != HTTP_CODE_OK) {
            Serial.printf("[METEO] HTTP %d\n", code);
            return false;
        }
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        Serial.println("[METEO] reponse JSON illisible");
        return false;
    }
    JsonObjectConst current = doc["current"], hourly = doc["hourly"], daily = doc["daily"];
    const char*     time    = current["time"] | "";
    if (strlen(time) < 16 || hourly["temperature_2m"].size() < 48 || daily["weather_code"].size() < 2) {
        Serial.println("[METEO] reponse incomplete");
        return false;
    }

    Weather w{};
    w.temp        = toDeg(current["temperature_2m"]);
    w.feels       = toDeg(current["apparent_temperature"]);
    w.code        = current["weather_code"] | 0;
    w.isDay       = (current["is_day"] | 1) != 0;
    w.minuteOfDay = atoi(time + 11) * 60 + atoi(time + 14); // "AAAA-MM-JJTHH:MM"
    w.fetchedMs   = millis();
    for (int d = 0; d < 2; d++) {
        WeatherDay& day = w.day[d];
        day.tMin        = toDeg(daily["temperature_2m_min"][d]);
        day.tMax        = toDeg(daily["temperature_2m_max"][d]);
        day.code        = daily["weather_code"][d] | 0;
        day.rainProb    = daily["precipitation_probability_max"][d] | 0;
        day.rainMm      = daily["precipitation_sum"][d] | 0.0f;
        for (int i = 0; i < WEATHER_SLOTS; i++) {
            const int    h = d * 24 + SLOT_HOURS[i];
            WeatherSlot& s = day.slots[i];
            s.hour         = SLOT_HOURS[i];
            s.temp         = toDeg(hourly["temperature_2m"][h]);
            s.rainProb     = hourly["precipitation_probability"][h] | 0;
            s.code         = hourly["weather_code"][h] | 0;
            s.isDay        = (hourly["is_day"][h] | 1) != 0;
        }
    }

    xSemaphoreTake(mtx, portMAX_DELAY);
    data    = w;
    hasData = true;
    xSemaphoreGive(mtx);
    weatherChanged = true;
    Serial.printf("[METEO] %d C (ressenti %d), code %u, releve de %02u:%02u\n", w.temp, w.feels, w.code,
                  w.minuteOfDay / 60, w.minuteOfDay % 60);
    return true;
}

static void weatherTask(void*) {
    uint32_t lastTry = 0;
    bool     lastOk  = false;
    for (;;) {
        delay(250);
        if (WiFi.status() != WL_CONNECTED) continue;
        const uint32_t interval = lastOk ? REFRESH_MS : RETRY_MS;
        if (!refreshNow && lastTry && millis() - lastTry < interval) continue;
        refreshNow = false;
        lastTry    = millis() | 1;
        lastOk     = fetchOnce();
        if (failed != !lastOk) {
            failed         = !lastOk;
            weatherChanged = true;
        }
    }
}

void weatherBegin() {
    mtx = xSemaphoreCreateMutex();
    if (artworkStatus() == ArtStatus::NoWifiConfig) {
        wifiConfig = false;
        return;
    }
    xTaskCreatePinnedToCore(weatherTask, "weather", 16384, nullptr, 1, nullptr, 1);
}

bool weatherGet(Weather& out) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    const bool ok = hasData;
    if (ok) out = data;
    xSemaphoreGive(mtx);
    return ok;
}

void weatherRefresh() { refreshNow = true; }

const char* weatherStatusText() {
    if (!wifiConfig) return "Wi-Fi non configuré";
    if (WiFi.status() != WL_CONNECTED) return "Connexion au Wi-Fi…";
    return failed ? "Météo indisponible" : "Chargement de la météo…";
}
