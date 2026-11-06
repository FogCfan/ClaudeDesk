#include <SDL3/SDL_main.h>

#include "app/App.h"
#include "util/Paths.h"

#ifdef _WIN32
// Demande le processeur graphique dédié sur les PC portables hybrides (NVIDIA Optimus, AMD).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char* argv[]) {
    util::setLaunchArgs(argc, argv);
    util::importLoginShellPath();  // macOS : PATH du shell (Homebrew, node, ~/.local/bin…)
    app::App application;
    return application.run(argc, argv);
}
