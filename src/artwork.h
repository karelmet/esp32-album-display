// Recherche et téléchargement de la pochette d'album, en tâche de fond.
#pragma once
#include <Arduino.h>

enum class ArtStatus { NoWifiConfig, WifiConnecting, Idle, Searching, Ready, NotFound };

// Démarre le Wi-Fi et la tâche de fond.
void artworkBegin();

// Signale le morceau en cours (chaînes vides = rien en lecture). Appel non bloquant.
void artworkRequest(const String& artist, const String& album, const String& title);

ArtStatus artworkStatus();

// Accès au JPEG courant. Si artworkLock() renvoie true, appeler artworkUnlock() ensuite.
bool artworkLock(const uint8_t** data, size_t* len);
void artworkUnlock();

// Verrou partagé : une seule requête HTTPS à la fois (pochette ou météo), pour limiter la mémoire.
void netLock();
void netUnlock();

// Passe à true quand l'image ou le statut change ; à remettre à false par l'afficheur.
extern volatile bool artworkChanged;
