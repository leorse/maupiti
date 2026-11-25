# Architecture du jeu d'origine (version PC) — étape 4, premier passage

## 1. Deux programmes

`MAUPITI.BAT` lance successivement :

```
maupiti1      (MAUPITI1.EXE)
maup %1       (maup.exe)
```

| Programme | Langage | Rôle |
|---|---|---|
| `MAUPITI1.EXE` | Turbo Pascal, compressé LZEXE 0.91 (70 Ko décompressé) | **logique du jeu**. Reste résident (`int 21h / AH=31h`) puis laisse la main à `maup.exe` |
| `maup.exe` | Turbo C 1988, non compressé (54 Ko) | **moteur** : vidéo (CGA/EGA/VGA/Hercules/Tandy/Amstrad), son (haut-parleur, AdLib, Sound Blaster), décompression, textes, voix, souris, clavier, fichiers |

C'est la même organisation que *Mortevielle* (logique en Pascal, rendu en
assembleur/C). La logique est **portée de la version Atari** : elle lit et
écrit directement des données big-endian du format ST via les services
mémoire du moteur (voir services 3-5, 31-33).

## 2. Liaison entre les deux : `int 0xFE`

`maup.exe` installe le gestionnaire de l'interruption `0xFE` (vecteur
0000:03F8 → `maup.exe` 0x0625) et range son segment de données en 0000:03C4.
`MAUPITI1.EXE` a laissé l'adresse de son point d'entrée en 0000:03C6 ;
`maup.exe` y saute après son initialisation.

Convention d'appel : la logique pousse ses paramètres, fait `mov cx, sp`,
`mov ax, <n° de service>`, `int 0FEh`. Le gestionnaire (0x0625) appelle
l'entrée `n` d'une table de sauts (0x0653, 8 octets par entrée). Le résultat
revient dans `DX:AX`. Chaque service a une enveloppe Pascal dans le segment
`0x10B8` de `MAUPITI1` (51 enveloppes pour 51 services).

## 3. Les 51 services du moteur

