#pragma once
#include "common/assembled_program.h"
#include "processor/processor_types.h"
#include "reorder/reorder_report.h"
#include <QDialog>
#include <QString>
#include <optional>
#include <string>

class QCheckBox;
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
// shows original and suggested code side by side, verifies both on the VM, and can apply the
// suggestion to the editor. Built in code (no .ui file).
//
// Verify runs on the GUI thread: constructing a processor (in a GUI build) creates its circuit
// scene, which holds widgets, and widgets may only be created on the GUI thread (on macOS a
// worker thread creating one aborts the app). The dialog shows "Verifying..." and blocks input
// while the run is bounded by the Cycle cap.
//
// "Also try loop unrolling" (off by default) runs unrollAndReorder() with the Seed and Cycle cap
// when the suggestion is made, on the GUI thread for the same reason. When an unrolled program
// passes the library's VM check, the suggested side shows it (inserted lines highlighted instead
// of moves), Verify compares it with the original, and Apply writes it to the editor.
class ReorderDialog : public QDialog
{
    Q_OBJECT

public:
    ReorderDialog(QWidget *parent, const QString &editorText, ProcessorType currentVm);
    ~ReorderDialog() override = default;

    // True when the editor text did not assemble; the details are in
    // globals::errors_dump_file_path (the same file Run uses), for marking the editor.
    bool assemblyFailed() const { return m_assemblyFailed; }

signals:
    void applyRequested(const QString &reorderedText);

private slots:
    void analyze();
    void verify();

private:
    void buildUi(ProcessorType currentVm);
    void assembleEditorText();
    void showAssemblyErrors(const QString &what);
    void setControlsEnabled(bool enabled);
    reorder::PipelineKind selectedKind() const;
    void showVerifyResult(const reorder_report::ReorderOutcome &outcome);
    // Runs the unrolling for the current pipeline, Seed and Cycle cap. On success fills the
    // suggested listing and the m_unrolled* members and returns true; otherwise leaves the
    // reorder-only listing in place. Either way the unrolling summary explains the result.
    bool tryLoopUnrolling(reorder::PipelineKind kind, QString &summary);

    std::string m_editorText;   // exactly as in the editor
    std::string m_expandedText; // after custom pseudo-instruction expansion (what gets assembled)
    bool m_assemblyFailed = false;
    std::optional<AssembledProgram> m_program;
    std::optional<reorder_report::ReorderAnalysis> m_analysis;
    std::string m_unrolledText;                  // unrolled + reordered source, when kept
    std::optional<AssembledProgram> m_unrolledProgram; // m_unrolledText assembled
    bool m_canApply = false;                     // Apply state after the last analysis
    QString m_applyNote;

    QComboBox *m_vmCombo = nullptr;
    QLabel *m_vmNote = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_suggestedHeading = nullptr;
    ReorderListingEditor *m_originalView = nullptr;
    ReorderListingEditor *m_suggestedView = nullptr;
    QTableWidget *m_blockTable = nullptr;
    QLabel *m_legendLabel = nullptr;
    QCheckBox *m_unrollCheck = nullptr;
    QLabel *m_unrollLabel = nullptr;
    QLineEdit *m_seedEdit = nullptr;
    QSpinBox *m_capSpin = nullptr;
    QPushButton *m_verifyButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QLabel *m_verifyLabel = nullptr;
    bool m_verifying = false;
};
} // namespace Kites
