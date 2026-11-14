#include "core/starter_studios.h"

#include <QCoreApplication>

#include <algorithm>

#include "core/production.h"

namespace {

using Kind = presets::StudioItem::Kind;
using Place = presets::StudioItem::Place;

Transform placed(Place place, double w, double h) {
    Transform t;
    switch (place) {
    case Place::Full:
        t.bounds = Transform::Bounds::Fit;
        t.boundsWidth = w;
        t.boundsHeight = h;
        break;
    case Place::Corner: {
        // A quarter of the width, in from the bottom right by the same margin both ways.
        const double margin = std::round(h * 0.04);
        t.bounds = Transform::Bounds::Fit;
        t.boundsWidth = std::round(w * 0.25);
        t.boundsHeight = std::round(h * 0.25);
        t.boundsAlignment = Transform::Alignment::BottomRight;
        t.x = w - margin - t.boundsWidth;
        t.y = h - margin - t.boundsHeight;
        break;
    }
    case Place::Centre:
        t.positionAlignment = Transform::Alignment::Centre;
        t.x = std::round(w / 2);
        t.y = std::round(h / 2);
        break;
    }
    return t;
}

}  // namespace

Collection starterCollection(const presets::Plan &plan, const presets::Choices &choices) {
    const int w = plan.profile.canvasWidth > 0 ? plan.profile.canvasWidth : 1920;
    const int h = plan.profile.canvasHeight > 0 ? plan.profile.canvasHeight : 1080;
    Production built;
    Collection empty;
    empty.name = plan.profile.name;
    empty.canvasWidth = w;
    empty.canvasHeight = h;
    built.reset(empty);
    QString display, camera, backdrop, microphone;  // source ids, made the first time they are needed
    QString first;
    for (const presets::Studio &studio : plan.studios) {
        QString id;
        if (first.isEmpty()) {
            id = built.collection().currentScene;  // the one an empty collection starts with
            built.renameScene(id, studio.name);
            first = id;
        } else {
            id = built.addScene(studio.name);
        }
        built.setCurrentScene(id);
        const auto add = [&](QString *shared, const QString &kind, const QString &name, const QVariantMap &settings, const Transform &t) {
            if (shared && !shared->isEmpty()) {
                built.addExistingSource(*shared, t);
                return;
            }
            const QString item = built.addSource(kind, name, settings, t);
            if (shared) *shared = built.collection().findItem(item)->source;
        };
        for (const presets::StudioItem &item : studio.items) {
            const Transform t = placed(item.place, w, h);
            switch (item.kind) {
            case Kind::Display:
                add(&display, QStringLiteral("display"), QCoreApplication::translate("Presets", "Display"), {{QStringLiteral("cursor"), true}}, t);
                break;
            case Kind::Camera:
                if (choices.camera.isEmpty()) break;
                add(&camera, QStringLiteral("device"),
                    choices.cameraName.isEmpty() ? QCoreApplication::translate("Presets", "Camera") : choices.cameraName,
                    {{QStringLiteral("node"), choices.camera}}, t);
                break;
            case Kind::Microphone: {
                if (choices.microphone.isEmpty() || choices.microphone == QLatin1String("none")) break;
                const QString name = choices.microphoneName.isEmpty() ? QCoreApplication::translate("Presets", "Microphone") : choices.microphoneName;
                // What the plan set for the voice (its recording tracks) goes with it.
                QVariantMap settings = plan.profile.microphoneAudio;
                settings[QStringLiteral("audioNode")] = choices.microphone;
                settings[QStringLiteral("audioLabel")] = name;
                add(&microphone, QStringLiteral("audio-input"), name, settings, Transform{});
                break;
            }
            case Kind::Backdrop:
                add(&backdrop, QStringLiteral("colour"), QCoreApplication::translate("Presets", "Backdrop"),
                    {{QStringLiteral("colour"), QStringLiteral("#16161a")}, {QStringLiteral("width"), w}, {QStringLiteral("height"), h}}, t);
                break;
            case Kind::Title:
                add(nullptr, QStringLiteral("text"), item.text,
                    {{QStringLiteral("text"), item.text}, {QStringLiteral("size"), std::max(24, h / 10)},
                     {QStringLiteral("colour"), QStringLiteral("#ffffff")}, {QStringLiteral("align"), QStringLiteral("centre")}}, t);
                break;
            }
        }
    }
    if (!first.isEmpty()) built.setCurrentScene(first);
    return built.collection();
}
