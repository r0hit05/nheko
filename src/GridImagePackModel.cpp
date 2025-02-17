// SPDX-FileCopyrightText: Nheko Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GridImagePackModel.h"

#include <QTextBoundaryFinder>

#include <algorithm>

#include "Cache.h"
#include "Cache_p.h"

Q_DECLARE_METATYPE(StickerImage)
Q_DECLARE_METATYPE(SectionDescription)
Q_DECLARE_METATYPE(QList<SectionDescription>)

GridImagePackModel::GridImagePackModel(const std::string &roomId, bool stickers, QObject *parent)
  : QAbstractListModel(parent)
  , room_id(roomId)
{
    [[maybe_unused]] static auto id  = qRegisterMetaType<StickerImage>();
    [[maybe_unused]] static auto id2 = qRegisterMetaType<SectionDescription>();
    [[maybe_unused]] static auto id3 = qRegisterMetaType<QList<SectionDescription>>();

    auto originalPacks = cache::client()->getImagePacks(room_id, stickers);

    for (auto &pack : originalPacks) {
        PackDesc newPack{};
        newPack.packname =
          pack.pack.pack ? QString::fromStdString(pack.pack.pack->display_name) : QString();
        newPack.room_id   = pack.source_room;
        newPack.state_key = pack.state_key;

        newPack.images.resize(pack.pack.images.size());
        std::ranges::transform(std::move(pack.pack.images), newPack.images.begin(), [](auto &&img) {
            return std::pair(std::move(img.second), QString::fromStdString(img.first));
        });

        size_t packRowCount =
          (newPack.images.size() / columns) + (newPack.images.size() % columns ? 1 : 0);
        newPack.firstRow = rowToPack.size();
        for (size_t i = 0; i < packRowCount; i++)
            rowToPack.push_back(packs.size());
        packs.push_back(std::move(newPack));
    }

    // prepare search index

    auto insertParts = [this](const QString &str, std::pair<std::uint32_t, std::uint32_t> id) {
        QTextBoundaryFinder finder(QTextBoundaryFinder::BoundaryType::Word, str);
        finder.toStart();
        do {
            auto start = finder.position();
            finder.toNextBoundary();
            auto end = finder.position();

            auto ref = str.midRef(start, end - start).trimmed();
            if (!ref.isEmpty())
                trie_.insert<ElementRank::second>(ref.toUcs4(), id);
        } while (finder.position() < str.size());
    };

    std::uint32_t packIndex = 0;
    for (const auto &pack : packs) {
        std::uint32_t imgIndex = 0;
        for (const auto &img : pack.images) {
            std::pair<std::uint32_t, std::uint32_t> key{packIndex, imgIndex};

            QString string1 = img.second.toCaseFolded();
            QString string2 = QString::fromStdString(img.first.body).toCaseFolded();

            if (!string1.isEmpty()) {
                trie_.insert<ElementRank::first>(string1.toUcs4(), key);
                insertParts(string1, key);
            }
            if (!string2.isEmpty()) {
                trie_.insert<ElementRank::first>(string2.toUcs4(), key);
                insertParts(string2, key);
            }

            imgIndex++;
        }
        packIndex++;
    }
}

int
GridImagePackModel::rowCount(const QModelIndex &) const
{
    return static_cast<int>(searchString_.isEmpty() ? rowToPack.size()
                                                    : rowToFirstRowEntryFromSearch.size());
}

QHash<int, QByteArray>
GridImagePackModel::roleNames() const
{
    return {
      {Roles::PackName, "packname"},
      {Roles::Row, "row"},
    };
}

