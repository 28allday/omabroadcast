#include "production.h"
#include "core/transitions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kHistoryLimit = 200;
constexpr int kSaveDelayMs = 500;
}  // namespace

Production::Production(QObject *parent) : QObject(parent) {
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { if (!save() && m_unsaved) emit saveFailed(m_path); });
}

void Production::reset(const Collection &collection) {
    ++m_generation;
    m_clipboard = {};
    m_copiedItems.clear();
    m_referenceSources.clear();
    m_referencesLost = false;
    m_collection = collection;
    m_unsaved = false;
    // At least one studio to be live (a file of groups alone has none).
    if (std::none_of(m_collection.scenes.cbegin(), m_collection.scenes.cend(), [](const Scene &s) { return !s.group; })) {
        QString id = m_collection.makeId(QLatin1Char('c'));
        while (m_collection.scene(id)) id = m_collection.makeId(QLatin1Char('c'));  // one not taken by a group
        Scene first{id, tr("Studio 1"), {}};
        m_collection.scenes.append(first);
        m_collection.currentScene = first.id;
    }
    if (!m_collection.current() || m_collection.current()->group)
        m_collection.currentScene = std::find_if(m_collection.scenes.cbegin(), m_collection.scenes.cend(),
                                                 [](const Scene &s) { return !s.group; })->id;
    m_steps.clear();
    m_undo = 0;
    m_gestureOpen = false;
    m_selection.clear();
    emit changed();
    emit selectionChanged();
    emit historyChanged();
    emit clipboardChanged();
}

bool Production::load(const QString &path) {
    setPath(path);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    // Written by a newer version: what this one does not know would be lost
    // at the next save, so the file is kept beside it first (not as .json, which
    // would list it as another collection).
    if (const int version = doc.object().value(QStringLiteral("version")).toInt(1); version > 1) {
        const QString kept = path + QStringLiteral(".v%1.kept").arg(version);
        if (!QFileInfo::exists(kept)) QFile::copy(path, kept);
    }
    Collection loaded = Collection::fromJson(doc.object());
    const bool protectedNow = loaded.protectNetworkStreams();
    reset(loaded);
    // The phone is told it once: the next run must not make another.
    if (protectedNow) saveSoon();
    return true;
}

bool Production::save() {
    if (m_path.isEmpty()) return false;
    if (m_notOverPath) {
        if (QFile::exists(m_path)) return false;
        m_notOverPath = false;
    }
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (m_collection.holdsPassphrase()) f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(m_collection.toJson()).toJson(QJsonDocument::Indented));
    if (!f.commit()) return false;
    m_unsaved = false;
    return true;
}

void Production::saveSoon() {
    m_unsaved = true;
    if (!m_path.isEmpty()) m_saveTimer.start();
}

void Production::commit(const Collection &next, bool notifyHistory) {
    m_collection = next;
    validateClipboardReferences();
    pruneSelection();
    saveSoon();
    emit changed();
    emit clipboardChanged(); // the destination may now introduce a studio loop
    if (notifyHistory) emit historyChanged();
}

void Production::edit(const QString &label, const QString &mergeKey,
                      const std::function<void(Collection &)> &change) {
    Collection next = m_collection;
    change(next);
    next.prune();
    if (next == m_collection) return;
    const bool merge = m_gestureOpen && !mergeKey.isEmpty() && m_undo > 0 && m_undo == int(m_steps.size()) &&
                       m_steps.back().mergeKey == mergeKey;
    if (merge) {
        m_steps.back().after = next;
    } else {
        m_steps.resize(size_t(m_undo));  // a new edit drops what could have been redone
        m_steps.push_back({label, mergeKey, m_collection, next});
        if (int(m_steps.size()) > kHistoryLimit) m_steps.erase(m_steps.begin());
        m_undo = int(m_steps.size());
    }
    m_gestureOpen = !mergeKey.isEmpty();
    commit(next, !merge);
}

void Production::endGesture() { m_gestureOpen = false; }

