// Regression tests for the "Suggest Reordering" dialog (src/ui/dialogs/reorder_dialog.*): Verify
// must never crash and must always end in a result or a clear message. Runs headless (offscreen
// unless QT_QPA_PLATFORM is set). The dialog's files go to a temporary directory and the app's
// saved settings are put back if anything touched them.
#include <gtest/gtest.h>

#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "common/globals.h"
#include "ui/dialogs/reorder_dialog.h"
#include "ui/dialogs/reorder_listing_editor.h"

using namespace Kites;

namespace
{
// The program the crash was reported on (rv5-h-nf, empty seed).
const char *kGcdProgram = R"(.data
.dword 3, 12, 3, 125, 50, 32, 16

.text
lui x3, 65536
ld x5, 0(x3)
addi x6, x3, 8
addi x7, x3, 512

loop:
beq x5, x0, exit
ld x10, 0(x6)
ld x11, 8(x6)

addi x6, x6, 16
beq x10, x0, return
beq x11, x0, return

gcd:
beq x10, x11, found
bltu x11, x10, greater

sub x11, x11, x10
jal x0, gcd

greater:
sub x10, x10, x11
jal x0, gcd

return:
addi x10, x0,0

found:
sd x10, 0(x7)
addi x7, x7, 8

addi x5, x5, -1
jal x0, loop

exit:
)";

std::string readFile(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

class ReorderDialogTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!qApp)
        {
            if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            {
                qputenv("QT_QPA_PLATFORM", "offscreen");
            }
            static int argc = 1;
            static char name[] = "kites_test";
            static char *argv[] = {name, nullptr};
            new QApplication(argc, argv); // lives for the rest of the test run
        }
        QSettings settings("Kites", "Kites");
        for (const QString &key : settings.allKeys())
        {
            s_savedSettings[key] = settings.value(key);
        }
    }

    static void TearDownTestSuite()
    {
        QSettings settings("Kites", "Kites");
        std::map<QString, QVariant> now;
        for (const QString &key : settings.allKeys())
        {
            now[key] = settings.value(key);
        }
        if (now != s_savedSettings) // the dialog's theme lookup must leave the user's settings alone
        {
            settings.clear();
            for (const auto &[key, value] : s_savedSettings)
            {
                settings.setValue(key, value);
            }
        }
    }

    void SetUp() override
    {
        m_oldStateDir = globals::vm_state_directory;
        m_oldErrorsPath = globals::errors_dump_file_path;
        // One directory per process and test: ctest -j runs tests in parallel processes.
        m_dir = std::filesystem::temp_directory_path() /
                ("kites_reorder_dialog_test_" + std::to_string(QCoreApplication::applicationPid()) +
                 "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(m_dir);
        globals::vm_state_directory = m_dir;
        globals::errors_dump_file_path = m_dir / "errors_dump.json";
    }

    void TearDown() override
    {
        globals::vm_state_directory = m_oldStateDir;
        globals::errors_dump_file_path = m_oldErrorsPath;
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    // Types the seed and clicks Verify; returns the status text.
    static QString clickVerify(ReorderDialog &dialog, const QString &seed)
    {
        auto *seedEdit = dialog.findChild<QLineEdit *>("seedEdit");
        auto *button = dialog.findChild<QPushButton *>("verifyButton");
        auto *label = dialog.findChild<QLabel *>("verifyLabel");
        EXPECT_NE(seedEdit, nullptr);
        EXPECT_NE(button, nullptr);
        EXPECT_NE(label, nullptr);
        if (!seedEdit || !button || !label)
        {
            return {};
        }
        EXPECT_TRUE(button->isEnabled());
        seedEdit->setText(seed);
        button->click();
        // The run is finished when click() returns: the button is back and usable.
        EXPECT_EQ(button->text(), "Verify");
        EXPECT_TRUE(button->isEnabled());
        return label->text();
    }

    static std::map<QString, QVariant> s_savedSettings;
    std::filesystem::path m_dir;
    std::filesystem::path m_oldStateDir;
    std::filesystem::path m_oldErrorsPath;
};

std::map<QString, QVariant> ReorderDialogTest::s_savedSettings;
} // namespace

TEST_F(ReorderDialogTest, GcdVerifiesWithEmptySeed)
{
    ReorderDialog dialog(nullptr, kGcdProgram, ProcessorType::RV5Stage_H_NF);
    ASSERT_FALSE(dialog.assemblyFailed());
    const QString status = clickVerify(dialog, "");
    EXPECT_TRUE(status.contains("Equal:")) << status.toStdString();
    EXPECT_TRUE(status.contains("rv5-h-nf")) << status.toStdString();
}