« pile » = octets de paramètres (valeur du `retf` de l'enveloppe Pascal) ;
« appels » = nombre d'appels depuis la logique. Les rôles marqués « ? » sont
des hypothèses tirées du code touché (fichiers, palette, son…).

| N° | Adresse | Pile | Appels | Rôle |
|---:|---|---:|---:|---|
| 1 | 05479 | 8 | 1 | ? (zone texte) |
| 2 | 05A38 | 0x34 | 2 | affichage d'un texte avec chargement (décodeur 5 bits) |
| 3 | 01259 | 4 | 144 | **lire un octet** en mémoire moteur (pointeur far) |
| 4 | 01273 | 4 | 112 | **lire un mot big-endian** |
| 5 | 0128C | 4 | 2 | **lire un long big-endian** |
| 6 | 054EC | 8 | 1 | ? |
| 7 | 01331 | 0 | 1 | adresse de base d'une zone de données partagée |
| 8 | 01BE6 | 0x10 | 3 | ? accès fichier |
| 9 | 057C3 | 0x38 | 12 | **affichage d'un texte** (décodeur 5 bits, mise en page, retour à la ligne) |
| 10 | 01A20 | 0x18 | 3 | ? accès fichier |
| 11 | 01E12 | 8 | 1 | ? accès fichier |
| 12 | 03887 | 8 | 23 | ? graphisme |
| 13 | 01C5A | 0x14 | 7 | ? accès fichier |
| 14 | 03EAA | 0x18 | 41 | ? chargement / affichage d'image (fichier, palette) |
| 15 | 05354 | 8 | 1 | ? |
| 16 | 03C92 | 8 | 3 | ? chargement de lieu |
| 17 | 038C6 | 0x18 | 7 | **attente d'action** (souris, clavier) |
| 18 | 07AB8 | 0x24 | 1 | ? palette / fondu |
| 19 | 027F7 | 4 | 1 | ? |
| 20 | 05D1F | 0x38 | 1 | ? texte + voix |
| 21 | 055D1 | 8 | 29 | **nombre aléatoire** entre deux bornes |
| 22 | 055EF | 0 | 84 | ? (le plus fréquent après la mémoire : boucle d'événements ?) |
| 23 | 0428C | 0x18 | 1 | ? |
| 24-27 | 02640… | | 1 chacun | ? images / palettes |
| 28 | 0135D | 0 | 1 | initialisation de la mémoire (allocations DOS) |
| 29 | 056ED | 0x18 | 5 | ? |
| 30 | 066ED | 0x28 | 1 | **voix** : préparation et lecture d'une réplique du script phonétique |
| 31 | 012AD | 8 | 0 | **écrire un octet** |
| 32 | 012C6 | 8 | 44 | **écrire un mot big-endian** |
| 33 | 012E0 | 8 | 5 | **écrire un long big-endian** |
| 34 | 05740 | 0x20 | 1 | ? |
| 35 | 01F39 | 8 | 1 | ? fichier (sauvegarde ?) |
| 36 | 01AC0 | 0x1C | 68 | ? |
| 37 | 0385F | 0 | 83 | ? |
| 38 | 01307 | 8 | 2 | taille d'une entrée dans une table d'offsets |
| 39 | 01CCD | 0x18 | 14 | ? fichier |
| 40-43 | 041D9… | | | ? chargements |
| 44-50 | 07515… | 0-4 | 1-13 | **musique et bruitages** (chargement MUS.HUF / SND.HUF, lecture, arrêt) |
| 51 | 08039 | 4 | 11 | ? son |

Ces 51 services forment la frontière à reproduire dans le nouveau moteur :
la logique portée appellera les mêmes fonctions, en C++.

## 4. Structure de `MAUPITI1.EXE`

Après décompression (`tools/re/unlzexe.py`) :

| Segment | Contenu |
|---|---|
| programme principal (0x0000–0xF210) | ~180 fonctions : la logique du jeu |
| 0x0F21 / 0x0F23 | bibliothèque d'exécution Turbo Pascal (514 appels) |
| 0x10B8 | enveloppes des 51 services `int 0xFE` (791 appels) |
| 0x10F1 | autre unité (70 appels) |

## 5. Suite de l'étape 4

1. Importer `maup.exe` et l'image décompressée de `MAUPITI1.EXE` dans Ghidra
   (x86 16 bits réel), appliquer les signatures Turbo C / Turbo Pascal.
2. Nommer les 51 services d'après leur code, puis remonter dans la logique.
3. Relier la logique aux données : bloc « logique » (G3/11, entrée 1 ;
   Atari archive 9), `SAUP.MAU` (état initial), fin des banques de sprites
   (zones), `DEMOFR.MAU` (démo, utile comme test automatique).
4. Rédiger le pseudo-code par sous-système : lieux et déplacements, actions,
   interrogatoires, horloge et événements, fins, sauvegardes.

Outils : `tools/re/disasm.py`, `tools/re/unlzexe.py`.

## 6. Interrogatoires et voix (étape 4, 2026-11-21)

Désassemblage de `MAUPITI1.EXE` décompressé : `python tools/re/unlzexe.py originals/pc/MAUPITI1.EXE out/re/maupiti1.exe`
puis `python tools/re/disasm.py x86img out/re/maupiti1.exe out/re/m1.asm` (adresses ci-dessous = image décompressée).

| Adresse | Rôle |
|---|---|
| `0x8A43` | **écran d'interrogatoire** (14 mots de paramètres, 1082 lignes) : zones cliquables du menu (sujets, « Répéter », « Questionner », « Argumenter »), boucle souris, horloge de 300 ticks sans réponse. 4 appelants : `0xB0BE`, `0xB840`, `0xB8EC`, `0xB91E` |
| `0x1773` | **dire une réplique** : `(locuteur 0..8, 4 longs…, réplique)`. Remplit une table de hauteurs (9 entrées) puis appelle le service 30 (voix) ; la hauteur est choisie par le locuteur |
| `0x63A3` | **moteur de règles** : recherche dans les tables de données (blocs `0xCA…0xE4` et `0x1EA…0x1F3`, 5 types) la réponse d'un personnage à un sujet |
| `0x5A97` | historique des répliques (pour « Répéter » : tables en `0x170` locuteur, `0x182` réplique) |

* **Table des hauteurs** (service 30, mot long par locuteur) :
  `+3, +3, +2, −1, 0, −2, −2, −1, −3`. Le locuteur est l'indice du personnage
  interrogé, dans l'ordre du menu (textes 78–86) :
  Maguy +3, Sue +3, Anita +2, Bruce −1, Roy 0, Chris −2, Bob −2, Anton −1, Juste −3.
  Les trois femmes ont bien les hauteurs les plus hautes et Juste (le vieux)
  la plus basse : l'hypothèse « indice = ordre du menu » est très plausible.
* **Numéro de réplique** : il est directement l'index du script phonétique
  (0 = texte 1138). Cas par défaut du code : 297, 362, 367, 375 et
  381/382 (tirage au hasard `381 + rand(0,1)`) = textes 1435, 1500, 1505,
  1513, 1519 et 1520, tous des phrases d'esquive (« Je n'ai rien à vous dire. »).
  Cela confirme l'alignement réplique k ↔ texte 1138 + k.
* Ce qui reste : le **moteur de règles** (`0x63A3`) donne à chaque couple
  (personnage, sujet) sa réponse ; son lecteur de tables n'est pas encore
  décodé. Pas besoin de tout décompiler : voir « Suite » ci-dessous.

### Suite proposée
Plutôt que décompiler à la main les ~180 fonctions, exécuter la vraie logique
dans un petit émulateur x86-16 (bibliothèque `unicorn`) dont les services
`int 0xFE` sont écrits en Python : l'émulateur sert d'**oracle** pour dresser
la liste (locuteur, sujet, réplique) en l'appelant pour chaque couple, et plus
tard pour **tester** la logique réécrite en C++ (étape 6) en comparant les
deux comportements.

## 7. Services graphiques, services de données, bloc « logique » (2026-11-21)

Lecture de `maup.exe` : la plupart des services sont **graphiques** (donc inutiles à
l'oracle, à simuler par un simple « ne rien faire ») :
13 rectangle plein, 39 trait, 36 copie de rectangle écran ↔ mémoire, 14 affichage d'image,
25 mise en place de l'écran à partir de blocs de données (appelle le chargeur de conteneurs
`0x3F9F`), 11 et 35 disquette de sauvegarde (2467 octets), etc.
Les services de **données** sont les seuls à compter pour la logique :
* 3/4/5 et 31/32/33 : lecture/écriture octet, mot, long **big-endian** ; les pointeurs
  sont de simples **adresses linéaires 20 bits** du PC (`hi:lo`, `hi` ≤ 0xF), pas un espace
  Atari émulé ;
* 7 : adresse linéaire de `DS:0x166` de MAUPITI1 (zone partagée) ;
* 28 : réserve la mémoire (allocations DOS) et renvoie la zone des données ;
* 15 / 38 : adresse d'un bloc dans une table de tailles (voir plus bas) ;
* 21 : nombre aléatoire ; 30 : voix ; 44-50 : musique.

**Bloc « logique »** (`tools/maupiti/pc/logic.py`) : 516 blocs adressés par une table de
tailles additionnée une fois (fix-up). Le **moteur de règles des interrogatoires**
(`0x63A3`) lit pour 5 types de règles les blocs `0xC8-0xE7` (types : ids `0xCA 0xCE 0xD6 0xDC
0xE4`, chacun suivi de ses blocs de clés, d'arités et de réponses) et les tables d'état
`0x1EA…0x1F3` ; il renvoie un numéro de réplique (0x154 = « aucune »). Format exact des
règles : à décoder en suivant le code de `0x63A3` (boucles aux adresses `0x645B-0x6746`).

L'émulateur (`tools/emu/`) exécute ce code tel quel : il suffit que la zone de données soit
chargée (services 25/chargeur `0x3F9F` : lecture des blocs dans les conteneurs `G*.CON`).

## 8. Réponses des personnages : matrices de la logique (2026-11-21)

La réponse d'un personnage dans l'écran d'interrogatoire (`0x87D4`) est le mot big-endian
d'indice `choix × 9 + colonne` du bloc `type + 1` du bloc « logique » (`0x10B8:0x5A` = lecture de mot) ;
0 avec le type 1 → esquive `0x121 + hasard(0..2)` (réplique 289-291). Les blocs 1 à 5 et 135
sont des matrices à 9 colonnes (un personnage par colonne ; ligne = locuteur dans le bloc 4).
`tools/maupiti/pc/dialogues.py` les lit ; l'oracle (`tools/emu`) a servi à fixer l'orientation et à
valider : tout ce qu'il avait trouvé y figure. Restent 61 répliques issues des blocs de règles.

## 9. Démarrage du jeu (2026-11-21, oracle + désassemblage)

* `maup.exe` (PC) ne montre pas d'écran titre : il pose les questions de configuration (carte vidéo,
  clavier `C` ou souris `S`), installe `int 0xFE` puis appelle la logique.
* Programme principal de la logique : initialisation (`0x2722` : ressources, souris), cadre (`0x86D9`),
  puis boucle `0xEBC8` jusqu'à la sortie (`[0x131]`). Au premier tour, `0xEBC8` :
  1. service 40 = **nouvelle partie** : `maup.exe` lit `SAUP.MAU` (0x9A3 octets) en `DS:0x166` et
     `DEMOFR.MAU` (script de démonstration) dans sa mémoire ;
  2. dessine le lieu (`0x41CB`, service 24 : ressource = lieu - 1) et le panneau ;
  3. fondu d'ouverture tiré au hasard (`0x25D5`) ;
  4. services 47/46 : musique du lieu (service 47 : cabine = musique 15 ou 16 en alternance ;
     lieux 0x1E et 0x1F : cycles 20-23 et 24-26) ;
  5. service 11 = proposer une **partie sauvegardée** (`SAUVEMAU.00x`) ; absente → on continue.
* État (`DS:0x166`, mots little-endian) : `[0x166]` lieu, `[0x168]` heure = **heures depuis le 30.01
  à 12 h**, `[0x16A]` tranches de 10 minutes, `[0x16C]` forme 0-99. Départ : lieu 26 (la cabine,
  ressource 25 = `G6` rang 2 = Atari arc 49), 20 h → **31.01, 8 h 00 AM**, forme 75.
* Le service 9 reçoit des numéros de texte **à partir de 1** (`maup.exe` retranche 1).
* Panneau (relevé sur les appels du service 9 : couleur, largeur, x, y ; ombre noire décalée d'un pixel) :
  lampe = personnages présents ou texte 35 « Vous êtes SEUL » (x 276, y 50, largeur 30, centré) ;
  forme = texte 36 + forme/20 (x 265, y 110, largeur 50) ; AM/PM 140/141 (x 270, y 164) ;
  date 228/229 (x 286, y 164) ; horloge centrée en (290, 141), aiguilles 12 et 9 px ;
  zone du bas au départ : texte 478 « Ça sent le roussi... » (centré, largeur 258).
* Les décors Atari réservent en noir les éléments animés (ventilateur de la cabine, pendule du
  lieu 3…), décrits par la 3e entrée de l'archive du lieu (script `9E/8E/96/86 … 7F`). Les images de
  ces animations ne sont pas encore localisées ; en attendant, `build_pack.py` remplit ces zones avec
  le dessin fixe du décor PC sur le mur Atari (`rooms/NN_patch.bin`).

## 10. Correction : numérotation des répliques (2026-11-22)

Comme pour les textes (service 9), les numéros de réplique passés au service 30 et rangés dans les
matrices **commencent à 1** : valeur u = réplique k = u - 1 du script phonétique = texte 1137 + u.
Matrices de l'interrogatoire (lignes, colonnes ; personnages dans l'ordre du menu, texte 78 + indice) :
bloc 1 = « Votre emploi du temps » [créneau du bloc 0x107][personnage] ; bloc 2 = « Qu'avez-vous
remarqué » [créneau][personnage] ; bloc 4 = avis [personnage qui répond][personne] ; bloc 5 = lignes 0-1
avis sur Marie, Lucie puis 20 sujets du bloc 0x106 [ligne][personnage] ; blocs 3 et 135 : à étiqueter.
Accueil = 368 + personnage. Menus : bloc 0x105 (questions + 11 personnes), 0x106 (sujets), 0x107
(39 créneaux), 0x10F (Répéter, Questionner, Argumenter, Stop).

## 11. Interrogatoire complet : clés, « Argumenter », objets (2026-11-22)

Clé d'une question (code de 0xADD4, personnage c dans l'ordre du menu) :
emploi du temps / remarques (types 1, 2) = c×40 + créneau ; « Que faites-vous là ? » (type 3) =
c×74 + (heure − 20) — réponse dans le **bloc 3**, selon l'heure de jeu ; avis sur un des 9 (type 4) =
c×9 + personne ; sujets, avis sur Marie/Lucie (type 5) = c×22 + ligne du bloc 5.
**Argumenter** (moteur de règles 0x63A3) : type t, en-tête 0xCA/0xCE/0xD6/0xDC/0xE4 = [N, arité des N+1
règles], bloc suivant = clés ; puis par étape s : octets (type de la déclaration opposée) et mots
[clé opposée × n][réplique opposée × n][nouvelle réponse × n]. Une règle se déclenche quand on oppose
au personnage, sur cette question, la déclaration décrite (ex. Chris / « Les coups de feu » face à Roy
« C'est Chris qui était visé » → « Le tireur visait Roy… ») ; sous la contrainte (drapeaux 0x3E5-0x3E7 :
menacer, soudoyer, tabasser) la condition est levée ; compteur d'étape par règle en DS:0x1B8.
**Montrer un objet** (bouton 5 de l'écran de réponse, choix dans l'inventaire par 0x1CD5) : bloc 135
[objet][personnage] (objet 0 = l'argent : « Pourquoi de l'argent ? »).
Décodeur : `tools/maupiti/pc/dialogues.py` (`static_attribution`, `rules`) ; matrice lisible :
`tools/emu/matrice.py`. Couverture : 445 répliques sur 450.