void Production::forEachKept(const std::function<void(const Collection &)> &visit) const {
    visit(m_collection);
    for (const Step &s : m_steps) {
        visit(s.before);
        visit(s.after);
    }
    visit(m_clipboard);
}

void Production::undo() {
    if (!canUndo()) return;
    m_gestureOpen = false;
    --m_undo;
    Collection back = m_steps[size_t(m_undo)].before;
    back.currentScene = back.scene(m_collection.currentScene) ? m_collection.currentScene : back.currentScene;
    // Ids are never given twice: hotkeys, restore tokens and transitions name
    // what had one before.
    back.nextId = std::max(back.nextId, m_collection.nextId);
    commit(back, true);
}

void Production::redo() {
    if (!canRedo()) return;
    m_gestureOpen = false;
    Collection forward = m_steps[size_t(m_undo)].after;
    ++m_undo;
    forward.currentScene = forward.scene(m_collection.currentScene) ? m_collection.currentScene : forward.currentScene;
    forward.nextId = std::max(forward.nextId, m_collection.nextId);
    commit(forward, true);
}

void Production::select(const QStringList &itemIds) {
    if (itemIds == m_selection) return;
    m_selection = itemIds;
    pruneSelection();
    emit selectionChanged();
}

void Production::pruneSelection() {
    const QStringList before = m_selection;
    m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(),
                                     [this](const QString &id) { return !m_collection.findItem(id); }),
                      m_selection.end());
    if (m_selection != before) emit selectionChanged();
}

// ---------------------------------------------------------------- scenes

QString Production::addScene(const QString &name, bool show) {
    QString id;
    edit(tr("Add studio"), {}, [&](Collection &c) {
        id = c.makeId(QLatin1Char('c'));
        c.scenes.append({id, name, {}});
    });
    if (show) setCurrentScene(id);
    return id;
}

void Production::removeScene(const QString &id) {
    int studios = 0;
    for (const Scene &s : m_collection.scenes) studios += s.group ? 0 : 1;
    if (studios <= 1) return;  // there is always a studio to show
    edit(tr("Remove studio"), {}, [&](Collection &c) {
        const int at = int(std::find_if(c.scenes.begin(), c.scenes.end(), [&](const Scene &s) { return s.id == id; }) -
                           c.scenes.begin());
        if (at >= c.scenes.size() || c.scenes[at].group) return;
        c.scenes.removeAt(at);
        if (c.currentScene == id) {
            // The nearest studio takes over; what showed this one goes in prune().
            c.currentScene.clear();
            for (int d = 0; d < c.scenes.size() && c.currentScene.isEmpty(); ++d)
                for (int i : {at - 1 - d, at + d})
                    if (i >= 0 && i < c.scenes.size() && !c.scenes[i].group && c.currentScene.isEmpty())
                        c.currentScene = c.scenes[i].id;
        }
    });
}

void Production::renameScene(const QString &id, const QString &name) {
    edit(tr("Rename studio"), {}, [&](Collection &c) {
        if (Scene *s = c.scene(id)) s->name = name;
    });
}

QString Production::duplicateScene(const QString &id) {
    QString copyId;
    edit(tr("Duplicate studio"), {}, [&](Collection &c) {
        const Scene *from = c.scene(id);
        if (!from) return;
        const int at = int(c.scenes.indexOf(*from));
        Scene copy = cloneScene(c, *from);
        copyId = copy.id;
        copy.name = tr("%1 copy").arg(copy.name);
        c.scenes.insert(at + 1, copy);
    });
    if (!copyId.isEmpty()) setCurrentScene(copyId);
    return copyId;
}

