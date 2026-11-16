#include "weather_ui.h"

#include "fonts.h"

namespace {

const int W = 320, H = 170;

struct RGB {
    uint8_t r, g, b;
};

uint16_t to565(RGB c) { return ((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3); }

RGB mix(RGB a, RGB b, float t) {
    t = constrain(t, 0.0f, 1.0f);
    return {(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t)};
}

RGB from565(uint16_t c) {
    return {(uint8_t)(((c >> 11) & 0x1F) * 255 / 31), (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
            (uint8_t)((c & 0x1F) * 255 / 31)};
}

const RGB BLACK{0, 0, 0}, WHITE{255, 255, 255};
const RGB SOFT{226, 233, 241};      // texte secondaire
const RGB RAIN{150, 214, 255};      // pluie
const RGB SUN{255, 210, 63};
const RGB MOON{241, 239, 208};
const RGB CLOUD_WHITE{244, 246, 248};
const RGB CLOUD_GREY{201, 209, 217};
const RGB CLOUD_DARK{154, 165, 177};

RGB rowColor[H]; // couleur du dégradé de fond, ligne par ligne

// ---------------------------------------------------------------------------
// Codes météo WMO
// ---------------------------------------------------------------------------
enum class Sky { Clear, Partly, Cloudy, Fog, Drizzle, Rain, Snow, Thunder };

Sky skyOf(uint8_t code) {
    if (code == 0) return Sky::Clear;
    if (code <= 2) return Sky::Partly;
    if (code == 3) return Sky::Cloudy;
    if (code == 45 || code == 48) return Sky::Fog;
    if (code >= 51 && code <= 57) return Sky::Drizzle;
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return Sky::Rain;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Sky::Snow;
    if (code >= 95) return Sky::Thunder;
    return Sky::Cloudy;
}

const char* labelOf(uint8_t code) {
    switch (code) {
        case 0:  return "Ciel dégagé";
        case 1:  return "Peu nuageux";
        case 2:  return "Éclaircies";
        case 3:  return "Couvert";
        case 45:
        case 48: return "Brouillard";
        case 51:
        case 53:
        case 55: return "Bruine";
        case 56:
        case 57: return "Bruine verglaçante";
        case 61: return "Pluie faible";
        case 63: return "Pluie";
        case 65: return "Forte pluie";
        case 66:
        case 67: return "Pluie verglaçante";
        case 71: return "Neige faible";
        case 73: return "Neige";
        case 75: return "Forte neige";
        case 77: return "Grains de neige";
        case 80: return "Averses faibles";
        case 81: return "Averses";
        case 82: return "Fortes averses";
        case 85:
        case 86: return "Averses de neige";
        case 95: return "Orage";
        case 96:
        case 99: return "Orage de grêle";
        default: return "";
    }
}

// ---------------------------------------------------------------------------
// Fond : dégradé vertical selon le temps et le jour/la nuit, plus un décor discret.
// ---------------------------------------------------------------------------
void drawBackground(TFT_eSprite& spr, Sky sky, bool day) {
    RGB top, bottom;
    switch (sky) {
        case Sky::Clear:
            top    = day ? RGB{28, 110, 214} : RGB{8, 12, 38};
            bottom = day ? RGB{108, 184, 238} : RGB{40, 44, 112};
            break;
        case Sky::Partly:
            top    = day ? RGB{52, 118, 196} : RGB{16, 22, 54};
            bottom = day ? RGB{134, 182, 222} : RGB{52, 58, 104};
            break;
        case Sky::Cloudy:
            top    = day ? RGB{76, 92, 110} : RGB{26, 31, 40};
            bottom = day ? RGB{132, 146, 160} : RGB{62, 70, 82};
            break;
        case Sky::Fog:
            top    = day ? RGB{98, 106, 114} : RGB{36, 40, 46};
            bottom = day ? RGB{150, 158, 164} : RGB{78, 84, 92};
            break;
        case Sky::Drizzle:
        case Sky::Rain:
            top    = day ? RGB{36, 52, 74} : RGB{12, 18, 28};
            bottom = day ? RGB{74, 104, 136} : RGB{38, 56, 78};
            break;
        case Sky::Snow:
            top    = day ? RGB{84, 104, 130} : RGB{30, 40, 56};
            bottom = day ? RGB{158, 176, 196} : RGB{82, 96, 118};
            break;
        default: // orage
            top    = RGB{24, 20, 40};
            bottom = RGB{72, 60, 106};
            break;
    }
    for (int y = 0; y < H; y++) {
        rowColor[y] = mix(top, bottom, y / (float)(H - 1));
        spr.drawFastHLine(0, y, W, to565(rowColor[y]));
    }

    const int decorBottom = 84; // le décor reste au-dessus du panneau de prévisions
    // Étoiles et flocons évitent la zone du texte (ils y ressembleraient à des pixels parasites).
    auto overText = [](int x, int y) { return y > 22 && x < 250; };
    if (!day && (sky == Sky::Clear || sky == Sky::Partly)) {
        for (int i = 0; i < 26; i++) { // étoiles, positions pseudo-aléatoires fixes
            const int x = (i * 97 + 31) % W, y = (i * 53 + 7) % decorBottom;
            if (overText(x, y)) continue;
            spr.drawPixel(x, y, to565(mix(rowColor[y], WHITE, 0.35f + 0.2f * (i % 3))));
        }
    } else if (sky == Sky::Rain || sky == Sky::Drizzle || sky == Sky::Thunder) {
        for (int i = 0; i < 22; i++) { // traits de pluie
            const int x = (i * 61 + 23) % W, y = (i * 37 + 5) % (decorBottom - 10);
            spr.drawLine(x, y, x - 3, y + 8, to565(mix(rowColor[y], WHITE, 0.13f)));
        }
    } else if (sky == Sky::Snow) {
        for (int i = 0; i < 26; i++) { // flocons
            const int x = (i * 71 + 13) % W, y = (i * 43 + 9) % decorBottom;
            if (overText(x, y)) continue;
            spr.fillCircle(x, y, i % 3 == 0 ? 1 : 0, to565(mix(rowColor[y], WHITE, 0.45f)));
        }
    }
}

// Halo lumineux autour d'un point (soleil).
void drawGlow(TFT_eSprite& spr, int cx, int cy, int radius, RGB color, float strength) {
    for (int y = max(0, cy - radius); y < min(H, cy + radius); y++) {
        for (int x = max(0, cx - radius); x < min(W, cx + radius); x++) {
            const float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / radius;
            if (d >= 1) continue;
            spr.drawPixel(x, y, to565(mix(rowColor[y], color, strength * (1 - d) * (1 - d))));
        }
    }
}

// Panneau sombre translucide à coins arrondis.
const float PANEL_ALPHA = 0.34f;
RGB         panelColor(int y) { return mix(rowColor[y], BLACK, PANEL_ALPHA); }

void drawPanel(TFT_eSprite& spr, int x, int y, int w, int h, int r) {
    for (int dy = 0; dy < h; dy++) {
        int k = 0; // distance verticale à l'intérieur du coin
        if (dy < r) k = r - dy;
        else if (dy >= h - r) k = dy - (h - 1 - r);
        const int inset = k ? r - (int)lroundf(sqrtf(r * r - k * k)) : 0;
        spr.drawFastHLine(x + inset, y + dy, w - 2 * inset, to565(panelColor(y + dy)));
    }
}

// Estompe une zone vers la couleur du panneau (créneaux déjà passés).
void dimArea(TFT_eSprite& spr, int x, int y, int w, int h, float amount) {
    for (int yy = y; yy < y + h; yy++) {
        const RGB target = panelColor(yy);
        for (int xx = x; xx < x + w; xx++) spr.drawPixel(xx, yy, to565(mix(from565(spr.readPixel(xx, yy)), target, amount)));
    }
}

// ---------------------------------------------------------------------------
// Texte (fond transparent : le lissage se fait sur l'image déjà dessinée)
// ---------------------------------------------------------------------------
const uint8_t* loadedFont = nullptr;

void useFont(TFT_eSprite& spr, const uint8_t* font) {
    if (font == loadedFont) return;
    if (loadedFont) spr.unloadFont();
    spr.loadFont(font);
    loadedFont = font;
}

int textWidth(TFT_eSprite& spr, const String& s, const uint8_t* font) {
    useFont(spr, font);
    return spr.textWidth(s);
}

void drawText(TFT_eSprite& spr, const String& s, int x, int y, const uint8_t* font, RGB color,
              uint8_t datum = TL_DATUM) {
    useFont(spr, font);
    const uint16_t c = to565(color);
    spr.setTextColor(c, c); // premier plan == fond : fond transparent
    spr.setTextDatum(datum);
    spr.drawString(s, x, y);
}

// ---------------------------------------------------------------------------
// Pictogrammes, dessinés autour de (cx, cy). `u` est l'échelle : l'ensemble
// tient dans un carré d'environ 36u de côté.
// ---------------------------------------------------------------------------
int px(float v) { return (int)lroundf(v); }

void drawCloud(TFT_eSprite& spr, float cx, float cy, float u, RGB color) {
    const uint16_t c = to565(color);
    spr.fillSmoothCircle(px(cx - 9 * u), px(cy + 3 * u), max(1, px(7 * u)), c);
    spr.fillSmoothCircle(px(cx - 1 * u), px(cy - 3 * u), max(2, px(10 * u)), c);
    spr.fillSmoothCircle(px(cx + 9 * u), px(cy + 2 * u), max(1, px(8 * u)), c);
    const int h = max(3, px(9 * u));
    spr.fillSmoothRoundRect(px(cx - 16 * u), px(cy + 10 * u) - h, px(33 * u), h, h / 2, c);
}

void drawSun(TFT_eSprite& spr, float cx, float cy, float u) {
    const uint16_t c = to565(SUN);
    spr.fillSmoothCircle(px(cx), px(cy), max(2, px(9 * u)), c);
    for (int i = 0; i < 8; i++) {
        const float a = i * PI / 4;
        spr.drawWideLine(cx + cosf(a) * 12.5f * u, cy + sinf(a) * 12.5f * u, cx + cosf(a) * 17 * u,
                         cy + sinf(a) * 17 * u, max(1.4f, 2.4f * u), c);
    }
}

void drawMoon(TFT_eSprite& spr, float cx, float cy, float u, RGB background) {
    spr.fillSmoothCircle(px(cx), px(cy), max(3, px(11 * u)), to565(MOON));
    spr.fillSmoothCircle(px(cx + 5 * u), px(cy - 4 * u), max(2, px(9 * u)), to565(background)); // croissant
}

void drawIcon(TFT_eSprite& spr, float cx, float cy, float u, Sky sky, bool day, RGB background) {
    switch (sky) {
        case Sky::Clear:
            if (day) drawSun(spr, cx, cy, u);
            else drawMoon(spr, cx, cy, u, background);
            break;
        case Sky::Partly:
            if (day) drawSun(spr, cx + 6 * u, cy - 6 * u, u * 0.72f);
            else drawMoon(spr, cx + 7 * u, cy - 6 * u, u * 0.72f, background);
            drawCloud(spr, cx - 2 * u, cy + 3 * u, u * 0.85f, CLOUD_WHITE);
            break;
        case Sky::Cloudy:
            drawCloud(spr, cx + 5 * u, cy - 5 * u, u * 0.7f, CLOUD_DARK);
            drawCloud(spr, cx - 2 * u, cy + 2 * u, u * 0.9f, CLOUD_GREY);
            break;
        case Sky::Fog:
            for (int i = 0; i < 4; i++) {
                static const float widths[] = {24, 32, 22, 28};
                const float        y        = cy - 10 * u + i * 6.5f * u, half = widths[i] * u / 2;
                const float        shift    = (i % 2 ? 3 : -3) * u;
                spr.drawWideLine(cx - half + shift, y, cx + half + shift, y, max(1.5f, 2.8f * u), to565(CLOUD_GREY));
            }
            break;
        case Sky::Drizzle:
        case Sky::Rain: {
            drawCloud(spr, cx, cy - 5 * u, u * 0.95f, CLOUD_GREY);
            const float len = (sky == Sky::Rain) ? 7 : 3.5f;
            for (int i = -1; i <= 1; i++) {
                const float x = cx + i * 8 * u;
                spr.drawWideLine(x + 1.5f * u, cy + 9 * u, x + 1.5f * u - len * 0.4f * u, cy + (9 + len) * u,
                                 max(1.3f, 1.9f * u), to565(RAIN));
            }
            break;
        }
        case Sky::Snow:
            drawCloud(spr, cx, cy - 5 * u, u * 0.95f, CLOUD_GREY);
            for (int i = -1; i <= 1; i++) {
                spr.fillSmoothCircle(px(cx + i * 8 * u), px(cy + (i == 0 ? 15 : 11) * u), max(1, px(1.9f * u)), TFT_WHITE);
            }
            break;
        case Sky::Thunder:
            drawCloud(spr, cx, cy - 5 * u, u * 0.95f, CLOUD_DARK);
            spr.fillTriangle(px(cx + 2 * u), px(cy + 5 * u), px(cx - 5 * u), px(cy + 14 * u), px(cx + 1 * u),
                             px(cy + 14 * u), to565(SUN));
            spr.fillTriangle(px(cx - 1 * u), px(cy + 12 * u), px(cx + 5 * u), px(cy + 12 * u), px(cx - 3 * u),
                             px(cy + 21 * u), to565(SUN));
            break;
    }
}

String degText(int t) { return String(t) + "°"; }

} // namespace

// ---------------------------------------------------------------------------
void drawWeather(TFT_eSprite& spr, const Weather* w, const char* message, int dayIndex) {
    if (!w) {
        drawBackground(spr, Sky::Clear, false);
        drawText(spr, "Lyon 3e", 12, 7, FONT_SMALL, SOFT);
        drawText(spr, message, W / 2, H / 2, FONT_TEXT, WHITE, MC_DATUM);
        return;
    }

    const bool        today = (dayIndex == 0);
    const WeatherDay& d     = w->day[dayIndex];
    const uint8_t     code  = today ? w->code : d.code;
    const bool        day   = today ? w->isDay : true;
    const Sky         sky   = skyOf(code);

    drawBackground(spr, sky, day);

    // --- Pictogramme du temps, en haut à droite ---
    const int iconX = 280, iconY = 34;
    if (sky == Sky::Clear && day) drawGlow(spr, iconX, iconY, 78, RGB{255, 244, 190}, 0.55f);
    drawIcon(spr, iconX, iconY, 1.3f, sky, day, rowColor[iconY]);

    // --- En-tête ---
    drawText(spr, today ? "Lyon 3e  ·  Aujourd’hui" : "Lyon 3e  ·  Demain", 12, 7, FONT_SMALL, SOFT);

    // --- Température en grand, puis le détail à sa droite ---
    const String big = degText(today ? w->temp : d.tMax);
    drawText(spr, big, 10, 30, FONT_HUGE, WHITE);
    const int tx = 10 + textWidth(spr, big, FONT_HUGE) + 12;

    const String   label     = labelOf(code);
    const uint8_t* labelFont = (textWidth(spr, label, FONT_TITLE) <= 248 - tx) ? FONT_TITLE : FONT_TEXT;
    drawText(spr, label, tx, 28, labelFont, WHITE);
    drawText(spr, today ? "Ressenti " + degText(w->feels) : String("Maximale"), tx, 51, FONT_SMALL, SOFT);
    drawText(spr, "Min " + degText(d.tMin) + "  ·  Max " + degText(d.tMax), tx, 67, FONT_SMALL, SOFT);

    // --- Pluie de la journée, sous le pictogramme ---
    String rain = "Pas de pluie";
    if (d.rainProb >= 10 || d.rainMm >= 0.1f) {
        String mm = String(d.rainMm, 1);
        mm.replace('.', ',');
        rain = String(d.rainProb) + " %  ·  " + mm + " mm";
    }
    drawText(spr, rain, W - 10, 67, FONT_SMALL, WHITE, TR_DATUM);
    const int dropX = W - 10 - textWidth(spr, rain, FONT_SMALL) - 10; // goutte devant le texte
    spr.fillSmoothCircle(dropX, 76, 4, to565(RAIN));
    spr.fillTriangle(dropX - 4, 75, dropX + 4, 75, dropX, 67, to565(RAIN));

    // --- Prévisions de la journée : six créneaux ---
    const int   panelX = 6, panelY = 90, panelW = W - 12, panelH = 76;
    const float colW = panelW / (float)WEATHER_SLOTS;
    drawPanel(spr, panelX, panelY, panelW, panelH, 10);

    const int hourNow = today ? w->hourNow() : -1;
    for (int i = 0; i < WEATHER_SLOTS; i++) {
        const WeatherSlot& s  = d.slots[i];
        const int          cx = px(panelX + colW * (i + 0.5f));
        drawText(spr, String(s.hour) + " h", cx, panelY + 5, FONT_SMALL, SOFT, TC_DATUM);
        drawIcon(spr, cx, panelY + 30, 0.5f, skyOf(s.code), s.isDay, panelColor(panelY + 30));
        drawText(spr, degText(s.temp), cx + 2, panelY + 42, FONT_TEXT, WHITE, TC_DATUM);
        if (s.rainProb >= 10) drawText(spr, String(s.rainProb) + " %", cx, panelY + 59, FONT_SMALL, RAIN, TC_DATUM);
        else drawText(spr, "–", cx, panelY + 59, FONT_SMALL, mix(SOFT, BLACK, 0.35f), TC_DATUM);

        if (today && s.hour < hourNow) { // créneau passé : estompé
            dimArea(spr, px(panelX + colW * i) + 6, panelY + 4, px(colW) - 12, panelH - 8, 0.6f);
        }
    }
}