QVariant
GridImagePackModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < rowCount() && index.row() >= 0) {
        if (searchString_.isEmpty()) {
            const auto &pack = packs[rowToPack[index.row()]];
            switch (role) {
            case Roles::PackName:
                return nameFromPack(pack);
            case Roles::Row: {
                std::size_t offset = static_cast<std::size_t>(index.row()) - pack.firstRow;
                QList<StickerImage> imgs;
                auto endOffset = std::min((offset + 1) * columns, pack.images.size());
                for (std::size_t img = offset * columns; img < endOffset; img++) {
                    const auto &data = pack.images.at(img);
                    imgs.push_back({.url         = QString::fromStdString(data.first.url),
                                    .shortcode   = data.second,
                                    .body        = QString::fromStdString(data.first.body),
                                    .descriptor_ = std::vector{
                                      pack.room_id,
                                      pack.state_key,
                                      data.second.toStdString(),
                                    }});
                }
                return QVariant::fromValue(imgs);
            }
            default:
                return {};
            }
        } else {
            if (static_cast<size_t>(index.row()) >= rowToFirstRowEntryFromSearch.size())
                return {};

            const auto firstIndex = rowToFirstRowEntryFromSearch[index.row()];
            const auto firstEntry = currentSearchResult[firstIndex];
            const auto &pack      = packs[firstEntry.first];

            switch (role) {
            case Roles::PackName:
                return nameFromPack(pack);
            case Roles::Row: {
                QList<StickerImage> imgs;
                for (auto img = firstIndex;
                     imgs.size() < columns && img < currentSearchResult.size() &&
                     currentSearchResult[img].first == firstEntry.first;
                     img++) {
                    const auto &data = pack.images.at(currentSearchResult[img].second);
                    imgs.push_back({.url         = QString::fromStdString(data.first.url),
                                    .shortcode   = data.second,
                                    .body        = QString::fromStdString(data.first.body),
                                    .descriptor_ = std::vector{
                                      pack.room_id,
                                      pack.state_key,
                                      data.second.toStdString(),
                                    }});
                }
                return QVariant::fromValue(imgs);
            }
            default:
                return {};
            }
        }
    }
    return {};
}

QString
GridImagePackModel::nameFromPack(const PackDesc &pack) const
{
    if (!pack.packname.isEmpty()) {
        return pack.packname;
    }

    if (!pack.state_key.empty()) {
        return QString::fromStdString(pack.state_key);
    }

    if (!pack.room_id.empty()) {
        auto info = cache::singleRoomInfo(pack.room_id);
        return QString::fromStdString(info.name);
    }

    return tr("Account Pack");
}

QString
GridImagePackModel::avatarFromPack(const PackDesc &pack) const
{
    if (!pack.packavatar.isEmpty()) {
        return pack.packavatar;
    }

    if (!pack.images.empty()) {
        return QString::fromStdString(pack.images.begin()->first.url);
    }

    return "";
}

QList<SectionDescription>
GridImagePackModel::sections() const
{
    QList<SectionDescription> sectionNames;
    if (searchString_.isEmpty()) {
        std::size_t packIdx = -1;
        for (std::size_t i = 0; i < rowToPack.size(); i++) {
            if (rowToPack[i] != packIdx) {
                const auto &pack = packs[rowToPack[i]];
                sectionNames.push_back({
                  .name         = nameFromPack(pack),
                  .url          = avatarFromPack(pack),
                  .firstRowWith = static_cast<int>(i),
                });
                packIdx = rowToPack[i];
            }
        }
    } else {
        std::uint32_t packIdx = -1;
        int row               = 0;
        for (const auto &i : rowToFirstRowEntryFromSearch) {
            const auto res = currentSearchResult[i];
            if (res.first != packIdx) {
                packIdx          = res.first;
                const auto &pack = packs[packIdx];
                sectionNames.push_back({
                  .name         = nameFromPack(pack),
                  .url          = avatarFromPack(pack),
                  .firstRowWith = row,
                });
            }
            row++;
        }
    }

    return sectionNames;
}

void
GridImagePackModel::setSearchString(QString key)
{
    beginResetModel();
    currentSearchResult.clear();
    rowToFirstRowEntryFromSearch.clear();
    searchString_ = key;

    if (!key.isEmpty()) {
        auto searchParts = key.toCaseFolded().toUcs4();
        auto tempResults =
          trie_.search(searchParts, static_cast<std::size_t>(columns * columns * 4));

        std::map<std::uint32_t, std::size_t> firstPositionOfPack;
        for (const auto &e : tempResults)
            firstPositionOfPack.emplace(e.first, firstPositionOfPack.size());

        std::ranges::stable_sort(tempResults, [&firstPositionOfPack](auto a, auto b) {
            return firstPositionOfPack[a.first] < firstPositionOfPack[b.first];
        });
        currentSearchResult = std::move(tempResults);

        std::size_t lastPack = -1;
        int columnIndex      = 0;
        for (std::size_t i = 0; i < currentSearchResult.size(); i++) {
            auto elem = currentSearchResult[i];
            if (elem.first != lastPack || columnIndex == columns) {
                columnIndex = 0;
                lastPack    = elem.first;
                rowToFirstRowEntryFromSearch.push_back(i);
            }
            columnIndex++;
        }
    }

    endResetModel();
    emit newSearchString();
}
