#include "reorder_dialog.h"
#include "assembler/assembler.h"
#include "common/globals.h"
#include "custom_pseudo_manager/custom_pseudo_manager.h"
#include "reorder/loop_unroll.h"
#include "reorder_listing_editor.h"
#include <QApplication>
#include <QCheckBox>
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
#include <algorithm>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

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

// Shows the wait cursor for as long as it lives, also when the scope is left by an exception.
struct WaitCursorGuard
{
    WaitCursorGuard() { QApplication::setOverrideCursor(Qt::WaitCursor); }
    ~WaitCursorGuard() { QApplication::restoreOverrideCursor(); }
    WaitCursorGuard(const WaitCursorGuard &) = delete;
    WaitCursorGuard &operator=(const WaitCursorGuard &) = delete;
};

// One sentence per line. The unrolled legend replaces the move sentences, since moves do not map
// onto a listing with inserted lines.
const char *kMovesLegend = "Each moved line has its own colour, the same on both sides.\n"
                           "\u2192 N in the original gutter is the line it moved to.\n"
                           "\u2190 N in the suggested gutter is the line it came from.\n"
                           "+N: predicted stall cycles before that line's instruction.";
const char *kUnrolledLegend = "Unrolled mode shows inserted lines instead of moves.\n"
                              "Lines added by unrolling are highlighted, marked \"new\" in the "
                              "suggested gutter.\n"
                              "Inside every copy the instructions are also reordered.\n"
                              "+N: predicted stall cycles before that line's instruction.";

// The lines of `unrolled` that unrollSource() added to `original`: it only inserts lines (the
// copies, the inverted branches, the exit labels), so walking both in order finds them. Empty
// when `unrolled` is not `original` plus inserted lines.
std::map<unsigned, ListingLineLink> insertedLineLinks(const std::string &original,
                                                      const std::string &unrolled)
{
    const std::vector<std::string> before = reorder::splitLines(original);
    const std::vector<std::string> after = reorder::splitLines(unrolled);
    std::map<unsigned, ListingLineLink> links;
    size_t next = 0; // the next line of `original` still to be found
    for (size_t i = 0; i < after.size(); ++i)
    {
        if (next < before.size() && after[i] == before[next])
        {
            ++next;
        }
        else if (!reorder_report::trimSpaces(after[i]).empty())
        {
            links[static_cast<unsigned>(i + 1)] = {ReorderListingEditor::kInsertedColorIndex,
                                                   "new"};
        }
    }
    if (next != before.size())
    {
        return {};
    }
    return links;
}

reorder::UnrollOutcome unrollOn(const std::string &source, const AssembledProgram &program,
                                const Seed &seed, PipelineKind kind, unsigned cap)
{
    if (kind == PipelineKind::HF)
    {
        return reorder::unrollAndReorder<RV5StageProcessorHF>(source, program, seed, kind, cap);
    }
    return reorder::unrollAndReorder<RV5StageProcessorHNF>(source, program, seed, kind, cap);
}

// Verify for the unrolled program: the same runs and verdict data as runReorder(), with the
// unrolled program as the suggested one.
template <class VM>
reorder_report::ReorderOutcome compareRuns(const AssembledProgram &original,
                                           const AssembledProgram &suggested, PipelineKind kind,
                                           const Seed &seed, unsigned cap)
{
    reorder_report::ReorderOutcome outcome;
    outcome.kind = kind;
    outcome.cycleCap = cap;
    outcome.original = original;
    outcome.reordered = suggested;
    outcome.before = runProgram<VM>(original, seed, cap, reorder_report::kDataWindowWords);
    outcome.after = runProgram<VM>(suggested, seed, cap, reorder_report::kDataWindowWords);
    outcome.equivalent = sameState(outcome.before, outcome.after);
    return outcome;
}

