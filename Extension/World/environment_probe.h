#pragma once
#include <string>
#include <string_view>

namespace dingosdk {
// Research probe for a continuous time of day (contrib/dynamic-time-of-day.md).
// Game thread only, from the console's game-thread dispatcher.
//   dump                         every visual environment of the loaded level, its components
//                                and their reflected fields; written to environment-probe.txt
//   poke <type> <field> <values> one property write through the component's own setter, and
//                                which live bytes it changed; "default" writes the authored value
std::string environment_probe(std::string_view action, std::string_view arguments);
}
