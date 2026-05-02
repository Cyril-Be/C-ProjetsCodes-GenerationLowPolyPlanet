# C-ProjetsCodes-GenerationLowPolyPlanet

Génération procédurale 2D d'une planète **low poly** en C avec SDL2.

## Aperçu

Le programme génère une planète unique à chaque exécution (ou reproductible via un seed) avec :

- **Triangulation de Delaunay** (algorithme Bowyer-Watson) pour le maillage low poly
- **bruit fBm + bruit ridgé** pour un terrain réaliste (océans, plaines, forêts, montagnes, neige)
- **Biomes colorés** : océan abyssal → mer → plage → prairie → forêt → savane → montagne → neige
- **Ombrage diffus** simulant un éclairage solaire directionnel
- **Halo atmosphérique** dégradé circulaire (couleur générée aléatoirement)
- **Calottes polaires** aux pôles nord et sud avec fondu
- **Nuages discrets** semi-transparents avec fondu radial
- **Fond étoilé** procédural

## Compilation

Nécessite **SDL2** :

```bash
sudo apt install libsdl2-dev   # Debian/Ubuntu
brew install sdl2              # macOS
```

```bash
make
```

## Utilisation

```bash
./planet          # planète aléatoire (seed = heure actuelle)
./planet 42       # planète reproductible avec le seed 42
./planet 1337     # autre planète
```

La fenêtre reste ouverte jusqu'à appui sur **Q / Échap** ou fermeture.  
Le rendu est automatiquement sauvegardé dans **`planet.bmp`**.

## Structure

```
C-ProjetsCodes-GenerationLowPolyPlanet/
├── Makefile
├── README.md
└── src/
    └── main.c    # générateur complet (~730 lignes)
```

## Algorithmes clés

| Composant | Technique |
|-----------|-----------|
| Maillage | Bowyer-Watson Delaunay (~2200 points, ~4400 triangles) |
| Terrain | fBm (8 octaves) + bruit ridgé (crêtes montagneuses) |
| Humidité | fBm indépendant → influence la végétation |
| Ombrage | Produit scalaire normal sphère × direction lumière |
| Atmosphère | Glow pixel-by-pixel (α = (1-t)³) |
| Nuages | Ellipses rotées avec fondu radial |