## 12. Son de la version Atari ST : le pilote de l'archive 2 (2026-11-22)

**Carte mémoire du jeu Atari** (table du programme de démarrage, archive 1 en `$73DE8`, et appels au
chargeur ; n° de requête + `$1E` → table `MAP` du chargeur → n° d'archive) :

| Adresse | Contenu |
|---|---|
| `$138` | **archive 2 : pilote son** (table de vecteurs `$138`-`$18E`, code, tables de mixage en `$3C70`) |
| `$501A4`, `$5C4F4`, `$68844` | archive 18 (2 lecteurs ; avec 1 lecteur : archive 4 en `$501A4`, `$5B16C`) |
| `$74C48` | archive 3 |
| `$B66C` | archive 0 : bibliothèque du moteur (table de pointeurs ; `$B6DC` = chargement d'une archive ; `$B774` → 3 × 256 niveaux de volume YM, recopiés par le pilote dans sa table de mixage `$3C70`) |
| `$148E8` | chargeur de disquettes (pistes 0) |
| `$178A4` | archive 6 (2 entrées) : programme principal |

Variables système de démarrage : `$C6` ≠ 0 = second lecteur présent, `$C8` ≠ 0 = plus de 512 Ko.

**API du pilote** (pointeurs ; `d0` = argument) :

| Vecteur | Fonction |
|---|---|
| `$15C` | charge la **banque musicale** n (archive 60, entrée n ; décompressée si `"CO"`) |
| `$164` | lance la séquence k de la banque chargée (canal `$37B0`) |
| `$148` | joue la banque n comme bruitage (canal suivant) |
| `$13C` | durée : `$36EC` = n × 80000 |
| `$168` | **tic de la partition**, appelé à chaque VBL (50 Hz) par le gestionnaire VBL de l'archive 0 |
| `$186`, `$18E`, `$18A` | initialisation : tables (archive 12), préchargement, banque du titre (archive 19) |
| `$17C` (variable) | 1 = pilote suspendu ; mis à 0 par le démarrage (`$73EE4`) |

**Données** : 76 entrées de l'archive 60 (les archives 54, 11 et 62 en sont des sous-ensembles, selon la
configuration) ; archive 61 = 146 sons numérisés (bruitages, chargés à la demande) ; archive 12 = tables
d'index (entrées : n° de requête dans l'octet fort, décalage sinon) ; archive 19 = musique du titre.
Musique de chaque lieu : table de l'archive 0 en `$BAF0`, indexée par le n° de requête du décor
(`$FA94` ; 0-6 → 5, 7 → 6, 8 → 3, 9 → 7, 10 → 12, 11 → 1, 12 → 4, 13 → 10, 14 → 9, 15 → 8, 16 → 11,
17-27 → 0, 28 → 10, 29 → 2 ; avec un seul lecteur, table de `$B7F6`).

