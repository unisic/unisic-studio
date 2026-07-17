#pragma once

class StudioApp;

// Hidden dev aid behind main.cpp's `--autozoom-test`. End-to-end, no portal:
// synthesise a recording (testsrc video + scripted cursor/click tracks), run the
// auto-camera through StudioApp, then export twice (camera on vs. off) and probe
// the rendered frames to prove the zoom crop + cursor overlay actually landed.
// Also measures generate() on a 10-min track and checks regenerate determinism.
// Drives the real subsystems; calls QCoreApplication::exit(0/1) when done.
namespace AutozoomSelfTest {
void run(StudioApp *studio);
}
