# Personnages et voix

État au 2026-11-21. Sert de base à l'étape « une voix par personnage » ; les
choix de voix et la prononciation des noms seront arrêtés à cette étape.
Catalogue des sons déjà générés : `out/tts/catalogue.md` (non versionné, il
contient les textes du jeu ; régénérer avec `python tools/tts/catalog.py`).

## Quels textes sont parlés

Dans le jeu d'origine, seules les **450 répliques des personnages** sont dites
à voix haute : textes **1138 à 1587**. La réplique n° k du script phonétique
(`G4.CON` 8/28/10/1/0, 450 répliques non vides) est le texte 1138 + k
(corrélation des longueurs 0,87 ; alignement vérifié du début à la fin ; une
trentaine de répliques n'étaient prononcées qu'en partie).

Tout le reste est muet et ne doit pas être lu : interface (0–315),
commentaires du narrateur (316–797), documents et récits (798–898), scènes
observées (899–1002), quiz final (1003–1137). Détail dans `tools/tts/lines.py`.

Conséquence : la première génération complète (Piper « tom », 1515 fichiers)
lisait aussi l'interface ; elle sera remplacée. `generate.py` ne produit plus
par défaut que les répliques parlées.

## Qui parle

Le menu d'interrogatoire liste 11 personnages (textes 78–88). Le moteur vocal
d'origine connaît **9 locuteurs**, avec un décalage de hauteur chacun
(table de `MAUPITI1.EXE`, fonction de synthèse vers `0x17xx`) :
`+3, +3, +2, −1, 0, −2, −2, −1, −3`. Dans l'ordre du menu, cela donne des
valeurs positives aux trois femmes et négatives aux hommes (confirmé par le
désassemblage : l'indice du locuteur est l'ordre du menu, `docs/architecture.md` §6). Marie et Lucie, absentes, ne parlent pas.

| # | Personnage | Sexe | Hauteur d'origine (hyp.) | Ce que disent les textes (n° de texte) | Proposition de voix (à valider) |
|---|---|---|---|---|---|
| 1 | **Maguy** | F | +3 | tient la maison (60, 981) ; très pieuse, « mère poule » (1215) ; aimait Marie comme sa fille (1281, 1284) ; se lève la nuit (1206) | femme de 55–60 ans, chaleureuse, maternelle, un peu autoritaire |
| 2 | **Sue** | F | +3 | une des « filles » ; amoureuse de Bob (1380) ; veut quitter l'île (992) | jeune femme, légère, charmeuse, un peu capricieuse |
| 3 | **Anita** | F | +2 | une des « filles » ; jalouse de Marie (1296) ; boit (984) ; pleure Juste (986) ; Bob l'accuse (1464) | jeune femme, voix un peu rauque, à fleur de peau |
| 4 | **Bruce** | H | −1 | lié au *Bamboo* (936) et à sa radio (1113) ; nerveux, boit (945, 948) ; découvre le corps de Marie (942) ; probablement capitaine du *Bamboo* (journal de bord 840–871) | homme de 45–50 ans, fatigué, tendu |
| 5 | **Roy** | H | 0 | marin du *Bamboo*, répare l'avarie (939, 1149) ; boit (960, 1503) ; visé par les coups de feu (1458) | marin bourru, la cinquantaine, gouailleur |
| 6 | **Chris** | H | −2 | nouveau venu, « pas marin » (904) ; pose des questions sur l'île (1191) ; fume des Gitanes (1542) ; suspecté (1308) | homme de 30–35 ans, calme, réservé, précis |
| 7 | **Bob** | H | −2 | patron du *Brisban* (1449) ; épris de Marie (1254) ; pressé de partir (1347, 1429) | homme de 40 ans, autoritaire, sec |
| 8 | **Anton** | H | −1 | marin du *Brisban* sous les ordres de Bob (1449) ; triste, soucieux (987) | jeune marin, voix douce, mélancolique |
| 9 | **Juste** | H | −3 | « le dernier des Maupitis » (986, 1263) ; pêcheur, jardinier, chante (901, 972) ; assassiné (663) | vieil homme de l'île, grave, lent, chantant |
| – | Marie | F | – | disparue, retrouvée morte ; ne parle pas | – |
| – | Lucie | F | – | partie avant l'enquête (93) ; ne parle pas | – |
| – | Jérôme Lange | H | – | le joueur, détective ; ne parle pas | (narrateur éventuel, à décider) |

