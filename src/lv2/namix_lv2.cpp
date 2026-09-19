// SPDX-License-Identifier: MIT
//
// NAMix.so — the LV2 DSP half.
//
// This is a HOST, not a second plug-in. It instantiates the same NamProcessor a VST3 DAW gets and
// drives it through IAudioProcessor; src/lv2/namixlv2.h says why that is a wrapper and not a port.
// The four entry points and where their work belongs:
//
//   instantiate()    builds the processor and every VST3 process structure once, so run() can
//                    allocate nothing.
//
//   run()            LV2's audio thread. Reads the control ports, turns changes into parameter
//                    points, calls process(), and copies the feedback out. Allocates nothing:
//                    every VST3 process structure is built in instantiate(), the parameter queues
//                    are pre-sized AND pre-touched, and the two message rings are fixed-capacity.
//
//   messageLoop()    one thread this file owns, and the reason it exists is not obvious. A
//                    controller -> processor message is handled by NamProcessor::notify, which
//                    reads a file, parses JSON and rebuilds a network. In VST3 that lands on the
//                    host's message thread. In LV2 the same message arrives as an atom IN run(),
//                    so calling notify() there would put file I/O on the audio thread, which the
//                    real-time contract forbids outright. So run() copies the atom into a ring and
//                    this thread drains it.
//
//                    The LV2 way to say that is work:schedule, and it was the first plan. It is
//                    not used because it is an OPTIONAL host feature: a host that does not offer
//                    it would leave an amp that cannot load a model, which is the whole product.
//                    A thread of our own works in every host, and it is simply the message thread
//                    the VST3 build gets from its host.
//
//   save()/restore() the host's own non-RT context, which is where getState/setState belong.
//
// The two rings are the only traffic between run() and the message thread, and neither direction
// ever blocks the audio thread: run() writes a request and moves on, and reads whatever replies
// happen to be waiting.

#include "lv2message.h"
#include "namixlv2.h"

#include "namprocessor.h"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace NAMix;
using namespace NAMix::lv2;

namespace
{

//------------------------------------------------------------------------
// A fixed-capacity ring of serialized atoms.
//
// Single producer, single consumer, and the two indices are the whole of the synchronisation: the
// producer publishes its write index with a release store after the bytes are in place, and the
// consumer's acquire load of it is what makes those bytes visible. No allocation and no lock on
// either side, which is what lets the audio thread be one of the two.
//
// A message too big for a slot is DROPPED and said so, rather than truncated: half a model path is
// a load of the wrong file, and half a state blob is a project that opens wrong.
class AtomRing
{
public:
    static constexpr std::uint32_t kSlots = 8;
    static constexpr std::uint32_t kSlotBytes = 32 * 1024;

    bool push(const void *data, std::uint32_t size)
    {
        if (size == 0 || size > kSlotBytes)
            return false;
        const std::uint32_t w = mWrite.load(std::memory_order_relaxed);
        const std::uint32_t r = mRead.load(std::memory_order_acquire);
        if (w - r >= kSlots)
            return false; // full
        Slot &slot = mSlots[w % kSlots];
        std::memcpy(slot.bytes, data, size);
        slot.size = size;
        mWrite.store(w + 1, std::memory_order_release);
        return true;
    }

    // The oldest slot, or nullptr when empty. Valid until drop().
    const std::uint8_t *peek(std::uint32_t &size) const
    {
        const std::uint32_t r = mRead.load(std::memory_order_relaxed);
        if (mWrite.load(std::memory_order_acquire) == r)
            return nullptr;
        const Slot &slot = mSlots[r % kSlots];
        size = slot.size;
        return slot.bytes;
    }

