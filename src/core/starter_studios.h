#pragma once

#include "core/presets.h"
#include "core/scene.h"

// A plan's starter studios as a new collection: one display capture (still
// to be chosen in the desktop's picker) and one camera shared by every
// studio that shows them, title cards on one shared backdrop.
Collection starterCollection(const presets::Plan &plan, const presets::Choices &choices);
