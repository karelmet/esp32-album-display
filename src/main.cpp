// Prototype : afficheur "musique en cours" sur LilyGo T-Display S3.
//
// L'ESP32 s'annonce en BLE, l'iPhone s'y connecte et s'y appaire, puis l'ESP32
// lit l'Apple Media Service (AMS) exposé par l'iPhone : titre, artiste, album
// et état de lecture, quelle que soit l'app qui joue. La pochette est cherchée
// en Wi-Fi (voir artwork.cpp).
//
// Deux affichages, qu'on alterne avec le bouton B2 :
//   - Musique : l'écran reste éteint tant que l'iPhone n'est pas connecté ou
//     qu'aucun morceau n'est en cours.
//   - Météo (Lyon 3e) : voir weather.cpp et weather_ui.cpp.
//
// Bouton B1 (GPIO14)
//   en musique : appui simple = lecture/pause, double appui = morceau suivant
//   en météo   : appui simple = aujourd'hui/demain, double appui = mise à jour
//   maintenu 5 s : efface l'appairage et redémarre
// Bouton B2 (GPIO0) : bascule musique/météo

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

#include "artwork.h"
#include "fonts.h"
#include "weather.h"
#include "weather_ui.h"

#define PIN_POWER_ON 15 // alimente l'écran quand la carte est sur batterie
#define PIN_B1       14
#define PIN_B2       0
#define DEVICE_NAME  "Now Playing"