    void drop()
    {
        mRead.store(mRead.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

private:
    struct Slot {
        std::uint32_t size = 0;
        // 8-byte aligned because an LV2_Atom is read in place out of this buffer and its bodies
        // are 64-bit aligned by the atom spec's own padding rule.
        alignas(8) std::uint8_t bytes[kSlotBytes];
    };
    Slot mSlots[kSlots];
    std::atomic<std::uint32_t> mWrite{0};
    std::atomic<std::uint32_t> mRead{0};
};

//------------------------------------------------------------------------
class NamixLv2;

// The processor's peer. NamProcessor::sendMessage lands here, on the message thread, and the atom
// is queued for run() to write into the notify port.
class UiPeer : public Vst::IConnectionPoint
{
public:
    explicit UiPeer(NamixLv2 &owner) : mOwner(owner)
    {
    }

    tresult PLUGIN_API connect(IConnectionPoint *) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint *) SMTG_OVERRIDE
    {
        return kResultOk;
    }
    tresult PLUGIN_API notify(Vst::IMessage *message) SMTG_OVERRIDE;

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) SMTG_OVERRIDE
    {
        if (!obj)
            return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(iid, Vst::IConnectionPoint::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IConnectionPoint *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // Non-deleting, and it has to be: ComponentBase holds its peer in an IPtr, so connect()
    // addRefs this and disconnect() releases it — and a generated release() would call
    // `delete this`, which on a member of the wrapper is a free of an interior pointer.
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE
    {
        return 1000;
    }
    uint32 PLUGIN_API release() SMTG_OVERRIDE
    {
        return 1000;
    }

private:
    NamixLv2 &mOwner;
};

//------------------------------------------------------------------------
class NamixLv2
{
public:
    NamixLv2() : mHostApp("NAMix LV2"), mUiPeer(*this)
    {
    }

    bool instantiate(double rate, const char *bundlePath, const LV2_Feature *const *features);
    void connectPort(std::uint32_t port, void *data);
    void activate();
    void run(std::uint32_t nframes);
    void deactivate();
    ~NamixLv2();

    LV2_State_Status save(LV2_State_Store_Function store, LV2_State_Handle handle,
                          const LV2_Feature *const *features);
    LV2_State_Status restore(LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
                             const LV2_Feature *const *features);

    // Message thread -> run(). Serialized on mReplyMutex by its two producers.
    void queueReply(Vst::IMessage *message);

private:
    void messageLoop();
    void dispatch(Message *message);
    void sendStateToUi();
    void readAtomInput();
    void writeAtomOutput();
    void pushPoint(Vst::ParamID id, double normalized);
    void sendSlim(double normalized);
    // Send one load message into the processor exactly as the controller would. Used by restore()
    // when state:mapPath says a file has moved.
    void sendLoad(int slot, const std::string &path);

    // --- host features ---------------------------------------------------
    LV2_URID_Map *mMap = nullptr;
    MessageUris mUris;
    LV2_URID mAtomInt = 0;
    LV2_URID mAtomLong = 0;
    LV2_URID mAtomChunk = 0;
    LV2_URID mAtomString = 0;
    LV2_URID mAtomPath = 0;
    LV2_URID mStateBlob = 0;
    LV2_URID mPathAbstract[kPathSlotCount] = {};
    LV2_URID mPathRaw[kPathSlotCount] = {};

    // --- the plug-in ------------------------------------------------------
    HostApp mHostApp;
    UiPeer mUiPeer;
    IPtr<NamProcessor> mProcessor;

    // --- ports ------------------------------------------------------------
    const float *mAudioIn = nullptr;
    float *mAudioOut[2] = {nullptr, nullptr};
    const LV2_Atom_Sequence *mAtomIn = nullptr;
    LV2_Atom_Sequence *mAtomOut = nullptr;
    const float *mControl[kControlInCount] = {};
    float *mFeedback[kFeedbackCount] = {};
    float *mLatency = nullptr;

    // Last value seen on each control port, so a point is pushed only when the host actually moved
    // something, and a flag saying whether anything has been seen there yet — which is what gets
    // the plug-in to the host's restored values before the first sample is processed.
    //
    // A SEPARATE BOOL, and never a NaN in mLastControl standing for "nothing yet". This
    // translation unit is built with -ffast-math, which implies -ffinite-math-only, under which
    // the compiler may assume no operand is a NaN and drops the unordered check from a floating
    // compare — so `plain == NaN` comes out TRUE. Written the other way this loop would skip every
    // port on the first block, leave the sentinel in place, and skip it again for the life of the
    // instance: every control input permanently dead, in a plug-in that loads, plays and meters
    // perfectly. A NaN means nothing to code built this way, so nothing may be built on one.
    double mLastControl[kControlInCount] = {};
    bool mControlSeen[kControlInCount] = {};

    // --- VST3 process plumbing, all built in instantiate() ----------------
    Vst::ProcessData mData;
    Vst::AudioBusBuffers mInBus;
    Vst::AudioBusBuffers mOutBus;
    float *mOutPtrs[2] = {nullptr, nullptr};
    const float *mInPtr = nullptr;
    Vst::ParameterChanges mInputChanges;
    Vst::ParameterChanges mOutputChanges;
    Vst::ProcessContext mContext = {};

    // --- the message thread -----------------------------------------------
    AtomRing mToMessage; // run() -> messageLoop()
    AtomRing mToRun;     // messageLoop() (and restore()) -> run()
    // The reply ring has two producers — the message thread and whichever thread the host calls
    // restore() on — so its push side is serialized here. run() is the single consumer and never
    // touches this.
    std::mutex mReplyMutex;
    std::thread mMessageThread;
    std::atomic<bool> mMessageRunning{false};

    // --- the Slim bridge ---------------------------------------------------
    //
    // Slim is the one control that is not finished by a parameter point. NamProcessor's queue
    // handler only records the value (namprocessor.cpp, the kSlimId case says so in as many
    // words); what actually resizes the network is kMsgSetSlim, handled on the message thread,
    // because SetSlimmableSize stages a partial rebuild and allocates.
    //
    // With the editor open that route already exists: NamController::setParamNormalized sends the
    // message, the tunnel carries it, and dispatch() hands it over. But a HOST may write the Slim
    // control port with no editor open at all — an automation lane, or its own generic UI — and
    // then there is no controller in the loop and nothing would ever call SetSlimmableSize. Slim
    // would be silently inert in exactly the case a host user would try first.
    //
    // So run() records the port's value here and the message thread sends the message. Coalescing
    // is correct rather than merely cheap: only the newest value means anything, and a knob drag
    // produces thirty a second.
    std::atomic<double> mPendingSlim{0.0};
    std::atomic<bool> mSlimPending{false};
    // Message thread only. What was last handed to the processor, so a drag through the editor —
    // which arrives BOTH as a tunnelled message and as a port write — costs one rebuild and not
    // two. dispatch() updates it for the editor's own messages.
    double mLastSlimSent = 0.0;
    bool mSlimSentOnce = false;

    // Scratch for forging one reply, owned by whichever producer holds mReplyMutex.
    std::vector<std::uint8_t> mReplyScratch;
    LV2_Atom_Forge mReplyForge = {};
    // run()'s own forge, aimed at the notify port.
    LV2_Atom_Forge mOutForge = {};

    double mSampleRate = 48000.0;
    std::int32_t mMaxBlockSize = 4096;
    // Warned once rather than every block, because a full ring under a stuck message thread would
    // otherwise print at the audio rate.
    std::atomic<bool> mWarnedDropped{false};

    // What the plug-in's file paths are, mirrored here for the same reason NamController mirrors
    // them: this half writes the LV2 state, and the processor's copies are private to it. Written
    // on the message thread and by restore(); read by save(). Both are the host's own non-RT
    // calls, and the host does not overlap them.
    std::string mPath[kPathSlotCount];
};

//------------------------------------------------------------------------
tresult PLUGIN_API UiPeer::notify(Vst::IMessage *message)
{
    mOwner.queueReply(message);
    return kResultOk;
}

//------------------------------------------------------------------------
bool NamixLv2::instantiate(double rate, const char *bundlePath,
                           const LV2_Feature *const *features)
{
    (void)bundlePath; // the DSP loads no art; the UI does, from its own copy of this path

    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI)
            continue;
        if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
            mMap = static_cast<LV2_URID_Map *>(features[i]->data);
    }
    if (!mMap || !mMap->map) {
        fprintf(stderr, "NAMix: the host provides no urid:map; the plug-in cannot run\n");
        return false;
    }

