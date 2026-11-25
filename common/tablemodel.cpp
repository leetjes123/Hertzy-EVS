#include "tablemodel.hpp"
#include <algorithm>
#include <numeric>

namespace hz {

QVariant TableModel::data(const QModelIndex& i, int role) const {
    if (!i.isValid() || i.row() >= (int)rows_.size() || i.column() >= (int)rows_[i.row()].size()) return {};
    const Cell& c = rows_[i.row()][i.column()];
    switch (role) {
        case Qt::DisplayRole: return c.text;
        case ColorRole: return c.color.isValid() ? QVariant(c.color) : QVariant();
        case KindRole: return c.kind;
        case CheckedRole: return c.checked;
        case DimRole: return c.dim;
        case RowKeyRole: return keys_[i.row()];
        case SelectedRole: return keys_[i.row()] == sel_key_;
    }
    return {};
}

QVariant TableModel::headerData(int section, Qt::Orientation o, int role) const {
    if (role == Qt::DisplayRole && o == Qt::Horizontal && section < headers_.size()) return headers_[section];
    return {};
}

QHash<int, QByteArray> TableModel::roleNames() const {
    return {{Qt::DisplayRole, "display"}, {ColorRole, "cellColor"}, {KindRole, "kind"}, {CheckedRole, "checked"},
            {DimRole, "dim"}, {RowKeyRole, "rowKey"}, {SelectedRole, "selected"}};
}

void TableModel::set(const QStringList& headers, std::vector<std::vector<Cell>> rows, std::vector<int> keys, const QList<int>& widths) {
    bool reset = headers != headers_ || rows.size() != rows_.size();
    if (reset) beginResetModel();
    headers_ = headers;
    widths_ = widths;
    rows_ = std::move(rows);
    keys_ = std::move(keys);
    if (reset) endResetModel();
    else if (!rows_.empty()) emit dataChanged(index(0, 0), index((int)rows_.size() - 1, (int)headers_.size() - 1));
    emit changed();
}

void TableModel::setSelectedKey(int key) {
    if (key == sel_key_) return;
    sel_key_ = key;
    if (!rows_.empty()) emit dataChanged(index(0, 0), index((int)rows_.size() - 1, (int)headers_.size() - 1), {SelectedRole});
    emit changed();
}

int TableModel::rowOfKey(int key) const {
    for (size_t r = 0; r < keys_.size(); ++r) if (keys_[r] == key) return (int)r;
    return -1;
}

void TableModel::sort(int col, bool asc) {
    if (rows_.empty() || col < 0 || col >= (int)headers_.size()) return;
    std::vector<size_t> ord(rows_.size());
    std::iota(ord.begin(), ord.end(), 0);
    std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
        double ka = rows_[a][col].key, kb = rows_[b][col].key;
        return asc ? ka < kb : ka > kb;
    });
    std::vector<std::vector<Cell>> r2;
    std::vector<int> k2;
    for (size_t o : ord) { r2.push_back(std::move(rows_[o])); k2.push_back(keys_[o]); }
    rows_ = std::move(r2);
    keys_ = std::move(k2);
    emit dataChanged(index(0, 0), index((int)rows_.size() - 1, (int)headers_.size() - 1));
    emit changed();
}

}  // namespace hz