Indices utiles pour l'étape 4 : certaines répliques sont au féminin
(1151, 1197, 1206, 1248, 1457) ou au masculin (1167, 1198, 1402, 1414, 1491,
1503), ce qui restreint leurs locuteurs possibles.

## Attribution des répliques : corrigée (2026-11-22)

Les numéros de réplique de la logique (matrices, service 30) **commencent à 1** : la valeur u désigne le
texte 1137 + u. Faute de l'avoir vu, toutes les attributions précédentes étaient décalées d'une
réplique. Corrigé, tout concorde avec le jeu PC réel (Bruce : « Oui, matelot ? », avis sur Maguy « Une
sacrée bonne femme », sur Sue « Un beau brin de fille », sur Anita « Prolos de tous les rades… », nuit du
30 « On a jeté l'ancre vers 2 heures du matin »). L'ordre des personnages est bien celui du menu.
Matrice lisible : `python tools/emu/matrice.py` → `out/tts/matrice_dialogues.md`.
Réactions conditionnelles (dérangement, insistance, argent, menace, refus) : communes aux 9 personnages.
Les règles « Argumenter » (réponses conditionnelles) sont décodées : 445 répliques sur 450 attribuées,
1174 couples (personnage, réplique). Restent 5 répliques (refus d'être soudoyé, objets…).

## Attribution des répliques (oracle, première version)

L'oracle (`tools/emu/`) a interrogé chaque personnage sur tous les menus : 389 répliques sur 450
sont attribuées (`docs/repliques_par_personnage.md`, numéros seulement). L'accueil de chaque
personnage confirme l'ordre du menu : « Tu viens, chéri ? » (Maguy), « Tu me cherchais, mon chou ? »
(Sue), « Oui, matelot ? » (Anita), « Oui ? » (Bruce), « C'est pourquoi ? » (Roy), « Que voulez-vous ? »
(Chris), « Je vous écoute. » (Bob), « Oui, monsieur ? » (Anton), « Mais de quoi parlez-vous ? » (Juste).
Une même réplique peut être dite par plusieurs personnages (184 cas) : il faut un fichier son par
couple (personnage, réplique), soit 955 fichiers pour ce qui est atteint.

## Ce qu'il reste à établir (étape 4)

- Le locuteur de chaque réplique. Une même réplique générique (« Je dormais. »,
  « Oui ? ») peut être dite par plusieurs personnages : il faudra une version
  par personnage concerné, et seulement pour ceux-là.
- Confirmer l'ordre de la table des 9 hauteurs.

## Voix retenue pour la suite

**Qwen3-TTS « stable »** (voir `tools/tts/README.md`) : chaque voix est
décrite une fois en texte (modèle VoiceDesign), puis réutilisée par clonage
(modèle Base). La voix reste ainsi constante d'une réplique à l'autre, et
aucune personne réelle n'est clonée. Meilleurs scores au contrôle Whisper
(4–6 % d'erreur).

## Prononciation des noms (brouillon)

Les modèles lisent les noms à la française ou de travers. À appliquer
uniquement au texte envoyé à la synthèse, jamais au texte affiché :

| Écrit | Prononcer | Remarque |
|---|---|---|
| Roy | Roille | |
| Sue | Sou | |
| Bruce | Brouce | à valider |
| Chris | Criss | à valider |
| Maguy | Maguie | à valider |
| Bamboo | Bambou | à valider |
| Oualassou | (inchangé) | vérifier à l'écoute |
