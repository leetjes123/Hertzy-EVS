// Generic read-mostly table for QML TableView: text cells with optional colour, sort key and checkbox.
#pragma once
#include <QAbstractTableModel>
#include <QColor>
#include <QStringList>
#include <vector>

namespace hz {

struct Cell {
    QString text;
    QColor color;          // invalid = default text colour
    double key = 0;        // sort key
    int kind = 0;          // 0 text, 1 checkbox
    bool checked = false;
    bool dim = false;      // secondary text
};

class TableModel : public QAbstractTableModel {
    Q_OBJECT
    Q_PROPERTY(int rows READ rowCountP NOTIFY changed)
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY changed)
public:
    enum Roles { ColorRole = Qt::UserRole + 1, KindRole, CheckedRole, DimRole, RowKeyRole, SelectedRole };
    using QAbstractTableModel::QAbstractTableModel;
    int rowCount(const QModelIndex& = {}) const override { return (int)rows_.size(); }
    int columnCount(const QModelIndex& = {}) const override { return (int)headers_.size(); }
    QVariant data(const QModelIndex& i, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void set(const QStringList& headers, std::vector<std::vector<Cell>> rows, std::vector<int> keys, const QList<int>& widths);
    void setSelectedKey(int key);
    Q_INVOKABLE int rowKey(int row) const { return row >= 0 && row < (int)keys_.size() ? keys_[row] : -1; }
    Q_INVOKABLE int columnWidth(int col) const { return col < widths_.size() ? widths_[col] : 80; }
    Q_INVOKABLE QString header(int col) const { return col < headers_.size() ? headers_[col] : QString(); }
    Q_INVOKABLE int rowOfKey(int key) const;
    int rowCountP() const { return rowCount(); }
    int selectedRow() const { return rowOfKey(sel_key_); }
    void sort(int col, bool asc);

signals:
    void changed();

private:
    QStringList headers_;
    QList<int> widths_;
    std::vector<std::vector<Cell>> rows_;
    std::vector<int> keys_;
    int sel_key_ = -1;
};

}  // namespace hz
