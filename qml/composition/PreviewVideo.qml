import QtQuick
import QtMultimedia

// Thin QtMultimedia wrapper: a MediaPlayer driving a VideoOutput. Instantiated
// ONLY through a Loader gated on Studio.capVideoPlayback, so `import
// QtMultimedia` is never evaluated when the plugin is absent (the same
// isolation Unisic uses for its VideoPreview). Exposes the minimal
// source/position/playing surface the editor transport drives; audio is omitted
// — the M1 preview is silent (no AudioOutput dependency).
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
