// namix_lv2check — what a host actually sees in the INSTALLED bundle, and whether it runs.
//
// The LV2 build is the SAME processor, controller and editor the VST3 bundle carries, wrapped so
// an LV2 host can drive them (src/lv2/namixlv2.h says why it is a wrapper and not a second
// plug-in). So the DSP needs no second proof — the VST3 validator is a statement about code this
// build shares. What DOES need proving is everything between the host and that code, and all of it
// fails SILENTLY:
//
//   * the bundle's Turtle has to parse, and its manifest has to OFFER the UI. A UI declared only
//     in the plug-in's own file is invisible to most hosts, and nothing about the build says so.
//   * every output port the editor reads has to carry a ui:portNotification. Without them the two
//     meters are permanently dead while the plug-in loads, plays and otherwise works perfectly.
//   * every control port's range, default and step count has to be the parameter's own. The TTL
//     generator checks the code against the controller; this checks the BUNDLE against the code.
//   * the whole editor-to-DSP round trip has to join up: a model load goes in as an atom, the
//     message thread reads the file, and the capability report comes back out of the notify port.
//     Every one of those seams is new in this format.
//
// It asks lilv — the reference discovery library, and what real hosts use — rather than reading
// the source tree, then loads the binary, runs it, and round-trips its state.
//
// Usage:
//   namix_lv2check <bundle directory> [--model <file.nam>] [--ir <file.wav>]

#include "lv2/lv2message.h"
#include "lv2/namixlv2.h"

#include "namids.h"
#include "version.h"

#include <lilv/lilv.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace NAMix;
using namespace NAMix::lv2;

