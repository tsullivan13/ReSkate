#include "environment_probe.h"
#include "local_world_controls.h"
#include "visual_environment.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/named_settings.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/World/world_layer_types.h"
#include "Engine/Vfs/content_cache.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

// Research probe, not a feature: contrib/dynamic-time-of-day.md says what it is for and how to
// read its report. It finds what a continuous time of day needs and ReSkate has not mapped yet:
// the components each visual environment carries beside fog, sky and wind (the outdoor light,
// which holds the sun), their reflected fields and authored values, where those values live in
// the live component, and the visual environment's own fields (a visibility or priority weight
// would let two time-of-day presets crossfade).
namespace dingosdk::profile_runtime {
namespace {
namespace reflect = addr::named_settings;

// The id a component's property setter takes is this hash of the field's name. Checked against
// four of the fog bindings in Engine/Game/Build/20260929/atmosphere.h.
constexpr std::uint32_t name_hash(std::string_view s) {
    std::uint32_t h = 5381;
    for (const unsigned char c : s) h = (h * 33) ^ c;
    return h;
}
static_assert(name_hash("Enable") == 0x8ba423e4u && name_hash("FogDistanceMultiplier") == 0x79cf6d43u &&
              name_hash("Start") == 0x0dc0efa5u && name_hash("End") == 0x0b87a32au);

constexpr std::size_t live_extent = 0x800; // bytes of a live object searched and compared

bool in_image(std::uintptr_t at, std::size_t size = 8) {
    const auto base = local_runtime().base;
    return base && at >= base && at - base <= supported_build::game_image_size - size;
}
bool heap_pointer(std::uintptr_t p) { return p >= 0x10000 && p < 0x7fffffff0000 && !(p & 7); }
std::string hex(std::uintptr_t v) {
    std::ostringstream s;
    s << "0x" << std::hex << v;
    return s.str();
}
std::string rva(std::uintptr_t v) { return in_image(v, 1) ? "rva " + hex(v - local_runtime().base) : hex(v); }

std::optional<std::string> c_string(std::uintptr_t at) {
    std::array<char, 160> text{};
    const auto n = memory::peek_cstring(at, text.data(), text.size());
    if (n <= 0) return {};
    for (std::ptrdiff_t i = 0; i < n; ++i)
        if (text[i] < 0x20 || text[i] > 0x7e) return {};
    return std::string(text.data(), static_cast<std::size_t>(n));
}

// The engine's reflection name registry (as named_settings.cpp reads it).
class Names {
  public:
    Names() {
        std::uintptr_t registry{};
        if (memory::read(local_runtime().base + reflect::symbol_registry, registry) && registry) {
            memory::read(registry + reflect::name_buckets, buckets_);
            memory::read(registry + reflect::name_bucket_count, count_);
        }
    }
    bool ready() const { return buckets_ && count_ && count_ <= 0x1000000; }
    std::optional<std::string> of(std::uintptr_t key) const {
        if (!ready()) return {};
        std::uintptr_t node{};
        if (!memory::read(buckets_ + (key % count_) * 8, node)) return {};
        for (unsigned hops = 0; node && hops < 4096; ++hops) {
            std::uintptr_t node_key{}, name{};
            if (!memory::read(node, node_key)) return {};
            if (node_key == key) return memory::read(node + 8, name) ? c_string(name) : std::nullopt;
            if (!memory::read(node + 0x10, node)) return {};
        }
        return {};
    }
    // Type names are keyed by the TypeInfo's descriptor.
    std::optional<std::string> type(std::uintptr_t type_info) const {
        std::uintptr_t descriptor{};
        if (!in_image(type_info) || !memory::read(type_info, descriptor) || !descriptor) return {};
        return of(descriptor);
    }

