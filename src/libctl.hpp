// Library page (QML: `lib`): projects (folders under the library root), recordings with thumbnails and metadata,
// details + saved measurements of the selected recording, file actions, background stats / thumbnails, and batch runs
// of a saved measurement (recipe) over several recordings.
#pragma once
#include <QObject>
#include <QTimer>
#include <QVariant>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "library.hpp"

namespace evs {

class LibraryController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString root READ root NOTIFY rootChanged)
    Q_PROPERTY(QVariantList projects READ projects NOTIFY projectsChanged)
    Q_PROPERTY(QString project READ project NOTIFY projectChanged)
    Q_PROPERTY(QString projectDescription READ projectDescription NOTIFY projectChanged)
    Q_PROPERTY(QVariantList clips READ clips NOTIFY clipsChanged)
    Q_PROPERTY(int totalClips READ totalClips NOTIFY clipsChanged)
    Q_PROPERTY(QString selected READ selected NOTIFY detailChanged)
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY detailChanged)
    Q_PROPERTY(QStringList checked READ checked NOTIFY checkedChanged)
    Q_PROPERTY(QString filter READ filter NOTIFY clipsChanged)
    Q_PROPERTY(QString sort READ sort NOTIFY clipsChanged)
    Q_PROPERTY(int pendingStats READ pendingStats NOTIFY statsChanged)
    Q_PROPERTY(QVariantMap batch READ batch NOTIFY batchChanged)
public:
    LibraryController(const QString& root, QObject* parent = nullptr);
    ~LibraryController() override;

    QString root() const { return lib_.root; }
    QVariantList projects() const { return projects_; }
    QString project() const { return project_; }
    QString projectDescription() const { return lib_.projectDescription(project_); }
    QVariantList clips() const { return view_; }
    int totalClips() const { return (int)all_.size(); }
    QString selected() const { return selected_; }
    QVariantMap detail() const { return detail_; }
    QStringList checked() const { return checked_; }
    QString filter() const { return filter_; }
    QString sort() const { return sort_; }
    int pendingStats() const { return pendingStats_.load(); }
    QVariantMap batch() const { return batch_; }

    Q_INVOKABLE void setRoot(const QString& path);
    Q_INVOKABLE void setProject(const QString& name);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool createProject(const QString& name);
    Q_INVOKABLE void setProjectDescription(const QString& text);
    Q_INVOKABLE void select(const QString& path);
    Q_INVOKABLE void toggleChecked(const QString& path);
    Q_INVOKABLE void checkAll(bool on);
    Q_INVOKABLE void setFilter(const QString& text);
    Q_INVOKABLE void setSort(const QString& key);
    Q_INVOKABLE void setDescription(const QString& path, const QString& text);
    Q_INVOKABLE bool rename(const QString& path, const QString& newName);
    Q_INVOKABLE bool moveTo(const QString& path, const QString& project);
    Q_INVOKABLE bool trash(const QString& path);
    Q_INVOKABLE void reveal(const QString& path) const;
    Q_INVOKABLE void regenerate(const QString& path);       // stats + thumbnail again
    Q_INVOKABLE QVariantList recipes() const;               // saved measurements in the current project
    Q_INVOKABLE void runBatch(const QString& recipeDir, const QString& name);
    Q_INVOKABLE void cancelBatch();
    Q_INVOKABLE QString urlToPath(const QVariant& url) const;
    Q_INVOKABLE QString pathToUrl(const QString& path) const;

    // from the other pages
    void clipChanged(const QString& rawPath);               // new recording / saved measurement

signals:
    void rootChanged();
    void projectsChanged();
    void projectChanged();
    void clipsChanged();
    void detailChanged();
    void checkedChanged();
    void statsChanged();
    void batchChanged();
    void toast(const QString& text);
    void openInAnalyse(const QString& path);

private:
    void scanProjects();
    void loadClips();
    void applyView();
    void updateDetail();
    void queueStats(const QString& raw, bool front = false);
    void statsWorker();
    QVariantMap toVariant(const ClipInfo& c) const;

    Library lib_;
    QString project_, selected_, filter_, sort_ = "date";
    QVariantList projects_, view_;
    std::vector<ClipInfo> all_;
    QVariantMap detail_;
    QStringList checked_;
    QTimer coalesce_;
    // stats / thumbnails
    std::thread sth_;
    std::mutex smx_;
    std::condition_variable scv_;
    std::deque<QString> squeue_;
    bool squit_ = false;
    std::atomic<bool> scancel_{false};
    std::atomic<int> pendingStats_{0};
    // batch
    std::thread bth_;
    std::atomic<bool> bcancel_{false};
    QVariantMap batch_;
};

}  // namespace evs