    mUris.map(mMap);
    mAtomInt = mMap->map(mMap->handle, LV2_ATOM__Int);
    mAtomLong = mMap->map(mMap->handle, LV2_ATOM__Long);
    mAtomChunk = mMap->map(mMap->handle, LV2_ATOM__Chunk);
    mAtomString = mMap->map(mMap->handle, LV2_ATOM__String);
    mAtomPath = mMap->map(mMap->handle, LV2_ATOM__Path);
    mStateBlob = mMap->map(mMap->handle, kStateBlobUri);
    for (int slot = 0; slot < kPathSlotCount; ++slot) {
        const std::string abstractUri = std::string(kStatePathAbstractPrefix) + kPathSlotName[slot];
        const std::string rawUri = std::string(kStatePathRawPrefix) + kPathSlotName[slot];
        mPathAbstract[slot] = mMap->map(mMap->handle, abstractUri.c_str());
        mPathRaw[slot] = mMap->map(mMap->handle, rawUri.c_str());
    }

    // The largest block the host says it will ever ask for. The processor loops in whole
    // sub-blocks of whatever it was set up with, so a host that exceeds this is handled rather
    // than clamped; what the figure really decides is how much buffer is allocated once.
    bool sawMaxBlock = false;
    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI || std::strcmp(features[i]->URI, LV2_OPTIONS__options) != 0)
            continue;
        const LV2_URID wanted = mMap->map(mMap->handle, LV2_BUF_SIZE__maxBlockLength);
        const auto *option = static_cast<const LV2_Options_Option *>(features[i]->data);
        for (; option && option->key; ++option) {
            if (option->key != wanted || !option->value)
                continue;
            if (option->type == mAtomInt) {
                mMaxBlockSize = *static_cast<const std::int32_t *>(option->value);
                sawMaxBlock = true;
            } else if (option->type == mAtomLong) {
                mMaxBlockSize =
                    static_cast<std::int32_t>(*static_cast<const std::int64_t *>(option->value));
                sawMaxBlock = true;
            }
        }
    }
    if (!sawMaxBlock)
        fprintf(stderr,
                "NAMix: the host states no bufsz:maxBlockLength; sizing buffers for "
                "%d frames and chunking anything larger\n",
                4096);
    mMaxBlockSize = std::clamp<std::int32_t>(mMaxBlockSize, 16, 1 << 16);
    mSampleRate = rate;

    mProcessor = owned(new (std::nothrow) NamProcessor());
    if (!mProcessor)
        return false;
    // Before anything else: allocateMessage() asks the host context for its IMessage instances, so
    // a processor initialised without one silently drops every reply to the editor.
    if (mProcessor->initialize(&mHostApp) != kResultOk) {
        fprintf(stderr, "NAMix: the processor refused to initialise\n");
        return false;
    }
    mProcessor->connect(&mUiPeer);

    // The buses initialize() declared are mono in / stereo out, which is exactly the port layout
    // above, so there is no arrangement to negotiate. Activating them is what a host does and what
    // keeps the two halves' idea of the bus state the same. There is no event bus to activate:
    // NAMix declares no MIDI parameters, which is also why the atom input port carries only
    // atom:Object.
    mProcessor->activateBus(Vst::kAudio, Vst::kInput, 0, true);
    mProcessor->activateBus(Vst::kAudio, Vst::kOutput, 0, true);

    Vst::ProcessSetup setup = {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = mMaxBlockSize;
    setup.sampleRate = mSampleRate;
    if (mProcessor->setupProcessing(setup) != kResultOk) {
        fprintf(stderr, "NAMix: the processor rejected the process setup\n");
        return false;
    }

    // --- the process structures, built once ------------------------------
    mInBus.numChannels = 1;
    mInBus.channelBuffers32 = const_cast<float **>(&mInPtr);
    mInBus.silenceFlags = 0;
    mOutBus.numChannels = 2;
    mOutBus.channelBuffers32 = mOutPtrs;
    mOutBus.silenceFlags = 0;

    mData.numInputs = 1;
    mData.numOutputs = 1;
    mData.inputs = &mInBus;
    mData.outputs = &mOutBus;
    mData.symbolicSampleSize = Vst::kSample32;
    mData.processMode = Vst::kRealtime;
    mData.inputParameterChanges = &mInputChanges;
    mData.outputParameterChanges = &mOutputChanges;
    // No inputEvents: the processor declares no event bus and reads none.
    mData.inputEvents = nullptr;
    mData.processContext = &mContext;
    mContext.sampleRate = mSampleRate;

    // One queue per parameter the wrapper can ever write. Sized here AND pre-touched below,
    // because ParameterChanges::addParameterData grows a vector when it runs out of reserved
    // queues and ParameterValueQueue::addPoint grows one when it runs out of reserved points —
    // both of which are a malloc on the audio thread.
    mInputChanges.setMaxParameters(kControlInCount);
    mOutputChanges.setMaxParameters(kFeedbackCount);

    for (int i = 0; i < kControlInCount; ++i)
        pushPoint(controlPortParam(i), 0.0);
    mInputChanges.clearQueue();

    for (int i = 0; i < kControlInCount; ++i) {
        mLastControl[i] = 0.0;
        mControlSeen[i] = false;
    }

    mReplyScratch.resize(AtomRing::kSlotBytes);
    lv2_atom_forge_init(&mReplyForge, mMap);
    lv2_atom_forge_init(&mOutForge, mMap);

    mMessageRunning.store(true, std::memory_order_release);
    mMessageThread = std::thread([this] { messageLoop(); });
    return true;
}