QString cyclesText(const RunResult &run, unsigned cap)
{
    return run.hitCap ? QString("hit the cycle cap (%1)").arg(cap) : QString::number(run.cycles);
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

    assembleEditorText();
    if (!m_assemblyFailed)
    {
        analyze();
    }
    connect(m_vmCombo, &QComboBox::currentIndexChanged, this, &ReorderDialog::analyze);
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
    m_originalView->setObjectName("originalView");
    m_suggestedView->setObjectName("suggestedView");
    layout->addWidget(splitter, 1);
    m_legendLabel = new QLabel(QString::fromUtf8(kMovesLegend), this);
    m_legendLabel->setObjectName("legendLabel");
    m_legendLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(m_legendLabel);

    // Predicted stalls per block.
    m_blockTable = new QTableWidget(0, 3, this);
    m_blockTable->setHorizontalHeaderLabels({"Block (source lines)", "Predicted stalls before",
                                             "Predicted stalls after"});
    m_blockTable->setObjectName("blockTable");
    m_blockTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_blockTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_blockTable->verticalHeader()->setVisible(false);
    m_blockTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_blockTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_blockTable->setMaximumHeight(150);
    layout->addWidget(m_blockTable);

    // Opt-in loop unrolling, and what it did.
    m_unrollCheck = new QCheckBox("Also try loop unrolling", this);
    m_unrollCheck->setObjectName("unrollCheck");
    m_unrollCheck->setToolTip("Unroll eligible loops 2 or 4 times, reorder every copy, and keep "
                              "the result only if it runs faster on the VM with the same final "
                              "state (uses the Seed and Cycle cap below)");
    layout->addWidget(m_unrollCheck);
    m_unrollLabel = new QLabel(this);
    m_unrollLabel->setObjectName("unrollLabel");
    m_unrollLabel->setTextFormat(Qt::RichText); // every inserted string is HTML-escaped
    m_unrollLabel->setWordWrap(true);
    m_unrollLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_unrollLabel->setVisible(false);
    layout->addWidget(m_unrollLabel);

    // Verification on the VM.
    auto *verifyRow = new QHBoxLayout();
    verifyRow->addWidget(new QLabel("Seed:", this));
    m_seedEdit = new QLineEdit(this);
    m_seedEdit->setObjectName("seedEdit");
    m_seedEdit->setPlaceholderText("optional, e.g. x10=0x10000000,mem:0x10000000=7");
    verifyRow->addWidget(m_seedEdit, 1);
    verifyRow->addWidget(new QLabel("Cycle cap:", this));
    m_capSpin = new QSpinBox(this);
    m_capSpin->setObjectName("capSpin");
    m_capSpin->setRange(100, 100000000);
    m_capSpin->setValue(static_cast<int>(reorder_report::kDefaultCycleCap));
    m_capSpin->setSingleStep(100000);
    verifyRow->addWidget(m_capSpin);
    m_verifyButton = new QPushButton("Verify", this);
    m_verifyButton->setObjectName("verifyButton");
    m_verifyButton->setToolTip("Run original and suggested code on the VM and compare final state");
    verifyRow->addWidget(m_verifyButton);
    layout->addLayout(verifyRow);

    m_verifyLabel = new QLabel(this);
    m_verifyLabel->setObjectName("verifyLabel");
    m_verifyLabel->setTextFormat(Qt::RichText); // every inserted string is HTML-escaped
    m_verifyLabel->setWordWrap(true);
    m_verifyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_verifyLabel);

    auto *buttons = new QDialogButtonBox(this);
    m_applyButton = buttons->addButton("Apply to Editor", QDialogButtonBox::AcceptRole);
    m_applyButton->setObjectName("applyButton");
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(m_verifyButton, &QPushButton::clicked, this, &ReorderDialog::verify);
    connect(m_applyButton, &QPushButton::clicked, this,
            [this]()
            {
                if (m_unrolledProgram)
                {
                    emit applyRequested(QString::fromStdString(m_unrolledText));
                    accept();
                }
                else if (m_analysis)
                {
                    emit applyRequested(QString::fromStdString(m_analysis->reorderedSource));
                    accept();
                }
            });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_unrollCheck, &QCheckBox::toggled, this, &ReorderDialog::analyze);
    // The unrolling is checked with the Seed and Cycle cap, so it is redone when they change.
    auto reanalyzeIfUnrolling = [this]()
    {
        if (m_unrollCheck->isChecked() && !m_verifying)
        {
            analyze();
        }
    };
    connect(m_seedEdit, &QLineEdit::editingFinished, this, reanalyzeIfUnrolling);
    connect(m_capSpin, &QSpinBox::editingFinished, this, reanalyzeIfUnrolling);
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
    m_unrollCheck->setEnabled(enabled);
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
        const QString cells[] = {lines, QString::number(b.stallsBefore),
                                 QString::number(b.stallsAfter)};
        for (int column = 0; column < 3; ++column)
        {
            auto *item = new QTableWidgetItem(cells[column]);
            item->setTextAlignment(Qt::AlignCenter);
            m_blockTable->setItem(row, column, item);
        }
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

    // Opt-in loop unrolling replaces the suggested side (and the summary) when it keeps a loop.
    m_unrolledText.clear();
    m_unrolledProgram.reset();
    m_legendLabel->setText(QString::fromUtf8(kMovesLegend));
    m_unrollLabel->clear();
    m_unrollLabel->setVisible(m_unrollCheck->isChecked());
    const bool unrolled = m_unrollCheck->isChecked() && tryLoopUnrolling(kind, summary);

    bool canApply = unrolled || (a.hasImprovement() && a.roundTripOk);
    QString applyNote;
    if (m_expandedText != m_editorText)
    {
        canApply = false;
        applyNote = "Apply is off: the program uses custom pseudo-instructions, and applying "
                    "would replace them with their expansion.";
    }
    else if (!unrolled && a.hasImprovement() && !a.roundTripOk)
    {
        applyNote = "Apply is off: the suggested source did not re-assemble to the same "
                    "instructions.";
    }
    if (!applyNote.isEmpty())
    {
        summary += "<br>" + applyNote;
    }
    m_summaryLabel->setText(summary);
    m_canApply = canApply;
    m_applyNote = applyNote;
    m_applyButton->setEnabled(canApply);
    m_applyButton->setToolTip(applyNote);
    m_verifyButton->setEnabled(true);
    m_verifyLabel->clear();
}

