@echo off
REM Genere les voix des personnages (voir tools\tts\generate_characters.py).
REM   generer_voix.bat --refs-only     cree d'abord les 9 voix de reference, pour les ecouter
REM   generer_voix.bat                 genere toutes les repliques (reprise possible : Ctrl+C puis relance)
REM   generer_voix.bat --only Maguy    un seul personnage ;  --limit 5 = essai ;  --dry-run = liste seulement
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
cd /d "%~dp0"
third_party\tts\qwen\Scripts\python.exe tools\tts\generate_characters.py %*
pause
