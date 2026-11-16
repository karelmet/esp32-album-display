// Météo de Lyon 3e (Open-Meteo), rafraîchie en tâche de fond.
#pragma once
#include <Arduino.h>

static const int WEATHER_SLOTS = 6; // créneaux affichés par jour

struct WeatherSlot {
    uint8_t hour;     // heure locale du créneau
    int8_t  temp;     // °C
    uint8_t rainProb; // probabilité de pluie, %
    uint8_t code;     // code météo WMO
    bool    isDay;
};

struct WeatherDay {
    int8_t      tMin, tMax;
    uint8_t     code;     // temps dominant de la journée
    uint8_t     rainProb; // probabilité de pluie maximale, %
    float       rainMm;   // cumul de pluie
    WeatherSlot slots[WEATHER_SLOTS];
};

struct Weather {
    int8_t     temp, feels; // actuelle et ressentie, °C
    uint8_t    code;
    bool       isDay;
    uint16_t   minuteOfDay; // heure locale du relevé
    uint32_t   fetchedMs;   // millis() à la réception
    WeatherDay day[2];      // aujourd'hui, demain

    // Heure locale courante, estimée depuis le dernier relevé (peut dépasser 23 après minuit).
    int hourNow() const { return (minuteOfDay + (millis() - fetchedMs) / 60000) / 60; }
};

void weatherBegin();           // après artworkBegin(), qui démarre le Wi-Fi
bool weatherGet(Weather& out); // false tant qu'aucune donnée n'est disponible
void weatherRefresh();         // force une mise à jour
const char* weatherStatusText(); // message à afficher quand il n'y a pas de données

extern volatile bool weatherChanged; // nouvelles données ; à remettre à false par l'afficheur
