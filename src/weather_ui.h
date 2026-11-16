// Dessin de l'écran météo dans un sprite plein écran (320 x 170).
#pragma once
#include <TFT_eSPI.h>

#include "weather.h"

// `w` nul : pas de données, `message` est affiché à la place.
// `dayIndex` : 0 = aujourd'hui, 1 = demain.
void drawWeather(TFT_eSprite& spr, const Weather* w, const char* message, int dayIndex);
