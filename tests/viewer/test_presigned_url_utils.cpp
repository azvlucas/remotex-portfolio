#include "viewer/presigned_url_utils.h"

#include <gtest/gtest.h>

namespace {

TEST(PresignedUrlUtilsTest, PreservesEncodedPresignedUrlVerbatim) {
    const QString raw_url =
        "https://bucket.r2.cloudflarestorage.com/recordings/company/machine/video.mp4"
        "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
        "&X-Amz-Credential=test%2F20260317%2Fauto%2Fs3%2Faws4_request"
        "&X-Amz-Date=20260317T120000Z"
        "&X-Amz-Expires=900"
        "&X-Amz-SignedHeaders=host"
        "&response-content-disposition=attachment%3B%20filename%3D%22recording.mp4%22"
        "&X-Amz-Signature=ab%2Bc%2Fdef%3D%3D";

    QString error;
    const QUrl url = remotex::viewer::BuildPresignedExternalUrl(raw_url, &error);

    EXPECT_TRUE(error.isEmpty());
    EXPECT_TRUE(url.isValid());
    EXPECT_EQ(QString::fromUtf8(url.toEncoded()), raw_url);
}

TEST(PresignedUrlUtilsTest, RejectsUnsupportedScheme) {
    QString error;
    const QUrl url = remotex::viewer::BuildPresignedExternalUrl("javascript:alert(1)", &error);

    EXPECT_FALSE(url.isValid());
    EXPECT_EQ(error, QString("URL de download invalida."));
}

} // namespace
