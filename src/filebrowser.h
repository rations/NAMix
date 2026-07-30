// FileBrowser — the modal .nam / .wav picker drawn inside the editor.
//
// The original plug-in browses files in-panel too (its NAMFileBrowserControl),
// and doing the same here has a hard practical benefit: nothing links a UI
// toolkit. A GTK or Qt dialog inside a plug-in has to share the host's process
// with whatever toolkit the host already initialised, and portal-based pickers
// drag in a D-Bus dependency. Painting the list ourselves through Canvas has
// neither problem, and it looks like the rest of the panel.
//
// Behaviour: directories first then matching files, both alphabetical; a ".."
// row to go up; the mouse wheel scrolls; clicking a directory descends, and
// clicking a file chooses it and closes. Everything runs on the run-loop
// thread, and directory listing is cheap enough to do inline on open.

#pragma once

#include "gfx/canvas.h"

#include <string>
#include <vector>

namespace NAMix
{

//------------------------------------------------------------------------
class FileBrowser
{
public:
    // What a click did, so the panel knows whether to repaint or act.
    enum class Result {
        None,     // click was outside the browser, or on nothing
        Handled,  // consumed; repaint
        Chosen,   // a file was picked; read chosenPath(), browser is closed
        Cancelled // dismissed without choosing
    };

    bool isOpen() const
    {
        return mOpen;
    }
    const std::string &chosenPath() const
    {
        return mChosen;
    }

    // `extension` is without the dot ("nam", "wav"). `startPath` may be a file
    // (its directory is used), a directory, or empty (falls back to $HOME).
    void open(const std::string &startPath, const std::string &extension, const std::string &title);
    void close();

    void draw(Canvas &c);
    Result handleClick(float x, float y);
    bool handleWheel(int delta);

private:
    struct Entry {
        std::string name;
        bool isDir = false;
    };

    void listDirectory();
    int rowAt(float x, float y) const; // index into mEntries, or -1
    int visibleRows() const;

    bool mOpen = false;
    std::string mDir;
    std::string mExt;
    std::string mTitle;
    std::string mChosen;
    std::vector<Entry> mEntries;
    int mScroll = 0;
};

} // namespace NAMix
