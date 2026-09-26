#include "usd/loader.h"
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    auto s = remix::usd::load({argv[1]});
    auto &j = s.scene;
    std::cout << nlohmann::json({{"meshes", j["meshes"].size()},
                                 {"materials", j["materials"].size()},
                                 {"lights", j["lights"].size()},
                                 {"cameras", j["cameras"].size()},
                                 {"warnings", j["warnings"]},
                                 {"meters_per_unit", j["meters_per_unit"]},
                                 {"up_axis", j["up_axis"]}})
                     .dump()
              << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
