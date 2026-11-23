#pragma once
#include "ui/common/kites_editor.h"
#include <QColor>
#include <QString>
#include <map>

namespace Kites
{
class MoveGutterColumn;
class StallGutterColumn;
class SyntaxHighlighter;

// Ties a moved line in one listing to its partner in the other: both lines get the same colour,
// and the gutter shows the partner's line number (for example "-> 12" or "<- 7").
struct ListingLineLink
{
    int colorIndex = 0;
    QString label;
};

// Read-only assembly listing for the reorder dialog: line numbers, predicted stall cycles per line
// ("+N"), and every moved line in its own colour with the line it moved to or came from. Same
// structure as ProfilerEditor (a KitesEditor with gutter columns).
class ReorderListingEditor : public KitesEditor
{
    Q_OBJECT
    friend class StallGutterColumn;
    friend class MoveGutterColumn;

public:
    explicit ReorderListingEditor(QWidget *parent = nullptr);

    // Line numbers are 1-based.
    void setListing(const QString &text, const std::map<unsigned, ListingLineLink> &links,
                    const std::map<unsigned, int> &stallsByLine);

    // Opacity of every moved-line colour (line highlight and gutter cell), drawn over the editor
    // background. At 0.35 the code text keeps a contrast of at least about 3:1 on all built-in
    // themes (about 8:1 on kites-light) and neighbouring colours stay clearly apart; no theme
    // needs a higher value.
    static constexpr double kLinkAlpha = 0.35;

    // The colour for a link: a translucent hue (alpha kLinkAlpha), so it reads in light and dark
    // themes. Neighbouring indices get clearly different hues.
    static QColor linkColor(int colorIndex);

    // Colour index for the lines added by loop unrolling: a light, more transparent green, so a
    // run of many inserted lines stays easy to read.
    static constexpr int kInsertedColorIndex = -1;
    static constexpr double kInsertedAlpha = 0.18;

protected:
    void changeEvent(QEvent *event) override;

private:
    // KitesEditor::highlightCurrentLine() replaces all extra selections on cursor moves and theme
    // changes, so the line colours are re-applied after it.
    void applyLinkHighlights();

    SyntaxHighlighter *m_syntaxHighlighter = nullptr;
    StallGutterColumn *m_stallColumn = nullptr;
    MoveGutterColumn *m_moveColumn = nullptr;
    std::map<unsigned, ListingLineLink> m_links;
    std::map<unsigned, int> m_stallsByLine;
};
} // namespace Kites
