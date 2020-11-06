// Copyright(c) 2017-2019 Alejandro Sirgo Rica & Contributors
// Copyright(c) 2017-2019 Alejandro Sirgo Rica & Contributors
//
// This file is part of Flameshot.
//
//     Flameshot is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.
//
//     Flameshot is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.
//
//     You should have received a copy of the GNU General Public License
//     along with Flameshot.  If not, see <http://www.gnu.org/licenses/>.

#include "imgs3uploader.h"
#include "src/core/controller.h"
#include "src/utils/confighandler.h"
#include "src/utils/history.h"
#include "src/utils/systemnotification.h"
#include "src/widgets/imagelabel.h"
#include "src/widgets/loadspinner.h"
#include "src/widgets/notificationwidget.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QShortcut>
#include <QThread>
#include <QUrlQuery>
#include <QVBoxLayout>

ImgS3Uploader::ImgS3Uploader(const QPixmap& capture, QWidget* parent)
  : ImgUploader(capture, parent)
{
    init(tr("Upload image to S3"), tr("Uploading Image..."));
}

ImgS3Uploader::ImgS3Uploader(QWidget* parent)
  : ImgUploader(parent)
{
    init(tr("Delete image from S3"), tr("Deleting image..."));
}

ImgS3Uploader::~ImgS3Uploader()
{
    clearProxy();
}

void ImgS3Uploader::init(const QString& title, const QString& label)
{
    m_multiPart = nullptr;
    m_networkAMUpload = nullptr;
    m_networkAMGetCreds = nullptr;
    m_networkAMRemove = nullptr;

    resultStatus = false;
    setWindowTitle(title);
    setWindowIcon(QIcon(":img/app/flameshot.svg"));
}

QNetworkProxy* ImgS3Uploader::proxy()
{
    return m_s3Settings.proxy();
}

void ImgS3Uploader::clearProxy()
{
    m_s3Settings.clearProxy();
}

void ImgS3Uploader::handleReplyPostUpload(QNetworkReply* reply)
{
    hideSpinner();
    m_storageImageName.clear();
    if (reply->error() == QNetworkReply::NoError) {
        // save history
        QString imageName = imageUrl().toString();
        int lastSlash = imageName.lastIndexOf("/");
        if (lastSlash >= 0) {
            imageName = imageName.mid(lastSlash + 1);
        }
        m_storageImageName = imageName;

        // save image to history
        History history;
        imageName = history.packFileName(
          SCREENSHOT_STORAGE_TYPE_S3, m_deleteToken, imageName);
        history.save(pixmap(), imageName);
        resultStatus = true;

        // Copy url to clipboard if required
        if (ConfigHandler().copyAndCloseAfterUploadEnabled()) {
            SystemNotification().sendMessage(tr("URL copied to clipboard."));
            Controller::getInstance()->updateRecentScreenshots();
            QApplication::clipboard()->setText(imageUrl().toString());
            close();
        } else {
            onUploadOk();
        }
    } else {
        QString reason =
          reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute)
            .toString();
        setInfoLabelText(reply->errorString());
    }
    new QShortcut(Qt::Key_Escape, this, SLOT(close()));
}

void ImgS3Uploader::handleReplyDeleteResource(QNetworkReply* reply)
{
    auto replyError = reply->error();
    if (replyError == QNetworkReply::NoError) {
        removeImagePreview();
    } else {
        hide();

        // generate error message
        QString message =
          tr("Unable to remove screenshot from the remote storage.");
        if (replyError == QNetworkReply::UnknownNetworkError) {
            message += "\n" + tr("Network error");
        } else if (replyError == QNetworkReply::UnknownServerError) {
            message += "\n" + tr("Possibly it doesn't exist anymore");
        }
        message += "\n\n" + reply->errorString();
        message +=
          "\n\n" +
          tr("Do you want to remove screenshot from local history anyway?");

        if (QMessageBox::Yes ==
            QMessageBox::question(NULL,
                                  tr("Remove screenshot from history?"),
                                  message,
                                  QMessageBox::Yes | QMessageBox::No)) {
            removeImagePreview();
        }
    }
    close();
}

void ImgS3Uploader::handleReplyGetCreds(QNetworkReply* reply)
{
    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument response = QJsonDocument::fromJson(reply->readAll());
        uploadToS3(response);
    } else {
        if (m_s3Settings.credsUrl().length() == 0) {
            setInfoLabelText(
              tr("Retrieving configuration file with s3 creds..."));
            if (!m_s3Settings.getConfigRemote()) {
                retry();
            }
            hide();

            if (!m_s3Settings.credsUrl().isEmpty()) {
                setInfoLabelText(tr("Uploading Image..."));
                upload();
                return;
            }
        } else {
            setInfoLabelText(reply->errorString());
        }
        // FIXME - remove not uploaded preview
    }
    new QShortcut(Qt::Key_Escape, this, SLOT(close()));
}

