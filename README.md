# Twitch 3DS

Client Twitch homebrew pour **New Nintendo 3DS / New 2DS** : navigation dans les
top streams, recherche, **lecture vidéo en direct** (décodage H.264 matériel via
MVD) et **chat intégré** (lecture + envoi de messages).

![Twitch 3DS](icon.png)

## Fonctionnalités

- **Top streams** et **recherche** de chaînes/jeux (API GQL Twitch, miniatures JPEG)
- **Lecture des streams live** :
  - New 3DS/2DS : vidéo 160p/360p/480p décodée par la puce H.264 matérielle (MVD) + audio AAC
  - Old 3DS/2DS : mode audio seul (pas de MVD sur ces consoles)
- **Chat Twitch** sur l'écran du bas : lecture anonyme immédiate, envoi de
  messages avec un token OAuth (voir plus bas)
- UI adaptée aux deux écrans : vidéo en haut, chat/navigation tactile en bas

## Installation

1. Console avec **Homebrew Launcher** (Luma3DS + boot9strap recommandés).
2. Copier `twitch3ds.3dsx` dans `sd:/3ds/`.
3. **Pour le son** : le fichier `sd:/3ds/dspfirm.cdc` doit exister. S'il manque,
   lancer une fois l'homebrew [DSP1](https://github.com/zoogie/DSP1)
   (« Dump DSP firmware ») — sans lui, la vidéo joue muette.
4. Lancer *Twitch 3DS* depuis le Homebrew Launcher (Wi-Fi activé).

## Contrôles

| Écran | Entrée | Action |
|---|---|---|
| Liste | D-Pad haut/bas | naviguer |
| Liste | A | lire le stream |
| Liste | X | actualiser |
| Liste | Y | recherche (clavier tactile) |
| Liste | SELECT | connexion au compte Twitch |
| Liste | R | installer la mise à jour (si dispo) |
| Lecture | B | retour à la liste |
| Lecture | SELECT | afficher/masquer les infos (qualité, buffer) |
| Lecture | bouton « Écrire » | envoyer un message dans le chat |
| Partout | START | quitter (depuis les listes) |

## Se connecter à son compte (pour écrire dans le chat)

La lecture du chat marche sans compte. Pour **écrire**, connecte-toi directement
depuis l'app, en toute sécurité (méthode « appareil » officielle de Twitch) :