bool ReorderDialog::tryLoopUnrolling(PipelineKind kind, QString &summary)
{
    const QString vm = QString::fromLatin1(reorder_report::vmName(kind));
    const unsigned cap = static_cast<unsigned>(m_capSpin->value());
    const QString heading = "<b>Loop unrolling:</b> ";
    Seed seed;
    try
    {
        seed = reorder_report::parseSeed(m_seedEdit->text().toStdString()); // empty: default
    }
    catch (const std::exception &e)
    {
        const QString reason = QString::fromStdString(e.what()).toHtmlEscaped();
        m_unrollLabel->setText(
            heading +
            QString("not tried, invalid seed: %1. Showing the reorder-only result.").arg(reason));
        return false;
    }

    m_unrollLabel->setText(heading + QString("running on %1...").arg(vm));
    m_unrollLabel->repaint();
    std::optional<reorder::UnrollOutcome> outcome;
    QString failure;
    {
        WaitCursorGuard waitCursor; // several VM runs, each bounded by the Cycle cap
        try
        {
            outcome = unrollOn(m_expandedText, *m_program, seed, kind, cap);
        }
        catch (const std::exception &e)
        {
            failure = QString::fromStdString(e.what());
        }
        catch (...)
        {
            failure = "unknown error";
        }
    }
    if (!outcome)
    {
        m_unrollLabel->setText(heading + QString("could not run: %1. Showing the reorder-only "
                                                 "result.")
                                             .arg(failure.toHtmlEscaped()));
        return false;
    }
    const reorder::UnrollOutcome &o = *outcome;

    // The kept program, written in the editor's layout (the unrolled source, reordered); it must
    // re-assemble to exactly the instructions the library checked.
    QString why;
    if (o.unrolled)
    {
        try
        {
            m_unrolledText =
                reorder_report::writeReorderedSource(o.finalSource, o.finalProgram, o.finalReport);
            std::istringstream stream(m_unrolledText);
            m_unrolledProgram = assemble(stream);
            if (m_unrolledProgram->text_buffer != o.finalReport.words)
            {
                why = "the unrolled source did not re-assemble to the checked instructions";
            }
        }
        catch (const std::exception &e)
        {
            why = QString("the unrolled source could not be written: %1")
                      .arg(QString::fromStdString(e.what()));
        }
        if (!why.isEmpty())
        {
            m_unrolledText.clear();
            m_unrolledProgram.reset();
        }
    }
    else if (o.loops.empty() && o.ineligible.empty())
    {
        why = "the program has no loop";
    }
    else if (o.loops.empty())
    {
        why = "no loop is eligible";
    }
    else if (o.original.hitCap)
    {
        why = "the original program hit the cycle cap";
    }
    else
    {
        why = "no loop was worth unrolling or passed the check";
    }
    const bool kept = m_unrolledProgram.has_value();

    QString text = heading;
    if (kept)
    {
        text += QString("%1. ").arg(QString::fromStdString(o.note).toHtmlEscaped());
    }
    else
    {
        text += QString("nothing kept, showing the reorder-only result: %1. ")
                    .arg(why.toHtmlEscaped());
    }
    text += QString("Cycles on %1: %2 (original) &rarr; %3 (%4).")
                .arg(vm, cyclesText(o.original, cap), cyclesText(o.final_, cap),
                     kept ? "unrolled and reordered" : "reorder-only");
    std::istringstream suggestions(reorder::unrollSuggestions(o));
    for (std::string line; std::getline(suggestions, line);)
    {
        text += "<br>" + QString::fromStdString(line).toHtmlEscaped();
    }
    m_unrollLabel->setText(text);
    if (!kept)
    {
        return false;
    }

    // Moves do not map onto inserted lines: show the inserted lines instead.
    std::vector<size_t> identity(m_unrolledProgram->text_buffer.size());
    std::iota(identity.begin(), identity.end(), size_t{0});
    const reorder_report::ListingMarks marks =
        reorder_report::listingMarks(*m_unrolledProgram, identity, kind);
    // writeReorderedSource() keeps the line layout of the unrolled source, so its inserted lines
    // are at the same line numbers.
    std::map<unsigned, ListingLineLink> inserted;
    if (reorder::splitLines(m_unrolledText).size() == reorder::splitLines(o.finalSource).size())
    {
        inserted = insertedLineLinks(m_expandedText, o.finalSource);
    }
    m_originalView->setListing(QString::fromStdString(m_expandedText), {},
                               m_analysis->originalMarks.stallsByLine);
    m_suggestedView->setListing(QString::fromStdString(m_unrolledText), inserted,
                                marks.stallsByLine);
    m_suggestedHeading->setText(QString("Suggested for %1 (loops unrolled)").arg(vm));
    m_legendLabel->setText(QString::fromUtf8(kUnrolledLegend));
    summary = QString("<b>Unrolled and reordered for %1</b> (checked on the VM; see the loop "
                      "unrolling summary below). Click Verify to compare final state.")
                  .arg(vm);
    return true;
}