// --- Apple Media Service ---
static const NimBLEUUID AMS_SERVICE("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
static const NimBLEUUID AMS_ENTITY_UPDATE("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
static const NimBLEUUID AMS_REMOTE_COMMAND("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
// Même UUID de service, en little-endian, pour la "sollicitation" dans l'annonce.
static const uint8_t AMS_SOLICITATION[] = {0x11, 0x15, 0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E,
                                           0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89};

enum : uint8_t { ENTITY_PLAYER = 0, ENTITY_QUEUE = 1, ENTITY_TRACK = 2 };
enum : uint8_t { PLAYER_NAME = 0, PLAYER_PLAYBACK_INFO = 1 };
enum : uint8_t { TRACK_ARTIST = 0, TRACK_ALBUM = 1, TRACK_TITLE = 2 };
enum : uint8_t { CMD_TOGGLE_PLAY_PAUSE = 2, CMD_NEXT_TRACK = 3 };

enum class Link { Waiting, Securing, NeedSetup, Ready, Failed };
enum class Mode { Music, Weather };

// --- État partagé entre la tâche BLE et loop() ---
static SemaphoreHandle_t stateMutex;
static volatile Link     linkState  = Link::Waiting;
static volatile uint16_t connHandle = 0;
static volatile bool     dirtyText  = true; // texte à redessiner
static volatile bool     dirtyAll   = true; // écran entier à redessiner
static String            title, artist, album, playerName;
static int               playbackState = -1; // 0 pause, 1 lecture, 2 retour, 3 avance
static String            failReason;

static NimBLEServer* server = nullptr;
static NimBLERemoteCharacteristic* volatile remoteCommand = nullptr; // télécommande AMS
static TFT_eSPI      tft;
static TFT_eSprite   sprite(&tft); // image de l'écran météo, en PSRAM

// --- Affichage (utilisé depuis loop() uniquement) ---
static Mode mode         = Mode::Music;
static bool screenOn     = false; // rétroéclairage allumé en mode musique
static int  weatherDay   = 0;     // 0 = aujourd'hui, 1 = demain
static bool weatherDirty = false; // écran météo à redessiner
static int  debugCode    = -1;    // code météo forcé par le port série (-1 = aucun)
static bool debugIsDay   = true;

// ---------------------------------------------------------------------------
// Affichage : pochette 170x170 à gauche, texte à droite.
// ---------------------------------------------------------------------------
static const int ART = 170;             // côté de la pochette
static const uint16_t COL_PANEL = 0x18E3; // gris très sombre
static const uint16_t COL_PLAY  = 0x3CBF; // bleu clair
static const uint16_t COL_PAUSE = 0xFA28; // rouge
static const uint16_t COL_ALBUM = 0x8410; // gris moyen

// Les espaces insécables ne sont pas dans la police : on les ramène à l'espace simple.
static String sanitize(String s) {
    s.replace("\u00A0", " ");
    s.replace("\u202F", " ");
    s.trim();
    return s;
}

// Une seule police lissée est chargée à la fois ; on ne recharge que si elle change.
static const uint8_t* loadedFont = nullptr;
static void useFont(const uint8_t* font) {
    if (font == loadedFont) return;
    if (loadedFont) tft.unloadFont();
    tft.loadFont(font);
    loadedFont = font;
}

static int textWidth(const String& s) { return tft.textWidth(s); }

// Nombre d'octets du plus long préfixe de `s` (coupé entre deux caractères UTF-8)
// dont la largeur, suffixe compris, tient dans `maxWidth`.
static int fitPrefix(const String& s, int maxWidth, const char* suffix = "") {
    int best = 0;
    for (int i = 1; i <= (int)s.length(); i++) {
        if (i < (int)s.length() && ((uint8_t)s[i] & 0xC0) == 0x80) continue; // milieu d'un caractère
        if (textWidth(s.substring(0, i) + suffix) > maxWidth) break;
        best = i;
    }
    if (best == 0) { // au moins un caractère entier
        best = 1;
        while (best < (int)s.length() && ((uint8_t)s[best] & 0xC0) == 0x80) best++;
    }
    return best;
}

// Dessine `text` avec retour à la ligne par mots, sur `maxLines` lignes au plus.
// `y` est le haut des minuscules hautes de la première ligne (les accents des
// capitales dépassent un peu au-dessus) ; renvoie l'ordonnée sous la dernière ligne.
static int drawWrapped(const String& text, int x, int y, int maxWidth, int maxLines, const uint8_t* font,
                       uint16_t color, uint16_t background = TFT_BLACK) {
    const int lineGap = 4;
    useFont(font);
    tft.setTextColor(color, background);
    tft.setTextDatum(TL_DATUM);
    const int lineHeight = tft.fontHeight() + lineGap;
    maxLines             = min(maxLines, (tft.height() - y + lineGap) / lineHeight); // ne pas déborder en bas

    String rest = text;
    for (int line = 0; line < maxLines && rest.length(); line++) {
        String out;
        if (textWidth(rest) <= maxWidth) {
            out  = rest;
            rest = "";
        } else if (line == maxLines - 1) {
            out = rest.substring(0, fitPrefix(rest, maxWidth, "..."));
            out.trim();
            out += "...";
            rest = "";
        } else {
            int cut = -1; // dernière espace avant laquelle tout tient
            for (int i = 1; i < (int)rest.length(); i++) {
                if (rest[i] != ' ') continue;
                if (textWidth(rest.substring(0, i)) > maxWidth) break;
                cut = i;
            }
            if (cut < 0) cut = fitPrefix(rest, maxWidth); // mot trop long : coupe dedans
            out  = rest.substring(0, cut);
            rest = rest.substring(cut);
            rest.trim();
        }
        tft.drawString(out, x, y);
        y += lineHeight;
    }
    return y;
}

static bool jpegBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= tft.height()) return false;
    tft.pushImage(x, y, w, h, bitmap);
    return true;
}

static void drawArt() {
    const uint8_t* data;
    size_t         len;
    if (artworkLock(&data, &len)) {
        const JRESULT res = TJpgDec.drawJpg(0, 0, data, len);
        artworkUnlock();
        if (res == JDR_OK) return;
        Serial.printf("[ART] decodage JPEG impossible (%d)\n", res);
    }
    // Pas d'image : panneau neutre avec le statut.
    tft.fillRect(0, 0, ART, ART, COL_PANEL);
    const char* msg = "";
    switch (artworkStatus()) {
        case ArtStatus::NoWifiConfig:   msg = "Wi-Fi non configuré"; break;
        case ArtStatus::WifiConnecting: msg = "Connexion au Wi-Fi..."; break;
        case ArtStatus::Searching:      msg = "Recherche de la pochette..."; break;
        case ArtStatus::NotFound:       msg = "Pochette introuvable"; break;
        default: break;
    }
    drawWrapped(msg, 12, 66, ART - 24, 3, FONT_SMALL, TFT_LIGHTGREY, COL_PANEL);
}

static void drawText() {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const String t = sanitize(title), ar = sanitize(artist), al = sanitize(album);
    const int    pb = playbackState;
    xSemaphoreGive(stateMutex);

    const int X = ART + 10, W = tft.width() - X - 6;
    tft.fillRect(ART, 0, tft.width() - ART, tft.height(), TFT_BLACK);

    // État de lecture : symbole + libellé (bleu en lecture, rouge en pause)
    const char*    stateText  = (pb == 1) ? "Lecture" : (pb == 0) ? "Pause" : (pb == 2) ? "Retour" : (pb == 3) ? "Avance" : "";
    const uint16_t stateColor = (pb == 0) ? COL_PAUSE : COL_PLAY;
    const int      iy         = 9; // haut du symbole, 10 px de haut
    if (pb == 0) {
        tft.fillRect(X, iy, 3, 10, stateColor);
        tft.fillRect(X + 6, iy, 3, 10, stateColor);
    } else if (pb > 0) {
        tft.fillTriangle(X, iy, X, iy + 10, X + 9, iy + 5, stateColor);
    }
    drawWrapped(stateText, X + 15, 7, W - 15, 1, FONT_SMALL, stateColor);

    int y = 34;
    y     = drawWrapped(t, X, y, W, 3, FONT_TITLE, TFT_WHITE) + 6;
    y     = drawWrapped(ar, X, y, W, 2, FONT_TEXT, TFT_LIGHTGREY) + 6;
    drawWrapped(al, X, y, W, 2, FONT_SMALL, COL_ALBUM);
}

// Vrai si l'iPhone annonce un morceau (en lecture ou en pause).
static bool hasTrack() {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const bool any = title.length() || artist.length();
    xSemaphoreGive(stateMutex);
    return any;
}

// Écran d'erreur, en pleine largeur. (Hors connexion, l'écran est éteint.)
static void drawFailureScreen() {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const String reason = failReason;
    xSemaphoreGive(stateMutex);

    const int M = 12, W = tft.width() - 2 * M;
    tft.fillScreen(TFT_BLACK);
    drawWrapped("Échec", M, 20, W, 1, FONT_TITLE, TFT_RED);
    drawWrapped(reason, M, 66, W, 4, FONT_TEXT, TFT_LIGHTGREY);
}

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------
static void setLink(Link l, const char* reason = nullptr) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    linkState = l;
    if (reason) failReason = reason;
    xSemaphoreGive(stateMutex);
    dirtyAll = true;
}