namespace
{

int gChecks = 0;
int gFailures = 0;

void ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ok(const char *fmt, ...)
{
    ++gChecks;
    fputs("  ok   ", stdout);
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
}

void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fail(const char *fmt, ...)
{
    ++gChecks;
    ++gFailures;
    fputs("  FAIL ", stdout);
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
}

void check(bool cond, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void check(bool cond, const char *fmt, ...)
{
    ++gChecks;
    if (!cond)
        ++gFailures;
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
}

void section(const char *title)
{
    printf("\n%s\n", title);
}

bool near(double a, double b)
{
    return std::fabs(a - b) <= 1e-5 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

//------------------------------------------------------------------------
// A urid map, which is the one feature this plug-in requires outright.
//------------------------------------------------------------------------
std::vector<std::string> gUris{""};
std::map<std::string, LV2_URID> gUridByName;

LV2_URID mapUri(LV2_URID_Map_Handle, const char *uri)
{
    auto it = gUridByName.find(uri);
    if (it != gUridByName.end())
        return it->second;
    gUris.emplace_back(uri);
    const auto id = static_cast<LV2_URID>(gUris.size() - 1);
    gUridByName[uri] = id;
    return id;
}

const char *unmapUri(LV2_URID_Unmap_Handle, LV2_URID urid)
{
    return (urid && urid < gUris.size()) ? gUris[urid].c_str() : nullptr;
}

//------------------------------------------------------------------------
// The state store/retrieve pair, holding whatever save() wrote.
//------------------------------------------------------------------------
struct StoredValue {
    std::vector<char> bytes;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
};
std::map<LV2_URID, StoredValue> gState;

LV2_State_Status stateStore(LV2_State_Handle, std::uint32_t key, const void *value, size_t size,
                            std::uint32_t type, std::uint32_t flags)
{
    StoredValue v;
    v.bytes.assign(static_cast<const char *>(value), static_cast<const char *>(value) + size);
    v.type = type;
    v.flags = flags;
    gState[key] = std::move(v);
    return LV2_STATE_SUCCESS;
}

const void *stateRetrieve(LV2_State_Handle, std::uint32_t key, size_t *size, std::uint32_t *type,
                          std::uint32_t *flags)
{
    auto it = gState.find(key);
    if (it == gState.end())
        return nullptr;
    if (size)
        *size = it->second.bytes.size();
    if (type)
        *type = it->second.type;
    if (flags)
        *flags = it->second.flags;
    return it->second.bytes.data();
}

//------------------------------------------------------------------------
constexpr std::uint32_t kFrames = 256;
constexpr std::uint32_t kAtomCapacity = 65536;

// Everything a run needs, so the two run-time sections can each have a fresh one.
struct Rig {
    LilvInstance *instance = nullptr;
    float in[kFrames] = {};
    float outL[kFrames] = {};
    float outR[kFrames] = {};
    float control[kControlInCount] = {};
    float feedback[kFeedbackCount] = {};
    float latency = 0.0f;
    alignas(8) std::uint8_t atomIn[kAtomCapacity] = {};
    alignas(8) std::uint8_t atomOut[kAtomCapacity] = {};

    void resetAtomIn(LV2_URID sequenceType)
    {
        auto *s = reinterpret_cast<LV2_Atom_Sequence *>(atomIn);
        s->atom.size = sizeof(LV2_Atom_Sequence_Body);
        s->atom.type = sequenceType;
        s->body.unit = 0;
        s->body.pad = 0;
    }
    void resetAtomOut(LV2_URID sequenceType)
    {
        auto *s = reinterpret_cast<LV2_Atom_Sequence *>(atomOut);
        s->atom.size = kAtomCapacity - sizeof(LV2_Atom);
        s->atom.type = sequenceType;
        s->body.unit = 0;
        s->body.pad = 0;
    }
};

void connectAll(Rig &rig)
{
    lilv_instance_connect_port(rig.instance, kPortAudioIn, rig.in);
    lilv_instance_connect_port(rig.instance, kPortAudioOutL, rig.outL);
    lilv_instance_connect_port(rig.instance, kPortAudioOutR, rig.outR);
    lilv_instance_connect_port(rig.instance, kPortAtomIn, rig.atomIn);
    lilv_instance_connect_port(rig.instance, kPortAtomOut, rig.atomOut);
    for (int i = 0; i < kControlInCount; ++i)
        lilv_instance_connect_port(rig.instance, kPortControlFirst + static_cast<std::uint32_t>(i),
                                   &rig.control[i]);
    for (int i = 0; i < kFeedbackCount; ++i)
        lilv_instance_connect_port(rig.instance, kPortFeedbackFirst + static_cast<std::uint32_t>(i),
                                   &rig.feedback[i]);
    lilv_instance_connect_port(rig.instance, kPortLatency, &rig.latency);
}

// Write one VST3 message into the rig's atom input port, exactly as the editor's own half does —
// with the plug-in's OWN codec, so what this exercises is the wire format the two halves use and
// not a second description of it.
bool sendMessage(Rig &rig, const MessageUris &uris, LV2_Atom_Forge &forge, const char *id,
                 const char *pathAttr, const std::string &path)
{
    Message message;
    message.setMessageID(id);
    if (pathAttr)
        message.attributes().setBinary(pathAttr, path.data(),
                                       static_cast<uint32>(path.size()));

    std::vector<std::uint8_t> scratch(32 * 1024);
    lv2_atom_forge_set_buffer(&forge, scratch.data(), static_cast<std::uint32_t>(scratch.size()));
    if (!forgeMessage(forge, uris, &message, message.attributes()))
        return false;
    const auto *atom = reinterpret_cast<const LV2_Atom *>(scratch.data());
    const std::uint32_t total = lv2_atom_total_size(atom);

    auto *sequence = reinterpret_cast<LV2_Atom_Sequence *>(rig.atomIn);
    auto *event = reinterpret_cast<LV2_Atom_Event *>(
        reinterpret_cast<std::uint8_t *>(sequence) + lv2_atom_total_size(&sequence->atom));
    if (lv2_atom_total_size(&sequence->atom) + sizeof(LV2_Atom_Event) + total > kAtomCapacity)
        return false;
    event->time.frames = 0;
    std::memcpy(&event->body, atom, total);
    sequence->atom.size += static_cast<std::uint32_t>(lv2_atom_pad_size(
        static_cast<std::uint32_t>(sizeof(LV2_Atom_Event)) + total));
    return true;
}

// The capabilities of whatever model is loaded, caught as the report goes past. The Slim check
// below needs them: Slim resizes a slimmable (A2) network and is a deliberate no-op on any other,
// so "the sound did not change" is a PASS on one model and a FAIL on another.
ModelCaps gCaps;

// Run blocks until a message with `wantId` appears on the notify port, or the budget runs out. The
// message thread polls at 10 ms, so this has to be wall-clock patient rather than block-count
// patient.
bool pumpFor(Rig &rig, const MessageUris &uris, LV2_Atom_Forge &forge, LV2_URID sequenceType,
             const char *wantId, int maxBlocks, std::string *caughtIds = nullptr)
{
    bool found = false;
    for (int blk = 0; blk < maxBlocks && !found; ++blk) {
        rig.resetAtomOut(sequenceType);
        lilv_instance_run(rig.instance, kFrames);
        rig.resetAtomIn(sequenceType);

        const auto *out = reinterpret_cast<const LV2_Atom_Sequence *>(rig.atomOut);
        LV2_ATOM_SEQUENCE_FOREACH(out, ev)
        {
            const LV2_Atom *atom = &ev->body;
            if (!lv2_atom_forge_is_object_type(&forge, atom->type))
                continue;
            Message *message =
                parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), uris);
            if (!message)
                continue;
            if (caughtIds) {
                if (!caughtIds->empty())
                    *caughtIds += ", ";
                *caughtIds += message->id();
            }
            if (message->id() == kMsgModelCaps) {
                int64 v = 0;
                auto attrs = message->getAttributes();
                gCaps.loaded = attrs->getInt(kCapsLoadedAttr, v) == kResultOk && v != 0;
                gCaps.slimmable = attrs->getInt(kCapsSlimmableAttr, v) == kResultOk && v != 0;
            }
            if (message->id() == wantId)
                found = true;
            message->release();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    return found;
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: namix_lv2check <bundle directory> [--model <f.nam>] "
                        "[--ir <f.wav>]\n");
        return 2;
    }
    std::string bundleDir = argv[1];
    if (bundleDir.empty() || bundleDir.back() != '/')
        bundleDir += '/';
    std::string modelPath, irPath;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--model") == 0)
            modelPath = argv[i + 1];
        else if (std::strcmp(argv[i], "--ir") == 0)
            irPath = argv[i + 1];
    }