**Restitution** : pas de sons « YM » : les voies de la puce sont coupées (registre 7 = `$FF`) et le pilote
joue **4 voix d'échantillons**, une par timer du MFP (A `$134`, B `$120`, C `$114`, D `$110` ; la
fréquence du timer donne la hauteur de la note). Chaque interruption lit l'octet suivant de sa voix
(adresse modifiée dans le code), additionne les 4 voix et écrit, par une table de 16 octets par niveau
(`$3C70`), les **3 registres de volume** (8, 9, 10) : la puce sert de convertisseur numérique-analogique.
Un octet nul termine un segment ; une file de 3 pointeurs par voix (`$8-$10`, `$14-$1C`…) enchaîne les
segments (boucles). 10 canaux de partition de 16 octets en `$37A0` ; le canal 0 (titre, jingles) est
prioritaire sur le canal 1 (musique du lieu).

**Instruments et bruitages** : table d'échantillons (`$335A`, 12 octets par entrée : décalage ou n° de
requête dans l'octet fort, type en +5, longueurs en +6/+8/+10). La routine `$102C` les charge à la demande
(archive 61 en jeu, archive 18 pour le titre) et les **décode** dans un tas (`$170`/`$174`, limite
`$334E`) : ADPCM 4 bits avec tables de deltas en `$1268` (type ≥ 0), ou 6 bits (types -1, -2). En jeu, la
zone de l'archive 18 (échantillons du titre) devient ce tas.
**Ordre d'initialisation** : `$18A` pendant l'intro (banque du titre sur le canal 0), puis `$186` et
`$18E` à l'entrée dans le jeu ; inverser l'ordre écrase les tables d'échantillons.
**Table de mixage** : `$3C70`, construite par `$C58` à partir de 3 × 256 volumes de l'archive 0
(pointeur `$B774` → `$C2C0`). Chaque entrée est un triplet de volumes choisi pour approcher, sur la puce
réelle, le niveau linéaire de son indice ; la somme des 4 voix (centre 4 × `$80`) sert d'indice. Pour la
restitution, on remplace donc chaque triplet écrit par l'indice de son entrée : niveau voulu par le jeu,
sans modèle de la puce.