// Notification "Entity Update" : [entité, attribut, drapeaux, valeur UTF-8...]
static void onEntityUpdate(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 3) return;
    const uint8_t entity = data[0], attr = data[1];
    String        value;
    value.reserve(len - 3);
    for (size_t i = 3; i < len; i++) value += (char)data[i];
    Serial.printf("[AMS] entite=%u attribut=%u valeur=\"%s\"\n", entity, attr, value.c_str());

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (entity == ENTITY_TRACK) {
        if (attr == TRACK_TITLE) title = value;
        else if (attr == TRACK_ARTIST) artist = value;
        else if (attr == TRACK_ALBUM) album = value;
    } else if (entity == ENTITY_PLAYER) {
        if (attr == PLAYER_NAME) playerName = value;
        else if (attr == PLAYER_PLAYBACK_INFO) playbackState = value.length() ? value.toInt() : -1;
    }
    if (entity == ENTITY_TRACK) artworkRequest(artist, album, title);
    xSemaphoreGive(stateMutex);
    dirtyText = true;
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        Serial.printf("[BLE] connecte : %s\n", info.getAddress().toString().c_str());
        connHandle = info.getConnHandle();
        setLink(Link::Securing);
        // AMS n'est lisible que sur un lien chiffré : on demande l'appairage.
        NimBLEDevice::startSecurity(info.getConnHandle());
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        Serial.printf("[BLE] deconnecte (raison 0x%x)\n", reason);
        remoteCommand = nullptr;
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        title = artist = album = playerName = "";
        playbackState                       = -1;
        xSemaphoreGive(stateMutex);
        artworkRequest("", "", "");
        setLink(Link::Waiting);
        NimBLEDevice::startAdvertising();
    }

    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        Serial.printf("[BLE] securite : chiffre=%d appaire=%d\n", info.isEncrypted(), info.isBonded());
        if (info.isEncrypted()) {
            setLink(Link::NeedSetup);
        } else {
            setLink(Link::Failed, "Jumelage refusé. Sur l’iPhone : oublier l’appareil, puis maintenir le bouton 1 pendant 5 s.");
        }
    }
};

