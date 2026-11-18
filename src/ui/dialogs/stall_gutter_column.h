#pragma once
#include "reorder_listing_editor.h"
#include "ui/common/gutter_column.h"
#include "ui/theme/theme_manager.h"
#include <QPaintEvent>
#include <QPainter>

namespace Kites
{
// Predicted stall cycles charged to each line, drawn as "+N" (same pattern as
// ExecutionCountGutterColumn in the profiler).
class StallGutterColumn : public GutterColumn
{
public:
    explicit StallGutterColumn(ReorderListingEditor *editor)
        : GutterColumn(editor), m_listingEditor(editor)
    {
    }

    QSize sizeHint() const override
    {
        return QSize(fontMetrics().horizontalAdvance(QStringLiteral("+99")) + 10, 0);
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
                auto it = m_listingEditor->m_stallsByLine.find(lineNumber);
                if (it != m_listingEditor->m_stallsByLine.end() && it->second > 0)
                {
                    painter.setPen(themeManager.getEditorErrorColor());
                    painter.drawText(0, top, width() - 5, fontMetrics().height(), Qt::AlignRight,
                                     QStringLiteral("+") + QString::number(it->second));
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
