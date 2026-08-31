#include <gtest/gtest.h>

#include <QApplication>
#include <QDateEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>

#include <atomic>
#include <mutex>

#include "viewer/reports_dialog.h"

namespace {

class ReportsDialogTest : public ::testing::Test {
protected:
    static QApplication* app_;
    static bool owns_app_;

    static void SetUpTestSuite() {
        if (QApplication::instance() == nullptr) {
            int argc = 1;
            char arg0[] = "test_reports_dialog";
            char* argv[] = {arg0, nullptr};
            app_ = new QApplication(argc, argv);
            owns_app_ = true;
        } else {
            app_ = qobject_cast<QApplication*>(QApplication::instance());
            owns_app_ = false;
        }
    }

    static void TearDownTestSuite() {
        if (owns_app_) {
            delete app_;
            app_ = nullptr;
        }
    }
};

QApplication* ReportsDialogTest::app_ = nullptr;
bool ReportsDialogTest::owns_app_ = false;

QDateEdit* FindDateEdit(remotex::viewer::ReportsDialog& dialog, const char* object_name) {
    auto* date_edit = dialog.findChild<QDateEdit*>(object_name);
    EXPECT_NE(date_edit, nullptr);
    return date_edit;
}

QLineEdit* FindLineEdit(QDateEdit* date_edit) {
    if (!date_edit) {
        return nullptr;
    }

    auto* line_edit = date_edit->findChild<QLineEdit*>();
    EXPECT_NE(line_edit, nullptr);
    return line_edit;
}

QPushButton* FindApplyButton(remotex::viewer::ReportsDialog& dialog) {
    auto* button = dialog.findChild<QPushButton*>("reports_apply");
    EXPECT_NE(button, nullptr);
    return button;
}

QLabel* FindStatusLabel(remotex::viewer::ReportsDialog& dialog) {
    auto* label = dialog.findChild<QLabel*>("reports_status");
    EXPECT_NE(label, nullptr);
    return label;
}

TEST_F(ReportsDialogTest, ResolveDateRangeUsesVisibleTextValues) {
    QDate resolved_from;
    QDate resolved_to;
    QString error;

    const bool ok = remotex::viewer::ReportsDialog::ResolveDateRange(
        "05/03/2026", "10/03/2026", "dd/MM/yyyy",
        &resolved_from, &resolved_to, &error);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.isEmpty());
    EXPECT_EQ(resolved_from, QDate(2026, 3, 5));
    EXPECT_EQ(resolved_to, QDate(2026, 3, 10));
}

TEST_F(ReportsDialogTest, AppliesTypedDatesBeforeDispatchingLoad) {
    std::atomic<int> call_count = 0;
    QString captured_from;
    QString captured_to;
    std::mutex capture_mutex;

    remotex::viewer::ReportsDialog dialog(
        nullptr,
        [&](const QString& date_from, const QString& date_to) {
            {
                std::lock_guard<std::mutex> lock(capture_mutex);
                captured_from = date_from;
                captured_to = date_to;
            }
            call_count.fetch_add(1);
            return remotex::core::ReportDashboardData{};
        },
        false);

    auto* date_from = FindDateEdit(dialog, "reports_date_from");
    auto* date_to = FindDateEdit(dialog, "reports_date_to");
    ASSERT_NE(date_from, nullptr);
    ASSERT_NE(date_to, nullptr);

    date_from->setDate(QDate(2026, 2, 1));
    date_to->setDate(QDate(2026, 2, 28));

    auto* from_line = FindLineEdit(date_from);
    auto* to_line = FindLineEdit(date_to);
    ASSERT_NE(from_line, nullptr);
    ASSERT_NE(to_line, nullptr);

    from_line->setText("05/03/2026");
    to_line->setText("10/03/2026");
    QCoreApplication::processEvents();

    EXPECT_EQ(from_line->text(), QString("05/03/2026"));
    EXPECT_EQ(to_line->text(), QString("10/03/2026"));

    const bool invoked = QMetaObject::invokeMethod(&dialog, "OnApplyFilter", Qt::DirectConnection);
    ASSERT_TRUE(invoked);

    QTRY_VERIFY_WITH_TIMEOUT(call_count.load() == 1, 2000);

    {
        std::lock_guard<std::mutex> lock(capture_mutex);
        EXPECT_EQ(captured_from, QString("2026-03-05"));
        EXPECT_EQ(captured_to, QString("2026-03-10"));
    }
}

TEST_F(ReportsDialogTest, RejectsInvertedDateRangeWithoutDispatchingLoad) {
    std::atomic<int> call_count = 0;

    remotex::viewer::ReportsDialog dialog(
        nullptr,
        [&](const QString&, const QString&) {
            call_count.fetch_add(1);
            return remotex::core::ReportDashboardData{};
        },
        false);

    auto* date_from = FindDateEdit(dialog, "reports_date_from");
    auto* date_to = FindDateEdit(dialog, "reports_date_to");
    auto* apply_button = FindApplyButton(dialog);
    auto* status_label = FindStatusLabel(dialog);

    ASSERT_NE(date_from, nullptr);
    ASSERT_NE(date_to, nullptr);
    ASSERT_NE(apply_button, nullptr);
    ASSERT_NE(status_label, nullptr);

    date_from->setDate(QDate(2026, 3, 20));
    date_to->setDate(QDate(2026, 3, 10));

    apply_button->click();
    QCoreApplication::processEvents();

    EXPECT_EQ(call_count.load(), 0);
    EXPECT_EQ(status_label->text(), QString("A data inicial nao pode ser maior que a data final."));
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
