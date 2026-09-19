// SPDX-License-Identifier: MIT
//
// The VST3 <-> atom message tunnel, and the two small VST3 host objects it needs.
//
// The plug-in's two halves already talk to each other through IConnectionPoint: the controller
// sends "load this model file" and the processor answers with what that model can do. A VST3 host
// provides the IMessage objects and carries them across. LV2 has no analogue, so this file is that
// host: it supplies the messages, and it turns each one into an atom and back.
//
// WHY IT IS GENERIC. VST3's IAttributeList cannot be enumerated — there is no "give me the keys"
// call in pluginterfaces/vst/ivstattributes.h — so a serializer that only had the interface to
// work with would have to know each message's attribute names, which is one hand-written encoder
// and one decoder per message type, and a new message would silently escape both. The way out is to
// supply the attribute list as well as read it: MessageAttributes RECORDS what the sender wrote,
// in order and with its type, and the serializer walks that. Neither half of the plug-in knows
// this is happening, and nothing in src/ changes.
//
// THE WIRE FORM. One atom Object of type namix:message, with two properties:
//
//   namix:msgId    String   the VST3 message id, e.g. "NAMLoadModel"
//   namix:msgBody  Tuple    [ String key, Int typeTag, value ] repeated
//
// A Tuple of triples rather than an Object of properties, because VST3 attribute keys are
// arbitrary byte strings and an Object's keys are URIDs: mapping each key to a URI would mean a
// table of names, which is exactly the per-message knowledge this design exists to avoid. The
// explicit type tag is what makes a String distinguishable from a Chunk, since a VST3 String is
// UTF-16 and is carried as its raw code units rather than converted — lossless, and no encoder to
// get wrong. (No message in this plug-in uses a String attribute today; it is carried so that one
// added later needs no change here.)

#pragma once

#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>

#include <cstdint>
#include <string>
#include <vector>

namespace NAMix
{
namespace lv2
{

// The URIDs the tunnel needs, mapped once at instantiate. Both halves map the same set, which is
// what lets one forge them and the other read them.
struct MessageUris {
    LV2_URID message = 0; // namix:message
    LV2_URID msgId = 0;   // namix:msgId
    LV2_URID msgBody = 0; // namix:msgBody
    LV2_URID atomString = 0;
    LV2_URID atomLong = 0;
    LV2_URID atomDouble = 0;
    LV2_URID atomChunk = 0;
    LV2_URID atomTuple = 0;
    LV2_URID atomEventTransfer = 0;

    void map(LV2_URID_Map *urid);
    bool valid() const
    {
        return message != 0 && msgId != 0 && msgBody != 0 && atomEventTransfer != 0;
    }
};

//------------------------------------------------------------------------
// An IAttributeList that remembers. Insertion order is kept because it is the only order the
// serializer can honestly claim; a key written twice replaces the earlier entry in place, which is
// what the SDK's own HostAttributeList does and what a caller would expect.
class MessageAttributes : public Steinberg::Vst::IAttributeList
{
public:
    enum class Type : Steinberg::int32 {
        Int = 1,
        Float = 2,
        String = 3, // UTF-16 code units, carried raw
        Binary = 4,
    };

    struct Entry {
        std::string key;
        Type type = Type::Int;
        Steinberg::int64 intValue = 0;
        double floatValue = 0.0;
        // String and Binary both land here: a String's bytes are its UTF-16 code units in this
        // machine's own order, which is all the round trip needs because the two halves of an LV2
        // plug-in share a process.
        std::vector<std::uint8_t> bytes;
    };

    //---from IAttributeList----------
    Steinberg::tresult PLUGIN_API setInt(AttrID id, Steinberg::int64 value) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getInt(AttrID id, Steinberg::int64 &value) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setFloat(AttrID id, double value) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getFloat(AttrID id, double &value) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setString(AttrID id,
                                            const Steinberg::Vst::TChar *string) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getString(AttrID id, Steinberg::Vst::TChar *string,
                                            Steinberg::uint32 sizeInBytes) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setBinary(AttrID id, const void *data,
                                            Steinberg::uint32 sizeInBytes) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getBinary(AttrID id, const void *&data,
                                            Steinberg::uint32 &sizeInBytes) SMTG_OVERRIDE;