// Découvre AMS chez l'iPhone et s'abonne aux mises à jour. À appeler depuis loop().
static bool setupAms() {
    NimBLEClient* client = server->getClient(connHandle);
    if (!client) {
        Serial.println("[AMS] pas de client GATT");
        return false;
    }
    NimBLERemoteService* svc = client->getService(AMS_SERVICE);
    if (!svc) {
        Serial.println("[AMS] service introuvable");
        return false;
    }
    NimBLERemoteCharacteristic* update = svc->getCharacteristic(AMS_ENTITY_UPDATE);
    if (!update) {
        Serial.println("[AMS] caracteristique Entity Update introuvable");
        return false;
    }
    if (!update->subscribe(true, onEntityUpdate)) {
        Serial.println("[AMS] abonnement refuse");
        return false;
    }
    const uint8_t wantTrack[]  = {ENTITY_TRACK, TRACK_ARTIST, TRACK_ALBUM, TRACK_TITLE};
    const uint8_t wantPlayer[] = {ENTITY_PLAYER, PLAYER_NAME, PLAYER_PLAYBACK_INFO};
    if (!update->writeValue(wantTrack, sizeof(wantTrack), true) ||
        !update->writeValue(wantPlayer, sizeof(wantPlayer), true)) {
        Serial.println("[AMS] ecriture de la selection refusee");
        return false;
    }
    remoteCommand = svc->getCharacteristic(AMS_REMOTE_COMMAND);
    if (!remoteCommand) Serial.println("[AMS] telecommande indisponible");
    Serial.println("[AMS] abonne aux infos de lecture");
    return true;
}

// Envoie une commande de lecture à l'iPhone. À appeler depuis loop().
static void sendCommand(uint8_t command) {
    NimBLERemoteCharacteristic* rc = remoteCommand;
    if (linkState != Link::Ready || !rc) return;
    const bool ok = rc->writeValue(&command, 1, true);
    Serial.printf("[AMS] commande %u : %s\n", command, ok ? "envoyee" : "refusee");
}

static void forgetPairing() {
    Serial.println("[BLE] effacement des appairages, redemarrage");
    NimBLEDevice::deleteAllBonds();
    tft.fillScreen(TFT_BLACK);
    drawWrapped("Appairage effacé", 12, 60, tft.width() - 24, 1, FONT_TITLE, TFT_ORANGE);
    digitalWrite(TFT_BL, HIGH);
    delay(1200);
    ESP.restart();
}

// ---------------------------------------------------------------------------
// Boutons
// ---------------------------------------------------------------------------
enum class Press { None, Single, Double, Long };

struct Button {
    uint8_t  pin;
    bool     wantDouble; // false : l'appui simple est signalé tout de suite
    bool     down = false, longSent = false;
    uint32_t downAt = 0, lastChange = 0, lastRelease = 0;
    int      clicks = 0;

