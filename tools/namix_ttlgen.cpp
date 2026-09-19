// namix_ttlgen — writes the LV2 bundle's manifest.ttl and NAMix.ttl, and refuses to write either
// one if the port table disagrees with the plug-in.
//
// WHY THIS IS A TOOL AND NOT A FILE. An LV2 bundle's TTL states every parameter's range, default,
// step count and name. Those already exist, once, in src/namcontroller.cpp and src/namids.h — so a
// hand-written TTL would be a second copy of thirteen parameters, kept in step by nobody. This
// links the real controller, asks it, and writes down what it says.
//
// WHAT IT CHECKS BEFORE IT WRITES, and each of these is a way the LV2 build could otherwise go
// quietly wrong:
//
//   * every port in src/lv2/namixlv2.h's table is a parameter the controller actually declares;
//   * every parameter the controller declares outside the read-only feedback block (200..) has a
//     port — so a control added to the plug-in and forgotten here fails the build instead of
//     becoming something no LV2 host can reach;
//   * each port's range, step count and default match the controller's own, to a tolerance far
//     tighter than any of them are specified to;
//   * every lv2:symbol is unique, since a repeat is a bundle a host silently mis-reads.
//
// Run by the build, into the staged bundle. It needs no X server and no model.

#include "lv2/namixlv2.h"

#include "namcontroller.h"
#include "namids.h"
#include "version.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace NAMix;
using namespace NAMix::lv2;

namespace
{

int gFailures = 0;

void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fail(const char *fmt, ...)
{
    fputs("namix_ttlgen: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    ++gFailures;
}

// A VST3 String128 as ASCII. Every title and unit string in this plug-in is ASCII by construction;
// anything outside it becomes '?' rather than a mangled byte in a TTL a parser will then reject.
std::string ascii(const Vst::TChar *s)
{
    std::string out;
    for (int i = 0; s && s[i] && i < 128; ++i)
        out.push_back(s[i] < 0x80 ? static_cast<char>(s[i]) : '?');
    return out;
}

// An lv2:symbol: a name a host may use as an identifier, and the key it stores automation under.
// Derived from the title so it reads as the control it is, with everything a symbol may not
// contain folded to '_'.
std::string symbolOf(const std::string &title)
{
    std::string out;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else if (!out.empty() && out.back() != '_')
            out.push_back('_');
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty())
        out = "control";
    if (std::isdigit(static_cast<unsigned char>(out[0])))
        out.insert(out.begin(), 'p');
    return out;
}

// Turtle wants a decimal point in a number that is to be read as a double, and "%g" will happily
// write "1" or "1e-05". Neither is what a strict parser takes as an xsd:decimal.
std::string number(double v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6f", v);
    std::string s(buf);
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.find_last_not_of('0');
        if (last == dot)
            ++last;
        s.erase(last + 1);
    }
    return s;
}

