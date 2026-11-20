#include "reorder_listing_editor.h"
#include "move_gutter_column.h"
#include "stall_gutter_column.h"
#include "ui/common/line_number_gutter_column.h"
#include "ui/common/syntax_highlighter.h"
#include "ui/theme/theme_manager.h"
#include <QEvent>
#include <QTextBlock>
#include <QTextEdit>

namespace Kites
{
ReorderListingEditor::ReorderListingEditor(QWidget *parent) : KitesEditor(parent)
{
    m_syntaxHighlighter = new SyntaxHighlighter(document());
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    addLeftGutterColumn(new LineNumberGutterColumn(this));
    m_stallColumn = new StallGutterColumn(this);
    addLeftGutterColumn(m_stallColumn);
    m_moveColumn = new MoveGutterColumn(this);
    addLeftGutterColumn(m_moveColumn);
    updateViewPortMargins();

    // connected after KitesEditor's own handlers, so these run after highlightCurrentLine()
    connect(this, &ReorderListingEditor::cursorPositionChanged, this,
            &ReorderListingEditor::applyLinkHighlights);
    connect(&ThemeManager::getInstance(), &ThemeManager::editorThemeChangedSignal, this,
            [this](const QString &)
            {
                applyLinkHighlights();
                m_stallColumn->update();
                m_moveColumn->update();
            });
}

QColor ReorderListingEditor::linkColor(int colorIndex)
{
    // 12 hues ordered so that consecutive indices (the usual neighbours in a listing) are far
    // apart on the colour wheel; after 12 links the sequence repeats and the gutter labels still
    // name the partner line.
    static const int kHues[] = {0, 210, 120, 30, 270, 165, 55, 320, 90, 240, 190, 300};
    QColor color = QColor::fromHsv(kHues[colorIndex % 12], 190, 235);
    color.setAlphaF(kLinkAlpha);
    return color;
}

void ReorderListingEditor::setListing(const QString &text,
                                      const std::map<unsigned, ListingLineLink> &links,
                                      const std::map<unsigned, int> &stallsByLine)
{
    m_links = links;
    m_stallsByLine = stallsByLine;
    setPlainText(text);
    updateViewPortMargins(); // the move column's width depends on the labels
    applyLinkHighlights();
    m_stallColumn->update();
    m_moveColumn->update();
}

void ReorderListingEditor::changeEvent(QEvent *event)
{
    KitesEditor::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
    {
        applyLinkHighlights();
    }
}

void ReorderListingEditor::applyLinkHighlights()
{
    QList<QTextEdit::ExtraSelection> selections;
    for (const auto &[lineNumber, link] : m_links)
    {
        QTextBlock block = document()->findBlockByNumber(static_cast<int>(lineNumber) - 1);
        if (!block.isValid())
        {
            continue;
        }
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(linkColor(link.colorIndex));
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.format.setToolTip(link.label);
        selection.cursor = QTextCursor(block);
        selections.append(selection);
    }
    setExtraSelections(selections);
}
} // namespace Kites