  private:
    std::uintptr_t buckets_{};
    std::uint32_t count_{};
};

struct Leaf { std::string path, type; unsigned offset{}; };
// A direct member of a class: what the property setter addresses by hash.
struct Member {
    std::string name, type;
    std::uint32_t hash{};
    unsigned offset{};
    std::vector<Leaf> leaves; // the member itself, or a value type's scalar parts
};

unsigned kind_of(std::uintptr_t type_info, std::uintptr_t &descriptor) {
    std::uint16_t flags{};
    if (!in_image(type_info) || !memory::read(type_info, descriptor) || !in_image(descriptor, 0x68) ||
        !memory::read(descriptor + 4, flags))
        return ~0u;
    return (flags >> 5) & 31;
}
bool field_array(std::uintptr_t type_info, unsigned want, std::uintptr_t &array, std::uint16_t &count) {
    std::uintptr_t descriptor{};
    if (kind_of(type_info, descriptor) != want ||
        !memory::read(descriptor + reflect::descriptor_field_count, count) || count > 1024 ||
        !memory::read(descriptor + (want == 3 ? reflect::class_fields : reflect::value_fields), array))
        return false;
    if (!count) return true;
    std::uint8_t first{}, last{};
    const auto bytes = std::size_t{count} * reflect::field_size;
    return array >= 0x10000 && array < 0x7fffffff0000 - bytes && memory::read(array, first) &&
           memory::read(array + bytes - 1, last);
}
struct FieldRecord { std::string name; std::uint32_t hash{}; unsigned offset{}; std::uintptr_t type{}; };
std::optional<FieldRecord> field_record(const Names &names, std::uintptr_t field) {
    FieldRecord f;
    std::uint16_t offset{};
    const auto name = names.of(field);
    if (!name || !memory::read(field, f.hash) || !memory::read(field + 8, offset) ||
        !memory::read(field + reflect::field_type, f.type))
        return {};
    f.name = *name;
    f.offset = offset;
    return f;
}
void leaves(const Names &names, std::uintptr_t type_info, const std::string &prefix, unsigned base,
            std::vector<Leaf> &out, unsigned depth = 0) {
    std::uintptr_t array{};
    std::uint16_t count{};
    if (depth > 6 || out.size() > 64 || !field_array(type_info, 2, array, count)) return;
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto f = field_record(names, array + std::size_t{i} * reflect::field_size);
        if (!f) continue;
        std::uintptr_t descriptor{};
        if (kind_of(f->type, descriptor) == 2) leaves(names, f->type, prefix + f->name + ".", base + f->offset, out, depth + 1);
        else out.push_back({prefix + f->name, names.type(f->type).value_or("?"), base + f->offset});
    }
}
void members(const Names &names, std::uintptr_t type_info, std::vector<Member> &out, unsigned depth = 0) {
    std::uintptr_t array{}, super{};
    std::uint16_t count{};
    if (depth > 16 || out.size() > 2048 || !field_array(type_info, 3, array, count)) return;
    if (memory::read(type_info + reflect::type_super, super) && super && super != type_info)
        members(names, super, out, depth + 1);
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto f = field_record(names, array + std::size_t{i} * reflect::field_size);
        if (!f) continue;
        Member m{f->name, names.type(f->type).value_or("?"), f->hash, f->offset, {}};
        std::uintptr_t descriptor{};
        if (kind_of(f->type, descriptor) == 2) leaves(names, f->type, m.name + ".", m.offset, m.leaves);
        else m.leaves.push_back({m.name, m.type, m.offset});
        out.push_back(std::move(m));
    }
}

std::size_t leaf_size(std::string_view type) {
    if (type == "Float32" || type == "Int32" || type == "Uint32") return 4;
    if (type == "Boolean" || type == "Int8" || type == "Uint8") return 1;
    if (type == "Int16" || type == "Uint16") return 2;
    if (type == "Float64" || type == "Int64" || type == "Uint64") return 8;
    return 0;
}
using Snapshot = std::vector<std::byte>;
// As much of an object as is readable, up to live_extent.
Snapshot snapshot(std::uintptr_t at) {
    for (std::size_t size = live_extent; size >= 0x40; size /= 2) {
        Snapshot bytes(size);
        if (memory::read_bytes(at, bytes.data(), size)) return bytes;
    }
    return {};
}
template <class T> std::optional<T> load(const Snapshot &s, std::size_t at) {
    if (at + sizeof(T) > s.size()) return {};
    T v{};
    std::memcpy(&v, s.data() + at, sizeof(T));
    return v;
}
std::string leaf_text(const Leaf &l, const Snapshot &s) {
    std::ostringstream out;
    if (l.type == "Float32") { if (auto v = load<float>(s, l.offset)) out << *v; }
    else if (l.type == "Float64") { if (auto v = load<double>(s, l.offset)) out << *v; }
    else if (l.type == "Boolean" || l.type == "Uint8") { if (auto v = load<std::uint8_t>(s, l.offset)) out << unsigned{*v}; }
    else if (l.type == "Int8") { if (auto v = load<std::int8_t>(s, l.offset)) out << int{*v}; }
    else if (l.type == "Int16") { if (auto v = load<std::int16_t>(s, l.offset)) out << *v; }
    else if (l.type == "Uint16") { if (auto v = load<std::uint16_t>(s, l.offset)) out << *v; }
    else if (l.type == "Int32") { if (auto v = load<std::int32_t>(s, l.offset)) out << *v; }
    else if (l.type == "Uint32") { if (auto v = load<std::uint32_t>(s, l.offset)) out << *v; }
    else if (auto v = load<std::uint32_t>(s, l.offset)) out << "raw32 " << hex(*v); // enums, references
    return out.str();
}
// Where an authored float could live in the live object: every aligned copy of its bits. Only
// distinctive values (not 0, 1 or -1) are searched, so a hit is a lead and not noise.
std::string live_candidates(const Leaf &l, const Snapshot &data, const Snapshot &live) {
    if (l.type != "Float32") return {};
    const auto v = load<float>(data, l.offset);
    if (!v || !std::isfinite(*v) || *v == 0 || *v == 1 || *v == -1) return {};
    std::string hits;
    unsigned found = 0;
    for (std::size_t at = 0; at + 4 <= live.size(); at += 4)
        if (std::memcmp(live.data() + at, &*v, 4) == 0 && ++found <= 4) hits += (hits.empty() ? "" : ",") + hex(at);
    if (found > 4) hits += ",...";
    return hits.empty() ? std::string{} : "  live " + hits;
}

