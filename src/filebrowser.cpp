// FileBrowser implementation. See filebrowser.h.

#include "filebrowser.h"
#include "namgeometry.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace NAMix
{

namespace fs = std::filesystem;

namespace
{

// Full-window overlay, inset from the panel edges.
constexpr float kPanelX = 40.0f, kPanelY = 34.0f;
constexpr float kPanelW = geo::kWinW - 2 * kPanelX;
constexpr float kPanelH = geo::kWinH - 2 * kPanelY;
constexpr float kHeaderH = 52.0f;
constexpr float kFooterH = 10.0f;
constexpr float kRowH = 24.0f;
constexpr float kRowIndent = 14.0f;

constexpr float kListTop = kPanelY + kHeaderH;
constexpr float kListH = kPanelH - kHeaderH - kFooterH;

// Close box, top-right of the overlay.
constexpr Rect kCloseBox(kPanelX + kPanelW - 30.0f, kPanelY + 14.0f, 16.0f, 16.0f);

bool endsWithExtension(const std::string &name, const std::string &ext)
{
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size())
        return false;
    return strcasecmp(name.c_str() + dot + 1, ext.c_str()) == 0;
}

} // namespace

//------------------------------------------------------------------------
void FileBrowser::open(const std::string &startPath, const std::string &extension,
                       const std::string &title)
{
    mExt = extension;
    mTitle = title;
    mChosen.clear();
    mScroll = 0;

    std::error_code ec;
    fs::path start(startPath);
    if (startPath.empty() || !fs::exists(start, ec)) {
        const char *home = std::getenv("HOME");
        start = home ? fs::path(home) : fs::path("/");
    } else if (!fs::is_directory(start, ec)) {
        start = start.parent_path();
    }

    mDir = start.string();
    listDirectory();
    mOpen = true;
}

void FileBrowser::close()
{
    mOpen = false;
    mEntries.clear();
}