Scene Production::cloneScene(Collection &c, const Scene &from) {
    // Groups are private to their studio: a copy gets its own, so editing one
    // never changes the other. Everything else is shared.
    Scene copy = from;
    copy.id = c.makeId(QLatin1Char('c'));
    for (SceneItem &i : copy.items) {
        i.id = c.makeId(QLatin1Char('i'));
        const SourceDef *src = c.source(i.source);
        if (!src || src->kind != QLatin1String("group")) continue;
        const Scene *inner = c.scene(c.shownStudio(*src));
        if (!inner) continue;
        SourceDef group = *src; // recursion can reallocate the source vector
        Scene innerCopy = cloneScene(c, *inner);
        c.scenes.append(innerCopy);
        group.id = c.makeId(QLatin1Char('s'));
        group.settings.insert(QStringLiteral("studio"), innerCopy.id);
        c.sources.append(group);
        i.source = group.id;
    }
    return copy;
}

void Production::moveScene(const QString &id, int toIndex) {
    // `toIndex` counts studios only: groups' boxes sit among them unseen.
    edit(tr("Reorder studios"), {}, [&](Collection &c) {
        qsizetype from = -1;
        for (qsizetype i = 0; i < c.scenes.size(); ++i)
            if (c.scenes[i].id == id && !c.scenes[i].group) from = i;
        if (from < 0) return;
        const Scene moving = c.scenes.takeAt(from);
        qsizetype at = c.scenes.size(), seen = 0;
        for (qsizetype i = 0; i < c.scenes.size(); ++i) {
            if (c.scenes[i].group) continue;
            if (seen++ == std::max(0, toIndex)) {
                at = i;
                break;
            }
        }
        c.scenes.insert(at, moving);
    });
}

void Production::setCurrentScene(const QString &id) {
    if (id == m_collection.currentScene || !m_collection.scene(id) || m_collection.scene(id)->group) return;
    Collection next = m_collection;
    next.currentScene = id;
    // Every step's snapshots follow, so undo never jumps scenes.
    for (Step &s : m_steps) {
        if (s.before.scene(id)) s.before.currentScene = id;
        if (s.after.scene(id)) s.after.currentScene = id;
    }
    m_selection.clear();
    commit(next, false);
    emit selectionChanged();
}

// --------------------------------------------------------- sources, items

QString Production::addSource(const QString &kind, const QString &name, const QVariantMap &settings,
                              const Transform &transform) {
    QString itemId;
    edit(tr("Add %1").arg(name), {}, [&](Collection &c) {
        Scene *scene = c.current();
        if (!scene) return;
        const QString sourceId = c.makeId(QLatin1Char('s'));
        c.sources.append({sourceId, kind, name, settings});
        itemId = c.makeId(QLatin1Char('i'));
        scene->items.append({itemId, sourceId, transform, true, false});
    });
    if (!itemId.isEmpty()) select({itemId});
    return itemId;
}

QString Production::addExistingSource(const QString &sourceId, const Transform &transform) {
    QString itemId;
    edit(tr("Add source"), {}, [&](Collection &c) {
        Scene *scene = c.current();
        const auto *source = c.source(sourceId);
        if (!scene || !source || source->kind == "group") return;
        const auto shown = c.shownStudio(*source);
        if (source->kind == "studio" && (shown.isEmpty() || !c.scene(shown) || c.scene(shown)->group)) return;
        if (!shown.isEmpty() && c.shows(shown, scene->id)) return;
        itemId = c.makeId(QLatin1Char('i'));
        scene->items.append({itemId, sourceId, transform, true, false});
    });
    if (!itemId.isEmpty()) select({itemId});
    return itemId;
}