**Oracle** : `tools/emu/atari_sound.py` exécute le pilote d'origine (unicorn, 68000) avec un MFP et une
VBL simulés et rend les volumes YM en WAV (`out/atari/son/`). Deux points indispensables pour un rendu sans trous : (1) le tic (`$168`) doit pouvoir être interrompu par les timers — pour démarrer des voix en phase, il attend en boucle (`$177C`) que l'échantillon d'une voix en cours avance ; (2) les gestionnaires `$31F0` et `$3214` écrivent la nouvelle fréquence dans l'instruction suivante (`ori.b #n,$FA1D` en `$320C`, `$3230`) avant de l'exécuter : un émulateur à traduction de code doit l'exécuter avec la valeur écrite. Rendu : environ le temps réel par morceau (`tout` : en parallèle). Le moteur fera de même avec Musashi
(`engine/third_party/musashi`, licence MIT) sur le pilote extrait des disquettes.

## 13. Lieux Atari : ambiance sonore (entrée 2) et animations (entrée 3) (2026-11-22)

Une archive de lieu a 4 entrées : 0 décor (palettes A, B puis image), 1 banque de sprites, **2 ambiance
sonore**, **3 animations et zones**. Chargement : archive 0, `$12470`-`$125EA`.

**Entrée 2 = ambiance sonore** (et non une animation graphique) : si elle fait plus de 4 octets, le jeu en passe
l'adresse au pilote son (`$FA8C` → vecteur `$158`) au changement de lieu, après la banque musicale (`$15C`) et
`$13C` ; format d'une banque musicale (en-tête, 4e octet = nombre de pistes). Chaque piste est démarrée par
`$138` (n° de piste, canaux `$37D0`+) : le système d'animation (`$12D62` → `$BC8A`) déclenche le bruitage de
chaque animation à chaque cycle. Juke-box : F3, « Ambiance - lieu N » (pistes redéclenchées en boucle).

