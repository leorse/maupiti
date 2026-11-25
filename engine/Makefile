# Construction avec MSYS2 (environnement UCRT64) : g++ et SDL2.
#   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2
#   make            -> build/maupiti.exe
#   make run        -> lance la visionneuse sur ../out/pack

CXX      ?= g++
SDL_CFG  ?= sdl2-config
CXXFLAGS ?= -std=c++20 -O2 -pipe -Wall -Wextra -Wpedantic
CXXFLAGS += $(shell $(SDL_CFG) --cflags)
# Musashi (émulateur 68000, licence MIT) : configuration du projet dans src/m68kconf_maupiti.h
MUSASHI  := third_party/musashi
M68KDEFS := -I$(MUSASHI) -Isrc -DMUSASHI_CNF='"m68kconf_maupiti.h"'
CXXFLAGS += $(M68KDEFS)
LDLIBS   += $(shell $(SDL_CFG) --libs) -static-libgcc -static-libstdc++

# Les outils MinGW ont besoin d'un dossier temporaire inscriptible (sinon : C:\WINDOWS).
BUILD_DIR_WIN := $(shell cygpath -m "$(CURDIR)/build" 2>/dev/null || echo "$(CURDIR)/build")
export TMP    := $(BUILD_DIR_WIN)
export TEMP   := $(BUILD_DIR_WIN)
export TMPDIR := $(BUILD_DIR_WIN)

ifeq ($(origin CC),default)
CC       := gcc
endif
SRC := $(wildcard src/*.cpp src/logic/*.cpp)
M68KOBJ := build/m68kcpu.o build/m68kops.o build/m68kdasm.o build/softfloat.o
OBJ := $(SRC:src/%.cpp=build/%.o) build/stb_vorbis.o $(M68KOBJ)
BIN := build/maupiti.exe
PACK ?= ../out/pack

all: $(BIN)

build/%.o: src/%.cpp $(wildcard src/*.hpp src/logic/*.hpp) | build
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# décodeur OGG Vorbis (domaine public, https://github.com/nothings/stb) : compilé en C, sans avertissements
build/stb_vorbis.o: third_party/stb_vorbis.c | build
	$(CC) -O2 -w -DSTB_VORBIS_NO_PUSHDATA_API -c $< -o $@

build/m68k%.o: $(MUSASHI)/m68k%.c src/m68kconf_maupiti.h | build
	$(CC) -O2 -w $(M68KDEFS) -c $< -o $@

build/softfloat.o: $(MUSASHI)/softfloat/softfloat.c | build
	$(CC) -O2 -w $(M68KDEFS) -c $< -o $@

# tables d'instructions de Musashi, produites par son générateur
$(MUSASHI)/m68kops.c $(MUSASHI)/m68kops.h: $(MUSASHI)/m68k_in.c $(MUSASHI)/m68kmake.c | build
	$(CC) -O2 -w -o build/m68kmake.exe $(MUSASHI)/m68kmake.c
	build/m68kmake.exe $(MUSASHI) $(MUSASHI)/m68k_in.c

build/m68kcpu.o: $(MUSASHI)/m68kops.h

$(BIN): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDLIBS)
	@for dll in SDL2.dll libwinpthread-1.dll; do \
	  cp -u "$(shell dirname "$(shell which $(SDL_CFG))")/$$dll" build/ 2>/dev/null || true; done

build:
	mkdir -p build

run: $(BIN)
	$(BIN) --pack $(PACK)

clean:
	rm -rf build

.PHONY: all run clean