bool Production::copyItems(const QStringList &itemIds) {
    // Walk only the visible tree, omitting selected descendants of a selected
    // group. A selection spanning different coordinate systems is ambiguous.
    QVector<SceneItem> items;
    QString owner;
    bool valid = true;
    QSet<QString> visiting;
    std::function<void(const Scene &)> collect = [&](const Scene &scene) {
        if (visiting.contains(scene.id)) { valid = false; return; }
        visiting.insert(scene.id);
        for (const auto &item : scene.items) {
            if (itemIds.contains(item.id)) {
                if (!owner.isEmpty() && owner != scene.id) valid = false;
                owner = scene.id; items << item;
            } else if (const auto *source = m_collection.source(item.source); source && source->kind == "group") {
                if (const auto *group = m_collection.scene(m_collection.shownStudio(*source))) collect(*group);
            }
        }
        visiting.remove(scene.id);
    };
    if (const auto *current = currentScene()) collect(*current);
    if (!valid || items.isEmpty()) return false;

    QSet<QString> references, checked;
    std::function<bool(const QString &)> checkSource = [&](const QString &id) {
        const auto *source = m_collection.source(id);
        if (!source) return false;
        const auto shown = m_collection.shownStudio(*source);
        if (shown.isEmpty()) return source->kind != "studio" && source->kind != "group";
        if (visiting.contains(shown)) return false;
        const auto *scene = m_collection.scene(shown);
        if (!scene || scene->group != (source->kind == "group")) return false;
        if (checked.contains(shown)) return true;
        visiting.insert(shown);
        for (const auto &item : scene->items) if (!checkSource(item.source)) return false;
        visiting.remove(shown); checked.insert(shown);
        return true;
    };
    for (const auto &item : items) if (!checkSource(item.source)) return false;
    std::function<void(const QString &)> referenceSource = [&](const QString &id) {
        if (references.contains(id)) return;
        references.insert(id);
        const auto *source = m_collection.source(id);
        if (source->kind == "group")
            for (const auto &item : m_collection.scene(m_collection.shownStudio(*source))->items) referenceSource(item.source);
    };
    for (const auto &item : items) referenceSource(item.source);
    m_clipboard = m_collection;
    m_copiedItems = items;
    m_referenceSources = references;
    m_referencesLost = false;
    emit clipboardChanged();
    return true;
}

void Production::validateClipboardReferences() {
    // IDs can be reused after undo and a new edit. Once a copied source has
    // disappeared, a later object with that ID must never become its reference.
    for (const auto &id : m_referenceSources)
        if (!m_collection.source(id)) { m_referencesLost = true; break; }
}

bool Production::canPasteItems(bool duplicate) const {
    if (m_copiedItems.isEmpty() || !currentScene()) return false;
    if (duplicate) return true;
    if (m_referencesLost) return false;
    for (const auto &id : m_referenceSources) {
        const auto *source = m_collection.source(id);
        if (!source) return false;
        if (source->kind == "studio") {
            const auto shown = m_collection.shownStudio(*source);
            const auto *scene = m_collection.scene(shown);
            if (!scene || scene->group || m_collection.shows(shown, m_collection.currentScene)) return false;
        }
    }
    return true;
}

QStringList Production::pasteItems(bool duplicate) {
    if (!canPasteItems(duplicate)) return {};
    QStringList pasted;
    edit(duplicate ? tr("Paste duplicate sources") : tr("Paste source references"), {}, [&](Collection &c) {
        const auto destination = c.currentScene;
        QHash<QString, QString> sources, studios;
        auto sourceName = [&](const QString &name) {
            const QString base = tr("%1 copy").arg(name);
            auto exists = [&](const QString &candidate) {
                return std::any_of(c.sources.cbegin(), c.sources.cend(), [&](const SourceDef &s) { return s.name == candidate; });
            };
            QString out = base;
            for (int n = 2; exists(out); ++n) out = tr("%1 %2").arg(base).arg(n);
            return out;
        };
        std::function<QString(const QString &)> copySource;
        std::function<QString(const QString &)> copyScene = [&](const QString &id) {
            Scene scene = *m_clipboard.scene(id);
            if (!scene.group && studios.contains(id)) return studios.value(id);
            scene.id = c.makeId('c');
            if (!scene.group) {
                studios[id] = scene.id;
                const auto base = tr("%1 copy").arg(scene.name);
                scene.name = base;
                auto exists = [&] { return std::any_of(c.scenes.cbegin(), c.scenes.cend(), [&](const Scene &s) { return s.name == scene.name; }); };
                for (int n = 2; exists(); ++n) scene.name = tr("%1 %2").arg(base).arg(n);
            }
            // Reserve the scene before descent so names are unique throughout
            // a copied graph. Never retain pointers across vector appends.
            c.scenes.append(scene);
            for (auto &item : scene.items) {
                item.id = c.makeId('i'); item.source = copySource(item.source);
            }
            *c.scene(scene.id) = scene;
            return scene.id;
        };
        copySource = [&](const QString &id) {
            SourceDef source = *m_clipboard.source(id);
            if (!duplicate && source.kind != "group") return id;
            if (source.kind != "group" && sources.contains(id)) return sources.value(id);
            source.id = c.makeId('s'); source.name = sourceName(source.name);
            if (source.kind != "group") sources[id] = source.id;
            c.sources.append(source);
            const auto shown = m_clipboard.shownStudio(source);
            if (!shown.isEmpty()) source.settings["studio"] = copyScene(shown);
            *c.source(source.id) = source;
            return source.id;
        };
        QVector<SceneItem> items;
        for (auto item : m_copiedItems) {
            item.id = c.makeId('i'); item.source = copySource(item.source);
            pasted << item.id; items << item;
        }
        c.scene(destination)->items += items;
    });
    select(pasted);
    return pasted;
}

