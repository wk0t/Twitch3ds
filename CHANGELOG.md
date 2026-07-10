# Changelog

## 1.0

Première version qui tourne sur New 3DS / New 2DS : navigation, recherche,
lecture vidéo H.264 (MVD), audio AAC, chat (lecture + envoi), connexion au
compte.

### Notes de dev / pièges rencontrés

- **Listes GQL** : `streams(first:)` et `searchStreams(first:)` plafonnent à 30
  côté Twitch (au-delà : erreur `argument 'first'...`). D'où la limite à 30.
- **Vidéo (MVD)** : `mvdstdRenderVideoFrame` renvoie INVALIDARG si les dimensions
  de sortie ne sont pas alignées sur 16 et égales à l'entrée. Solution : aligner
  la sortie sur 16, laisser MVD écrire dans un buffer compact, puis recopier vers
  un buffer strié en puissance de 2 pour la texture citro2d.
- **Audio** : l'anneau ndsp (~0,7 s) doit être rechargé en continu, pas seulement
  quand le thread réseau tourne — sinon le son coupe au bout d'~1 s dès que le
  téléchargement se met en pause. D'où `audio_update()` appelé chaque frame.
- **Login** : le pseudo se récupère via `id.twitch.tv/oauth2/validate` (marche
  avec n'importe quel jeton), pas via Helix (qui exige un client-id enregistré).
- **Chat (IRC)** : un `connect` non bloquant + `select()` ne détecte pas la fin
  de connexion sur le SOC du 3DS. Repassé en `connect` bloquant.

### Limites connues

- Émotes affichées en texte, pas d'images.
- Live seulement (pas de VOD / clips).
- Old 3DS / 2DS classique : audio seul (pas de MVD).