bool near(double a, double b)
{
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

struct PortInfo {
    Vst::ParamID id = 0;
    std::string title;
    std::string symbol;
    std::string unit;
    ControlSpec spec = {};
    std::vector<std::string> scalePoints; // list parameters only
};

// The standard unit for a VST3 unit string, or an empty string when there is no standard one and
// the caller has to spell the unit out. dBu is the interesting case: LV2's units vocabulary has
// db, hz, ms, pc and a dozen more, and no dBu — so that one is written as a units:Unit of its own
// rather than mislabelled as dB, which is a different quantity by 0.775 volts.
std::string standardUnit(const std::string &vst3Unit)
{
    if (vst3Unit == "dB")
        return "units:db";
    if (vst3Unit == "Hz")
        return "units:hz";
    if (vst3Unit == "ms")
        return "units:ms";
    if (vst3Unit == "%")
        return "units:pc";
    return std::string();
}

//------------------------------------------------------------------------
// The checks.
//------------------------------------------------------------------------
void checkAgainstController(NamController &controller, const std::vector<PortInfo> &ports)
{
    std::set<Vst::ParamID> ported;
    for (const PortInfo &p : ports)
        ported.insert(p.id);

    // Every parameter that ought to have a port, has one.
    const int32 count = controller.getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info = {};
        if (controller.getParameterInfo(i, info) != kResultOk)
            continue;
        bool feedback = false;
        for (int f = 0; f < kFeedbackCount; ++f)
            feedback = feedback || kFeedbackIds[f] == info.id;
        if (feedback)
            continue;
        if (ported.count(info.id) == 0)
            fail("parameter %u (%s) has no LV2 port; add it to kControlIds", info.id,
                 ascii(info.title).c_str());
    }

    // Every port is a real parameter, and its range is the parameter's own.
    for (const PortInfo &p : ports) {
        Vst::Parameter *param = controller.getParameterObject(p.id);
        if (!param) {
            fail("port %s names parameter %u, which the controller does not declare",
                 p.symbol.c_str(), p.id);
            continue;
        }
        const Vst::ParameterInfo &info = param->getInfo();
        if (!near(param->toPlain(0.0), p.spec.min) || !near(param->toPlain(1.0), p.spec.max))
            fail("%s: the table says [%s, %s] and the controller says [%s, %s]", p.symbol.c_str(),
                 number(p.spec.min).c_str(), number(p.spec.max).c_str(),
                 number(param->toPlain(0.0)).c_str(), number(param->toPlain(1.0)).c_str());
        if (info.stepCount != p.spec.steps)
            fail("%s: the table says %d steps and the controller says %d", p.symbol.c_str(),
                 p.spec.steps, info.stepCount);
        const double controllerDefault = param->toPlain(info.defaultNormalizedValue);
        if (!near(controllerDefault, p.spec.def))
            fail("%s: the table's default is %s and the controller's is %s", p.symbol.c_str(),
                 number(p.spec.def).c_str(), number(controllerDefault).c_str());
    }

    // The feedback parameters exist and really are read-only, since the TTL declares them as
    // OUTPUT ports and a host may not write one.
    for (int f = 0; f < kFeedbackCount; ++f) {
        Vst::Parameter *param = controller.getParameterObject(kFeedbackIds[f]);
        if (!param) {
            fail("feedback parameter %u is not declared by the controller",
                 static_cast<unsigned>(kFeedbackIds[f]));
            continue;
        }
        if ((param->getInfo().flags & Vst::ParameterInfo::kIsReadOnly) == 0)
            fail("feedback parameter %u is not kIsReadOnly, but its LV2 port is an output",
                 static_cast<unsigned>(kFeedbackIds[f]));
    }

    // Unique symbols. A repeat is not a parse error — a host simply takes one of them and stores
    // the other's automation under it.
    std::set<std::string> symbols;
    for (const PortInfo &p : ports)
        if (!symbols.insert(p.symbol).second)
            fail("two ports share the symbol \"%s\"", p.symbol.c_str());
}

//------------------------------------------------------------------------
std::vector<PortInfo> collectPorts(NamController &controller)
{
    std::vector<PortInfo> ports;
    ports.reserve(kControlInCount);
    for (int i = 0; i < kControlInCount; ++i) {
        PortInfo p;
        p.spec = controlSpec(i);
        p.id = p.spec.id;
        Vst::Parameter *param = controller.getParameterObject(p.id);
        if (param) {
            p.title = ascii(param->getInfo().title);
            p.unit = ascii(param->getInfo().units);
            if (p.spec.steps > 0 && (param->getInfo().flags & Vst::ParameterInfo::kIsList) != 0) {
                for (int step = 0; step <= p.spec.steps; ++step) {
                    Vst::String128 text = {};
                    const Vst::ParamValue norm =
                        static_cast<double>(step) / static_cast<double>(p.spec.steps);
                    if (controller.getParamStringByValue(p.id, norm, text) == kResultOk)
                        p.scalePoints.push_back(ascii(text));
                }
            }
        }
        if (p.title.empty())
            p.title = "Control";
        p.symbol = symbolOf(p.title);
        ports.push_back(std::move(p));
    }
    return ports;
}