void Production::removeItem(const QString &itemId) {
    // A source no item shows any more goes with it (and a group with its box).
    edit(tr("Remove source"), {}, [&](Collection &c) {
        Scene *owner = nullptr;
        if (!c.findItem(itemId, &owner)) return;
        owner->items.removeIf([&](const SceneItem &i) { return i.id == itemId; });
    });
}

void Production::removeItems(const QStringList &itemIds) {
    edit(itemIds.size() > 1 ? tr("Remove sources") : tr("Remove source"), {}, [&](Collection &c) {
        for (Scene &s : c.scenes) s.items.removeIf([&](const SceneItem &i) { return itemIds.contains(i.id); });
    });
}

void Production::setItemVisible(const QString &itemId, bool visible) {
    edit(visible ? tr("Show source") : tr("Hide source"), {}, [&](Collection &c) {
        if (SceneItem *i = c.findItem(itemId)) i->visible = visible;
    });
}

void Production::setItemLocked(const QString &itemId, bool locked) {
    edit(locked ? tr("Lock source") : tr("Unlock source"), {}, [&](Collection &c) {
        if (SceneItem *i = c.findItem(itemId)) i->locked = locked;
    });
}

void Production::setItemTransition(const QString &itemId, bool show, const QString &transition, int ms) {
    edit(show ? tr("Change show transition") : tr("Change hide transition"), {}, [&](Collection &c) {
        if (SceneItem *i = c.findItem(itemId)) {
            (show ? i->showTransition : i->hideTransition) = transition;
            (show ? i->showMs : i->hideMs) = std::clamp(ms, 0, 20000);
        }
    });
}

void Production::moveItem(const QString &itemId, int toIndex) {
    edit(tr("Reorder sources"), {}, [&](Collection &c) {
        Scene *s = nullptr;
        if (!c.findItem(itemId, &s)) return;
        for (int i = 0; i < s->items.size(); ++i)
            if (s->items[i].id == itemId) {
                s->items.move(i, std::clamp(toIndex, 0, int(s->items.size()) - 1));
                return;
            }
    });
}

void Production::renameSource(const QString &sourceId, const QString &name) {
    edit(tr("Rename source"), {}, [&](Collection &c) {
        if (SourceDef *s = c.source(sourceId)) s->name = name;
    });
}

void Production::setSourceSettings(const QString &sourceId, const QVariantMap &settings, const QString &mergeKey) {
    edit(tr("Change source"), mergeKey, [&](Collection &c) {
        if (SourceDef *s = c.source(sourceId)) s->settings = settings;
    });
}

void Production::setTransform(const QString &itemId, const Transform &transform, const QString &mergeKey) {
    edit(tr("Move source"), mergeKey, [&](Collection &c) {
        if (SceneItem *i = c.findItem(itemId)) i->transform = transform;
    });
}

