#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "core/scene.h"
#include "engine/frame.h"

// Transitions between studios: what a transition's settings mean, and which
// one a change of live studio uses.
//
// Settings: kind (cut, fade, colour, swipe, cover, uncover, slide, wipe,
// stinger), side (left, right, top, bottom), colour ("#rrggbb") and point
// (0..1) for fade through colour, mask, softness (0..1) and invert for wipes.
// A stinger plays a video file over the change: file, cutMs or cutFrames
// (when the studios change under it, by cutUnit: ms or frames), audioFade (crossfade, or outIn: out to the cut,
// in after it) and monitor (off, only, both: whether its sound is heard
// in monitoring), matte (none; horizontal: colour left, matte right;
// vertical: colour above, matte below; mask: the whole video is the matte;
// separate: matteFile, another video played with it, is the matte)
// and matteInvert; it lasts as long as the file. With a matte the studios
// change where it is bright, not at the cut.
namespace Transitions {

// {id, name} lists for the UI; kinds() also says which settings each uses.
QVariantList kinds();
QVariantList sides();
QVariantList masks();

// The settings with every key present and in range.
QVariantMap normalized(const QVariantMap &settings);
engine::TransitionStyle style(const QVariantMap &settings);
bool isStinger(const QVariantMap &settings);

// Built-in and made transitions, in the order people see them: {id, name, settings}.
QVariantList all(const Collection &collection);
// A transition's settings by id; empty for one that does not exist.
QVariantMap settings(const Collection &collection, const QString &id);
bool exists(const Collection &collection, const QString &id);

struct Choice {
    QString transition;
    int durationMs = 0;
};
// Going live with `scene`: its own override, or the collection's.
Choice choiceFor(const Collection &collection, const QString &scene);

}  // namespace Transitions