**Entrée 3** : table de décalages (mots, fin 0 ; relatifs au début - 2), puis enregistrements typés (`$1262E`) :
type `$0E` → deux tables du moteur ; types 0/4/8/12 → `$128BA`. **Type 0 = animation** (`$128C8`) :

    +0 type 0, +2 taille de l'espace de travail, +4 emplacement de l'image décodée,
    +6 décalages de descripteurs (relatifs : position après le mot - 2 + valeur) jusqu'à FFFF,
       puis décalage de la bande d'images (format Lankhor, décodée par $E5AC dans l'espace de travail)
    descripteur : programme (entrées de 4 mots : table, durée, ?, ? ; FFFE boucle, FFFF fin), tables (valeurs
       = décalage en pixels y*320+x dans l'espace de travail, bit 0 = drapeau ; fin FFFF, double FFFF à la fin),
       position écran (décalage mémoire vidéo ST : 160 octets/ligne, 8 octets par bloc de 16 pixels),
       largeur en blocs - 1, hauteur - 1, puis emplacements (octets, 160 par ligne) jusqu'à un mot négatif

À l'initialisation, chaque emplacement reçoit une copie du décor à l'écran, puis la bande est décodée par-dessus à
sa position : un emplacement couvert par la bande est une image de l'animation, les autres sont des tampons de
fond. La visionneuse (F1, touche N ; F5) fait défiler les images couvertes par la bande, à 10 images/s ; l'ordre
et les durées exacts (programme) restent à exécuter comme le jeu (étape 6). Exemple : cabine (archive 49),
ventilateur mural en (224,47), 32×30, 4 images. Prototype Python : `tools/maupiti/atari/anims.py`.

## 14. Moteur hybride : la logique PC exécutée par le moteur (2026-11-22)

Décision (voir PLAN.md) : pour rendre le jeu jouable vite et fidèlement, le moteur exécute la logique
d'origine de la version PC (`MAUPITI1.EXE` décompressé, Turbo Pascal, 8086) et fournit en C++ les 51
services du moteur (`int 0xFE`) avec les graphismes, la musique et les voix Atari. La logique sera ensuite
réécrite fonction par fonction en C++, chacune comparée à l'originale, jusqu'à retirer l'émulateur.

* `engine/src/logic/x86.{hpp,cpp}` : processeur 8086 (mode réel, ajouts 80186), sans dépendance externe.
  La logique n'utilise que des instructions 8086 courantes (relevé statique : 59 mnémoniques, réels Pascal
  calculés en logiciel).
* `engine/src/logic/host.{hpp,cpp}` : chargement à l'adresse 0, DOS minimal (version, mise en résident
  `AH=31h` puis reprise à l'adresse de la boîte aux lettres `0000:03C6`, comme `maup.exe`), lecture des
  octets de paramètres de chaque service dans les enveloppes Pascal, services de données (3/4/5, 31/32/33, 7,
  15, 21, 25, 28, 38, 40, 11) ; les autres sont délégués au jeu (`on_service`).
* Données : `pack/pc/` (`build_pack.py`) : `maupiti1.img`, `maup.img` (tables des ressources), `G*.CON`,
  `SAUP.MAU`, `DEMOFR.MAU`.