//------------------------------------------------------------------------
void writeControlPort(FILE *f, const PortInfo &p, std::uint32_t index)
{
    fprintf(f, "[\n\t\ta lv2:InputPort ,\n\t\t\tlv2:ControlPort ;\n");
    fprintf(f, "\t\tlv2:index %u ;\n", index);
    fprintf(f, "\t\tlv2:symbol \"%s\" ;\n", p.symbol.c_str());
    fprintf(f, "\t\tlv2:name \"%s\" ;\n", p.title.c_str());
    fprintf(f, "\t\tlv2:default %s ;\n", number(p.spec.def).c_str());
    fprintf(f, "\t\tlv2:minimum %s ;\n", number(p.spec.min).c_str());
    fprintf(f, "\t\tlv2:maximum %s", number(p.spec.max).c_str());

    if (p.spec.steps == 1) {
        fprintf(f, " ;\n\t\tlv2:portProperty lv2:toggled");
    } else if (p.spec.steps > 1) {
        fprintf(f, " ;\n\t\tlv2:portProperty lv2:integer ,\n\t\t\tlv2:enumeration");
        for (size_t s = 0; s < p.scalePoints.size(); ++s) {
            fprintf(f,
                    " ;\n\t\tlv2:scalePoint [\n\t\t\trdfs:label \"%s\" ;\n\t\t\trdf:value %s\n"
                    "\t\t]",
                    p.scalePoints[s].c_str(), number(static_cast<double>(s)).c_str());
        }
    }

    if (!p.unit.empty()) {
        const std::string standard = standardUnit(p.unit);
        if (!standard.empty())
            fprintf(f, " ;\n\t\tunits:unit %s", standard.c_str());
        else
            fprintf(f,
                    " ;\n\t\tunits:unit [\n\t\t\ta units:Unit ;\n\t\t\tunits:name \"%s\" ;\n"
                    "\t\t\tunits:symbol \"%s\" ;\n\t\t\tunits:render \"%%f %s\"\n\t\t]",
                    p.unit.c_str(), p.unit.c_str(), p.unit.c_str());
    }
    fprintf(f, "\n\t]");
}