QString Production::addStudioSource(const QString &studioId, const Transform &transform) {
    const Scene *target = m_collection.scene(studioId);
    // A studio cannot show itself, directly or through the studios it shows.
    if (!target || target->group || m_collection.shows(studioId, m_collection.currentScene)) return {};
    for (const SourceDef &s : m_collection.sources)
        if (s.kind == QLatin1String("studio") && m_collection.shownStudio(s) == studioId)
            return addExistingSource(s.id, transform);
    return addSource(QStringLiteral("studio"), target->name, {{QStringLiteral("studio"), studioId}}, transform);
}

QString Production::groupItems(const QStringList &itemIds, const QRectF &box) {
    QString groupItem;
    edit(tr("Group"), {}, [&](Collection &c) {
        Scene *owner = nullptr;
        if (itemIds.isEmpty() || !c.findItem(itemIds.first(), &owner) || box.isEmpty()) return;
        // Every item from the same studio or group; kept in stacking order.
        // Items scaled far past the canvas make a big box: no bigger than a picture can be.
        Scene inner{c.makeId(QLatin1Char('c')), tr("Group"), {}, true, int(std::clamp(std::ceil(box.width()), 1.0, double(kMaxGroupSize))),
                    int(std::clamp(std::ceil(box.height()), 1.0, double(kMaxGroupSize)))};
        int at = -1;
        for (int i = 0; i < owner->items.size(); ++i) {
            if (!itemIds.contains(owner->items[i].id)) continue;
            SceneItem moved = owner->items[i];
            moved.transform.x -= box.x();
            moved.transform.y -= box.y();
            inner.items.append(moved);
            at = i;  // the group sits where its topmost item was
        }
        if (inner.items.isEmpty()) return;
        owner->items.removeIf([&](const SceneItem &i) { return itemIds.contains(i.id); });
        at -= int(inner.items.size()) - 1;
        const QString source = c.makeId(QLatin1Char('s'));
        int n = 1;
        for (const SourceDef &s : c.sources) n += s.kind == QLatin1String("group") ? 1 : 0;
        c.sources.append({source, QStringLiteral("group"), tr("Group %1").arg(n), {{QStringLiteral("studio"), inner.id}}});
        Transform t;
        t.x = box.x();
        t.y = box.y();
        groupItem = c.makeId(QLatin1Char('i'));
        owner->items.insert(std::clamp(at, 0, int(owner->items.size())), {groupItem, source, t, true, false});
        // Last: adding a studio may move the others, and `owner` points into them.
        c.scenes.append(inner);
    });
    if (!groupItem.isEmpty()) select({groupItem});
    return groupItem;
}

QStringList Production::ungroup(const QString &itemId, const std::function<QSizeF(const QString &sourceId)> &sourceSize) {
    return ungroupItems({itemId}, sourceSize);
}

QStringList Production::ungroupItems(const QStringList &itemIds, const std::function<QSizeF(const QString &sourceId)> &sourceSize) {
    QStringList freed;
    edit(tr("Ungroup"), {}, [&](Collection &c) {
        for (const QString &itemId : itemIds) {
            Scene *owner = nullptr;
            const SceneItem *groupItem = c.findItem(itemId, &owner);
            const SourceDef *src = groupItem ? c.source(groupItem->source) : nullptr;
            const Scene *inner = src && src->kind == QLatin1String("group") ? c.scene(c.shownStudio(*src)) : nullptr;
            if (!inner) continue;
            // Map through the actual content, including fitted bounds and source
            // crop/mirroring, rather than treating the outer box as a stretched source.
            const auto mapping = itemMapping(groupItem->transform, inner->width, inner->height);
            QVector<SceneItem> out;
            for (SceneItem i : inner->items) {
                const QSizeF size = sourceSize(i.source);
                const Outline o = itemOutline(i.transform, int(size.width()), int(size.height()));
                const auto placed = mappedOutline(mapping, o);
                i.transform.rotation = std::fmod(placed.rotation + 360.0, 360.0);
                if (mapping.determinant() < 0) {
                    i.transform.flipV = !i.transform.flipV;
                    const int a = int(i.transform.boundsAlignment);
                    i.transform.boundsAlignment = Transform::Alignment((2 - a / 3) * 3 + a % 3);
                }
                setItemBox(i.transform, int(size.width()), int(size.height()), placed.x, placed.y, placed.width, placed.height);
                out.append(i);
                freed.append(i.id);
            }
            const int at = int(owner->items.indexOf(*groupItem));
            owner->items.removeAt(at);
            for (int k = 0; k < out.size(); ++k) owner->items.insert(at + k, out[k]);
            // The emptied group goes now: until then its items would be found twice,
            // should the next one asked for be among them.
            c.prune();
        }
    });
    if (!freed.isEmpty()) select(freed);
    return freed;
}