//------------------------------------------------------------------------
NamixLv2::~NamixLv2()
{
    mMessageRunning.store(false, std::memory_order_release);
    if (mMessageThread.joinable())
        mMessageThread.join();
    if (mProcessor) {
        mProcessor->disconnect(&mUiPeer);
        mProcessor->terminate();
    }
}

//------------------------------------------------------------------------
void NamixLv2::connectPort(std::uint32_t port, void *data)
{
    switch (port) {
        case kPortAudioIn:
            mAudioIn = static_cast<const float *>(data);
            return;
        case kPortAudioOutL:
            mAudioOut[0] = static_cast<float *>(data);
            return;
        case kPortAudioOutR:
            mAudioOut[1] = static_cast<float *>(data);
            return;
        case kPortAtomIn:
            mAtomIn = static_cast<const LV2_Atom_Sequence *>(data);
            return;
        case kPortAtomOut:
            mAtomOut = static_cast<LV2_Atom_Sequence *>(data);
            return;
        default:
            break;
    }
    if (port >= kPortControlFirst && port < kPortFeedbackFirst) {
        mControl[port - kPortControlFirst] = static_cast<const float *>(data);
    } else if (port >= kPortFeedbackFirst && port < kPortLatency) {
        mFeedback[port - kPortFeedbackFirst] = static_cast<float *>(data);
    } else if (port == kPortLatency) {
        mLatency = static_cast<float *>(data);
    }
}