    printf("namix_lv2check: %s\n", bundleDir.c_str());

    //--------------------------------------------------------------------
    section("1. discovery - what lilv sees, as a host would");

    LilvWorld *world = lilv_world_new();
    // The specification bundles first: without them lilv cannot resolve a class hierarchy and
    // would report lv2:Plugin for a plug-in that correctly declares lv2:DistortionPlugin.
    lilv_world_load_all(world);
    LilvNode *bundleUri = lilv_new_file_uri(world, nullptr, bundleDir.c_str());
    lilv_world_load_bundle(world, bundleUri);

    LilvNode *pluginUri = lilv_new_uri(world, kPluginUri);
    const LilvPlugins *plugins = lilv_world_get_all_plugins(world);
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(plugins, pluginUri);
    if (!plugin) {
        fail("the bundle offers no plug-in at %s", kPluginUri);
        printf("\n%d checks, %d failed\n", gChecks, gFailures);
        return 1;
    }
    ok("the bundle offers %s", kPluginUri);

    if (LilvNode *name = lilv_plugin_get_name(plugin)) {
        check(std::strcmp(lilv_node_as_string(name), stringPluginName) == 0,
              "doap:name is \"%s\" (version.h says \"%s\")", lilv_node_as_string(name),
              stringPluginName);
        lilv_node_free(name);
    } else {
        fail("the plug-in states no doap:name");
    }

    // The vendor. Stated on the plug-in AND on the project, because a host with its own RDF reader
    // has no reason to follow lv2:project and several do not - which shows up as a plug-in list
    // with no vendor beside a VST3 entry for the same product that reads "NAMix (rations)".
    if (LilvNode *author = lilv_plugin_get_author_name(plugin)) {
        check(std::strcmp(lilv_node_as_string(author), stringCompanyName) == 0,
              "doap:maintainer is \"%s\" (version.h says \"%s\")", lilv_node_as_string(author),
              stringCompanyName);
        lilv_node_free(author);
    } else {
        fail("the plug-in names no maintainer; a host's plug-in list will show no vendor");
    }

    const LilvPluginClass *cls = lilv_plugin_get_class(plugin);
    const char *clsUri = cls ? lilv_node_as_uri(lilv_plugin_class_get_uri(cls)) : "(none)";
    check(std::strcmp(clsUri, kLv2PluginClassUri) == 0, "class is %s", clsUri);

    check(lilv_plugin_get_num_ports(plugin) == kPortCount, "%u ports, and the code says %u",
          lilv_plugin_get_num_ports(plugin), static_cast<unsigned>(kPortCount));