* **Validation** : `maupiti --pack out/pack --logique N trace.txt` (hasard figé) contre l'oracle Python
  (`out/re/trace_py.py`) : **219 852 appels de services identiques**, du démarrage jusqu'à l'attente d'une
  action (7,2 millions d'instructions, 0,17 s).

Premiers relevés des services graphiques (mots dans l'ordre `w[0]` = dernier empilé ; les pointeurs
`lo hi` désignent des tampons du moteur PC, par exemple l'écran de travail en `0x5618A`) :
9 texte (n°, ?, centré, couleur, hauteur, largeur, y, x, puis police, tampon, tables) ; 13 rectangle plein
(y2, x2, y1, x1, tampon) ; 39 trait (couleur, y1, x1, y2, x2, tampon) ; 36 copie de rectangle (y2, x2, y1,
x1, source, destination, sens) ; 24 décor (ressource, tampons) ; 6 puis 14 chargement et affichage d'image ;
12, 22, 23, 37, 42, 51 à établir dans `maup.exe` (table de sauts `0x653`).

## 15. Services du moteur réécrits (partie pilotée par la logique d'origine) (2026-11-22)

`engine/src/game.cpp` : la logique PC dessine tout l'écran par les services, réécrits sur un écran de
travail indexé 320×200 (comme celui de `maup.exe`, segment `[0x494]`, un octet par pixel) affiché avec la
palette courante. Table de sauts de `maup.exe` : `0x653`, service n = entrée n - 1 (`call far`) ;
désassemblage : `out/re/maup_services.asm`.

| Service | Rôle (paramètres : mots, `w[0]` = dernier empilé ; « long » = 2 mots) |
|---|---|
| 1, 6 | emplacement du n-ième élément d'une table de tailles (octets / mots BE) : descripteur de 4 / 8 octets (rendu à une adresse libre) |
| 2 | liste de textes (menus), `0493:1108` : n° = octets à `w14:w15`+1 (pas de 2), relatifs au groupe de `w6:w7` ; nombre `w16`, x `w20`, y `w18`, couleur `w24`, style `w12` ; les `w22` premiers alignés à droite (finissent en x), les suivants à partir de x ; lignes de 12 px faites de **tuiles 16×12** (bords tirés parmi 8, milieux en x 128, couleur 2 transparente) prises dans la planche copiée par le service 23 (64 premières lignes de l'écran) ; texte 2 px plus bas |
| 9 | texte : longs n°, ?, centré, couleur, hauteur, largeur, y, x, police, tampon, table, **entrée de groupe** (p11), … ; n° global = (p11 - table des longueurs) / 2 + n° - 1 (répliques : 1138) ; retour à la ligne de `0493:0FC6` : somme des avances, au-delà de la largeur le dernier espace devient un saut de ligne et la somme repart de 0 |
| 12, 22, 37 | curseur : forme, cacher, montrer (le moteur montre le curseur du système) |
| 13 | rectangle plein couleur 0 (y2, x2, y1, x1) ; tampon nul = recopie à l'écran |
| 14 | image Lankhor à `w6:w7` (calculée par la logique avec le service 6), décalage **x = `w4`, y = `w2`** (décodeur 0493:0000 : positions des morceaux +4/+6 augmentées de [bp+0xC]/[bp+0xE]) ; si la dernière ressource chargée est 0x21 (documents) et que l'image a sa palette (octet 0 ≠ 1), celle-ci est recopiée dans la palette courante de la logique (appliquée ensuite par le service 18) ; `w10:w11` non nul : recopie du rectangle à l'écran (routine du service 34). Sprites du décor (bloc 1 de la ressource du lieu) : mêmes images, même ordre et mêmes positions que les sprites Atari (entrée 1 de l'archive du lieu) ; le moteur dessine le sprite Atari de même rang (rang retrouvé en parcourant le bloc PC avec `image_size`) |
| 23 | souris et horloge : adresse de l'enregistrement d'entrée (+0 clic, +2 x, +4 y BE, +0x16 horloge). L'interruption 1Ch de `maup.exe` (`02DB:06F0`) ajoute 0x200 au mot **en little-endian** toutes les 36 interruptions (≈ 2 s) ; la logique le lit en big-endian (+2), l'additionne et divise par 60 (`0x3943`) : **10 minutes de jeu par minute réelle** |
| 24 | décor : blocs 0 (palettes A, B, image en +0x40) et 1 (sprites) de la ressource chargés comme `maup.exe` ; le moteur dessine le **décor Atari** (lieux.txt) avec sa palette A et lance la musique Atari du lieu |
| 30 | voix : réplique `w[0]` (à partir de 1), hauteur signée `w[2]` (table 3, 3, 2, -1, 0, -2, -2, -1, -3 dans l'ordre du menu) → voix générée du locuteur possible qui a cette hauteur |
| 36 | sauvegarde / restauration d'un rectangle, 4 bits par pixel (x arrondis à 16), sens `w12` |
| 39 | trait (couleur, y1, x1, y2, x2) |
| 11, 35 | reprendre / sauvegarder l'état (taille `w0`, adresse `w2:w3`, 2459 octets) : fichier `sauvegardes/partie.mau` (au lieu des disquettes `SAUVEMAU.00x`) |
| 18 | transition d'écran (7 modes, `07AB:0008`) ; applique la palette courante de la logique (16 mots ST à `w12:w13`) ; effet non reproduit (immédiat) |
| 26 | écran d'interrogatoire (ressource 0x22) : bloc 0 = fond, bloc 3 × personnage + 1 = buste (décalage 0x1C, -1), + 3 = morceau ; personnage `w8` ; le moteur dessine **l'archive Atari 15** (même structure) |
| 30 | (suite) la voix d'origine est **bloquante** : la logique attend la fin de la réplique (texte affiché), au moins 60 ms par caractère |
| 42 | palette selon l'heure (`02DB:1A13`) : t = (w6 / 2) mod 24 ; 8-18 h A, 6-8 h et 18-20 h B, nuit C ; copiée dans la palette courante de la logique (`w2:w3`) |
| 27 | carte de l'île (filature, `0159:0FF3`) : ressource 0x23 chargée à `w2:w3`, bloc 0 = carte (sa palette va dans les trois palettes A, B, C), bloc `w4`+1 = portrait de la personne suivie au décalage (-82, -26) ; rend DX:AX = 0x00C4:0x000B. Équivalent Atari : archive 14 (entrée 0 = carte, mêmes rangs) |
| 17 | marqueurs de la carte (`02DB:0B16`) : formes 7 et 8 du curseur (planche, (128,36) et (176,36), 16×12), dessinées à l'écran ; marqueur `w10` (0/1), `w8` ≠ 0 : l'effacer ; cible (x `w2`, y `w0`), départ (x `w6`, y `w4`) ou, si `w4` = -1, l'ancienne cible ; quand les deux ont une cible, ils s'y déplacent d'un point par pas (`[0x63D]` = 1) et la logique attend |
| 41 | changement d'heure (`02DB:18AF`) : palette A, B ou C de l'écran courant selon (`w2` / 2) mod 24, avec fondu ; `maup.exe` garde les trois palettes en `0x79E`/`0x7BE`/`0x7DE` (remplies par le décor, ou par la carte) |
| 29 | recopie d'un rectangle de l'écran de travail à l'écran (y2, x2, y1, x1 ; `[0xDC]:[0xDE]` = mémoire vidéo) |
| 34 | recoloriage d'un rectangle (x1 `w4`, y1 `w6`, x2 `w8`, y2 `w10`) par une table de 16 octets (`w12:w13`) ; maup.exe l'encadre d'une inversion des 4 bits (ordre VGA), nos indices Atari prennent la table telle quelle. Avant l'interrogatoire : le décor passe en gris et se voit à travers le fond (archive 15, non opaque) |
| 8 | point 2×2 (couleur `w0`, y `w2`, x `w4`) |
| 10, 16, 19, 20 | non écrits / approchés : cadres (10), palette + tremblement d'écran (16), recopie 32×32 à l'écran (19 : tout l'écran est recopié), loupe ×2 (20) |
| 47 | son du contexte `[0x66B]` (ressource du lieu, 0x1F carte, 0x20 interrogatoire), comme le moteur Atari (`$BA48`) : code de la table `$BAF0`. Cas général : **pas de morceau** — la « banque » n'est qu'un jeu de sons : `$15C` banque, `$13C(0x41)`, séquence d'ambiance du lieu (entrée 2 de son archive) par `$158` si elle existe, `$154` (échantillon numérisé 0x90 d'entrée dans le lieu). Cabine (0x19) : ce cas la 1re fois (ambiance très faible : pas de musique), puis bruitages 0x21 / 0x1E en alternance + `$13C(0x44)`. `$FE` (carte) : bruitages 0x22-0x26 à tour de rôle ; `$FF` : 0x27, 0x1F, 0x28, 0x29 ; `$FD`/`$FC` : rien |
| 48 | jingle : bruitages 0x1D / 0x20 en alternance (Atari `$B88C` ; PC pistes 17/18), sauf après l'entrée en cabine ; 45 : arrêt |
| 46 | heure à l'entrée dans un lieu (demi-heures depuis le 30.01 à 12 h) = Atari `$BB12` : séquence de la banque du lieu selon l'heure (`$BB8E`[code] → liste `$BB9C` de couples seuil / séquence, `$164`, par-dessus l'ambiance) ; heures 0x23, 0x28, 0x2B, 0x4B, 0x5A (`$BBFC`) : une fois, bruitage n + 14 dans les lieux 0-6 et 12 ; sinon 1 fois sur 8 : bruitage d'ambiance suivant de `$BC5E` (42-47) par `$144` ; heure 0x45 : échantillon 0x83 (`$B906`, une fois) |
| 10 | panneau (0159:0490) : tuiles 16×36 de la planche (gauche 160,0 / milieu 144,0 répété / droite 176,0 ; w10 ≠ 0 : 208 / 192 / 224), x w4, y w6, largeur w8 ; texte 9 sans table (pointeur −1) : nombre p0 (pourcentage d'indices) |
| couleurs | les n° de couleur passés par la logique (textes 9, traits 39, points 8) sont dans l'ordre VGA de maup.exe (4 bits inversés) : convertis vers l'ordre Atari |
| jingle / fouille | joués par-dessus l'ambiance (appels du pilote sans remise à zéro) et arrêtés comme l'Atari par `$17E` (canaux 0 et 2) : au retour au décor (service 24) et au 2e effacement du cadran (fin de fouille) |
| 13 (fouille) | une fouille fructueuse efface le cadran (service 13 sur 0x108,0x6C-0x13C,0xC3) ; à ce moment la version Atari (programme principal `$22F32`) joue au hasard le bruitage 67 ou 68 par `$144` (canal 1 ; 9 s / 21 s). La logique PC n'appelle aucun service son à cet endroit : le moteur se cale sur cet effacement |
| 43, 44, 46, 49-51 | non écrits : son PC (44 piste n, 46 événements horaires dans les lieux ≤ 6 ou 12, 51 bruitages) ; correspondance avec les bruitages Atari à établir. Programme principal Atari (archive 6, entrée 1 chargée en `$23BF4`) : talons `movea.l $B7xx,a0 ; jmp (a0)` vers la table de la bibliothèque (`$B730` → `$144`, `$B758` → `$148`, `$B734` → `$17E` arrêt, `$B74C` hasard) |

**Écran de travail et écran affiché** : maup.exe dessine dans un écran de travail (`[0x494]`) et n'en recopie à l'écran que des rectangles : service 18 (modes 2 et 3 : tout l'écran avec effet ; 4 et 5 : rectangle, x arrondis à 16 ; 0, 1, 6 : fondus de palette seuls), 29, et les services 8, 13, 14 (zone de l'horloge), 36, 39 appelés sans tampon ; les menus (service 2) sont dessinés aussi directement à l'écran. Le moteur garde les deux écrans : les états intermédiaires (horloge dessinée 15 points plus haut pendant la filature) ne se voient pas.

**Graphismes Atari** : `map.bin` (table du chargeur Atari) donne l'archive Atari de chaque ressource PC (39 → 10 interface, 31 → 22 objets, 32 → 20, 33 → 21 documents, 34 → 15 interrogatoire, 35 → 14 carte, lieux → leur archive). Le service 14 dessine l'image Atari de même sous-bloc et de même rang (géométrie vérifiée).

La logique s'exécute à environ 4 millions d'instructions par seconde (vitesse d'un PC de l'époque).
Résultat : écran de départ complet, menus en planches de bois, déplacements, actions (« fouiller » puis un clic
sur le décor), interrogatoire complet (accoster Bruce, Questionner > emploi du temps > « Le 30 » : « On a jeté
l'ancre vers 2 heures du matin. », voix de Bruce), Stop, sauvegarde et reprise : capture `maupiti --screenshot partie f.bmp`,
clics simulés `MAUPITI_CLICKS="x,y;…"`, traces `MAUPITI_DEBUG=1`.
