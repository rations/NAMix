// NAMix native editor (IPlugView / kPlatformTypeX11EmbedWindowID).
//
// NamEditorView is the IPlugView the controller returns from createView(). It
// derives from X11PlugView, which owns the embedded X window, the run-loop
// registrations and the double buffer; everything below is the panel itself —
// the amp-head layout of the original Neural Amp Modeler, painted through
// Canvas.
//
// The controller pushes value changes in through ParamChanged() and model
// capabilities through ModelCapsChanged(); user edits go out through
// EditController::beginEdit/performEdit/endEdit, which reach the host's
// IComponentHandler. All of it runs on the host's run-loop thread, the same
// thread the controller is called on, so none of it needs locking.

#pragma once

#include "filebrowser.h"
#include "gfx/canvas.h"
#include "gfx/fontstack.h"
#include "gfx/image.h"
#include "gfx/svg.h"
#include "namgeometry.h"
#include "platform/x11plugview.h"

#include <string>

namespace NAMix
{

class NamController;

//------------------------------------------------------------------------
class NamEditorView : public Steinberg::X11PlugView
{
public:
    explicit NamEditorView(NamController *controller);

    // Called by the controller whenever a parameter value changes
    // (automation, generic UI, state load, metering).
    void ParamChanged(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);

    // Called by the controller when the processor reports model capabilities,
    // so the editor can disable the model-gated controls.
    void ModelCapsChanged(bool slimmable, bool hasInputLevel, bool hasOutputLevel);

protected:
    //---from X11PlugView-------------
    void onAttached() SMTG_OVERRIDE;
    void onDraw(cairo_t *cr) SMTG_OVERRIDE;
    void onMouseDown(int x, int y, int button) SMTG_OVERRIDE;
    void onMouseUp(int x, int y, int button) SMTG_OVERRIDE;
    void onMouseMove(int x, int y) SMTG_OVERRIDE;
    void onMouseWheel(int x, int y, int delta) SMTG_OVERRIDE;
    void onTick() SMTG_OVERRIDE;

private:
    //--- drawing ---------------------------------------------------------
    void compose(Canvas &c);
    void drawTitle(Canvas &c);
    void drawKnobBody(Canvas &c, double norm, int cx, int cy, int r);
    void drawKnob(Canvas &c, const geo::KnobSpec &k);
    void drawToggle(Canvas &c, const geo::ToggleSpec &t);
    void drawFileRow(Canvas &c, const geo::FileRow &row, bool model);
    void drawMeter(Canvas &c, const geo::MeterRect &m, float level, const char *label);
    void drawGear(Canvas &c);
    void drawSlimIcon(Canvas &c);
    void dimPanel(Canvas &c);
    void drawSettings(Canvas &c);
    void drawSlimOverlay(Canvas &c);

    //--- interaction -----------------------------------------------------
    void editParam(Steinberg::Vst::ParamID id, double norm);
    void nudgeParam(Steinberg::Vst::ParamID id, double delta);
    void startDrag(Steinberg::Vst::ParamID id, float y);
    void handleSettingsClick(float x, float y);
    bool handleFileRowClick(const geo::FileRow &row, float x, float y, bool model);
    void cycleFile(bool model, int dir);
    void openBrowser(bool model);
    void openUrl(const char *url);

    //--- helpers ---------------------------------------------------------
    bool filePath(bool model, std::string &out) const;
    double paramValue(Steinberg::Vst::ParamID id) const;
    std::string paramText(Steinberg::Vst::ParamID id) const;
    static bool hitCircle(float x, float y, float cx, float cy, float r);

    // Settings-overlay control rects; drawSettings() lays them out the same way.
    static Rect settingsCloseBox();
    static Rect calValueRect();
    static Rect calibrateToggleRect();
    static Rect outputModeRow(int i);

    NamController *mController = nullptr;

    FontStack mFonts;
    ImageCache mImages;
    SvgCache mSvgs;
    bool mResourcesLoaded = false;

    FileBrowser mBrowser;
    bool mBrowserForModel = true;

    Steinberg::Vst::ParamID mDragParam = 0; // 0 = no active drag
    float mDragStartY = 0;
    double mDragStartNorm = 0;
    float mMouseX = 0, mMouseY = 0;

    float mInDisp = 0, mOutDisp = 0;

    bool mSettingsOpen = false;
    bool mSlimOpen = false;
    bool mSlimmable = false;
    bool mHasInputLevel = false;
    bool mHasOutputLevel = false;
};

} // namespace NAMix