    // The version. lv2core.ttl: an odd minor or micro version, or minor version zero, marks a
    // development build that hosts SHOULD NOT show by default.
    LilvNode *minorPred = lilv_new_uri(world, LV2_CORE__minorVersion);
    LilvNode *microPred = lilv_new_uri(world, LV2_CORE__microVersion);
    LilvNodes *minors = lilv_plugin_get_value(plugin, minorPred);
    LilvNodes *micros = lilv_plugin_get_value(plugin, microPred);
    const int minor = minors && lilv_nodes_size(minors) ? lilv_node_as_int(lilv_nodes_get_first(minors)) : -1;
    const int micro = micros && lilv_nodes_size(micros) ? lilv_node_as_int(lilv_nodes_get_first(micros)) : -1;
    check(minor == kLv2MinorVersion && micro == kLv2MicroVersion,
          "lv2:minorVersion %d, lv2:microVersion %d (the code says %d, %d)", minor, micro,
          kLv2MinorVersion, kLv2MicroVersion);
    check(minor > 0 && minor % 2 == 0 && micro % 2 == 0,
          "the version is even and non-zero, so hosts will show the plug-in by default");

    //--------------------------------------------------------------------
    section("2. the port table - the bundle against the code");

    LilvNode *clsInput = lilv_new_uri(world, LV2_CORE__InputPort);
    LilvNode *clsOutput = lilv_new_uri(world, LV2_CORE__OutputPort);
    LilvNode *clsAudio = lilv_new_uri(world, LV2_CORE__AudioPort);
    LilvNode *clsControl = lilv_new_uri(world, LV2_CORE__ControlPort);
    LilvNode *clsAtom = lilv_new_uri(world, LV2_ATOM__AtomPort);
    LilvNode *propToggled = lilv_new_uri(world, LV2_CORE__toggled);
    LilvNode *propEnumeration = lilv_new_uri(world, LV2_CORE__enumeration);
    LilvNode *propReportsLatency = lilv_new_uri(world, LV2_CORE__reportsLatency);

    auto portAt = [&](std::uint32_t index) { return lilv_plugin_get_port_by_index(plugin, index); };

    check(lilv_port_is_a(plugin, portAt(kPortAudioIn), clsAudio) &&
              lilv_port_is_a(plugin, portAt(kPortAudioIn), clsInput),
          "port %u is the audio input", static_cast<unsigned>(kPortAudioIn));
    check(lilv_port_is_a(plugin, portAt(kPortAudioOutL), clsAudio) &&
              lilv_port_is_a(plugin, portAt(kPortAudioOutL), clsOutput) &&
              lilv_port_is_a(plugin, portAt(kPortAudioOutR), clsAudio) &&
              lilv_port_is_a(plugin, portAt(kPortAudioOutR), clsOutput),
          "ports %u and %u are the stereo audio output",
          static_cast<unsigned>(kPortAudioOutL), static_cast<unsigned>(kPortAudioOutR));
    check(lilv_port_is_a(plugin, portAt(kPortAtomIn), clsAtom) &&
              lilv_port_is_a(plugin, portAt(kPortAtomIn), clsInput),
          "port %u is the atom input", static_cast<unsigned>(kPortAtomIn));
    check(lilv_port_is_a(plugin, portAt(kPortAtomOut), clsAtom) &&
              lilv_port_is_a(plugin, portAt(kPortAtomOut), clsOutput),
          "port %u is the atom output (notify)", static_cast<unsigned>(kPortAtomOut));

    std::vector<float> mins(kPortCount), maxs(kPortCount), defs(kPortCount);
    lilv_plugin_get_port_ranges_float(plugin, mins.data(), maxs.data(), defs.data());

    for (int i = 0; i < kControlInCount; ++i) {
        const std::uint32_t index = kPortControlFirst + static_cast<std::uint32_t>(i);
        const LilvPort *port = portAt(index);
        const ControlSpec spec = controlSpec(i);
        const char *symbol = lilv_node_as_string(lilv_port_get_symbol(plugin, port));

        if (!lilv_port_is_a(plugin, port, clsControl) || !lilv_port_is_a(plugin, port, clsInput)) {
            fail("port %u (%s) is not a control input", index, symbol);
            continue;
        }
        const bool ranges = near(mins[index], spec.min) && near(maxs[index], spec.max) &&
                            near(defs[index], spec.def);
        check(ranges, "port %2u %-24s [%g, %g] default %g", index, symbol,
              static_cast<double>(mins[index]), static_cast<double>(maxs[index]),
              static_cast<double>(defs[index]));
        if (!ranges)
            printf("       the code says [%g, %g] default %g\n", spec.min, spec.max, spec.def);

        // A stepped control has to SAY it is stepped, or a host draws a continuous slider for a
        // three-way switch and lands between its positions.
        if (spec.steps == 1)
            check(lilv_port_has_property(plugin, port, propToggled), "       %s is lv2:toggled",
                  symbol);
        else if (spec.steps > 1)
            check(lilv_port_has_property(plugin, port, propEnumeration),
                  "       %s is lv2:enumeration", symbol);
    }