    Button(uint8_t pin, bool wantDouble) : pin(pin), wantDouble(wantDouble) {}

    Press poll() {
        const uint32_t now     = millis();
        const bool     pressed = digitalRead(pin) == LOW;
        if (pressed != down && now - lastChange > 30) { // anti-rebond
            lastChange = now;
            down       = pressed;
            if (pressed) {
                downAt   = now;
                longSent = false;
            } else if (now - downAt < 600) {
                clicks++;
                lastRelease = now;
            } else {
                clicks = 0; // appui trop long pour un clic
            }
        }
        if (down) {
            if (!longSent && now - downAt > 5000) {
                longSent = true;
                return Press::Long;
            }
            return Press::None;
        }
        if (clicks >= 2 || (clicks == 1 && !wantDouble)) {
            const Press p = clicks >= 2 ? Press::Double : Press::Single;
            clicks        = 0;
            return p;
        }
        if (clicks == 1 && now - lastRelease > 350) { // pas de second appui : clic simple
            clicks = 0;
            return Press::Single;
        }
        return Press::None;
    }
};

static Button b1(PIN_B1, true), b2(PIN_B2, false);

static void setMode(Mode m) {
    mode = m;
    if (m == Mode::Weather) {
        weatherDay   = 0;
        weatherDirty = true;
    } else { // retour à la musique : on repart écran éteint, loop() rallume si un morceau est en cours
        digitalWrite(TFT_BL, LOW);
        tft.fillScreen(TFT_BLACK);
        screenOn = false;
    }
    Serial.printf("[UI] affichage : %s\n", m == Mode::Weather ? "meteo" : "musique");
}

static void handleButtons() {
    const Press p1 = b1.poll(), p2 = b2.poll();
    if (p1 == Press::Long) forgetPairing();
    if (p2 == Press::Single) setMode(mode == Mode::Music ? Mode::Weather : Mode::Music);

    if (mode == Mode::Music) {
        if (p1 == Press::Single) sendCommand(CMD_TOGGLE_PLAY_PAUSE);
        else if (p1 == Press::Double) sendCommand(CMD_NEXT_TRACK);
    } else {
        if (p1 == Press::Single) {
            weatherDay   = 1 - weatherDay;
            weatherDirty = true;
        } else if (p1 == Press::Double) {
            Serial.println("[METEO] mise a jour demandee");
            weatherRefresh();
        }
    }
}

// ---------------------------------------------------------------------------
// Écran météo
// ---------------------------------------------------------------------------
static void renderWeather() {
    if (!sprite.created()) {
        sprite.setColorDepth(16);
        if (!sprite.createSprite(tft.width(), tft.height())) {
            Serial.println("[UI] memoire insuffisante pour l'ecran meteo");
            return;
        }
    }
    Weather    w;
    const bool ok = weatherGet(w);
    if (ok && debugCode >= 0) {
        w.code  = debugCode;
        w.isDay = debugIsDay;
    }
    drawWeather(sprite, ok ? &w : nullptr, weatherStatusText(), weatherDay);
    sprite.pushSprite(0, 0);
    digitalWrite(TFT_BL, HIGH);
}

// Commandes de mise au point sur le port série (une par ligne) :
//   w        bascule musique/météo        d       aujourd'hui/demain
//   c<code><j|n>  force un code météo WMO, de jour ou de nuit (c seul : annule)
//   s        envoie l'image de l'écran météo (RGB565, en hexadécimal)
static void handleSerial() {
    static String line;
    while (Serial.available()) {
        const char ch = Serial.read();
        if (ch != '\n' && ch != '\r') {
            if (line.length() < 16) line += ch;
            continue;
        }
        if (line == "w") {
            setMode(mode == Mode::Music ? Mode::Weather : Mode::Music);
        } else if (line == "d") {
            weatherDay   = 1 - weatherDay;
            weatherDirty = true;
        } else if (line.startsWith("c")) {
            debugCode    = line.length() > 1 ? line.substring(1).toInt() : -1;
            debugIsDay   = !line.endsWith("n");
            weatherDirty = true;
        } else if (line == "s" && sprite.created()) {
            const uint16_t* pixels = (const uint16_t*)sprite.getPointer();
            Serial.printf("SHOT %d %d\n", sprite.width(), sprite.height());
            for (int y = 0; y < sprite.height(); y++) {
                for (int x = 0; x < sprite.width(); x++) Serial.printf("%04x", pixels[y * sprite.width() + x]);
                Serial.println();
            }
            Serial.println("END");
        }
        line = "";
    }
}

