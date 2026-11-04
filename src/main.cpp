#include <SDL3/SDL_main.h>

#include "app/App.h"

// Demande le processeur graphique dédié sur les PC portables hybrides (NVIDIA Optimus, AMD).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argc, char* argv[]) {
    app::App application;
    return application.run(argc, argv);
}