    for (int i = 0; i < kFeedbackCount; ++i) {
        const std::uint32_t index = kPortFeedbackFirst + static_cast<std::uint32_t>(i);
        const LilvPort *port = portAt(index);
        check(lilv_port_is_a(plugin, port, clsControl) && lilv_port_is_a(plugin, port, clsOutput),
              "port %u %s is a control OUTPUT", index,
              lilv_node_as_string(lilv_port_get_symbol(plugin, port)));
    }
    check(lilv_port_has_property(plugin, portAt(kPortLatency), propReportsLatency),
          "port %u carries lv2:reportsLatency", static_cast<unsigned>(kPortLatency));

    //--------------------------------------------------------------------
    section("3. the editor - offered, and told what it needs to hear");

    // Scanning MANIFESTS only, which is what a host does before it reads any plug-in's full
    // description. A UI declared only in NAMix.ttl is invisible to most of them.
    LilvWorld *manifestOnly = lilv_world_new();
    lilv_world_load_specifications(manifestOnly);
    LilvNode *bundleUri2 = lilv_new_file_uri(manifestOnly, nullptr, bundleDir.c_str());
    lilv_world_load_bundle(manifestOnly, bundleUri2);
    LilvNode *pluginUri2 = lilv_new_uri(manifestOnly, kPluginUri);
    const LilvPlugin *fromManifest =
        lilv_plugins_get_by_uri(lilv_world_get_all_plugins(manifestOnly), pluginUri2);
    LilvUIs *manifestUis = fromManifest ? lilv_plugin_get_uis(fromManifest) : nullptr;
    check(manifestUis && lilv_uis_size(manifestUis) > 0,
          "manifest.ttl offers the UI (a UI declared only in NAMix.ttl is invisible to most "
          "hosts)");

    LilvUIs *uis = lilv_plugin_get_uis(plugin);
    const LilvUI *ui = nullptr;
    if (uis && lilv_uis_size(uis) > 0) {
        LILV_FOREACH(uis, it, uis)
        {
            const LilvUI *candidate = lilv_uis_get(uis, it);
            if (std::strcmp(lilv_node_as_uri(lilv_ui_get_uri(candidate)), kUiUri) == 0)
                ui = candidate;
        }
    }
    check(ui != nullptr, "the UI is %s", kUiUri);