void ReorderDialog::verify()
{
    if (!m_program || m_verifying)
    {
        return;
    }
    try
    {
        Seed seed;
        try
        {
            seed = reorder_report::parseSeed(m_seedEdit->text().toStdString()); // empty: default
        }
        catch (const std::exception &e)
        {
            m_verifyLabel->setText(
                QString("Invalid seed: %1").arg(QString::fromStdString(e.what()).toHtmlEscaped()));
            return;
        }

        // Copies only: the run never touches the app's processor or the editor.
        const AssembledProgram program = *m_program;
        const PipelineKind kind = selectedKind();
        const unsigned cap = static_cast<unsigned>(m_capSpin->value());

        // Runs on the GUI thread (see the class comment); input is blocked until it returns.
        m_verifying = true;
        setControlsEnabled(false);
        m_verifyButton->setText("Verifying...");
        m_verifyLabel->setText(QString("Running original and suggested code on %1...")
                                   .arg(QString::fromLatin1(reorder_report::vmName(kind))));
        m_verifyButton->repaint();
        m_verifyLabel->repaint();

        std::optional<reorder_report::ReorderOutcome> outcome;
        QString failure;
        {
            WaitCursorGuard waitCursor;
            try
            {
                if (m_unrolledProgram)
                {
                    const AssembledProgram unrolled = *m_unrolledProgram;
                    outcome =
                        kind == PipelineKind::HF
                            ? compareRuns<RV5StageProcessorHF>(program, unrolled, kind, seed, cap)
                            : compareRuns<RV5StageProcessorHNF>(program, unrolled, kind, seed, cap);
                }
                else
                {
                    outcome = reorder_report::runReorder(program, kind, seed, cap);
                }
            }
            catch (const std::exception &e)
            {
                failure = QString::fromStdString(e.what());
            }
            catch (...)
            {
                failure = "unknown error";
            }
        }

        m_verifying = false;
        m_verifyButton->setText("Verify");
        setControlsEnabled(true);
        // Restore the Apply state of the last analysis (re-analysing would redo the unrolling).
        m_applyButton->setEnabled(m_canApply);
        m_applyButton->setToolTip(m_applyNote);
        if (!outcome)
        {
            m_verifyLabel->setText(
                QString("Verification could not run: %1").arg(failure.toHtmlEscaped()));
            return;
        }
        showVerifyResult(*outcome);
    }
    catch (const std::exception &e)
    {
        m_verifying = false;
        m_verifyButton->setText("Verify");
        m_verifyButton->setEnabled(true);
        m_verifyLabel->setText(QString("Verification could not run: %1")
                                   .arg(QString::fromStdString(e.what()).toHtmlEscaped()));
    }
    catch (...)
    {
        m_verifying = false;
        m_verifyButton->setText("Verify");
        m_verifyButton->setEnabled(true);
        m_verifyLabel->setText("Verification could not run: unknown error");
    }
}

void ReorderDialog::showVerifyResult(const reorder_report::ReorderOutcome &o)
{
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
    m_verifyLabel->setText(verdict);
    if (!o.equivalent)
    {
        m_applyButton->setEnabled(false);
        m_applyButton->setToolTip("Apply is off: verification did not show equal results.");
    }
}
} // namespace Kites
