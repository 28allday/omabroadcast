#pragma once

#include <QObject>
#include <QString>
#include <QRectF>
#include <QSizeF>
#include <QTimer>
#include <QSet>

#include <functional>
#include <vector>

#include "core/scene.h"

// The scene collection being worked on, every change to it, and its history.
//
// Each edit is recorded as the whole collection before and after, so undo is
// exact (a reload and an undo give back the same bytes) and needs no inverse
// for each kind of change. A gesture — dragging an item, a slider — passes a
// merge key, and its many small edits land as one step until endGesture().
// Switching scenes and selecting are not edits: undo never moves the live
// scene under a broadcast.
class Production : public QObject {
    Q_OBJECT

public:
    explicit Production(QObject *parent = nullptr);

    const Collection &collection() const { return m_collection; }
    const Scene *currentScene() const { return m_collection.current(); }

    // A new collection, or the one in this file. Clears the history.
    void reset(const Collection &collection);
    bool load(const QString &path);
    // Saves now; edits also save themselves shortly after they happen.
    bool save();
    void setPath(const QString &path) { m_path = path; m_notOverPath = false; }
    // As setPath, for a file there that does not read: it is not written over,
    // and saving works once it has gone.
    void setPathOnceGone(const QString &path) { m_path = path; m_notOverPath = true; }
    QString path() const { return m_path; }
    // Whether there is anything the file does not have yet.
    bool unsaved() const { return m_unsaved; }
    quint64 generation() const { return m_generation; }

    // Scenes
    // `show`: it becomes the current studio (as from the app).
    QString addScene(const QString &name, bool show = true);
    void removeScene(const QString &id);
    void renameScene(const QString &id, const QString &name);
    QString duplicateScene(const QString &id);
    void moveScene(const QString &id, int toIndex);  // among studios, groups not counted
    void setCurrentScene(const QString &id);

    // Sources and items. addSource makes the source and puts it in the
    // current scene; the item id comes back.
    QString addSource(const QString &kind, const QString &name, const QVariantMap &settings,
                      const Transform &transform);
    QString addExistingSource(const QString &sourceId, const Transform &transform);
    // A collection-local clipboard. Groups get private item trees in either
    // mode; duplicates also copy leaf sources and nested studios.
    bool copyItems(const QStringList &itemIds);
    bool canPasteItems(bool duplicate) const;
    QStringList pasteItems(bool duplicate);
    // Another studio as a source here; empty (and nothing added) when it would
    // show itself, directly or through others.
    QString addStudioSource(const QString &studioId, const Transform &transform);
    // Puts items (all in one studio or group) into a new group whose box is
    // `box` in their coordinates; nothing moves on screen. The group's item id.
    QString groupItems(const QStringList &itemIds, const QRectF &box);
    // The reverse; `sourceSize` gives a source's pixel size. The freed items.
    QStringList ungroup(const QString &itemId, const std::function<QSizeF(const QString &sourceId)> &sourceSize);
    // Several at once, as one step.
    QStringList ungroupItems(const QStringList &itemIds, const std::function<QSizeF(const QString &sourceId)> &sourceSize);
    void removeItem(const QString &itemId);
    // Several at once, as one step.
    void removeItems(const QStringList &itemIds);
    void setItemVisible(const QString &itemId, bool visible);
    void setItemLocked(const QString &itemId, bool locked);
    // How an item is shown (show) or hidden: a transition id, empty for at once.
    void setItemTransition(const QString &itemId, bool show, const QString &transition, int ms);
    void moveItem(const QString &itemId, int toIndex);
    void renameSource(const QString &sourceId, const QString &name);
    void setSourceSettings(const QString &sourceId, const QVariantMap &settings, const QString &mergeKey = {});
    // Several sources' settings as one step (relinking moved files).
    void setSourcesSettings(const QHash<QString, QVariantMap> &settings, const QString &label);
    // The collection's own name; not an undo step.
    void setName(const QString &name);
    // Keys for this collection's actions; not an undo step either.
    void setHotkeys(const QVariantMap &hotkeys);
    // The automatic studio switcher's rules; not an undo step.
    void setSwitcher(const QVariantMap &switcher);

    // Transitions (core/transitions.h). Made ones are edits; which one is
    // chosen, and for how long, is a live control like the studio, not an
    // undo step.
    QString addTransition(const QString &name, const QVariantMap &settings);
    void removeTransition(const QString &id);
    void renameTransition(const QString &id, const QString &name);
    void setTransitionSettings(const QString &id, const QVariantMap &settings, const QString &mergeKey = {});
    void setSceneTransition(const QString &sceneId, const QString &transition, int durationMs);
    void setQuickTransitions(const QVector<QuickTransition> &quick);
    void setCurrentTransition(const QString &id, int durationMs);
    void setTransform(const QString &itemId, const Transform &transform, const QString &mergeKey = {});
    // Several items at once: one step (a drag of a multiple selection).
    void setTransforms(const QHash<QString, Transform> &transforms, const QString &mergeKey = {});
    void endGesture();

    // Scales every item in every scene (and in the history) to a new canvas
    // size, so a layout keeps its proportions, and keeps that size with it. Not
    // an undo step.
    void rescaleCanvas(int fromWidth, int fromHeight, int toWidth, int toHeight);
    // The same from the size the layout is at, whichever collection is open and
    // however the canvas came to be this size. A collection that does not say
    // what size it is at is taken to be at this one.
    void followCanvas(int width, int height);

    // History
    bool canUndo() const { return m_undo > 0; }
    bool canRedo() const { return m_undo < int(m_steps.size()); }
    QString undoText() const { return canUndo() ? m_steps[m_undo - 1].label : QString(); }
    QString redoText() const { return canRedo() ? m_steps[m_undo].label : QString(); }
    void undo();
    void redo();

    // Every collection still held here: the one open, each step of the history
    // (what Undo and Redo can bring back) and what was copied.
    void forEachKept(const std::function<void(const Collection &)> &visit) const;

    // Selection, in the current scene. Not part of the history.
    QStringList selection() const { return m_selection; }
    void select(const QStringList &itemIds);

signals:
    void changed();           // the collection changed (edit, undo, load, scene switch)
    void selectionChanged();
    void historyChanged();
    void clipboardChanged();
    // The timed save after an edit could not write the file.
    void saveFailed(const QString &path);

private:
    quint64 m_generation = 0; // changes only when a different collection is loaded/reset
    struct Step {
        QString label;
        QString mergeKey;
        Collection before, after;
    };
    // Runs `change` on a copy; if it changed anything, records the step.
    void edit(const QString &label, const QString &mergeKey, const std::function<void(Collection &)> &change);
    void commit(const Collection &next, bool notifyHistory);
    void saveSoon();
    static Scene cloneScene(Collection &c, const Scene &from);
    void pruneSelection();
    void validateClipboardReferences();

    Collection m_clipboard;
    QVector<SceneItem> m_copiedItems;
    QSet<QString> m_referenceSources;
    bool m_referencesLost = false;

    Collection m_collection;
    std::vector<Step> m_steps;
    int m_undo = 0;               // steps [0, m_undo) are applied
    bool m_gestureOpen = false;
    QStringList m_selection;
    QString m_path;
    bool m_notOverPath = false;   // the file at m_path does not read: leave it
    bool m_unsaved = false;
    QTimer m_saveTimer;
};