static void startBle() {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true); // appairage mémorisé, sans code
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setMTU(247);

    server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    server->start();

    NimBLEAdvertisementData adv, scan;
    adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    adv.addData(AMS_SOLICITATION, sizeof(AMS_SOLICITATION));
    scan.setName(DEVICE_NAME);

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setAdvertisementData(adv);
    advertising->setScanResponseData(scan);
    advertising->start();
    Serial.printf("[BLE] annonce en cours, appairages memorises : %d\n", NimBLEDevice::getNumBonds());
}

// ---------------------------------------------------------------------------
void setup() {
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    pinMode(PIN_B1, INPUT_PULLUP);
    pinMode(PIN_B2, INPUT_PULLUP);
    Serial.begin(115200);

    stateMutex = xSemaphoreCreateMutex();

    tft.init();
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW); // rétroéclairage éteint jusqu'à la connexion de l'iPhone
    tft.setSwapBytes(true);
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(jpegBlock);

    artworkBegin();
    weatherBegin();
    startBle();
}

void loop() {
    static uint32_t nextSetupTry = 0;
    static int      setupTries = 0;
    static Link     lastLink   = Link::Waiting;

    const Link link = linkState;
    if (link != lastLink) {
        if (link == Link::NeedSetup) {
            setupTries   = 0;
            nextSetupTry = millis() + 300;
        }
        lastLink = link;
    }

    if (link == Link::NeedSetup && (int32_t)(millis() - nextSetupTry) >= 0) {
        if (setupAms()) {
            setLink(Link::Ready);
        } else if (linkState == Link::NeedSetup) {
            if (++setupTries >= 5) setLink(Link::Failed, "L’iPhone n’expose pas le service de lecture (AMS).");
            else nextSetupTry = millis() + 1500;
        }
    }

    handleButtons();
    handleSerial();

    if (mode == Mode::Weather) {
        static uint32_t lastDraw = 0;
        // Redessin à chaque changement, et une fois par minute pour estomper les créneaux passés.
        if (weatherDirty || weatherChanged || millis() - lastDraw > 60000) {
            weatherDirty   = false;
            weatherChanged = false;
            renderWeather();
            lastDraw = millis();
        }
        delay(20);
        return;
    }

    // L'écran n'est allumé que si l'iPhone est connecté avec un morceau en cours
    // (lecture ou pause), ou pour signaler un échec.
    const Link now  = linkState;
    const bool show = (now == Link::Failed) || (now == Link::Ready && hasTrack());
    if (!show) {
        if (screenOn) {
            digitalWrite(TFT_BL, LOW);
            tft.fillScreen(TFT_BLACK);
            screenOn = false;
        }
    } else {
        // Lire les drapeaux avant de dessiner : une mise à jour arrivée entre-temps relance un dessin.
        const bool all = dirtyAll || !screenOn, text = dirtyText, art = artworkChanged;
        dirtyAll = false;
        if (text) dirtyText = false;
        if (art) artworkChanged = false;
        if (now == Link::Failed) {
            if (all) drawFailureScreen();
        } else {
            if (all || art) drawArt();
            if (all || text) drawText();
        }
        if (!screenOn) { // on allume une fois l'image prête
            digitalWrite(TFT_BL, HIGH);
            screenOn = true;
        }
    }
    delay(20);
}