1. Sur l'écran d'accueil, appuie sur **SELECT**.
2. L'app affiche un **code** (ex. `GNSLNMLF`).
3. Sur ton téléphone ou PC, va sur **twitch.tv/activate**, connecte-toi
   (avec ta **double authentification / 2FA** comme d'habitude — elle est gérée
   par Twitch, **jamais** par l'app) et entre le code.
4. L'app détecte l'autorisation et enregistre la session : le bouton
   « Écrire » devient actif dans le chat.

> 🔒 Ton mot de passe et ta 2FA ne transitent **jamais** par la console : la
> connexion se fait entièrement sur le site de Twitch. L'app ne reçoit qu'un
> jeton limité au chat (`chat:read` + `chat:edit`), stocké dans
> `sd:/3ds/twitch3ds/config.ini`.

### Configuration manuelle (optionnel)

`sd:/3ds/twitch3ds/config.ini` peut aussi être édité à la main :

```ini
username=TonPseudoTwitch
oauth=oauth:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
quality=160p
volume=100
```

`quality` accepte `160p`, `360p`, `480p` ou `audio_only`.
En Wi-Fi faible, `160p` (par défaut) est le plus fiable ; `480p` demande une
très bonne connexion et sollicite fortement le décodeur.

## Mises à jour

Au démarrage, l'app vérifie s'il existe une version plus récente sur les
[releases GitHub](https://github.com/wk0t/Twitch3ds/releases). Si oui, un
bandeau vert « Mise à jour dispo » s'affiche sur l'accueil — appuie sur **R**
pour télécharger et remplacer le `.3dsx` automatiquement, puis relance l'app.

Tu peux aussi récupérer la dernière version à la main sur la page des releases.

## Compiler depuis les sources

Prérequis : [devkitPro](https://devkitpro.org/wiki/Getting_Started) avec
devkitARM, libctru, citro2d/citro3d et les portlibs `3ds-curl`, `3ds-mbedtls`,
`3ds-zlib`, `3ds-libjpeg-turbo` :

```bash
pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib 3ds-libjpeg-turbo
make
```

Le `twitch3ds.3dsx` est produit à la racine.

## Architecture technique

| Module | Rôle |
|---|---|
| `source/net_http.c` | HTTPS via libcurl + mbedtls (CA Mozilla embarquée en romfs) |
| `source/twitch.c` | API GQL non officielle (client-id public du player web) + usher HLS |
| `source/hls.c` | parseur de playlists média live |
| `source/ts_demux.c` | démuxeur MPEG-TS (PAT/PMT dynamiques, PES → AU H.264 + ADTS AAC) |
| `source/video_mvd.c` | décodage H.264 matériel MVDSTD → texture citro2d (New 3DS) |
| `source/audio_aac.c` | décodage AAC-LC (Helix, virgule fixe ARM) → ndsp ; horloge maîtresse A/V |
| `source/player.c` | orchestration : thread de téléchargement (cœur 2 New 3DS) + synchro sur l'horloge audio |
| `source/irc.c` | chat IRC (tags IRCv3, PING/PONG, reconnexion) |
| `source/ui/` | scènes citro2d (accueil, recherche, lecture) |

## Problèmes fréquents

| Ce que tu vois | D'où ça vient | Quoi faire |
|---|---|---|
| « Erreur reseau (…) » sur l'accueil | Wi-Fi coupé ou souci réseau | Vérifier que le Wi-Fi est actif. Le code entre parenthèses aide (voir plus bas) |
| Erreur qui parle de `SSL` / `certificate` | **Date/heure de la console fausse** → certificat TLS refusé | Régler la date dans *Paramètres de la console → Autres paramètres → Date et heure* |
| Vidéo muette (image OK, pas de son) | `sd:/3ds/dspfirm.cdc` absent | Lancer une fois [DSP1](https://github.com/zoogie/DSP1) pour dumper le firmware du DSP |
| Pas d'image, juste le nom + « Lecture… » | Console sans puce MVD (Old 3DS / 2DS classique), ou MVD indisponible → mode audio seul | Normal sur Old 3DS/2DS. Sur New 3DS/2DS, réessayer |
| Ça se fige ou coupe après quelques secondes | Wi-Fi trop lent pour la qualité choisie | Passer en `160p` (`quality=160p` dans `config.ini`) |
| Image saccadée / en retard sur le son | 480p trop lourd pour la connexion / la console | Choisir `160p` ou `360p` |
| Chat « Connexion au chat… » puis « Déconnecté » en boucle | Le réseau bloque le port 6667 (IRC) — rare, surtout en partage de connexion mobile | Essayer un autre Wi-Fi |
| Bouton « Lecture seule » dans le chat | Pas connecté, ou jeton expiré | **SELECT** sur l'accueil pour (re)connecter le compte |
| « Stream terminé / hors ligne » | La chaîne n'est pas en direct | Choisir un stream réellement en live |
| Connexion : « Code expire » | Trop de temps pour autoriser sur twitch.tv/activate | Refaire **SELECT** et autoriser plus vite |
| Connexion : « login introuvable » | Souci réseau pendant la validation du jeton | Réessayer |
| Mise à jour : « Echec du telechargement » | Coupure réseau pendant le téléchargement | Réessayer, ou récupérer le `.3dsx` à la main sur la page des [Releases](https://github.com/wk0t/Twitch3ds/releases) |
| Rien ne s'affiche / texte invisible (émulateur) | Fichiers système (police) manquants | Sur vraie console c'est bon ; sur émulateur il faut fournir les fichiers système. La vidéo, elle, ne marche **pas** sur émulateur (MVD non émulé) |

### Décoder le code d'« Erreur reseau (…) »

- **-6 / -7** → DNS ou connexion impossible (Wi-Fi ?)
- **-28** → délai dépassé (réseau lent)
- **-35 / -60** → problème TLS / certificat (le plus souvent : **date de la console** à corriger)
- **-403 / -429** → Twitch a refusé la requête (réessayer un peu plus tard)

## Limites connues

- Les émotes du chat s'affichent en texte (pas d'images d'émotes).
- Les VODs et clips ne sont pas gérés (live uniquement).
- Sur Old 3DS/2DS : audio seul.
- La 1ʳᵉ connexion à une chaîne prend quelques secondes (token + playlist + buffer).

## Crédits

- Décodeur AAC : [Helix AAC](https://en.wikipedia.org/wiki/Helix_Universal_Server) (RealNetworks, RPSL)
  via [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio)
- JSON : [cJSON](https://github.com/DaveGamble/cJSON) (MIT)
- CA : bundle Mozilla via [curl.se](https://curl.se/docs/caextract.html)
- Toolchain : [devkitPro](https://devkitpro.org) (libctru, citro2d, portlibs)

## Performance New 3DS

Pour tirer parti de la New 3DS/2DS, l'app :
- active le mode **804 MHz** (`osSetSpeedupEnable`) ;
- décode la vidéo H.264 sur la **puce MVD matérielle** (aucune charge CPU vidéo) ;
- place le thread lourd (réseau + démux + décodage AAC) sur le **cœur 2**
  exclusif de la New 3DS, laissant le cœur applicatif à l'UI 60 fps.

Application non affiliée à Twitch/Amazon ni à Nintendo.