    const std::vector<Entry> &entries() const
    {
        return mEntries;
    }
    // Used by the deserializer, which builds the list rather than being written into through the
    // interface. Bounded by the caller against the atom it came out of.
    void append(Entry entry)
    {
        mEntries.push_back(std::move(entry));
    }
    void clear()
    {
        mEntries.clear();
    }

    // Refcounting that never deletes, because this object is always a MEMBER of its Message and
    // never separately owned. IMPLEMENT_FUNKNOWN_METHODS' release() calls `delete this`, which on
    // a member is a free of an interior pointer; standalone/main.cpp's ComponentHandler uses the
    // same 1000 for the same reason.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    Entry &slot(AttrID id, Type type);
    const Entry *find(AttrID id) const;

    std::vector<Entry> mEntries;
};

//------------------------------------------------------------------------
// The IMessage the two halves are handed when they call allocateMessage(). Refcounted like any
// VST3 object, because the sender releases it and the SDK's ComponentBase helpers assume it can.
class Message : public Steinberg::Vst::IMessage
{
public:
    // FUNKNOWN_CTOR and not `= default`: DECLARE_FUNKNOWN_METHODS declares the reference count and
    // does not initialise it, so a message built without this starts at whatever was in that
    // memory and the first release() either leaks it or frees it early.
    Message(){FUNKNOWN_CTOR} Steinberg::FIDString PLUGIN_API getMessageID() SMTG_OVERRIDE
    {
        return mId.c_str();
    }
    void PLUGIN_API setMessageID(Steinberg::FIDString id) SMTG_OVERRIDE
    {
        mId = id ? id : "";
    }
    Steinberg::Vst::IAttributeList *PLUGIN_API getAttributes() SMTG_OVERRIDE
    {
        return &mAttributes;
    }

    MessageAttributes &attributes()
    {
        return mAttributes;
    }
    const std::string &id() const
    {
        return mId;
    }

    DECLARE_FUNKNOWN_METHODS

private:
    std::string mId;
    MessageAttributes mAttributes;
};

//------------------------------------------------------------------------
// The host context handed to IPluginBase::initialize. Its one real job is createInstance, which is
// how ComponentBase::allocateMessage gets an IMessage — without it every controller -> processor
// message is silently dropped and the plug-in presents as one whose knobs work and which never
// loads a model. (standalone/main.cpp:295 carries the same warning at the same place.)
//
// Not refcounted in any meaningful way: it is a member of the wrapper and outlives everything that
// holds it, so the counts exist only to satisfy FUnknown.
class HostApp : public Steinberg::Vst::IHostApplication
{
public:
    explicit HostApp(const char *name) : mName(name)
    {
    }

    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;

    // ComponentBase holds the host context in an IPtr, so it addRefs and releases it. This object
    // is a member of the wrapper and outlives everything that holds it, so its release must never
    // free anything — the same non-deleting pattern standalone/main.cpp's ComponentHandler uses.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                 void **obj) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    Steinberg::uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    const char *mName;
};

//------------------------------------------------------------------------
// Serialization. Both return false rather than writing a partial atom, so a message too big for
// the buffer it was aimed at is dropped whole and says so, instead of arriving truncated.

// Write `message` into `forge` as one namix:message object. The caller has already pointed the
// forge at wherever the atom is to land — an event in a sequence, or a plain buffer.
bool forgeMessage(LV2_Atom_Forge &forge, const MessageUris &uris, Steinberg::Vst::IMessage *message,
                  const MessageAttributes &attributes);

// Rebuild a message from an atom. `object` is untrusted input from the other side of the host, so
// every size is checked against the atom's own bounds before it is believed — a malformed one
// must degrade, never crash inside a host.
// Returns nullptr for anything that is not a well-formed namix:message; the caller ignores it.
// The returned message has one reference and is the caller's to release.
Message *parseMessage(const LV2_Atom_Object *object, const MessageUris &uris);

} // namespace lv2
} // namespace NAMix
