import QtQuick
import QtMultimedia

// Thin QtMultimedia wrapper: a MediaPlayer driving a VideoOutput. Instantiated
// ONLY through a Loader gated on Studio.capVideoPlayback, so `import
// QtMultimedia` is never evaluated when the plugin is absent (the same
// isolation Unisic uses for its VideoPreview). Exposes the minimal
// source/position/playing surface the editor transport drives. Audio follows
// the project's clip-audio settings (audioVolume/audioMuted) so the preview is
// WYSIWYG with the export mux; the guard keeps any instantiation outside the
// editor window's context (no `editorProject`) safe and silent-by-default.
Item {
    id: root

    property url source
    readonly property int  positionMs: player.position
    readonly property int  durationMs: player.duration
    readonly property bool isPlaying: player.playbackState === MediaPlayer.PlayingState

    function play()  { player.play() }
    function pause() { player.pause() }
    function togglePlay() {
        player.playbackState === MediaPlayer.PlayingState ? player.pause() : player.play()
    }
    function seek(ms) { player.position = ms }

    MediaPlayer {
        id: player
        source: root.source
        videoOutput: out
        // Both instances (master clip AND the webcam-overlay feed) follow the
        // clip volume — the webcam sidecar carries no meaningful audio, so one
        // shared setting is acceptable. AudioOutput.volume is linear gain, the
        // same scale the export's ffmpeg `volume=` filter applies.
        audioOutput: AudioOutput {
            muted: (typeof editorProject !== "undefined" && editorProject)
                   ? editorProject.audioMuted : false
            volume: (typeof editorProject !== "undefined" && editorProject)
                    ? editorProject.audioVolume : 1.0
        }
    }

    VideoOutput {
        id: out
        anchors.fill: parent
        // Stretch to fill the composition's video region: the region carries the
        // exact aspect of the camera viewport (source aspect in fit mode, output
        // aspect in fill mode) and CompositionRoot's zoom transform crops from
        // there, so a plain stretch is undistorted AND matches the export renderer
        // (VideoFrameItem always fills its rect). PreserveAspectFit would letterbox
        // inside the region in fill mode.
        fillMode: VideoOutput.Stretch
    }
}
