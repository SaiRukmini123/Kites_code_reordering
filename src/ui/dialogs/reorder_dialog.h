#pragma once
#include "common/assembled_program.h"
#include "processor/processor_types.h"
#include "reorder/reorder_report.h"
#include <QDialog>
#include <QFutureWatcher>
#include <QString>
#include <optional>
#include <string>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace Kites
{
class ReorderListingEditor;

// "Suggest reordering": schedules the editor's program for a hazard pipeline (A's scheduler),
// shows original and suggested code side by side, verifies both on the VM in a background
// thread, and can apply the suggestion to the editor. Built in code (no .ui file).
class ReorderDialog : public QDialog
{
    Q_OBJECT

public:
    ReorderDialog(QWidget *parent, const QString &editorText, ProcessorType currentVm);
    ~ReorderDialog() override;

    // True when the editor text did not assemble; the details are in
    // globals::errors_dump_file_path (the same file Run uses), for marking the editor.
    bool assemblyFailed() const { return m_assemblyFailed; }

signals:
    void applyRequested(const QString &reorderedText);

private slots:
    void analyze();
    void verify();
    void verifyFinished();

private:
    void buildUi(ProcessorType currentVm);
    void assembleEditorText();
    void showAssemblyErrors(const QString &what);
    void setControlsEnabled(bool enabled);
    reorder::PipelineKind selectedKind() const;

    std::string m_editorText;   // exactly as in the editor
    std::string m_expandedText; // after custom pseudo-instruction expansion (what gets assembled)
    bool m_assemblyFailed = false;
    std::optional<AssembledProgram> m_program;
    std::optional<reorder_report::ReorderAnalysis> m_analysis;

    QComboBox *m_vmCombo = nullptr;
    QLabel *m_vmNote = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_suggestedHeading = nullptr;
    ReorderListingEditor *m_originalView = nullptr;
    ReorderListingEditor *m_suggestedView = nullptr;
    QTableWidget *m_blockTable = nullptr;
    QLineEdit *m_seedEdit = nullptr;
    QSpinBox *m_capSpin = nullptr;
    QPushButton *m_verifyButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QLabel *m_verifyLabel = nullptr;
    QFutureWatcher<reorder_report::ReorderOutcome> *m_verifyWatcher = nullptr;
};
} // namespace Kites