struct Component {
    std::uintptr_t object{}, vtable{}, setter{}, data{}, type{};
    std::string type_name;
    int kind = -1; // 0 fog, 1 sky, 2 wind: the components ReSkate already binds
};
// `any`: a component ReSkate already tracks, accepted whatever its type is named.
std::optional<Component> component_at(const Names &names, std::uintptr_t p, bool any = false) {
    Component c;
    c.object = p;
    if (!heap_pointer(p) || !memory::read(p, c.vtable) || !in_image(c.vtable, 0x20) ||
        !memory::read(c.vtable + 0x18, c.setter) || !memory::read(p + 8, c.data) || !heap_pointer(c.data) ||
        !memory::read(c.data + 8, c.type) || !in_image(c.type))
        return {};
    const auto name = names.type(c.type);
    if (!name || (!any && name->find("Component") == std::string::npos)) return {};
    c.type_name = *name;
    return c;
}
// The components beside a known one. Its +0x18 is the collection that owns it (whose first
// qword is the visual environment entity); the others sit in it or one pointer away.
std::vector<Component> sibling_components(const Names &names, const std::vector<std::uintptr_t> &starts) {
    std::map<std::uintptr_t, Component> found;
    const auto scan = [&](std::uintptr_t at, bool deeper, const auto &self) -> void {
        std::array<std::uintptr_t, 64> words{};
        if (!memory::read(at, words)) return;
        for (const auto word : words) {
            if (!heap_pointer(word) || found.contains(word)) continue;
            if (auto c = component_at(names, word)) found.emplace(word, *c);
            else if (deeper) self(word, false, self);
        }
    };
    for (const auto start : starts) scan(start, true, scan);
    std::vector<Component> result;
    for (auto &[_, c] : found) result.push_back(std::move(c));
    return result;
}

struct Environment {
    const VisualEnvironment *node{};
    std::vector<Component> components;
};
// Visual environments of the loaded level (all of them when none is selected), each with
// every component found beside its fog, sky and wind.
std::vector<Environment> environments(const Names &names, bool selected_only) {
    auto &controls = world_control_runtime();
    std::vector<Environment> result;
    for (auto &[entity, node] : visual_environments().nodes) {
        if (!visual_environment_identity(node, false)) continue;
        if (selected_only && (controls.map == WorldMap::none || node.map != controls.map)) continue;
        Environment e{&node, {}};
        std::vector<std::uintptr_t> starts{entity};
        std::map<std::uintptr_t, unsigned> kinds;
        for (const auto &[component, n] : controls.nodes) {
            if (n.parent != entity) continue;
            kinds[component] = n.kind;
            std::uintptr_t collection{};
            if (memory::read(component + 0x18, collection) && heap_pointer(collection)) starts.push_back(collection);
        }
        e.components = sibling_components(names, starts);
        for (const auto &[component, _] : kinds)
            if (std::none_of(e.components.begin(), e.components.end(), [&](const Component &c) { return c.object == component; }))
                if (auto c = component_at(names, component, true)) e.components.push_back(*c);
        for (auto &c : e.components)
            if (const auto k = kinds.find(c.object); k != kinds.end()) c.kind = static_cast<int>(k->second);
        result.push_back(std::move(e));
    }
    return result;
}

