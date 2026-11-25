#include "audio.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <iterator>
#include <vector>

// Décodeur OGG Vorbis (third_party/stb_vorbis.c, domaine public), compilé à part en C.
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate,
                                        short** output);

namespace maupiti {

namespace fs = std::filesystem;

Audio::Audio() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "Audio indisponible : %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want{};
    want.freq = 44100;   // les voix (22 ou 44 kHz selon le modèle) sont converties à cette fréquence
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &spec_, 0);
    if (!dev_) std::fprintf(stderr, "Audio indisponible : %s\n", SDL_GetError());
    else SDL_PauseAudioDevice(dev_, 0);
}

Audio::~Audio() {
    if (dev_) SDL_CloseAudioDevice(dev_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

namespace {

// Échantillons 16 bits et leur format, quel que soit le fichier d'origine.
struct Pcm {
    std::vector<Uint8> data;
    SDL_AudioFormat format = AUDIO_S16SYS;
    Uint8 channels = 1;
    int freq = 0;
};

bool load_ogg(const fs::path& path, Pcm& out) {
    std::ifstream f(path, std::ios::binary);        // chemin large sous Windows (accents possibles)
    if (!f) return false;
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int channels = 0, rate = 0;
    short* samples = nullptr;
    const int frames = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &channels, &rate, &samples);
    if (frames <= 0 || !samples) return false;
    const auto* p = reinterpret_cast<const Uint8*>(samples);
    out.data.assign(p, p + static_cast<size_t>(frames) * channels * sizeof(short));
    std::free(samples);
    out.format = AUDIO_S16SYS;
    out.channels = static_cast<Uint8>(channels);
    out.freq = rate;
    return true;
}

bool load_wav(const fs::path& path, Pcm& out) {
    SDL_AudioSpec src{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.string().c_str(), &src, &buf, &len)) return false;
    out.data.assign(buf, buf + len);
    SDL_FreeWAV(buf);
    out.format = src.format;
    out.channels = src.channels;
    out.freq = src.freq;
    return true;
}

bool load(const fs::path& path, Pcm& out) {
    std::string ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".ogg" ? load_ogg(path, out) : load_wav(path, out);
}

}  // namespace

bool Audio::play(const fs::path& file) {
    if (!dev_) return false;
    stop();
    fs::path path = file;
    if (!fs::exists(path)) {                       // même nom, autre format (.wav <-> .ogg)
        fs::path other = path;
        other.replace_extension(path.extension() == ".ogg" ? ".wav" : ".ogg");
        if (!fs::exists(other)) return false;
        path = other;
    }
    Pcm pcm;
    if (!load(path, pcm)) return false;
    SDL_AudioCVT cvt;
    const int r = SDL_BuildAudioCVT(&cvt, pcm.format, pcm.channels, pcm.freq, spec_.format, spec_.channels, spec_.freq);
    if (r < 0) return false;
    const int len = static_cast<int>(pcm.data.size());
    pcm.data.resize(static_cast<size_t>(len) * (cvt.len_mult > 0 ? cvt.len_mult : 1));
    cvt.buf = pcm.data.data();
    cvt.len = len;
    if (r > 0 && SDL_ConvertAudio(&cvt) != 0) return false;
    SDL_QueueAudio(dev_, pcm.data.data(), static_cast<Uint32>(r > 0 ? cvt.len_cvt : len));
    return true;
}

void Audio::stop() {
    if (dev_) SDL_ClearQueuedAudio(dev_);
}

bool Audio::playing() const { return dev_ && SDL_GetQueuedAudioSize(dev_) > 0; }

}  // namespace maupiti
