// Lecture des voix : OGG Vorbis (voix des personnages, tools/tts/generate_characters.py) ou WAV.
#pragma once

#include <SDL.h>

#include <filesystem>

namespace maupiti {

class Audio {
public:
    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool ok() const { return dev_ != 0; }
    // Joue un fichier .ogg ou .wav (interrompt la réplique en cours). Si le fichier demandé n'existe pas,
    // essaie le même nom avec l'autre extension. Renvoie false si aucun n'est lisible.
    bool play(const std::filesystem::path& file);
    void stop();
    bool playing() const;

private:
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec spec_{};
};

}  // namespace maupiti
