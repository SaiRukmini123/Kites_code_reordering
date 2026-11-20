#pragma once
#include "reorder_listing_editor.h"
#include "ui/common/gutter_column.h"
#include "ui/theme/theme_manager.h"
#include <QPaintEvent>
#include <QPainter>

namespace Kites
{
// For each moved line: its link colour as a cell background and the partner line ("-> 12" on the
// original side, "<- 7" on the suggested side), so a line can be traced across both listings.
class MoveGutterColumn : public GutterColumn
{
public:
    explicit MoveGutterColumn(ReorderListingEditor *editor)
        : GutterColumn(editor), m_listingEditor(editor)
    {
    }

    QSize sizeHint() const override
    {
        int widest = fontMetrics().horizontalAdvance(QStringLiteral("<- 999"));
        for (const auto &[line, link] : m_listingEditor->m_links)
        {
            widest = std::max(widest, fontMetrics().horizontalAdvance(link.label));
        }
        return QSize(widest + 12, 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        const auto &themeManager = ThemeManager::getInstance();
        painter.fillRect(event->rect(), themeManager.getEditorBackgroundColor());

        QTextBlock block = firstVisibleBlock();
        int top = blockTop(block);
        int bottom = top + blockHeight(block);
        while (block.isValid() && top <= event->rect().bottom())
        {
            if (block.isVisible() && bottom >= event->rect().top())
            {
                const unsigned lineNumber = static_cast<unsigned>(block.blockNumber() + 1);
                auto it = m_listingEditor->m_links.find(lineNumber);
                if (it != m_listingEditor->m_links.end())
                {
                    // translucent, over the background filled above: same colour as the line
                    painter.fillRect(QRect(0, top, width(), bottom - top),
                                     ReorderListingEditor::linkColor(it->second.colorIndex));
                    painter.setPen(themeManager.getEditorForegroundColor());
                    painter.drawText(4, top, width() - 8, fontMetrics().height(), Qt::AlignLeft,
                                     it->second.label);
                }
            }
            block = block.next();
            top = bottom;
            bottom = top + blockHeight(block);
        }
    }

private:
    ReorderListingEditor *m_listingEditor;
};
} // namespace Kites