    if (ui) {
        LilvNode *x11 = lilv_new_uri(world, LV2_UI__X11UI);
        check(lilv_ui_is_a(ui, x11), "the UI is a ui:X11UI");
        lilv_node_free(x11);
        check(lilv_ui_get_binary_uri(ui) != nullptr, "the UI names a binary");

        // ui:idleInterface in BOTH directions. As a FEATURE because the editor owns no thread and
        // idle() is its run loop; as EXTENSION DATA because that is how the host obtains the
        // callback. One without the other is a UI that never runs or that the host cannot drive.
        LilvNode *idleUri = lilv_new_uri(world, LV2_UI__idleInterface);
        LilvNode *reqFeature = lilv_new_uri(world, LV2_CORE__requiredFeature);
        LilvNode *extData = lilv_new_uri(world, LV2_CORE__extensionData);
        LilvNodes *feats = lilv_world_find_nodes(world, lilv_ui_get_uri(ui), reqFeature, nullptr);
        LilvNodes *datas = lilv_world_find_nodes(world, lilv_ui_get_uri(ui), extData, nullptr);
        check(feats && lilv_nodes_contains(feats, idleUri),
              "the UI requires ui:idleInterface as a FEATURE");
        check(datas && lilv_nodes_contains(datas, idleUri),
              "the UI offers ui:idleInterface as EXTENSION DATA");
        lilv_node_free(idleUri);
        lilv_node_free(reqFeature);
        lilv_node_free(extData);

        // The notifications. Without one per output port the meters are dead and nothing says so.
        LilvNode *notifPred = lilv_new_uri(world, LV2_UI__portNotification);
        LilvNode *portIndexPred = lilv_new_uri(world, LV2_UI__portIndex);
        LilvNodes *notifs = lilv_world_find_nodes(world, lilv_ui_get_uri(ui), notifPred, nullptr);
        std::vector<int> notified;
        if (notifs) {
            LILV_FOREACH(nodes, it, notifs)
            {
                const LilvNode *blank = lilv_nodes_get(notifs, it);
                LilvNodes *idx = lilv_world_find_nodes(world, blank, portIndexPred, nullptr);
                if (idx && lilv_nodes_size(idx))
                    notified.push_back(lilv_node_as_int(lilv_nodes_get_first(idx)));
            }
        }
        for (int i = 0; i < kFeedbackCount; ++i) {
            const int index = static_cast<int>(kPortFeedbackFirst) + i;
            const bool has =
                std::find(notified.begin(), notified.end(), index) != notified.end();
            check(has, "port %d has a ui:portNotification (without it its readout is dead)",
                  index);
        }
        const bool notifyPort = std::find(notified.begin(), notified.end(),
                                          static_cast<int>(kPortAtomOut)) != notified.end();
        check(notifyPort, "port %u (notify) has a ui:portNotification",
              static_cast<unsigned>(kPortAtomOut));
        lilv_node_free(notifPred);
        lilv_node_free(portIndexPred);
    }

    //--------------------------------------------------------------------
    section("4. it runs - instantiate, process, and reach the DSP from a control port");

    LV2_URID_Map map = {nullptr, mapUri};
    LV2_URID_Unmap unmap = {nullptr, unmapUri};
    LV2_Feature featMap = {LV2_URID__map, &map};
    LV2_Feature featUnmap = {LV2_URID__unmap, &unmap};

    std::int32_t blockLength = kFrames;
    const LV2_URID atomInt = mapUri(nullptr, LV2_ATOM__Int);
    const LV2_URID sequenceType = mapUri(nullptr, LV2_ATOM__Sequence);
    LV2_Options_Option options[] = {
        {LV2_OPTIONS_INSTANCE, 0, mapUri(nullptr, LV2_BUF_SIZE__maxBlockLength),
         sizeof(std::int32_t), atomInt, &blockLength},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    LV2_Feature featOptions = {LV2_OPTIONS__options, options};
    const LV2_Feature *features[] = {&featMap, &featUnmap, &featOptions, nullptr};

    MessageUris uris;
    uris.map(&map);
    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &map);

    Rig rig;
    rig.instance = lilv_plugin_instantiate(plugin, 48000.0, features);
    if (!rig.instance) {
        fail("instantiate returned NULL");
        printf("\n%d checks, %d failed\n", gChecks, gFailures);
        return 1;
    }
    ok("instantiate");

    for (int i = 0; i < kControlInCount; ++i)
        rig.control[i] = static_cast<float>(controlSpec(i).def);
    connectAll(rig);
    rig.resetAtomIn(sequenceType);
    rig.resetAtomOut(sequenceType);