//------------------------------------------------------------------------
void NamixLv2::activate()
{
    if (!mProcessor)
        return;
    mProcessor->setActive(true);
    mProcessor->setProcessing(true);
}

void NamixLv2::deactivate()
{
    if (!mProcessor)
        return;
    mProcessor->setProcessing(false);
    mProcessor->setActive(false);
}

//------------------------------------------------------------------------
// One parameter point, on the audio thread, allocating nothing.
//
// Two SDK behaviours make that true and neither is obvious. addParameterData reuses a queue that
// setMaxParameters already reserved, and grows its vector when it runs out — which is why every id
// this wrapper can ever write is counted there. And addPoint REPLACES a point at the same sample
// offset rather than appending one (public.sdk/source/vst/hosting/parameterchanges.cpp), so the
// offset of 0 used here means a parameter written twice in one block costs no second point.
//
// Sample-accurate automation is given up by that choice, and it costs nothing: every parameter here
// is a control rather than a signal, and the processor reads only the last point of each queue.
void NamixLv2::pushPoint(Vst::ParamID id, double normalized)
{
    int32 index = 0;
    if (Vst::IParamValueQueue *queue = mInputChanges.addParameterData(id, index))
        queue->addPoint(0, normalized, index);
}

//------------------------------------------------------------------------
void NamixLv2::readAtomInput()
{
    if (!mAtomIn)
        return;
    LV2_ATOM_SEQUENCE_FOREACH(mAtomIn, ev)
    {
        const LV2_Atom *atom = &ev->body;
        if (!lv2_atom_forge_is_object_type(&mOutForge, atom->type))
            continue;
        const auto *object = reinterpret_cast<const LV2_Atom_Object *>(atom);
        if (object->body.otype != mUris.message)
            continue;
        // Straight into the ring, bytes and all: parsing it here would mean allocating on the
        // audio thread, and acting on it here would mean file I/O.
        if (!mToMessage.push(atom, lv2_atom_total_size(atom)) &&
            !mWarnedDropped.exchange(true, std::memory_order_relaxed)) {
            fprintf(stderr, "NAMix: an editor message was dropped; the message queue is "
                            "full or the message is too large\n");
        }
    }
}

//------------------------------------------------------------------------
void NamixLv2::writeAtomOutput()
{
    if (!mAtomOut)
        return;
    // The capacity the host gave us is in the atom's own size field before we overwrite it, which
    // is the LV2 convention for an output atom port.
    const std::uint32_t capacity = mAtomOut->atom.size;
    lv2_atom_forge_set_buffer(&mOutForge, reinterpret_cast<std::uint8_t *>(mAtomOut), capacity);

    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_sequence_head(&mOutForge, &frame, 0);

    for (;;) {
        std::uint32_t size = 0;
        const std::uint8_t *bytes = mToRun.peek(size);
        if (!bytes)
            break;
        // Stop at the first one that does not fit and leave it in the ring for the next block,
        // rather than dropping it: a reply the editor never receives is a panel that never catches
        // up, and one block later there is room.
        if (mOutForge.offset + sizeof(LV2_Atom_Event) + size > mOutForge.size)
            break;
        if (!lv2_atom_forge_frame_time(&mOutForge, 0))
            break;
        if (!lv2_atom_forge_write(&mOutForge, bytes, size))
            break;
        mToRun.drop();
    }

    lv2_atom_forge_pop(&mOutForge, &frame);
}