void ImgS3Uploader::retry()
{
    setInfoLabelText(
      tr("S3 Creds URL is not found in your configuration file"));
    if (QMessageBox::Retry ==
        QMessageBox::question(nullptr,
                              tr("Error"),
                              tr("Unable to get s3 credentials, please check "
                                 "your VPN connection and try again"),
                              QMessageBox::Retry | QMessageBox::Cancel)) {
        setInfoLabelText(tr("Retrieving configuration file with s3 creds..."));
        if (!m_s3Settings.getConfigRemote()) {
            retry();
        }
    } else {
        hide();
    }
}

void ImgS3Uploader::uploadToS3(QJsonDocument& response)
{
    // set parameters from "fields"
    if (nullptr != m_multiPart) {
        delete m_multiPart;
        m_multiPart = nullptr;
    }
    m_multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    // read JSON response
    QJsonObject json = response.object();
    QString resultUrl = json["resultURL"].toString();
    QJsonObject formData = json["formData"].toObject();
    QString url = formData["url"].toString();
    m_deleteToken = json["deleteToken"].toString();

    QJsonObject fields = formData["fields"].toObject();
    foreach (auto key, fields.keys()) {
        QString field = fields[key].toString();
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QVariant("form-data; name=\"" + key + "\""));
        part.setBody(field.toLatin1());
        m_multiPart->append(part);
    }

    QHttpPart imagePart;
    imagePart.setHeader(QNetworkRequest::ContentTypeHeader,
                        QVariant("image/png"));
    imagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QVariant("form-data; name=\"file\""));

    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    buffer.open(QIODevice::WriteOnly);
    pixmap().save(&buffer, "PNG");

    imagePart.setBody(byteArray);
    m_multiPart->append(imagePart);

    setImageUrl(QUrl(resultUrl));

    QUrl qUrl(url);
    QNetworkRequest request(qUrl);

    // upload
    m_networkAMUpload->post(request, m_multiPart);
}

void ImgS3Uploader::deleteResource(const QString& fileName,
                                   const QString& deleteToken)
{
    // read network settings on each call to simplify configuration management
    // without restarting
    clearProxy();
    if (m_networkAMRemove != nullptr) {
        delete m_networkAMRemove;
        m_networkAMRemove = nullptr;
    }
    m_networkAMRemove = new QNetworkAccessManager(this);
    connect(m_networkAMRemove,
            &QNetworkAccessManager::finished,
            this,
            &ImgS3Uploader::handleReplyDeleteResource);
    if (proxy() != nullptr) {
        m_networkAMRemove->setProxy(*proxy());
    }

    QNetworkRequest request;
    m_storageImageName = fileName;
    m_deleteToken = deleteToken;

    request.setUrl(m_s3Settings.credsUrl().toUtf8() + fileName);
    request.setRawHeader("X-API-Key", m_s3Settings.xApiKey().toLatin1());
    request.setRawHeader("Authorization", "Bearer " + deleteToken.toLatin1());
    m_networkAMRemove->deleteResource(request);
}

void ImgS3Uploader::upload()
{
    m_deleteToken.clear();
    m_storageImageName.clear();
    show();

    // read network settings on each call to simplify configuration management
    // without restarting init creds and upload network access managers
    clearProxy();
    cleanNetworkAccessManagers();

    m_networkAMGetCreds = new QNetworkAccessManager(this);
    connect(m_networkAMGetCreds,
            &QNetworkAccessManager::finished,
            this,
            &ImgS3Uploader::handleReplyGetCreds);

    m_networkAMUpload = new QNetworkAccessManager(this);
    connect(m_networkAMUpload,
            &QNetworkAccessManager::finished,
            this,
            &ImgS3Uploader::handleReplyPostUpload);
    if (proxy() != nullptr) {
        m_networkAMGetCreds->setProxy(*proxy());
        m_networkAMUpload->setProxy(*proxy());
    }

    // get creads
    QNetworkRequest requestCreds(QUrl(m_s3Settings.credsUrl()));
    if (m_s3Settings.xApiKey().length() > 0) {
        requestCreds.setRawHeader(
          QByteArray("X-API-Key"),
          QByteArray(m_s3Settings.xApiKey().toLocal8Bit()));
    }
    m_networkAMGetCreds->get(requestCreds);
}

void ImgS3Uploader::removeImagePreview()
{
    // remove local file
    History history;
    QString packedFileName = history.packFileName(
      SCREENSHOT_STORAGE_TYPE_S3, m_deleteToken, m_storageImageName);
    QString fullFileName = history.path() + packedFileName;

    QFile file(fullFileName);
    if (file.exists()) {
        file.remove();
    }
    m_deleteToken.clear();
    m_storageImageName.clear();
    resultStatus = true;
}

void ImgS3Uploader::cleanNetworkAccessManagers()
{
    if (nullptr != m_networkAMUpload) {
        delete m_networkAMUpload;
        m_networkAMUpload = nullptr;
    }
    if (nullptr != m_networkAMGetCreds) {
        delete m_networkAMGetCreds;
        m_networkAMGetCreds = nullptr;
    }
    if (nullptr != m_networkAMRemove) {
        delete m_networkAMRemove;
        m_networkAMRemove = nullptr;
    }
}