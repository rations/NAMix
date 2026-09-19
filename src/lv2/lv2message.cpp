// SPDX-License-Identifier: MIT
//
// The VST3 <-> atom message tunnel. See lv2message.h for the wire form and why it is generic.

#include "lv2message.h"

#include "namixlv2.h"

#include <lv2/atom/util.h>

#include <cstring>
#include <new>

using namespace Steinberg;

namespace NAMix
{
namespace lv2
{

//------------------------------------------------------------------------
void MessageUris::map(LV2_URID_Map *urid)
{
    if (!urid || !urid->map)
        return;
    message = urid->map(urid->handle, kAtomMessageUri);
    msgId = urid->map(urid->handle, kAtomMessageIdUri);
    msgBody = urid->map(urid->handle, kAtomMessageBodyUri);
    atomString = urid->map(urid->handle, LV2_ATOM__String);
    atomLong = urid->map(urid->handle, LV2_ATOM__Long);
    atomDouble = urid->map(urid->handle, LV2_ATOM__Double);
    atomChunk = urid->map(urid->handle, LV2_ATOM__Chunk);
    atomTuple = urid->map(urid->handle, LV2_ATOM__Tuple);
    atomEventTransfer = urid->map(urid->handle, LV2_ATOM__eventTransfer);
}

//------------------------------------------------------------------------
// MessageAttributes
//------------------------------------------------------------------------
tresult PLUGIN_API MessageAttributes::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (FUnknownPrivate::iidEqual(iid, Vst::IAttributeList::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<Vst::IAttributeList *>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

MessageAttributes::Entry &MessageAttributes::slot(AttrID id, Type type)
{
    const char *key = id ? id : "";
    for (Entry &e : mEntries) {
        if (e.key == key) {
            e.type = type;
            return e;
        }
    }
    mEntries.push_back(Entry{});
    Entry &e = mEntries.back();
    e.key = key;
    e.type = type;
    return e;
}

const MessageAttributes::Entry *MessageAttributes::find(AttrID id) const
{
    const char *key = id ? id : "";
    for (const Entry &e : mEntries) {
        if (e.key == key)
            return &e;
    }
    return nullptr;
}

tresult PLUGIN_API MessageAttributes::setInt(AttrID id, int64 value)
{
    slot(id, Type::Int).intValue = value;
    return kResultOk;
}

tresult PLUGIN_API MessageAttributes::getInt(AttrID id, int64 &value)
{
    const Entry *e = find(id);
    if (!e || e->type != Type::Int)
        return kResultFalse;
    value = e->intValue;
    return kResultOk;
}

tresult PLUGIN_API MessageAttributes::setFloat(AttrID id, double value)
{
    slot(id, Type::Float).floatValue = value;
    return kResultOk;
}

tresult PLUGIN_API MessageAttributes::getFloat(AttrID id, double &value)
{
    const Entry *e = find(id);
    if (!e || e->type != Type::Float)
        return kResultFalse;
    value = e->floatValue;
    return kResultOk;
}

tresult PLUGIN_API MessageAttributes::setString(AttrID id, const Vst::TChar *string)
{
    if (!string)
        return kInvalidArgument;
    // The interface says "must be null-terminated", so the length is found the same way the SDK's
    // own attribute list finds it. The code units are stored as they stand.
    size_t units = 0;
    while (string[units] != 0)
        ++units;
    Entry &e = slot(id, Type::String);
    e.bytes.resize(units * sizeof(Vst::TChar));
    if (units > 0)
        std::memcpy(e.bytes.data(), string, e.bytes.size());
    return kResultOk;
}

tresult PLUGIN_API MessageAttributes::getString(AttrID id, Vst::TChar *string, uint32 sizeInBytes)
{
    const Entry *e = find(id);
    if (!e || e->type != Type::String || !string)
        return kResultFalse;
    // sizeInBytes counts the terminator's room too, so the longest string that fits is one code
    // unit shorter than the buffer. A caller's buffer is untrusted input like everything else.
    if (sizeInBytes < sizeof(Vst::TChar))
        return kResultFalse;
    const size_t room = sizeInBytes / sizeof(Vst::TChar) - 1;
    const size_t units = std::min(room, e->bytes.size() / sizeof(Vst::TChar));
    if (units > 0)
        std::memcpy(string, e->bytes.data(), units * sizeof(Vst::TChar));
    string[units] = 0;
    return kResultOk;
}

tresult PLUGIN_API MessageAttributes::setBinary(AttrID id, const void *data, uint32 sizeInBytes)
{
    if (!data && sizeInBytes > 0)
        return kInvalidArgument;
    Entry &e = slot(id, Type::Binary);
    e.bytes.assign(static_cast<const std::uint8_t *>(data),
                   static_cast<const std::uint8_t *>(data) + sizeInBytes);
    return kResultOk;
}

tresult PLUGIN_API MessageAttributes::getBinary(AttrID id, const void *&data, uint32 &sizeInBytes)
{
    const Entry *e = find(id);
    if (!e || e->type != Type::Binary)
        return kResultFalse;
    data = e->bytes.data();
    sizeInBytes = static_cast<uint32>(e->bytes.size());
    return kResultOk;
}

//------------------------------------------------------------------------
// Message
//------------------------------------------------------------------------
IMPLEMENT_FUNKNOWN_METHODS(Message, Vst::IMessage, Vst::IMessage::iid)

//------------------------------------------------------------------------
// HostApp
//------------------------------------------------------------------------
tresult PLUGIN_API HostApp::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (FUnknownPrivate::iidEqual(iid, Vst::IHostApplication::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<Vst::IHostApplication *>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API HostApp::getName(Vst::String128 name)
{
    if (!name)
        return kInvalidArgument;
    // ASCII to UTF-16 by hand rather than through the SDK's UString, which would pull the string
    // helpers into two shared objects for one short constant.
    int i = 0;
    for (; mName && mName[i] && i < 127; ++i)
        name[i] = static_cast<Vst::TChar>(static_cast<unsigned char>(mName[i]));
    name[i] = 0;
    return kResultOk;
}

tresult PLUGIN_API HostApp::createInstance(TUID cid, TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    *obj = nullptr;
    // The only object a host must be able to make is IMessage, and it is the only one either half
    // of this plug-in ever asks for. Anything else is a clean refusal rather than a crash.
    if (FUnknownPrivate::iidEqual(cid, Vst::IMessage::iid) &&
        FUnknownPrivate::iidEqual(iid, Vst::IMessage::iid)) {
        // One reference, which the caller owns — FUNKNOWN_CTOR sets it, and adding another here
        // would leak every message the plug-in ever sent.
        auto *message = new (std::nothrow) Message();
        if (!message)
            return kOutOfMemory;
        *obj = static_cast<Vst::IMessage *>(message);
        return kResultOk;
    }
    return kResultFalse;
}

//------------------------------------------------------------------------
// Serialization
//------------------------------------------------------------------------
namespace
{

// Every forge call returns 0 when the buffer is full, and a caller that ignores that writes a
// half-formed atom and reports success. Collecting the answers is what makes "dropped whole"
// true rather than hoped for.
inline bool ok(LV2_Atom_Forge_Ref ref)
{
    return ref != 0;
}

} // namespace

bool forgeMessage(LV2_Atom_Forge &forge, const MessageUris &uris, Vst::IMessage *message,
                  const MessageAttributes &attributes)
{
    if (!uris.valid() || !message)
        return false;
    const char *id = message->getMessageID();
    if (!id)
        id = "";

    LV2_Atom_Forge_Frame object;
    if (!ok(lv2_atom_forge_object(&forge, &object, 0, uris.message)))
        return false;

    if (!ok(lv2_atom_forge_key(&forge, uris.msgId)) ||
        !ok(lv2_atom_forge_string(&forge, id, static_cast<uint32_t>(std::strlen(id)))))
        return false;

    if (!ok(lv2_atom_forge_key(&forge, uris.msgBody)))
        return false;
    LV2_Atom_Forge_Frame body;
    if (!ok(lv2_atom_forge_tuple(&forge, &body)))
        return false;

    for (const MessageAttributes::Entry &e : attributes.entries()) {
        if (!ok(lv2_atom_forge_string(&forge, e.key.c_str(),
                                      static_cast<uint32_t>(e.key.size()))) ||
            !ok(lv2_atom_forge_int(&forge, static_cast<int32_t>(e.type))))
            return false;

        switch (e.type) {
            case MessageAttributes::Type::Int:
                if (!ok(lv2_atom_forge_long(&forge, e.intValue)))
                    return false;
                break;
            case MessageAttributes::Type::Float:
                if (!ok(lv2_atom_forge_double(&forge, e.floatValue)))
                    return false;
                break;
            case MessageAttributes::Type::String:
            case MessageAttributes::Type::Binary: {
                // Both travel as a Chunk; the type tag written above is what tells them apart on
                // the way back, so nothing has to be guessed from the atom's own type.
                const uint32_t size = static_cast<uint32_t>(e.bytes.size());
                if (!ok(lv2_atom_forge_atom(&forge, size, forge.Chunk)))
                    return false;
                if (size > 0 && !ok(lv2_atom_forge_write(&forge, e.bytes.data(), size)))
                    return false;
                break;
            }
        }
    }

    lv2_atom_forge_pop(&forge, &body);
    lv2_atom_forge_pop(&forge, &object);
    return true;
}

//------------------------------------------------------------------------
namespace
{

// The atom body a tuple iterator is standing on, with its own bounds already checked by
// lv2_atom_tuple_is_end. Reading the payload still needs the size the header claims to be inside
// the atom the caller handed us, which the iterator guarantees.
const char *stringBody(const LV2_Atom *atom, const MessageUris &uris, size_t &length)
{
    if (!atom || atom->type != uris.atomString || atom->size == 0)
        return nullptr;
    const char *text = static_cast<const char *>(LV2_ATOM_BODY_CONST(atom));
    // An atom:String is null-terminated by the spec, but it arrived from outside this object and
    // a missing terminator would run the read past the atom. Take the shorter of the two answers.
    length = 0;
    while (length + 1 < atom->size && text[length] != '\0')
        ++length;
    return text;
}

} // namespace

Message *parseMessage(const LV2_Atom_Object *object, const MessageUris &uris)
{
    if (!object || !uris.valid())
        return nullptr;
    if (object->body.otype != uris.message)
        return nullptr;

    const LV2_Atom *idAtom = nullptr;
    const LV2_Atom *bodyAtom = nullptr;
    lv2_atom_object_get(object, uris.msgId, &idAtom, uris.msgBody, &bodyAtom, 0);

    size_t idLength = 0;
    const char *id = stringBody(idAtom, uris, idLength);
    if (!id)
        return nullptr;

    auto *message = new (std::nothrow) Message();
    if (!message)
        return nullptr;
    message->setMessageID(std::string(id, idLength).c_str());

    if (!bodyAtom || bodyAtom->type != uris.atomTuple)
        return message; // a message with no attributes is legitimate: kMsgRequestCaps is one

    const auto *tuple = reinterpret_cast<const LV2_Atom_Tuple *>(bodyAtom);
    const LV2_Atom *it = lv2_atom_tuple_begin(tuple);
    while (!lv2_atom_tuple_is_end(LV2_ATOM_BODY_CONST(bodyAtom), bodyAtom->size, it)) {
        // key
        size_t keyLength = 0;
        const char *key = stringBody(it, uris, keyLength);
        it = lv2_atom_tuple_next(it);
        if (lv2_atom_tuple_is_end(LV2_ATOM_BODY_CONST(bodyAtom), bodyAtom->size, it))
            break;
        // type tag
        const LV2_Atom *tagAtom = it;
        it = lv2_atom_tuple_next(it);
        if (lv2_atom_tuple_is_end(LV2_ATOM_BODY_CONST(bodyAtom), bodyAtom->size, it))
            break;
        // value
        const LV2_Atom *valueAtom = it;
        it = lv2_atom_tuple_next(it);

        if (!key || !tagAtom || tagAtom->size != sizeof(int32_t))
            continue;
        const int32_t tag = *static_cast<const int32_t *>(LV2_ATOM_BODY_CONST(tagAtom));

        MessageAttributes::Entry entry;
        entry.key.assign(key, keyLength);
        switch (tag) {
            case static_cast<int32_t>(MessageAttributes::Type::Int):
                if (!valueAtom || valueAtom->size != sizeof(int64_t))
                    continue;
                entry.type = MessageAttributes::Type::Int;
                entry.intValue = *static_cast<const int64_t *>(LV2_ATOM_BODY_CONST(valueAtom));
                break;
            case static_cast<int32_t>(MessageAttributes::Type::Float):
                if (!valueAtom || valueAtom->size != sizeof(double))
                    continue;
                entry.type = MessageAttributes::Type::Float;
                entry.floatValue = *static_cast<const double *>(LV2_ATOM_BODY_CONST(valueAtom));
                break;
            case static_cast<int32_t>(MessageAttributes::Type::String):
            case static_cast<int32_t>(MessageAttributes::Type::Binary): {
                if (!valueAtom)
                    continue;
                entry.type = tag == static_cast<int32_t>(MessageAttributes::Type::String)
                                 ? MessageAttributes::Type::String
                                 : MessageAttributes::Type::Binary;
                const auto *bytes =
                    static_cast<const std::uint8_t *>(LV2_ATOM_BODY_CONST(valueAtom));
                entry.bytes.assign(bytes, bytes + valueAtom->size);
                break;
            }
            default:
                continue; // a type this build does not know; skipped, never guessed at
        }
        message->attributes().append(std::move(entry));
    }

    return message;
}

} // namespace lv2
} // namespace NAMix