//------------------------------------------------------------------------
void FileBrowser::listDirectory()
{
    mEntries.clear();
    mScroll = 0;

    std::vector<Entry> dirs, files;
    std::error_code ec;

    // A directory we cannot read (permissions, a stale path) must not throw
    // out of the run loop: the iterator is constructed with an error_code and
    // simply yields nothing, leaving a browsable ".." row.
    fs::directory_iterator it(mDir, fs::directory_options::skip_permission_denied, ec);
    if (!ec) {
        for (const fs::directory_entry &entry : it) {
            const std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.')
                continue; // hidden files, as most pickers default to

            std::error_code dirEc;
            if (entry.is_directory(dirEc))
                dirs.push_back({name, true});
            else if (endsWithExtension(name, mExt))
                files.push_back({name, false});
        }
    }

    auto byName = [](const Entry &a, const Entry &b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    if (fs::path(mDir).has_parent_path() && mDir != "/")
        mEntries.push_back({"..", true});
    mEntries.insert(mEntries.end(), dirs.begin(), dirs.end());
    mEntries.insert(mEntries.end(), files.begin(), files.end());
}

//------------------------------------------------------------------------
int FileBrowser::visibleRows() const
{
    return static_cast<int>(kListH / kRowH);
}

int FileBrowser::rowAt(float x, float y) const
{
    if (x < kPanelX || x >= kPanelX + kPanelW || y < kListTop || y >= kListTop + kListH)
        return -1;
    const int row = static_cast<int>((y - kListTop) / kRowH) + mScroll;
    return (row >= 0 && row < static_cast<int>(mEntries.size())) ? row : -1;
}

//------------------------------------------------------------------------
void FileBrowser::draw(Canvas &c)
{
    if (!mOpen)
        return;

    // Dim the panel behind, then the overlay card.
    c.setColor(0x000000, 170);
    c.fillRect(c.bounds());

    const Rect panel(kPanelX, kPanelY, kPanelW, kPanelH);
    c.setColor(0x161318);
    c.fillRoundRect(panel, 10.0f);
    c.setColor(0x2E2B34);
    c.setPenSize(1.0f);
    c.strokeRoundRect(panel, 10.0f);

    // --- header: title, current directory, close ---
    c.setFont(Font::Title);
    c.setFontSize(14);
    c.setColor(geo::kTextColor);
    c.drawString(mTitle.c_str(), kPanelX + kRowIndent, kPanelY + 26.0f);
    c.setFont(Font::Body);

    c.setFontSize(11);
    c.setColor(geo::kDimColor);
    {
        const std::string dir = c.clipToWidth(mDir, kPanelW - 2 * kRowIndent - 30.0f);
        c.drawString(dir.c_str(), kPanelX + kRowIndent, kPanelY + 44.0f);
    }

    c.setColor(geo::kDimColor);
    c.setPenSize(2.0f);
    c.strokeLine(kCloseBox.left(), kCloseBox.top(), kCloseBox.right(), kCloseBox.bottom());
    c.strokeLine(kCloseBox.left(), kCloseBox.bottom(), kCloseBox.right(), kCloseBox.top());
    c.setPenSize(1.0f);

    // --- list ---
    c.pushClip(Rect(kPanelX, kListTop, kPanelW, kListH));
    c.setFontSize(12);
    const int rows = visibleRows();
    for (int i = 0; i < rows; ++i) {
        const int index = mScroll + i;
        if (index >= static_cast<int>(mEntries.size()))
            break;
        const Entry &e = mEntries[static_cast<size_t>(index)];
        const float y = kListTop + i * kRowH;

        if (index % 2)
            c.setColor(0x1C1920);
        else
            c.setColor(0x191620);
        c.fillRect(Rect(kPanelX + 1.0f, y, kPanelW - 2.0f, kRowH));

        c.setColor(e.isDir ? geo::kAzure : geo::kTextColor);
        const std::string label = e.isDir ? (e.name + "/") : e.name;
        c.drawString(c.clipToWidth(label, kPanelW - 2 * kRowIndent).c_str(), kPanelX + kRowIndent,
                     y + kRowH - 7.0f);
    }
    if (mEntries.empty()) {
        c.setColor(geo::kDimColor);
        const std::string msg = "No ." + mExt + " files here";
        c.drawString(msg.c_str(), kPanelX + kRowIndent, kListTop + kRowH);
    }
    c.popClip();

    // --- scroll indicator, only when the list overflows ---
    if (static_cast<int>(mEntries.size()) > rows) {
        const float trackX = kPanelX + kPanelW - 6.0f;
        const float frac = static_cast<float>(rows) / static_cast<float>(mEntries.size());
        const float thumbH = std::max(kListH * frac, 20.0f);
        const float maxScroll = static_cast<float>(mEntries.size() - rows);
        const float pos = maxScroll > 0 ? static_cast<float>(mScroll) / maxScroll : 0.0f;
        c.setColor(0x2E2B34);
        c.fillRect(Rect(trackX, kListTop, 3.0f, kListH));
        c.setColor(geo::kAzure);
        c.fillRect(Rect(trackX, kListTop + pos * (kListH - thumbH), 3.0f, thumbH));
    }
}

//------------------------------------------------------------------------
FileBrowser::Result FileBrowser::handleClick(float x, float y)
{
    if (!mOpen)
        return Result::None;

    // Clicking outside the card, or on the close cross, dismisses.
    const Rect panel(kPanelX, kPanelY, kPanelW, kPanelH);
    if (!panel.contains(x, y) || kCloseBox.inset(-6.0f).contains(x, y)) {
        close();
        return Result::Cancelled;
    }

    const int row = rowAt(x, y);
    if (row < 0)
        return Result::Handled; // inside the card but on no row: swallow it

    const Entry &e = mEntries[static_cast<size_t>(row)];
    if (e.isDir) {
        std::error_code ec;
        const fs::path next =
            (e.name == "..") ? fs::path(mDir).parent_path() : fs::path(mDir) / e.name;
        if (fs::is_directory(next, ec)) {
            mDir = next.string();
            listDirectory();
        }
        return Result::Handled;
    }

    mChosen = (fs::path(mDir) / e.name).string();
    close();
    return Result::Chosen;
}

//------------------------------------------------------------------------
bool FileBrowser::handleWheel(int delta)
{
    if (!mOpen)
        return false;
    const int rows = visibleRows();
    const int maxScroll = std::max(0, static_cast<int>(mEntries.size()) - rows);
    const int next = std::min(std::max(mScroll - delta * 3, 0), maxScroll);
    if (next == mScroll)
        return false;
    mScroll = next;
    return true;
}

} // namespace NAMix