void Production::setSourcesSettings(const QHash<QString, QVariantMap> &settings, const QString &label) {
    edit(label, {}, [&](Collection &c) {
        for (auto it = settings.begin(); it != settings.end(); ++it)
            if (SourceDef *s = c.source(it.key())) s->settings = it.value();
    });
}

QString Production::addTransition(const QString &name, const QVariantMap &settings) {
    QString id;
    edit(tr("Add transition"), {}, [&](Collection &c) {
        id = c.makeId('t');
        c.transitions.append({id, name, Transitions::normalized(settings)});
    });
    return id;
}

void Production::removeTransition(const QString &id) {
    edit(tr("Remove transition"), {}, [&](Collection &c) {
        c.transitions.removeIf([&](const TransitionDef &t) { return t.id == id; });
        for (Scene &s : c.scenes) {
            if (s.transition == id) s.transition.clear();
            // Sources shown or hidden through it are so at once instead.
            for (SceneItem &i : s.items) {
                if (i.showTransition == id) i.showTransition.clear();
                if (i.hideTransition == id) i.hideTransition.clear();
            }
        }
        c.quickTransitions.removeIf([&](const QuickTransition &q) { return q.transition == id; });
        if (c.currentTransition == id) c.currentTransition = QStringLiteral("fade");
    });
    // A choice is not history: an undone removal brings the transition back, not the choice.
    for (Step &s : m_steps)
        for (Collection *c : {&s.before, &s.after}) {
            c->currentTransition = m_collection.currentTransition;
            c->transitionMs = m_collection.transitionMs;
        }
}

void Production::renameTransition(const QString &id, const QString &name) {
    edit(tr("Rename transition"), {}, [&](Collection &c) {
        for (TransitionDef &t : c.transitions)
            if (t.id == id) t.name = name;
    });
}

void Production::setTransitionSettings(const QString &id, const QVariantMap &settings, const QString &mergeKey) {
    edit(tr("Change transition"), mergeKey, [&](Collection &c) {
        for (TransitionDef &t : c.transitions)
            if (t.id == id) t.settings = Transitions::normalized(settings);
    });
}

void Production::setSceneTransition(const QString &sceneId, const QString &transition, int durationMs) {
    edit(tr("Change studio transition"), {}, [&](Collection &c) {
        Scene *s = c.scene(sceneId);
        if (!s || s->group || (!transition.isEmpty() && !Transitions::exists(c, transition))) return;
        s->transition = transition;
        s->transitionMs = std::clamp(durationMs, 0, 20000);
    });
}

void Production::setQuickTransitions(const QVector<QuickTransition> &quick) {
    edit(tr("Change quick transitions"), {}, [&](Collection &c) {
        c.quickTransitions.clear();
        for (const QuickTransition &q : quick)
            if (Transitions::exists(c, q.transition)) c.quickTransitions.append({q.transition, std::clamp(q.durationMs, 0, 20000)});
    });
}