//------------------------------------------------------------------------
void NamixLv2::run(std::uint32_t nframes)
{
    if (!mProcessor || !mAudioIn || !mAudioOut[0])
        return;

    mInputChanges.clearQueue();
    mOutputChanges.clearQueue();

    // Every control port whose value has moved becomes one parameter point. The host owns these
    // values, so this is also how a project's restored settings arrive: the first block sees every
    // one of them as unseen and publishes it.
    for (int i = 0; i < kControlInCount; ++i) {
        if (!mControl[i])
            continue;
        const double plain = static_cast<double>(*mControl[i]);
        if (mControlSeen[i] && plain == mLastControl[i])
            continue;
        mLastControl[i] = plain;
        mControlSeen[i] = true;
        const ControlSpec spec = controlSpec(i);
        const double norm = controlNorm(spec, plain);
        pushPoint(spec.id, norm);
        // Slim needs a message as well as a point; see mPendingSlim.
        if (spec.id == kSlimId) {
            mPendingSlim.store(norm, std::memory_order_relaxed);
            mSlimPending.store(true, std::memory_order_release);
        }
    }

    readAtomInput();

    mInPtr = mAudioIn;
    mOutPtrs[0] = mAudioOut[0];
    // A host is obliged to connect every port, but a disconnected right channel would be a null
    // dereference inside the processor rather than a missing channel, so it is checked here and
    // the bus narrowed instead.
    mOutPtrs[1] = mAudioOut[1];
    mOutBus.numChannels = mAudioOut[1] ? 2 : 1;
    mData.numSamples = static_cast<int32>(nframes);
    mProcessor->process(mData);

    // The feedback block: the two meters. The processor publishes them through the output
    // parameter queue every block, and the ui:portNotification declarations in the TTL are what
    // carry them on to the editor.
    const int32 changed = mOutputChanges.getParameterCount();
    for (int32 q = 0; q < changed; ++q) {
        Vst::IParamValueQueue *queue = mOutputChanges.getParameterData(q);
        if (!queue)
            continue;
        const int32 points = queue->getPointCount();
        if (points <= 0)
            continue;
        int32 offset = 0;
        Vst::ParamValue value = 0.0;
        if (queue->getPoint(points - 1, offset, value) != kResultTrue)
            continue;
        const Vst::ParamID id = queue->getParameterId();
        for (int f = 0; f < kFeedbackCount; ++f) {
            if (kFeedbackIds[f] == id && mFeedback[f])
                *mFeedback[f] = static_cast<float>(value);
        }
    }

    writeAtomOutput();

    if (mLatency)
        *mLatency = static_cast<float>(mProcessor->getLatencySamples());
}

//------------------------------------------------------------------------
void NamixLv2::queueReply(Vst::IMessage *message)
{
    auto *concrete = static_cast<Message *>(message);
    if (!concrete)
        return;
    std::lock_guard<std::mutex> lock(mReplyMutex);
    lv2_atom_forge_set_buffer(&mReplyForge, mReplyScratch.data(),
                              static_cast<std::uint32_t>(mReplyScratch.size()));
    if (!forgeMessage(mReplyForge, mUris, message, concrete->attributes())) {
        fprintf(stderr, "NAMix: a reply to the editor did not fit and was dropped (%s)\n",
                concrete->id().c_str());
        return;
    }
    const auto *atom = reinterpret_cast<const LV2_Atom *>(mReplyScratch.data());
    if (!mToRun.push(atom, lv2_atom_total_size(atom)))
        fprintf(stderr, "NAMix: the editor is not draining its messages; one was dropped\n");
}

//------------------------------------------------------------------------
void NamixLv2::sendStateToUi()
{
    if (!mProcessor)
        return;
    MemoryStream stream;
    if (mProcessor->getState(&stream) != kResultOk)
        return;

    Message reply;
    reply.setMessageID(kMsgLv2State);
    reply.attributes().setBinary(kLv2StateAttr, stream.getData(),
                                 static_cast<uint32>(stream.getSize()));
    queueReply(&reply);
}

//------------------------------------------------------------------------
void NamixLv2::sendSlim(double normalized)
{
    if (!mProcessor)
        return;
    Message message;
    message.setMessageID(kMsgSetSlim);
    message.attributes().setFloat(kSlimAttr, normalized);
    mProcessor->notify(&message);
    mLastSlimSent = normalized;
    mSlimSentOnce = true;
}

//------------------------------------------------------------------------
void NamixLv2::dispatch(Message *message)
{
    if (!message)
        return;
    // The wrapper's own message never reaches the processor: it exists because LV2 has no
    // setComponentState, and the answer is one the wrapper can give on its own.
    if (message->id() == kMsgLv2RequestState) {
        sendStateToUi();
        return;
    }

    // Everything else is ordinary VST3 traffic and goes straight in. Mirror the paths on the way
    // past, because this half is the one that writes the LV2 state and the processor's own copies
    // are private to it.
    const std::string &id = message->id();
    if (id == kMsgLoadModel || id == kMsgLoadIr) {
        const int slot = (id == kMsgLoadModel) ? kPathSlotModel : kPathSlotIr;
        const void *data = nullptr;
        uint32 size = 0;
        if (message->getAttributes()->getBinary(kMsgPathAttr, data, size) == kResultOk && data)
            mPath[slot].assign(static_cast<const char *>(data), size);
        else
            mPath[slot].clear();
    } else if (id == kMsgSetSlim) {
        // The editor's own Slim message. Remember it, so the port write that accompanies it does
        // not stage a second rebuild of the same size.
        double v = 0.0;
        if (message->getAttributes()->getFloat(kSlimAttr, v) == kResultOk) {
            mLastSlimSent = v;
            mSlimSentOnce = true;
        }
    }

    mProcessor->notify(message);
}