void dump_members(std::ostream &out, const Names &names, std::uintptr_t type, std::uintptr_t data, std::uintptr_t live_object) {
    std::vector<Member> list;
    members(names, type, list);
    const auto authored = snapshot(data), live = snapshot(live_object);
    for (const auto &m : list) {
        out << "    " << m.name << " : " << m.type << "  +" << hex(m.offset) << "  hash " << hex(m.hash)
            << (m.hash == name_hash(m.name) ? "" : " (not the name hash " + hex(name_hash(m.name)) + ")") << "\n";
        for (const auto &l : m.leaves) {
            const auto value = leaf_text(l, authored);
            out << "      " << l.path << " " << l.type << " +" << hex(l.offset) << " = " << value
                << live_candidates(l, authored, live) << "\n";
        }
    }
}

std::string dump() {
    const Names names;
    if (!names.ready()) return "error: The engine's reflection registry is not readable.";
    const auto list = environments(names, false);
    std::ostringstream out;
    out << "ReSkate environment probe. Selected map: " << world_map_key(world_control_runtime().map) << "\n"
        << "Offsets: members are from the authored data object; 'live' are aligned copies of the same\n"
        << "value in the live object (component or entity), i.e. where the running game reads it.\n\n";
    std::map<std::string, unsigned> types;
    for (const auto &e : list) {
        const auto &ve = *e.node;
        out << "== " << ve.name << "  map " << world_map_key(ve.map) << "  entity " << hex(ve.entity) << "\n";
        std::uintptr_t ve_type{};
        if (memory::read(ve.data + 8, ve_type))
            out << "  data " << names.type(ve_type).value_or("?") << " @" << hex(ve.data) << "  type " << rva(ve_type) << "\n";
        if (ve_type) dump_members(out, names, ve_type, ve.data, ve.entity);
        for (const auto &c : e.components) {
            ++types[c.type_name];
            out << "  component " << c.type_name << " @" << hex(c.object) << "  vtable " << rva(c.vtable)
                << "  setter " << rva(c.setter) << "  data type " << rva(c.type)
                << (c.kind >= 0 ? "  (bound by ReSkate)" : "") << "\n";
            // Fog, sky and wind are mapped already: only what is new is listed in full.
            if (c.kind < 0) dump_members(out, names, c.type, c.data, c.object);
        }
        out << "\n";
    }
    std::filesystem::path path;
    try {
        path = content_cache::directory() / L"environment-probe.txt";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << out.str();
        if (!file) return "error: Could not write " + path.string();
    } catch (...) { return "error: Could not write the probe report."; }
    std::string summary = std::to_string(list.size()) + " visual environments.";
    for (const auto &[name, count] : types) summary += " " + name + " x" + std::to_string(count) + ";";
    dingosdk::logging::event(dingosdk::logging::Channel::world,
                             dingosdk::Json{{"event", "environment_probe"}, {"environments", list.size()},
                                            {"report", path.string()}}.dump().c_str());
    return summary + " Report: " + path.string();
}

