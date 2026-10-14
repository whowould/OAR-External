#pragma once
#include "game/scene.h"
#include "settings.h"
#include "widgets.h"

namespace scene { class Scanner; }

namespace menu
{
void  Init(scene::Scanner* scanner);
void  Shutdown();          // writes the autosave
void  Render();
float IdleDelay();         // seconds until another frame is needed if no input arrives (0 = keep animating)
void  OnDpiChanged();      // call after theme::SetDpi()
int   ViewCount();         // every page/sub-tab combination (used by --shots)
void  SetView(int index);

void SetSnapshot(scene::SnapshotPtr snap); // the picture every page of this frame shows
Settings& Cfg();           // the live settings (overlay and hotkeys read and toggle them)
void HandleEvent(const scene::Event& e); // toast + activity entry for something the scanner reported
void Notice(ui::ToastKind kind, unsigned icon, const char* text, const char* highlight = nullptr);
}
