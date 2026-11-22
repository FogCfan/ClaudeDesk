// Préréglages d'énergie (Settings) et migration des réglages existants.
#include <doctest/doctest.h>

#include <filesystem>

#include "app/Settings.h"
#include "util/Paths.h"
#include "util/Strings.h"

using namespace app;
namespace fs = std::filesystem;

TEST_CASE("énergie : chaque préréglage se reconnaît après application") {
    for (int m : {EnergyPerformance, EnergyBalanced, EnergySaver}) {
        Settings s;
        applyEnergyPreset(s, m);
        CHECK(energyPresetMatches(s, m));
        for (int other : {EnergyPerformance, EnergyBalanced, EnergySaver})
            if (other != m) CHECK_FALSE(energyPresetMatches(s, other));
    }
    Settings s;
    applyEnergyPreset(s, EnergySaver);
    CHECK(s.fpsMode == 1);
    CHECK(s.fpsCap == 30);
    CHECK(s.ecoIdle);
    CHECK(s.backgroundFps == 10);
    CHECK_FALSE(s.animations);
    // Un réglage de détail modifié : ce n'est plus le préréglage (l'application passe en « Personnalisé »).
    s.animations = true;
    CHECK_FALSE(energyPresetMatches(s, EnergySaver));
    // Automatique et Personnalisé ne sont pas des préréglages fixes.
    CHECK_FALSE(energyPresetMatches(s, EnergyAuto));
    CHECK_FALSE(energyPresetMatches(s, EnergyCustom));
}

TEST_CASE("énergie : réglages par défaut = Performance, migration des anciens réglages") {
    Settings def;
    CHECK(def.energyMode == EnergyAuto);
    CHECK(energyPresetMatches(def, EnergyPerformance));  // un fixe garde exactement le rendu d'avant

    fs::path f = fs::temp_directory_path() / ("claudedesk_energy_" + util::newUuid().substr(0, 8) + ".json");
    // Ancien fichier aux réglages d'origine : Automatique
    util::writeFileAtomic(f, R"({"fpsMode":0,"animations":true})");
    Settings a;
    a.load(util::pathToUtf8(f));
    CHECK(a.energyMode == EnergyAuto);
    // Ancien fichier personnalisé (économie au repos cochée à la main) : on ne l'écrase pas
    util::writeFileAtomic(f, R"({"fpsMode":0,"ecoIdle":true})");
    Settings b;
    b.load(util::pathToUtf8(f));
    CHECK(b.energyMode == EnergyCustom);
    // Aller-retour
    b.energyMode = EnergyBalanced;
    b.energyOnBattery = EnergyBalanced;
    b.backgroundFps = 12;
    b.save(util::pathToUtf8(f));
    Settings c;
    c.load(util::pathToUtf8(f));
    CHECK(c.energyMode == EnergyBalanced);
    CHECK(c.energyOnBattery == EnergyBalanced);
    CHECK(c.backgroundFps == 12);
    std::error_code ec;
    fs::remove(f, ec);
}