void Production::setCurrentTransition(const QString &id, int durationMs) {
    if (!Transitions::exists(m_collection, id)) return;
    durationMs = std::clamp(durationMs, 0, 20000);
    if (id == m_collection.currentTransition && durationMs == m_collection.transitionMs) return;
    // Every snapshot too, so undo never changes the choice.
    m_collection.currentTransition = id;
    m_collection.transitionMs = durationMs;
    for (Step &s : m_steps)
        for (Collection *c : {&s.before, &s.after})
            if (Transitions::exists(*c, id)) { c->currentTransition = id; c->transitionMs = durationMs; }
    saveSoon();
    emit changed();
}

void Production::setHotkeys(const QVariantMap &hotkeys) {
    if (hotkeys == m_collection.hotkeys) return;
    m_collection.hotkeys = hotkeys;
    for (Step &s : m_steps) s.before.hotkeys = s.after.hotkeys = hotkeys;
    saveSoon();
    emit changed();
}

void Production::setSwitcher(const QVariantMap &switcher) {
    if (switcher == m_collection.switcher) return;
    m_collection.switcher = switcher;
    for (Step &s : m_steps) s.before.switcher = s.after.switcher = switcher;
    saveSoon();
    emit changed();
}

void Production::setName(const QString &name) {
    // Every snapshot too, so undo never brings an old name back.
    m_collection.name = name;
    for (Step &s : m_steps) {
        s.before.name = name;
        s.after.name = name;
    }
    saveSoon();
    emit changed();
}

void Production::setTransforms(const QHash<QString, Transform> &transforms, const QString &mergeKey) {
    edit(tr("Move sources"), mergeKey, [&](Collection &c) {
        for (auto it = transforms.begin(); it != transforms.end(); ++it)
            if (SceneItem *i = c.findItem(it.key())) i->transform = it.value();
    });
}

void Production::followCanvas(int width, int height) {
    const bool known = m_collection.canvasWidth > 0 && m_collection.canvasHeight > 0;
    rescaleCanvas(known ? m_collection.canvasWidth : width, known ? m_collection.canvasHeight : height, width, height);
}

void Production::rescaleCanvas(int fromW, int fromH, int toW, int toH) {
    if (fromW <= 0 || fromH <= 0 || toW <= 0 || toH <= 0) return;
    const double fx = double(toW) / fromW, fy = double(toH) / fromH;
    auto scale = [&](Collection &c) {
        c.canvasWidth = toW;
        c.canvasHeight = toH;
        if (fromW == toW && fromH == toH) return;
        for (Scene &s : c.scenes) {
            if (s.group) {
                s.width = std::max(1, int(std::lround(s.width * fx)));
                s.height = std::max(1, int(std::lround(s.height * fy)));
            }
            for (SceneItem &i : s.items) {
                Transform &t = i.transform;
                t.x *= fx;
                t.y *= fy;
                t.boundsWidth *= fx;
                t.boundsHeight *= fy;
                // A group or studio grows with the canvas itself: its scale
                // already follows, and its crop is in its own pixels.
                const SourceDef *src = c.source(i.source);
                if (src && (src->kind == QLatin1String("group") || src->kind == QLatin1String("studio"))) {
                    t.cropLeft = int(std::lround(t.cropLeft * fx));
                    t.cropRight = int(std::lround(t.cropRight * fx));
                    t.cropTop = int(std::lround(t.cropTop * fy));
                    t.cropBottom = int(std::lround(t.cropBottom * fy));
                } else {
                    t.scaleX *= fx;
                    t.scaleY *= fy;
                }
            }
        }
    };
    Collection next = m_collection;
    scale(next);
    if (next == m_collection) return;
    for (Step &s : m_steps) {
        scale(s.before);
        scale(s.after);
    }
    scale(m_clipboard);  // copied sources paste where they were, at the new size
    // Saved when it can be, but not an edit: the file and the canvas come to the
    // same layout again, so a file that cannot be written holds no one here.
    const bool edited = m_unsaved;
    commit(next, false);
    m_unsaved = edited;
}