    for (std::uint32_t i = 0; i < kFrames; ++i)
        rig.in[i] = 0.25f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);

    lilv_instance_activate(rig.instance);
    for (int blk = 0; blk < 8; ++blk) {
        rig.resetAtomOut(sequenceType);
        lilv_instance_run(rig.instance, kFrames);
    }

    bool finite = true;
    for (std::uint32_t i = 0; i < kFrames; ++i)
        finite = finite && std::isfinite(rig.outL[i]) && std::isfinite(rig.outR[i]);
    check(finite, "the output is finite with no model loaded");
    check(rig.feedback[0] > 0.01f, "the input meter moved (%.3f) with signal on the input",
          static_cast<double>(rig.feedback[0]));

    // Bypass is the cheapest end-to-end proof that a control PORT reaches the DSP: the whole path
    // is port write -> run() notices -> parameter point -> processor.
    rig.control[0] = 1.0f; // kBypassId is the first control port
    for (int blk = 0; blk < 4; ++blk) {
        rig.resetAtomOut(sequenceType);
        lilv_instance_run(rig.instance, kFrames);
    }
    float worst = 0.0f;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        worst = std::max(worst, std::fabs(rig.outL[i] - rig.in[i]));
        worst = std::max(worst, std::fabs(rig.outR[i] - rig.in[i]));
    }
    check(worst < 1e-6f,
          "bypass passes the input through both channels (max |out-in| = %g), so a control port "
          "reaches the processor",
          static_cast<double>(worst));
    rig.control[0] = 0.0f;

    //--------------------------------------------------------------------
    section("5. the editor-to-DSP round trip - an atom in, a reply out");

    // The UI asks for the plug-in's state at startup, because LV2 has no setComponentState. If
    // this does not come back the panel opens with no model or IR name on it.
    check(sendMessage(rig, uris, forge, kMsgLv2RequestState, nullptr, std::string()),
          "the state request encodes into an atom");
    std::string seen;
    check(pumpFor(rig, uris, forge, sequenceType, kMsgLv2State, 200, &seen),
          "the DSP answers %s on the notify port", kMsgLv2State);
    if (!seen.empty())
        printf("       replies seen: %s\n", seen.c_str());

    if (!modelPath.empty()) {
        check(sendMessage(rig, uris, forge, kMsgLoadModel, kMsgPathAttr, modelPath),
              "the model load encodes into an atom");
        std::string caps;
        const bool got = pumpFor(rig, uris, forge, sequenceType, kMsgModelCaps, 400, &caps);
        check(got, "loading %s reports its capabilities back as %s", modelPath.c_str(),
              kMsgModelCaps);

        float peak = 0.0f;
        bool sane = true;
        for (int blk = 0; blk < 8; ++blk) {
            rig.resetAtomOut(sequenceType);
            lilv_instance_run(rig.instance, kFrames);
            for (std::uint32_t i = 0; i < kFrames; ++i) {
                sane = sane && std::isfinite(rig.outL[i]);
                peak = std::max(peak, std::fabs(rig.outL[i]));
            }
        }
        check(sane && peak > 1e-4f, "the amp makes a finite, non-silent sound (peak %.4f)",
              static_cast<double>(peak));
        check(rig.feedback[1] > 0.01f, "the output meter moved (%.3f)",
              static_cast<double>(rig.feedback[1]));

        // SLIM, which is the one control a parameter point does not finish.
        //
        // NamProcessor's queue handler only RECORDS kSlimId; what resizes the network is
        // kMsgSetSlim, handled off the audio thread. With the editor open the controller sends
        // that message. With no editor — an automation lane, or a host's own generic UI, which is
        // exactly what an LV2 user reaches for first — there is no controller in the loop, and
        // the DSP wrapper's own bridge is the only thing that makes Slim do anything at all.
        // Nothing else in this file would notice if that bridge were removed.
        if (gCaps.slimmable) {
            // SELF-CALIBRATING, because "the sound changed" needs something to be louder than.
            // The input is a continuous sine and the network carries state, so two snapshots
            // taken at different times differ a little whatever Slim does. So measure that drift
            // first, with Slim held still, and then require the change to beat it by a wide
            // margin. A fixed threshold was tried and was worthless: with the bridge deliberately
            // removed the drift alone measured 2.8e-05, which sailed past a 1e-6 bar and passed a
            // test of nothing.
            auto settle = [&](int blocks) {
                for (int blk = 0; blk < blocks; ++blk) {
                    rig.resetAtomOut(sequenceType);
                    lilv_instance_run(rig.instance, kFrames);
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            };
            auto maxDiff = [](const float *a, const float *b) {
                float d = 0.0f;
                for (std::uint32_t i = 0; i < kFrames; ++i)
                    d = std::max(d, std::fabs(a[i] - b[i]));
                return d;
            };

            float a[kFrames], b[kFrames], c[kFrames];
            settle(40);
            std::memcpy(a, rig.outL, sizeof a);
            settle(40);
            std::memcpy(b, rig.outL, sizeof b);
            const float drift = maxDiff(a, b); // Slim never moved between these two

            rig.control[10] = 1.0f; // kSlimId is the eleventh control port
            settle(40);
            std::memcpy(c, rig.outL, sizeof c);
            const float delta = maxDiff(b, c);

            bool sane = true;
            for (std::uint32_t i = 0; i < kFrames; ++i)
                sane = sane && std::isfinite(c[i]);
            check(sane, "the output is finite after Slim moved");
            check(delta > drift * 20.0f && delta > 1e-4f,
                  "writing the Slim control port changed the sound (delta %g against a drift "
                  "floor of %g) - the DSP wrapper's kMsgSetSlim bridge works with no editor "
                  "attached",
                  static_cast<double>(delta), static_cast<double>(drift));
            rig.control[10] = 0.0f;
        } else {
            printf("  skip  %s is not a slimmable (A2) model, so the Slim port is a no-op by "
                   "design and cannot be tested with it\n",
                   modelPath.c_str());
        }
    } else {
        printf("  skip  no --model given; the load path and the sound are not exercised\n");
    }

    if (!irPath.empty()) {
        check(sendMessage(rig, uris, forge, kMsgLoadIr, kMsgPathAttr, irPath),
              "the IR load encodes into an atom");
        for (int blk = 0; blk < 60; ++blk) {
            rig.resetAtomOut(sequenceType);
            lilv_instance_run(rig.instance, kFrames);
            rig.resetAtomIn(sequenceType);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        bool sane = true;
        for (std::uint32_t i = 0; i < kFrames; ++i)
            sane = sane && std::isfinite(rig.outL[i]);
        check(sane, "the output is still finite with %s loaded", irPath.c_str());
    } else {
        printf("  skip  no --ir given; the IR path is not exercised\n");
    }

    //--------------------------------------------------------------------
    section("6. state - saved, and read back into a second instance");

    const LV2_State_Interface *stateIface = nullptr;
    if (lilv_instance_get_descriptor(rig.instance)->extension_data)
        stateIface = static_cast<const LV2_State_Interface *>(
            lilv_instance_get_descriptor(rig.instance)->extension_data(LV2_STATE__interface));
    check(stateIface != nullptr, "the plug-in offers state:interface");

    if (stateIface) {
        rig.control[1] = 7.5f; // input gain, so the blob has something distinctive in it
        for (int blk = 0; blk < 4; ++blk) {
            rig.resetAtomOut(sequenceType);
            lilv_instance_run(rig.instance, kFrames);
        }
        const LV2_Feature *saveFeatures[] = {&featMap, &featUnmap, nullptr};
        const LV2_State_Status saved = stateIface->save(
            lilv_instance_get_handle(rig.instance), stateStore, nullptr, 0, saveFeatures);
        check(saved == LV2_STATE_SUCCESS, "save() succeeded");
        const LV2_URID blobKey = mapUri(nullptr, kStateBlobUri);
        check(gState.count(blobKey) && !gState[blobKey].bytes.empty(),
              "the VST3 state blob is stored under one key (%zu bytes)",
              gState.count(blobKey) ? gState[blobKey].bytes.size() : 0u);
        if (!modelPath.empty()) {
            const LV2_URID rawKey =
                mapUri(nullptr, (std::string(kStatePathRawPrefix) + kPathSlotName[kPathSlotModel])
                                    .c_str());
            check(gState.count(rawKey) > 0,
                  "the model's raw path is stored, which is what restore() compares against the "
                  "abstract one to tell a moved session from an ordinary reopen");
        }

        // A SECOND instance, which is what a host reopening a project actually builds.
        Rig second;
        second.instance = lilv_plugin_instantiate(plugin, 48000.0, features);
        if (!second.instance) {
            fail("a second instance could not be created");
        } else {
            for (int i = 0; i < kControlInCount; ++i)
                second.control[i] = static_cast<float>(controlSpec(i).def);
            connectAll(second);
            second.resetAtomIn(sequenceType);
            second.resetAtomOut(sequenceType);
            const LV2_State_Status restored =
                stateIface->restore(lilv_instance_get_handle(second.instance), stateRetrieve,
                                    nullptr, 0, saveFeatures);
            check(restored == LV2_STATE_SUCCESS, "restore() succeeded on a fresh instance");

            lilv_instance_activate(second.instance);
            std::string ids;
            check(pumpFor(second, uris, forge, sequenceType, kMsgLv2State, 200, &ids),
                  "restore() pushes the state at an editor that may already be open");
            bool sane = true;
            for (std::uint32_t i = 0; i < kFrames; ++i)
                sane = sane && std::isfinite(second.outL[i]);
            check(sane, "the restored instance produces finite output");
            lilv_instance_deactivate(second.instance);
            lilv_instance_free(second.instance);
        }
    }

    lilv_instance_deactivate(rig.instance);
    lilv_instance_free(rig.instance);

    //--------------------------------------------------------------------
    printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