//------------------------------------------------------------------------
void NamixLv2::messageLoop()
{
    // A poll rather than a condition variable, because the producer is the audio thread and
    // notifying a condition variable takes its mutex. Ten milliseconds is far below anything a
    // person notices for a file load and costs a hundred wakeups a second on a thread that does
    // nothing the rest of the time.
    using namespace std::chrono_literals;
    while (mMessageRunning.load(std::memory_order_acquire)) {
        // Slim, if the host moved its control port. See mPendingSlim for why this is not finished
        // by the parameter point run() already pushed.
        if (mSlimPending.exchange(false, std::memory_order_acquire)) {
            const double slim = mPendingSlim.load(std::memory_order_relaxed);
            if (!mSlimSentOnce || slim != mLastSlimSent)
                sendSlim(slim);
        }

        for (;;) {
            std::uint32_t size = 0;
            const std::uint8_t *bytes = mToMessage.peek(size);
            if (!bytes)
                break;
            const auto *atom = reinterpret_cast<const LV2_Atom *>(bytes);
            if (lv2_atom_forge_is_object_type(&mOutForge, atom->type)) {
                if (Message *message =
                        parseMessage(reinterpret_cast<const LV2_Atom_Object *>(atom), mUris)) {
                    dispatch(message);
                    message->release();
                }
            }
            mToMessage.drop();
        }
        std::this_thread::sleep_for(10ms);
    }
}

//------------------------------------------------------------------------
void NamixLv2::sendLoad(int slot, const std::string &path)
{
    if (slot < 0 || slot >= kPathSlotCount || !mProcessor)
        return;
    Message message;
    message.setMessageID(slot == kPathSlotModel ? kMsgLoadModel : kMsgLoadIr);
    message.attributes().setBinary(kMsgPathAttr, path.data(), static_cast<uint32>(path.size()));
    mProcessor->notify(&message);
    mPath[slot] = path;
}