//------------------------------------------------------------------------
bool writePluginTtl(const std::string &path, const std::vector<PortInfo> &ports)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        fail("cannot write %s", path.c_str());
        return false;
    }

    fprintf(f,
            "# GENERATED by tools/namix_ttlgen.cpp. Do not edit.\n"
            "#\n"
            "# Every range, default, step count and name below is read out of the plug-in's own\n"
            "# NamController at build time, so this file cannot drift from the parameters it\n"
            "# describes. To change a control, change src/namcontroller.cpp and rebuild.\n\n");
    fprintf(f, "@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .\n"
               "@prefix bufsz: <http://lv2plug.in/ns/ext/buf-size#> .\n"
               "@prefix doap:  <http://usefulinc.com/ns/doap#> .\n"
               "@prefix foaf:  <http://xmlns.com/foaf/0.1/> .\n"
               "@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .\n"
               "@prefix opts:  <http://lv2plug.in/ns/ext/options#> .\n"
               "@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n"
               "@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .\n"
               "@prefix rsz:   <http://lv2plug.in/ns/ext/resize-port#> .\n"
               "@prefix state: <http://lv2plug.in/ns/ext/state#> .\n"
               "@prefix ui:    <http://lv2plug.in/ns/extensions/ui#> .\n"
               "@prefix units: <http://lv2plug.in/ns/extensions/units#> .\n"
               "@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .\n\n");

    // --- the UI ----------------------------------------------------------
    fprintf(
        f,
        "# The editor is an X11 UI: the LV2UI_Widget it hands back is an X11 Window ID, not a\n"
        "# pointer, created as a child of the window passed through ui:parent.\n"
        "#\n"
        "# ui:idleInterface is required in BOTH directions and neither is optional. As a\n"
        "# FEATURE, because the UI owns no thread and no timer — the plug-in's X11 editor\n"
        "# borrows its host's run loop, and idle() is the run loop here, so without the host\n"
        "# calling it nothing drains X events or repaints. As EXTENSION DATA, because that is\n"
        "# how the host obtains the callback. One without the other gives a UI that either\n"
        "# never runs or that the host cannot drive.\n"
        "#\n"
        "# ui:portNotification is load-bearing rather than decorative. The UI extension says a\n"
        "# host calls port_event() for control port INPUTS by default. Ports %u..%u are\n"
        "# OUTPUTS — the input and output meters — so without these declarations both readouts\n"
        "# are permanently dead while the plug-in loads, plays and otherwise works perfectly.\n"
        "# The same is true of the notify port at %u, which carries the loaded model's\n"
        "# capabilities and the plug-in's state to the editor.\n"
        "#\n"
        "# ui:resize is deprecated in the specification and is still the only way a UI can\n"
        "# state its size, so it is declared OPTIONAL: the editor asks once at startup and\n"
        "# carries on without it. ui:noUserResize IS declared, because this editor is a fixed\n"
        "# size — X11PlugView does not override canResize, so the VST3 build tells a host the\n"
        "# same thing.\n",
        kPortFeedbackFirst, kPortFeedbackFirst + kFeedbackCount - 1, kPortAtomOut);
    fprintf(f,
            "<%s>\n\ta ui:X11UI ;\n\tlv2:binary <NAMix_ui.so> ;\n\tui:binary <NAMix_ui.so> ;\n"
            "\tlv2:requiredFeature ui:idleInterface ,\n\t\turid:map ;\n"
            "\tlv2:optionalFeature ui:resize ;\n"
            "\tui:portNotification [\n\t\tui:plugin <%s> ;\n\t\tui:portIndex %u ;\n"
            "\t\tui:protocol atom:eventTransfer ;\n\t\tui:notifyType atom:Object\n\t]",
            kUiUri, kPluginUri, kPortAtomOut);
    for (int i = 0; i < kFeedbackCount; ++i) {
        fprintf(f,
                " , [\n\t\tui:plugin <%s> ;\n\t\tui:portIndex %u ;\n"
                "\t\tui:protocol ui:floatProtocol\n\t]",
                kPluginUri, kPortFeedbackFirst + static_cast<std::uint32_t>(i));
    }
    fprintf(f, " ;\n\tlv2:extensionData ui:idleInterface ;\n"
               "\tlv2:portProperty ui:noUserResize .\n\n");

    // --- the plug-in -----------------------------------------------------
    //
    // EVERY LINE OF THIS BLOCK IS THE VST3'S OWN, out of src/version.h and the factory that reads
    // it, because this is the half a host puts on the screen and the two formats have to be the
    // same product there. Hand-written literals are what would make them diverge: the VST3 factory
    // names a vendor, and a bundle that named nobody would leave a host's "Name (vendor)" list
    // with nothing to put in the brackets beside a VST3 entry that reads correctly.
    //
    // doap:Project alongside the two LV2 classes, and it is what makes the maintainer below legal
    // where a host will actually read it. doap:maintainer carries `rdfs:domain doap:Project`, so
    // stating it about this URI ALREADY asserts the URI is a project — that is what an rdfs:domain
    // means. Declaring the type explicitly only writes down the entailment. Hosts find plug-ins by
    // asking for lv2:Plugin, and an extra type is no more remarkable than lv2:DistortionPlugin
    // beside it.
    fprintf(f, "<%s>\n\ta lv2:Plugin ,\n\t\t%s ,\n\t\tdoap:Project ;\n", kPluginUri,
            kLv2PluginClass);
    fprintf(f,
            "\tdoap:name \"%s\" ;\n"
            "\tdoap:license <http://opensource.org/licenses/MIT> ;\n"
            "\tlv2:minorVersion %d ;\n"
            "\tlv2:microVersion %d ;\n"
            // The maintainer is stated TWICE, on the plug-in and on the project, and the
            // duplication is the point rather than an oversight.
            //
            // On the PROJECT is where the schema puts it: doap.ttl gives doap:maintainer
            // `rdfs:domain doap:Project`, so a maintainer hanging off an lv2:Plugin is a domain
            // violation. lilv reads it either way — it looks on the plug-in first and falls back
            // to the project (lilv/src/plugin.c, lilv_plugin_get_author).
            //
            // On the PLUG-IN is where a host reads it: a host with its own RDF reader has no
            // reason to follow lv2:project, and several do not, which shows up as a plug-in list
            // with no vendor at all beside a VST3 entry for the same product that reads
            // "NAMix (rations)".
            //
            // Stating both satisfies the schema and the hosts at once, and costs four lines.
            "\tdoap:maintainer [\n\t\ta foaf:Person ;\n"
            "\t\tfoaf:name \"%s\" ;\n"
            "\t\tfoaf:mbox <%s> ;\n"
            "\t\tfoaf:homepage <%s>\n\t] ;\n"
            "\tlv2:project [\n\t\ta doap:Project ;\n"
            "\t\tdoap:name \"%s\" ;\n"
            "\t\tdoap:homepage <%s> ;\n"
            "\t\tdoap:maintainer [\n\t\t\ta foaf:Person ;\n"
            "\t\t\tfoaf:name \"%s\" ;\n"
            "\t\t\tfoaf:mbox <%s> ;\n"
            "\t\t\tfoaf:homepage <%s>\n\t\t]\n\t] ;\n",
            stringPluginName, kLv2MinorVersion, kLv2MicroVersion, stringCompanyName,
            stringCompanyEmail, stringCompanyWeb, stringPluginName, stringCompanyWeb,
            stringCompanyName, stringCompanyEmail, stringCompanyWeb);
    fprintf(f, "\tlv2:requiredFeature urid:map ;\n"
               "\tlv2:optionalFeature lv2:hardRTCapable ,\n"
               "\t\tbufsz:boundedBlockLength ,\n"
               "\t\topts:options ;\n"
               "\tlv2:extensionData state:interface ;\n");
    fprintf(f, "\tui:ui <%s> ;\n", kUiUri);
    fprintf(f, "\tlv2:port\n");

    // The audio port NAMES are the VST3's bus names (addAudioInput "Input", addAudioOutput
    // "Output" as a stereo bus), so a patchbay labels the two formats alike. LV2 has no buses, so
    // the stereo output becomes two ports and the side is appended. The SYMBOLS are the stable
    // identity and do not follow the names anywhere.
    fprintf(f,
            "\t[\n\t\ta lv2:AudioPort ,\n\t\t\tlv2:InputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tlv2:symbol \"in\" ;\n\t\tlv2:name \"Input\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAudioIn));
    fprintf(f,
            "[\n\t\ta lv2:AudioPort ,\n\t\t\tlv2:OutputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tlv2:symbol \"out_l\" ;\n\t\tlv2:name \"Output Left\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAudioOutL));
    fprintf(f,
            "[\n\t\ta lv2:AudioPort ,\n\t\t\tlv2:OutputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tlv2:symbol \"out_r\" ;\n\t\tlv2:name \"Output Right\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAudioOutR));

    // The control port carries the editor's own messages and nothing else. NAMix declares no MIDI
    // parameters — there is no footswitch and no CC mapping — so midi:MidiEvent is deliberately
    // absent rather than forgotten, and a host has no reason to route MIDI here.
    fprintf(f,
            "[\n\t\ta atom:AtomPort ,\n\t\t\tlv2:InputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tatom:bufferType atom:Sequence ;\n"
            "\t\tatom:supports atom:Object ;\n"
            "\t\tlv2:designation lv2:control ;\n"
            "\t\tlv2:symbol \"control\" ;\n\t\tlv2:name \"Control\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAtomIn));
    // The notify port carries what the editor cannot get any other way: the loaded model's
    // capabilities, and the plug-in's state at startup. rsz:minimumSize because that state blob
    // carries two file paths and a host's default atom buffer is not promised to hold them.
    fprintf(f,
            "[\n\t\ta atom:AtomPort ,\n\t\t\tlv2:OutputPort ;\n\t\tlv2:index %u ;\n"
            "\t\tatom:bufferType atom:Sequence ;\n"
            "\t\tatom:supports atom:Object ;\n"
            "\t\trsz:minimumSize 65536 ;\n"
            "\t\tlv2:symbol \"notify\" ;\n\t\tlv2:name \"Notify\"\n\t] , ",
            static_cast<std::uint32_t>(kPortAtomOut));

    for (int i = 0; i < kControlInCount; ++i) {
        writeControlPort(f, ports[static_cast<size_t>(i)],
                         kPortControlFirst + static_cast<std::uint32_t>(i));
        fprintf(f, " , ");
    }

    static const char *kFeedbackNames[kFeedbackCount] = {"Input Meter", "Output Meter"};
    static const char *kFeedbackSymbols[kFeedbackCount] = {"input_meter", "output_meter"};
    for (int i = 0; i < kFeedbackCount; ++i) {
        fprintf(f,
                "[\n\t\ta lv2:OutputPort ,\n\t\t\tlv2:ControlPort ;\n\t\tlv2:index %u ;\n"
                "\t\tlv2:symbol \"%s\" ;\n\t\tlv2:name \"%s\" ;\n"
                "\t\tlv2:default 0.0 ;\n\t\tlv2:minimum 0.0 ;\n\t\tlv2:maximum 1.0\n\t] , ",
                kPortFeedbackFirst + static_cast<std::uint32_t>(i), kFeedbackSymbols[i],
                kFeedbackNames[i]);
    }

    // Latency. The VST3 build reports it through getLatencySamples(); LV2's way of saying the same
    // thing is this port, and the wrapper copies the same figure into it every block.
    fprintf(f,
            "[\n\t\ta lv2:OutputPort ,\n\t\t\tlv2:ControlPort ;\n\t\tlv2:index %u ;\n"
            "\t\tlv2:symbol \"latency\" ;\n\t\tlv2:name \"Latency\" ;\n"
            "\t\tlv2:designation lv2:latency ;\n"
            "\t\tlv2:portProperty lv2:reportsLatency ,\n\t\t\tlv2:integer ;\n"
            "\t\tlv2:minimum 0 ;\n\t\tlv2:maximum 65536\n\t] .\n",
            static_cast<std::uint32_t>(kPortLatency));

    fclose(f);
    return true;
}

//------------------------------------------------------------------------
bool writeManifest(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        fail("cannot write %s", path.c_str());
        return false;
    }
    fprintf(f, "# GENERATED by tools/namix_ttlgen.cpp. Do not edit.\n\n");
    fprintf(f, "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
               "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
               "@prefix ui:   <http://lv2plug.in/ns/extensions/ui#> .\n\n");
    fprintf(f,
            "<%s>\n\ta lv2:Plugin ;\n\tlv2:binary <NAMix.so> ;\n\trdfs:seeAlso <NAMix.ttl> .\n\n",
            kPluginUri);
    fprintf(
        f,
        "# The UI is declared HERE as well as in NAMix.ttl, because hosts discover UIs while\n"
        "# scanning manifests, before any plug-in's full description is read: one declared only\n"
        "# in the plug-in's own file is invisible to most of them.\n"
        "#\n"
        "# Both lv2:binary and ui:binary are given. ui:binary is deprecated in favour of\n"
        "# lv2:binary and the specification still requires hosts to support it, and loaders\n"
        "# differ in which they look for — lilv reads lv2:binary and falls back to ui:binary,\n"
        "# while other hosts only ever learned ui:binary.\n");
    fprintf(f,
            "<%s>\n\ta ui:X11UI ;\n\tlv2:binary <NAMix_ui.so> ;\n\tui:binary <NAMix_ui.so> ;\n"
            "\trdfs:seeAlso <NAMix.ttl> .\n",
            kUiUri);
    fclose(f);
    return true;
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: namix_ttlgen <bundle directory>\n");
        return 2;
    }
    const std::string dir = argv[1];

    // The controller is instantiated exactly as a host does, minus the host context: nothing here
    // sends a message, and initialize() is what declares every parameter this file reads.
    IPtr<NamController> controller = owned(new NamController());
    if (controller->initialize(nullptr) != kResultOk) {
        fprintf(stderr, "namix_ttlgen: the controller refused to initialise\n");
        return 1;
    }

    const std::vector<PortInfo> ports = collectPorts(*controller);
    checkAgainstController(*controller, ports);
    if (gFailures > 0) {
        fprintf(stderr, "namix_ttlgen: %d problem(s); no TTL written\n", gFailures);
        return 1;
    }

    if (!writeManifest(dir + "/manifest.ttl") || !writePluginTtl(dir + "/NAMix.ttl", ports))
        return 1;

    printf("namix_ttlgen: %d control ports, %d feedback ports, %u ports in all -> %s\n",
           kControlInCount, kFeedbackCount, static_cast<unsigned>(kPortCount), dir.c_str());
    controller->terminate();
    return 0;
}
