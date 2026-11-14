#include <gtest/gtest.h>
#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VideoEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("VideoEditorTests"));

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
