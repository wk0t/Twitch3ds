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
