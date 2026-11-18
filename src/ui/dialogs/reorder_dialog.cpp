#include "reorder_dialog.h"
#include "assembler/assembler.h"
#include "common/globals.h"
#include "custom_pseudo_manager/custom_pseudo_manager.h"
#include "reorder_listing_editor.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace Kites
{
namespace
{
using reorder::PipelineKind;

const char *processorTypeName(ProcessorType type)
{
    switch (type)
    {
    case ProcessorType::RVSS: return "rvss";
    case ProcessorType::RV5Stage_NH_NF: return "rv5-nh-nf";
    case ProcessorType::RV5Stage_H_NF: return "rv5-h-nf";
    case ProcessorType::RV5Stage_NH_F: return "rv5-nh-f";
    case ProcessorType::RV5Stage_H_F: return "rv5-h-f";
    default: return "unknown";
    }
}

QString cpiText(unsigned cycles, unsigned retired)
{
    return QString::fromStdString(reorder_report::formatCpi(cycles, retired));
}

// One colour per moved source line, shared by its line in the original listing and the line(s)
// it ended up on in the suggested listing, plus "-> N" / "<- N" labels naming the partner lines.
// The words of one source line (a split `la`, say) share that line's colour.
void buildLineLinks(const AssembledProgram &original, const reorder_report::ReorderAnalysis &a,
                    std::map<unsigned, ListingLineLink> &originalLinks,
                    std::map<unsigned, ListingLineLink> &suggestedLinks)
{
    std::map<unsigned, std::set<unsigned>> targetsOf; // original line -> suggested lines
    std::map<unsigned, std::set<unsigned>> sourcesOf; // suggested line -> original lines
    for (const reorder::Move &move : a.report.moves)
    {
        const unsigned fromLine = reorder::sourceLineOf(original, move.originalIndex);
        const unsigned toLine = reorder::sourceLineOf(a.reorderedProgram, move.newIndex);
        if (fromLine != 0 && toLine != 0)
        {
            targetsOf[fromLine].insert(toLine);
            sourcesOf[toLine].insert(fromLine);
        }
    }

    auto joinLines = [](const QString &arrow, const std::set<unsigned> &lines)
    {
        QStringList numbers;
        for (unsigned line : lines)
        {
            numbers << QString::number(line);
        }
        return arrow + numbers.join(", ");
    };
    std::map<unsigned, int> colorOfLine; // original line -> colour, in source order
    for (const auto &[fromLine, toLines] : targetsOf)
    {
        const int colorIndex = static_cast<int>(colorOfLine.size());
        colorOfLine[fromLine] = colorIndex;
        originalLinks[fromLine] = {colorIndex, joinLines(QString::fromUtf8("\u2192 "), toLines)};
    }
    for (const auto &[toLine, fromLines] : sourcesOf)
    {
        // a suggested line holds one instruction, so one original line
        suggestedLinks[toLine] = {colorOfLine[*fromLines.begin()],
                                  joinLines(QString::fromUtf8("\u2190 "), fromLines)};
    }
}
} // namespace

ReorderDialog::ReorderDialog(QWidget *parent, const QString &editorText, ProcessorType currentVm)
    : QDialog(parent), m_editorText(editorText.toStdString())
{
    setWindowTitle("Suggest Reordering");
    resize(1100, 720);
    buildUi(currentVm);

    m_verifyWatcher = new QFutureWatcher<reorder_report::ReorderOutcome>(this);
    connect(m_verifyWatcher, &QFutureWatcher<reorder_report::ReorderOutcome>::finished, this,
            &ReorderDialog::verifyFinished);

    assembleEditorText();
    if (!m_assemblyFailed)
    {
        analyze();
    }
    connect(m_vmCombo, &QComboBox::currentIndexChanged, this, &ReorderDialog::analyze);
}

ReorderDialog::~ReorderDialog()
{
    // runProgram silences the process-wide std::cout while it runs, so a verification must not
    // outlive the dialog (the main VM could start printing). It is bounded by the cycle cap.
    if (m_verifyWatcher && m_verifyWatcher->isRunning())
    {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_verifyWatcher->waitForFinished();
        QApplication::restoreOverrideCursor();
    }
}

void ReorderDialog::buildUi(ProcessorType currentVm)
{
    auto *layout = new QVBoxLayout(this);

    // Pipeline choice: the cost model exists only for the two hazard pipelines.
    auto *vmRow = new QHBoxLayout();
    vmRow->addWidget(new QLabel("Pipeline:", this));
    m_vmCombo = new QComboBox(this);
    m_vmCombo->addItem("rv5-h-f (hazard unit, forwarding)", static_cast<int>(PipelineKind::HF));
    m_vmCombo->addItem("rv5-h-nf (hazard unit, no forwarding)", static_cast<int>(PipelineKind::HNF));
    m_vmNote = new QLabel(this);
    m_vmNote->setWordWrap(true);
    if (currentVm == ProcessorType::RV5Stage_H_NF)
    {
        m_vmCombo->setCurrentIndex(1);
        m_vmNote->setText("Using the processor selected in the app.");
    }
    else if (currentVm == ProcessorType::RV5Stage_H_F)
    {
        m_vmNote->setText("Using the processor selected in the app.");
    }
    else
    {
        m_vmNote->setText(QString("The app's processor (%1) has no hazard unit to model; showing "
                                  "rv5-h-f. Pick a pipeline here.")
                              .arg(QString::fromLatin1(processorTypeName(currentVm))));
    }
    vmRow->addWidget(m_vmCombo);
    vmRow->addWidget(m_vmNote, 1);
    layout->addLayout(vmRow);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_summaryLabel);

    // Side-by-side listings.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    auto makePane = [splitter](const QString &title, ReorderListingEditor *&view, QLabel *&heading)
    {
        auto *pane = new QWidget(splitter);
        auto *paneLayout = new QVBoxLayout(pane);
        paneLayout->setContentsMargins(0, 0, 0, 0);
        heading = new QLabel(title, pane);
        view = new ReorderListingEditor(pane);
        paneLayout->addWidget(heading);
        paneLayout->addWidget(view, 1);
        splitter->addWidget(pane);
    };
    QLabel *originalHeading = nullptr;
    makePane("Original", m_originalView, originalHeading);
    makePane("Suggested", m_suggestedView, m_suggestedHeading);
    layout->addWidget(splitter, 1);
    auto *legend = new QLabel(QString::fromUtf8(
                                  "Each moved line has its own colour, the same on both sides; "
                                  "\u2192 N in the original gutter is the line it moved to, "
                                  "\u2190 N in the suggested gutter the line it came from.  "
                                  "+N: predicted stall cycles before that line's instruction."),
                              this);
    legend->setWordWrap(true);
    layout->addWidget(legend);

    // Predicted stalls per block.
    m_blockTable = new QTableWidget(0, 3, this);
    m_blockTable->setHorizontalHeaderLabels({"Block (source lines)", "Predicted stalls before",
                                             "Predicted stalls after"});
    m_blockTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_blockTable->verticalHeader()->setVisible(false);
    m_blockTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_blockTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_blockTable->setMaximumHeight(150);
    layout->addWidget(m_blockTable);

    // Verification on the VM.
    auto *verifyRow = new QHBoxLayout();
    verifyRow->addWidget(new QLabel("Seed:", this));
    m_seedEdit = new QLineEdit(this);
    m_seedEdit->setPlaceholderText("optional, e.g. x10=0x10000000,mem:0x10000000=7");
    verifyRow->addWidget(m_seedEdit, 1);
    verifyRow->addWidget(new QLabel("Cycle cap:", this));
    m_capSpin = new QSpinBox(this);
    m_capSpin->setRange(100, 100000000);
    m_capSpin->setValue(static_cast<int>(reorder_report::kDefaultCycleCap));
    m_capSpin->setSingleStep(100000);
    verifyRow->addWidget(m_capSpin);
    m_verifyButton = new QPushButton("Verify", this);
    m_verifyButton->setToolTip("Run original and suggested code on the VM and compare final state");
    verifyRow->addWidget(m_verifyButton);
    layout->addLayout(verifyRow);

    m_verifyLabel = new QLabel(this);
    m_verifyLabel->setWordWrap(true);
    m_verifyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_verifyLabel);

    auto *buttons = new QDialogButtonBox(this);
    m_applyButton = buttons->addButton("Apply to Editor", QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(m_verifyButton, &QPushButton::clicked, this, &ReorderDialog::verify);
    connect(m_applyButton, &QPushButton::clicked, this,
            [this]()
            {
                if (m_analysis)
                {
                    emit applyRequested(QString::fromStdString(m_analysis->reorderedSource));
                    accept();
                }
            });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ReorderDialog::assembleEditorText()
{
    // Same steps as MainWindow::tryParseAndLoadProgram(), but into a file of our own so the
    // editor's Run state is not touched.
    try
    {
        m_expandedText = customPseudoManager::expandPseudoInstruction(m_editorText);
        const std::filesystem::path inputPath = globals::vm_state_directory / "reorder_input.s";
        {
            std::ofstream out(inputPath);
            out << m_expandedText;
        }
        m_program = assemble(inputPath.string());
    }
    catch (const std::exception &e)
    {
        m_assemblyFailed = true;
        showAssemblyErrors(QString::fromStdString(e.what()));
        return;
    }
    if (m_program->text_buffer.empty())
    {
        m_program.reset();
        m_summaryLabel->setText("Nothing to reorder: the program has no instructions.");
        m_originalView->setListing(QString::fromStdString(m_editorText), {}, {});
        setControlsEnabled(false);
    }
}

void ReorderDialog::showAssemblyErrors(const QString &what)
{
    QString text = "<b>The editor's program does not assemble.</b> Fix the errors (they are also "
                   "marked in the editor) and open this dialog again.";
    QFile file(QString::fromStdString(globals::errors_dump_file_path.string()));
    if (file.open(QIODevice::ReadOnly))
    {
        const QJsonArray errors =
            QJsonDocument::fromJson(file.readAll()).object().value("errors").toArray();
        for (const QJsonValue &value : errors)
        {
            const QJsonObject error = value.toObject();
            text += QString("<br>line %1: %2")
                        .arg(error.value("line").toInt())
                        .arg(error.value("message").toString().toHtmlEscaped());
        }
        if (errors.isEmpty())
        {
            text += "<br>" + what.toHtmlEscaped();
        }
    }
    else
    {
        text += "<br>" + what.toHtmlEscaped();
    }
    m_summaryLabel->setText(text);
    m_originalView->setListing(QString::fromStdString(m_editorText), {}, {});
    setControlsEnabled(false);
}

void ReorderDialog::setControlsEnabled(bool enabled)
{
    m_vmCombo->setEnabled(enabled);
    m_verifyButton->setEnabled(enabled);
    m_seedEdit->setEnabled(enabled);
    m_capSpin->setEnabled(enabled);
    m_applyButton->setEnabled(enabled);
}

PipelineKind ReorderDialog::selectedKind() const
{
    return static_cast<PipelineKind>(m_vmCombo->currentData().toInt());
}

void ReorderDialog::analyze()
{
    if (!m_program)
    {
        return;
    }
    const PipelineKind kind = selectedKind();
    const QString vm = QString::fromLatin1(reorder_report::vmName(kind));
    try
    {
        m_analysis = reorder_report::analyzeReorder(m_expandedText, *m_program, kind);
    }
    catch (const std::exception &e)
    {
        m_analysis.reset();
        m_summaryLabel->setText(QString("Could not schedule this program: %1")
                                    .arg(QString::fromStdString(e.what()).toHtmlEscaped()));
        m_applyButton->setEnabled(false);
        m_verifyButton->setEnabled(false);
        return;
    }
    const reorder_report::ReorderAnalysis &a = *m_analysis;

    std::map<unsigned, ListingLineLink> originalLinks, suggestedLinks;
    if (a.roundTripOk) // the suggested source's own line numbers come from re-assembling it
    {
        buildLineLinks(*m_program, a, originalLinks, suggestedLinks);
    }
    m_originalView->setListing(QString::fromStdString(m_expandedText), originalLinks,
                               a.originalMarks.stallsByLine);
    m_suggestedView->setListing(QString::fromStdString(a.reorderedSource), suggestedLinks,
                                a.reorderedMarks.stallsByLine);
    m_suggestedHeading->setText(QString("Suggested for %1").arg(vm));

    m_blockTable->setRowCount(static_cast<int>(a.blocks.size()));
    for (int row = 0; row < static_cast<int>(a.blocks.size()); ++row)
    {
        const reorder_report::BlockSummary &b = a.blocks[static_cast<size_t>(row)];
        const QString lines = b.firstLine == b.lastLine
                                  ? QString("line %1").arg(b.firstLine)
                                  : QString("lines %1-%2").arg(b.firstLine).arg(b.lastLine);
        m_blockTable->setItem(row, 0, new QTableWidgetItem(lines));
        m_blockTable->setItem(row, 1, new QTableWidgetItem(QString::number(b.stallsBefore)));
        m_blockTable->setItem(row, 2, new QTableWidgetItem(QString::number(b.stallsAfter)));
    }

    QString summary;
    if (!a.hasImprovement())
    {
        summary = QString("<b>No reordering found</b> for %1: no instruction can move to remove a "
                          "predicted stall (predicted stalls: %2).")
                      .arg(vm)
                      .arg(a.report.stallsBefore);
    }
    else
    {
        summary = QString("<b>Predicted stalls on %1: %2 &rarr; %3</b> (%4 instruction(s) "
                          "moved). Click Verify to measure on the VM.")
                      .arg(vm)
                      .arg(a.report.stallsBefore)
                      .arg(a.report.stallsAfter)
                      .arg(a.report.moves.size());
    }

    bool canApply = a.hasImprovement() && a.roundTripOk;
    QString applyNote;
    if (m_expandedText != m_editorText)
    {
        canApply = false;
        applyNote = "Apply is off: the program uses custom pseudo-instructions, and applying "
                    "would replace them with their expansion.";
    }
    else if (a.hasImprovement() && !a.roundTripOk)
    {
        applyNote = "Apply is off: the suggested source did not re-assemble to the same "
                    "instructions.";
    }
    if (!applyNote.isEmpty())
    {
        summary += "<br>" + applyNote;
    }
    m_summaryLabel->setText(summary);
    m_applyButton->setEnabled(canApply);
    m_applyButton->setToolTip(applyNote);
    m_verifyButton->setEnabled(true);
    m_verifyLabel->clear();
}

void ReorderDialog::verify()
{
    if (!m_program || m_verifyWatcher->isRunning())
    {
        return;
    }
    Seed seed;
    try
    {
        seed = reorder_report::parseSeed(m_seedEdit->text().toStdString());
    }
    catch (const std::exception &e)
    {
        m_verifyLabel->setText(QString("Seed: %1").arg(QString::fromStdString(e.what()).toHtmlEscaped()));
        return;
    }

    const AssembledProgram program = *m_program;
    const PipelineKind kind = selectedKind();
    const unsigned cap = static_cast<unsigned>(m_capSpin->value());
    setControlsEnabled(false);
    m_verifyButton->setText("Verifying...");
    m_verifyLabel->setText(QString("Running original and suggested code on %1...")
                               .arg(QString::fromLatin1(reorder_report::vmName(kind))));
    // Copies only: the worker never touches the dialog.
    m_verifyWatcher->setFuture(QtConcurrent::run([program, kind, seed, cap]()
                                                 { return reorder_report::runReorder(program, kind, seed, cap); }));
}

void ReorderDialog::verifyFinished()
{
    m_verifyButton->setText("Verify");
    setControlsEnabled(true);
    analyze(); // restores the Apply state (and clears the verify label)

    const reorder_report::ReorderOutcome o = m_verifyWatcher->result();
    QString verdict;
    if (o.before.hitCap || o.after.hitCap)
    {
        verdict = QString("<b>Not verified:</b> the %1 run hit the cycle cap (%2 cycles). Raise the "
                          "cap if the program is meant to run that long.")
                      .arg(o.before.hitCap ? "original" : "suggested")
                      .arg(o.cycleCap);
    }
    else if (o.equivalent)
    {
        verdict = "<b>Equal:</b> same final registers and data memory.";
    }
    else
    {
        verdict = "<b>NOT equal:</b> the final registers or data memory differ.";
    }
    m_verifyLabel->setText(
        QString("%1<br>On %2: cycles %3 &rarr; %4, stall cycles %5 &rarr; %6, CPI %7 &rarr; %8, "
                "instructions retired %9 &rarr; %10.")
            .arg(verdict, QString::fromLatin1(reorder_report::vmName(o.kind)))
            .arg(o.before.cycles)
            .arg(o.after.cycles)
            .arg(o.before.stalls)
            .arg(o.after.stalls)
            .arg(cpiText(o.before.cycles, o.before.retired), cpiText(o.after.cycles, o.after.retired))
            .arg(o.before.retired)
            .arg(o.after.retired));
    if (!o.equivalent)
    {
        m_applyButton->setEnabled(false);
        m_applyButton->setToolTip("Apply is off: verification did not show equal results.");
    }
}
} // namespace Kites