using Setter = void (*)(std::uintptr_t, const EnvironmentChange *);
bool call_setter(std::uintptr_t setter, std::uintptr_t object, const EnvironmentChange *change) {
    __try {
        reinterpret_cast<Setter>(setter)(object, change);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> result;
    while (!text.empty()) {
        const auto start = text.find_first_not_of(' ');
        if (start == std::string_view::npos) break;
        text.remove_prefix(start);
        const auto end = text.find(' ');
        result.push_back(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end);
    }
    return result;
}
std::string lower_text(std::string_view s) {
    std::string r(s);
    for (auto &c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}
// Which live bytes a write changed: the cache offsets a binding needs.
std::string changes(const Snapshot &before, const Snapshot &after) {
    std::string result;
    const auto size = std::min(before.size(), after.size());
    for (std::size_t at = 0; at < size;) {
        if (before[at] == after[at]) { ++at; continue; }
        auto end = at;
        while (end < size && before[end] != after[end]) ++end;
        result += " +" + hex(at) + ".." + hex(end);
        if (at % 4 == 0 && end - at <= 16)
            for (auto i = at; i + 4 <= end; i += 4)
                result += " [" + std::to_string(*load<float>(before, i)) + "->" + std::to_string(*load<float>(after, i)) + "]";
        at = end;
    }
    return result.empty() ? " nothing" : result;
}

std::string poke(std::string_view arguments) {
    const auto args = words(arguments);
    if (args.size() < 3)
        return "error: Usage: envprobe poke <component type> <field> <value...>|default";
    const Names names;
    if (!names.ready()) return "error: The engine's reflection registry is not readable.";
    if (world_control_runtime().map == WorldMap::none) return "error: Load a level first.";
    const auto type_filter = lower_text(args[0]);
    const auto field = lower_text(args[1]);
    const bool authored = lower_text(args[2]) == "default";
    std::vector<float> numbers;
    if (!authored)
        for (std::size_t i = 2; i < args.size(); ++i) {
            float v{};
            const auto r = std::from_chars(args[i].data(), args[i].data() + args[i].size(), v);
            if (r.ec != std::errc{} || r.ptr != args[i].data() + args[i].size() || !std::isfinite(v))
                return "error: Not a number: " + std::string(args[i]);
            numbers.push_back(v);
        }
    std::string report;
    unsigned written = 0;
    for (const auto &e : environments(names, true)) {
        for (const auto &c : e.components) {
            if (lower_text(c.type_name).find(type_filter) == std::string::npos) continue;
            std::vector<Member> list;
            members(names, c.type, list);
            const auto m = std::find_if(list.begin(), list.end(), [&](const Member &x) { return lower_text(x.name) == field; });
            if (m == list.end()) return "error: " + c.type_name + " has no field " + std::string(args[1]) + ".";
            std::size_t size = 0;
            for (const auto &l : m->leaves) {
                if (!leaf_size(l.type)) return "error: " + m->name + " is not made of numbers (" + l.type + ").";
                size = std::max<std::size_t>(size, l.offset - m->offset + leaf_size(l.type));
            }
            std::array<std::byte, 80> value{};
            if (size > value.size()) return "error: " + m->name + " is too large to write.";
            if (authored) {
                if (!memory::read_bytes(c.data + m->offset, value.data(), size)) return "error: The authored value is unreadable.";
            } else {
                if (numbers.size() != m->leaves.size())
                    return "error: " + m->name + " takes " + std::to_string(m->leaves.size()) + " number(s).";
                for (std::size_t i = 0; i < numbers.size(); ++i) {
                    const auto &l = m->leaves[i];
                    auto *at = value.data() + (l.offset - m->offset);
                    if (l.type == "Float32") std::memcpy(at, &numbers[i], 4);
                    else if (l.type == "Float64") { const double d = numbers[i]; std::memcpy(at, &d, 8); }
                    else {
                        const auto n = static_cast<std::int64_t>(numbers[i]);
                        std::memcpy(at, &n, leaf_size(l.type)); // little-endian truncation
                    }
                }
            }
            if (!in_image(c.setter, 1)) return "error: " + c.type_name + " has no setter in the game image.";
            const auto before = snapshot(c.object);
            const EnvironmentChange change{m->hash, 0, value.data()};
            if (!call_setter(c.setter, c.object, &change)) return "error: The setter faulted; restart the game.";
            const auto after = snapshot(c.object);
            ++written;
            report += "\n" + e.node->name + " " + c.type_name + "." + m->name + " (hash " + hex(m->hash) +
                      ", setter " + rva(c.setter) + "): changed" + changes(before, after);
        }
    }
    if (!written) return "error: No component of the selected map's environments matches \"" + std::string(args[0]) + "\".";
    dingosdk::logging::event(dingosdk::logging::Channel::world,
                             dingosdk::Json{{"event", "environment_probe_poke"}, {"result", report}}.dump().c_str());
    return "Wrote " + std::to_string(written) + " component(s):" + report;
}
} // namespace
} // namespace dingosdk::profile_runtime

namespace dingosdk {
std::string environment_probe(std::string_view action, std::string_view arguments) {
    using namespace profile_runtime;
    if (!local_runtime().active.load(std::memory_order_acquire) ||
        !world_control_runtime().environment_active.load(std::memory_order_acquire))
        return "error: World controls are not active in this build.";
    try {
        // Same lock order as update_environment_controls: parents, then components.
        std::lock_guard parents(visual_environments().mutex);
        std::lock_guard lock(world_control_runtime().mutex);
        if (action == "dump") return dump();
        if (action == "poke") return poke(arguments);
        return "error: Usage: envprobe dump | envprobe poke <component type> <field> <value...>|default";
    } catch (...) {
        return "error: The probe failed.";
    }
}
} // namespace dingosdk