TEST_F(ReorderDialogTest, GcdVerifiesWithValidSeed)
{
    ReorderDialog dialog(nullptr, kGcdProgram, ProcessorType::RV5Stage_H_NF);
    const QString status = clickVerify(dialog, "x10=0x10000000,mem:0x10000000=7");
    EXPECT_TRUE(status.contains("Equal:")) << status.toStdString();
}

TEST_F(ReorderDialogTest, MalformedSeedShowsMessage)
{
    ReorderDialog dialog(nullptr, kGcdProgram, ProcessorType::RV5Stage_H_NF);
    for (const char *seed : {"x10=", "garbage", "x40=1", "mem:zz=1"})
    {
        const QString status = clickVerify(dialog, seed);
        EXPECT_TRUE(status.startsWith("Invalid seed: ")) << seed << ": " << status.toStdString();
        EXPECT_GT(status.size(), QString("Invalid seed: ").size()) << "the reason is shown";
    }
    // A good seed afterwards still verifies.
    EXPECT_TRUE(clickVerify(dialog, "").contains("Equal:"));
}

TEST_F(ReorderDialogTest, ProgramWithReorderingVerifiesAndKeepsApply)
{
    const std::string source = readFile("../bench/programs/reference/load_use.s");
    ASSERT_FALSE(source.empty());
    for (ProcessorType vm : {ProcessorType::RV5Stage_H_NF, ProcessorType::RV5Stage_H_F})
    {
        ReorderDialog dialog(nullptr, QString::fromStdString(source), vm);
        auto *apply = dialog.findChild<QPushButton *>("applyButton");
        ASSERT_NE(apply, nullptr);
        const bool applyBefore = apply->isEnabled();
        for (const QString &seed : {QString(""), QString("x10=0x10000000,mem:0x10000000=7"),
                                    QString("not a seed")})
        {
            const QString status = clickVerify(dialog, seed);
            if (seed == "not a seed")
            {
                EXPECT_TRUE(status.startsWith("Invalid seed: ")) << status.toStdString();
            }
            else
            {
                EXPECT_TRUE(status.contains("Equal:")) << status.toStdString();
            }
            EXPECT_EQ(apply->isEnabled(), applyBefore) << "Verify must not change Apply when equal";
        }
    }
}

TEST_F(ReorderDialogTest, LegendHasFourSentencesOnSeparateLines)
{
    ReorderDialog dialog(nullptr, kGcdProgram, ProcessorType::RV5Stage_H_NF);
    auto *legend = dialog.findChild<QLabel *>("legendLabel");
    ASSERT_NE(legend, nullptr);
    const QStringList lines = legend->text().split('\n');
    ASSERT_EQ(lines.size(), 4);
    EXPECT_EQ(lines[0], "Each moved line has its own colour, the same on both sides.");
    EXPECT_EQ(lines[1], QString::fromUtf8("→ N in the original gutter is the line it moved to."));
    EXPECT_EQ(lines[2], QString::fromUtf8("← N in the suggested gutter is the line it came from."));
    EXPECT_EQ(lines[3], "+N: predicted stall cycles before that line's instruction.");
}

TEST_F(ReorderDialogTest, BlockTableIsCentred)
{
    const std::string source = readFile("../bench/programs/reference/load_use.s");
    ReorderDialog dialog(nullptr, QString::fromStdString(source), ProcessorType::RV5Stage_H_NF);
    auto *table = dialog.findChild<QTableWidget *>("blockTable");
    ASSERT_NE(table, nullptr);
    ASSERT_GT(table->rowCount(), 0);
    EXPECT_EQ(table->horizontalHeader()->defaultAlignment(), Qt::AlignCenter);
    for (int row = 0; row < table->rowCount(); ++row)
    {
        for (int column = 0; column < table->columnCount(); ++column)
        {
            ASSERT_NE(table->item(row, column), nullptr);
            EXPECT_EQ(table->item(row, column)->textAlignment(), Qt::AlignCenter);
        }
    }
}

TEST_F(ReorderDialogTest, MovedLineColoursAreTranslucentAndDistinct)
{
    for (int i = 0; i < 12; ++i)
    {
        const QColor color = ReorderListingEditor::linkColor(i);
        EXPECT_NEAR(color.alphaF(), ReorderListingEditor::kLinkAlpha, 1e-3); // QColor keeps 16 bits
        EXPECT_NE(color.rgb(), ReorderListingEditor::linkColor(i + 1).rgb());
    }
    EXPECT_GE(ReorderListingEditor::kLinkAlpha, 0.35);
    EXPECT_LE(ReorderListingEditor::kLinkAlpha, 0.45);
}