//------------------------------------------------------------------------
LV2_State_Status NamixLv2::save(LV2_State_Store_Function store, LV2_State_Handle handle,
                                const LV2_Feature *const *features)
{
    if (!mProcessor || !store)
        return LV2_STATE_ERR_UNKNOWN;

    const LV2_State_Map_Path *mapPath = nullptr;
    const LV2_State_Free_Path *freePath = nullptr;
    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI)
            continue;
        if (std::strcmp(features[i]->URI, LV2_STATE__mapPath) == 0)
            mapPath = static_cast<const LV2_State_Map_Path *>(features[i]->data);
        else if (std::strcmp(features[i]->URI, LV2_STATE__freePath) == 0)
            freePath = static_cast<const LV2_State_Free_Path *>(features[i]->data);
    }

    MemoryStream stream;
    if (mProcessor->getState(&stream) != kResultOk)
        return LV2_STATE_ERR_UNKNOWN;
    store(handle, mStateBlob, stream.getData(), static_cast<size_t>(stream.getSize()), mAtomChunk,
          LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

    for (int slot = 0; slot < kPathSlotCount; ++slot) {
        if (mPath[slot].empty())
            continue;
        // The raw path is what the blob above contains; the abstract one is what survives the
        // session being moved. Restore compares them, which is what lets it tell a move from an
        // ordinary reopen without parsing a blob it does not own.
        store(handle, mPathRaw[slot], mPath[slot].c_str(), mPath[slot].size() + 1, mAtomString,
              LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
        if (!mapPath || !mapPath->abstract_path)
            continue;
        char *abstract = mapPath->abstract_path(mapPath->handle, mPath[slot].c_str());
        if (!abstract)
            continue;
        store(handle, mPathAbstract[slot], abstract, std::strlen(abstract) + 1, mAtomPath,
              LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
        if (freePath && freePath->free_path)
            freePath->free_path(freePath->handle, abstract);
        else
            std::free(abstract);
    }
    return LV2_STATE_SUCCESS;
}

//------------------------------------------------------------------------
LV2_State_Status NamixLv2::restore(LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
                                   const LV2_Feature *const *features)
{
    if (!mProcessor || !retrieve)
        return LV2_STATE_ERR_UNKNOWN;

    const LV2_State_Map_Path *mapPath = nullptr;
    const LV2_State_Free_Path *freePath = nullptr;
    for (int i = 0; features && features[i]; ++i) {
        if (!features[i]->URI)
            continue;
        if (std::strcmp(features[i]->URI, LV2_STATE__mapPath) == 0)
            mapPath = static_cast<const LV2_State_Map_Path *>(features[i]->data);
        else if (std::strcmp(features[i]->URI, LV2_STATE__freePath) == 0)
            freePath = static_cast<const LV2_State_Free_Path *>(features[i]->data);
    }

    size_t size = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    const void *blob = retrieve(handle, mStateBlob, &size, &type, &flags);
    if (blob && size > 0) {
        // The blob is untrusted input from a project file, which is what setState is already
        // written to assume: a malformed one is a clean refusal there, never a crash here.
        MemoryStream stream(const_cast<void *>(blob), static_cast<TSize>(size));
        mProcessor->setState(&stream);
    }

    for (int slot = 0; slot < kPathSlotCount; ++slot) {
        const void *abstract = retrieve(handle, mPathAbstract[slot], &size, &type, &flags);
        if (!abstract || size == 0)
            continue;
        const std::string abstractPath(static_cast<const char *>(abstract),
                                       strnlen(static_cast<const char *>(abstract), size));
        if (abstractPath.empty())
            continue;

        std::string absolute = abstractPath;
        if (mapPath && mapPath->absolute_path) {
            if (char *resolved = mapPath->absolute_path(mapPath->handle, abstractPath.c_str())) {
                absolute = resolved;
                if (freePath && freePath->free_path)
                    freePath->free_path(freePath->handle, resolved);
                else
                    std::free(resolved);
            }
        }

        std::string raw;
        if (const void *rawValue = retrieve(handle, mPathRaw[slot], &size, &type, &flags)) {
            if (size > 0)
                raw.assign(static_cast<const char *>(rawValue),
                           strnlen(static_cast<const char *>(rawValue), size));
        }
        mPath[slot] = raw.empty() ? absolute : raw;

        // Only when the file actually moved. The setState above has already loaded what the blob
        // named, and re-sending an identical path would read and rebuild the same network for
        // nothing.
        if (absolute == raw)
            continue;
        sendLoad(slot, absolute);
    }

    // An editor that is already open has a stale mirror of the paths now. Push the restored state
    // at it; an editor that is not open simply finds the ring empty when it opens and asks for
    // itself.
    sendStateToUi();
    return LV2_STATE_SUCCESS;
}

//------------------------------------------------------------------------
// The LV2 entry points.
//------------------------------------------------------------------------
LV2_Handle lv2Instantiate(const LV2_Descriptor *, double rate, const char *bundlePath,
                          const LV2_Feature *const *features)
{
    auto *self = new (std::nothrow) NamixLv2();
    if (!self)
        return nullptr;
    if (!self->instantiate(rate, bundlePath, features)) {
        delete self;
        return nullptr;
    }
    return static_cast<LV2_Handle>(self);
}

void lv2ConnectPort(LV2_Handle instance, std::uint32_t port, void *data)
{
    if (auto *self = static_cast<NamixLv2 *>(instance))
        self->connectPort(port, data);
}

void lv2Activate(LV2_Handle instance)
{
    if (auto *self = static_cast<NamixLv2 *>(instance))
        self->activate();
}

void lv2Run(LV2_Handle instance, std::uint32_t nframes)
{
    if (auto *self = static_cast<NamixLv2 *>(instance))
        self->run(nframes);
}

void lv2Deactivate(LV2_Handle instance)
{
    if (auto *self = static_cast<NamixLv2 *>(instance))
        self->deactivate();
}

void lv2Cleanup(LV2_Handle instance)
{
    delete static_cast<NamixLv2 *>(instance);
}

LV2_State_Status lv2Save(LV2_Handle instance, LV2_State_Store_Function store,
                         LV2_State_Handle handle, std::uint32_t, const LV2_Feature *const *features)
{
    auto *self = static_cast<NamixLv2 *>(instance);
    return self ? self->save(store, handle, features) : LV2_STATE_ERR_UNKNOWN;
}

LV2_State_Status lv2Restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
                            LV2_State_Handle handle, std::uint32_t,
                            const LV2_Feature *const *features)
{
    auto *self = static_cast<NamixLv2 *>(instance);
    return self ? self->restore(retrieve, handle, features) : LV2_STATE_ERR_UNKNOWN;
}

const LV2_State_Interface kStateInterface = {lv2Save, lv2Restore};

const void *lv2ExtensionData(const char *uri)
{
    if (uri && std::strcmp(uri, LV2_STATE__interface) == 0)
        return &kStateInterface;
    return nullptr;
}

const LV2_Descriptor kDescriptor = {
    kPluginUri, lv2Instantiate, lv2ConnectPort, lv2Activate,
    lv2Run,     lv2Deactivate,  lv2Cleanup,     lv2ExtensionData,
};

} // namespace

extern "C" {

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
}

} // extern "C"
