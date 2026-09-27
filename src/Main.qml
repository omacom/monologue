import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Shapes
import QtMultimedia

ApplicationWindow {
    id: win
    width: 960; height: 700
    minimumWidth: 720; minimumHeight: 460
    visible: true
    title: "monologue"
    color: "#0e0e10"
    Material.theme: Material.Dark
    Material.accent: theme.accent
    readonly property color accent: theme.accent
    readonly property color accentForeground: theme.foreground
    readonly property int cornerRadius: theme.radius
    readonly property bool finished: backend.state === "finished" || backend.state === "saving"
    readonly property bool busy: backend.state === "finalizing" || backend.state === "saving"
    readonly property bool overlayOpen: closeDialog.visible || recordingsDialog.visible || discardDialog.visible || restartDialog.visible || helpDialog.visible || backend.dialogOpen
    property bool quitting: false
    property string discardId: ""
    property var playbackOutput: null
    function time(seconds) {
        var s = Math.max(0, Math.floor(seconds))
        return (s >= 3600 ? Math.floor(s/3600) + ":" : "") + String(Math.floor(s/60)%60).padStart(2,"0") + ":" + String(s%60).padStart(2,"0")
    }
    function preciseTime(seconds) {
        var cs = Math.round(Math.max(0, seconds) * 100)
        var m = Math.floor(cs / 6000), s = (cs - m * 6000) / 100
        return String(m).padStart(2, "0") + ":" + s.toFixed(2).padStart(5, "0")
    }
    // Where playback continues from t: t itself inside a clip, else the next
    // clip's start; -1 past the last clip.
    function playableFrom(t) {
        var clips = backend.clips
        for (var i = 0; i < clips.length; ++i)
            if (t < clips[i].end - 0.03) return Math.max(t, clips[i].start)
        return -1
    }
    function seekTo(t) {
        t = Math.max(0, Math.min(t, backend.duration))
        editBar.playheadSec = t
        player.position = Math.round(t * 1000)
    }
    function togglePlayback() {
        if (playbackOutput === null) playbackOutput = playbackAudio.createObject(win)
        if (player.playbackState === MediaPlayer.PlayingState) { player.pause(); return }
        var from = playableFrom(editBar.playheadSec)
        seekTo(from < 0 ? backend.clips[0].start : from)
        player.play()
    }
    // The gap before clip n; n equal to the clip count is the tail after the last clip.
    function gapAt(t) {
        var n = 0
        while (n < backend.clips.length && backend.clips[n].end <= t) ++n
        return n
    }
    function restoreGap(gap) {
        var clips = backend.clips
        if (gap === 0) backend.setClip(0, 0, clips[0].end)
        else if (gap === clips.length) backend.setClip(gap - 1, clips[gap - 1].start, backend.duration)
        else backend.joinClips(gap - 1)
    }
    function removeOrRestore() {
        var i = editBar.clipAt(editBar.playheadSec)
        if (i >= 0) backend.removeClip(i)
        else restoreGap(gapAt(editBar.playheadSec))
    }
    // Moves an edge of the clip under the playhead to it; in a gap, the neighbouring clip grows.
    function trimToPlayhead(start) {
        var t = editBar.playheadSec, clips = backend.clips
        var i = editBar.clipAt(t)
        if (i < 0) i = start ? gapAt(t) : gapAt(t) - 1
        if (i < 0 || i >= clips.length) return
        if (start) backend.setClip(i, t, clips[i].end)
        else backend.setClip(i, clips[i].start, t)
    }
    function jumpToPause(direction) {
        var marks = backend.pauses.slice()
        for (var c = 0; c < backend.clips.length; ++c) marks.push(backend.clips[c].start, backend.clips[c].end)
        marks.sort((a, b) => a - b)
        var t = editBar.playheadSec, target = direction > 0 ? marks[marks.length - 1] : marks[0]
        for (var i = 0; i < marks.length; ++i) {
            if (direction > 0 && marks[i] > t + 0.01) { target = marks[i]; break }
            if (direction < 0 && marks[i] < t - 0.01) target = marks[i]
        }
        player.pause(); seekTo(target)
    }
    function space() { if (finished) togglePlayback(); else backend.toggleRecording() }
    function closeSafely() {
        if (busy || backend.dialogOpen) return
        if (backend.takeActive) closeDialog.open()
        else { quitting = true; win.close() }
    }
    onClosing: close => {
        if (!quitting && (backend.takeActive || busy || backend.dialogOpen)) { close.accepted = false; closeSafely() }
    }
    Connections {
        target: backend
        function onSafeToClose() { win.quitting = true; win.close() }
        function onOverwriteRequested(path) { overwriteDialog.targetPath = path; overwriteDialog.open() }
        function onChanged() { if (!win.finished) player.stop() }
    }
    Component.onCompleted: backend.setPreview(liveVideo.videoSink)

    // Controls handle Space themselves. A window shortcut only takes it when
    // focus is on the preview/background; no double action from focused buttons.
    readonly property bool controlFocused: activeFocusItem && (activeFocusItem instanceof AbstractButton || activeFocusItem instanceof ComboBox || activeFocusItem instanceof Slider || activeFocusItem instanceof TextInput)
    Shortcut {
        sequence: "Space"; context: Qt.WindowShortcut; autoRepeat: false
        enabled: !win.overlayOpen && !cameraChoice.popup.visible && !microphoneChoice.popup.visible && !win.controlFocused && !win.busy
        onActivated: win.space()
    }
    // Stopping ends the take and opens it for editing.
    Shortcut { sequences: ["Ctrl+Return", "Ctrl+Enter"]; context: Qt.WindowShortcut; autoRepeat: false; enabled: backend.takeActive && !win.busy && !win.overlayOpen; onActivated: backend.finish() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.WindowShortcut; autoRepeat: false; enabled: backend.takeActive && !win.busy && !win.overlayOpen && !win.controlFocused; onActivated: backend.finish() }
    Shortcut { sequence: "Ctrl+S"; context: Qt.WindowShortcut; autoRepeat: false; enabled: win.finished && !win.busy && !win.overlayOpen; onActivated: { player.pause(); backend.save() } }
    Shortcut {
        sequence: "Escape"; context: Qt.WindowShortcut; autoRepeat: false
        enabled: (backend.takeActive || win.finished) && !win.busy && !win.overlayOpen && !cameraChoice.popup.visible && !microphoneChoice.popup.visible
        onActivated: { player.pause(); restartDialog.open() }
    }
    // Editing the finished clip.
    readonly property bool editing: win.finished && !win.busy && !win.overlayOpen && !editBar.interacting
    Shortcut { sequence: "Left"; context: Qt.WindowShortcut; enabled: win.editing && !win.controlFocused; onActivated: win.seekTo(editBar.playheadSec - 1) }
    Shortcut { sequence: "Right"; context: Qt.WindowShortcut; enabled: win.editing && !win.controlFocused; onActivated: win.seekTo(editBar.playheadSec + 1) }
    Shortcut { sequence: "Shift+Left"; context: Qt.WindowShortcut; enabled: win.editing; onActivated: win.seekTo(editBar.playheadSec - 5) }
    Shortcut { sequence: "Shift+Right"; context: Qt.WindowShortcut; enabled: win.editing; onActivated: win.seekTo(editBar.playheadSec + 5) }
    Shortcut { sequence: "Alt+Left"; context: Qt.WindowShortcut; enabled: win.editing; onActivated: win.seekTo(editBar.playheadSec - 0.2) }
    Shortcut { sequence: "Alt+Right"; context: Qt.WindowShortcut; enabled: win.editing; onActivated: win.seekTo(editBar.playheadSec + 0.2) }
    Shortcut { sequence: "["; context: Qt.WindowShortcut; enabled: win.editing; onActivated: win.jumpToPause(-1) }
    Shortcut { sequence: "]"; context: Qt.WindowShortcut; enabled: win.editing; onActivated: win.jumpToPause(1) }
    Shortcut { sequence: "S"; context: Qt.WindowShortcut; autoRepeat: false; enabled: win.editing; onActivated: backend.split(editBar.playheadSec) }
    Shortcut { sequences: ["X", "Delete", "Backspace"]; context: Qt.WindowShortcut; autoRepeat: false; enabled: win.editing && !win.controlFocused; onActivated: win.removeOrRestore() }
    Shortcut { sequence: "Ctrl+Space"; context: Qt.WindowShortcut; autoRepeat: false; enabled: win.editing; onActivated: win.trimToPlayhead(true) }
    Shortcut { sequence: "Alt+Space"; context: Qt.WindowShortcut; autoRepeat: false; enabled: win.editing; onActivated: win.trimToPlayhead(false) }
    Shortcut { sequence: "Z"; context: Qt.WindowShortcut; autoRepeat: false; enabled: win.editing; onActivated: editBar.toggleZoom() }
    Shortcut { sequence: "Ctrl+Z"; context: Qt.WindowShortcut; enabled: win.editing && backend.canUndo; onActivated: backend.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; context: Qt.WindowShortcut; enabled: win.editing && backend.canRedo; onActivated: backend.redo() }
    Shortcut { sequence: "Q"; context: Qt.WindowShortcut; autoRepeat: false; enabled: !win.overlayOpen; onActivated: win.closeSafely() }
    Shortcut { sequence: "?"; context: Qt.WindowShortcut; enabled: !win.overlayOpen; onActivated: helpDialog.open() }

    readonly property color textColor: "#eeeef0"
    readonly property color dimColor: "#8d8d96"
    readonly property color faintColor: "#5c5c64"
    readonly property color recordColor: "#f0605c"
    readonly property var icons: ({
        camera: "M4.5 6h9a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-9a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2z M15.5 10.5l6-3.5v10l-6-3.5z",
        microphone: "M12 3a3 3 0 0 1 3 3v5a3 3 0 0 1-6 0V6a3 3 0 0 1 3-3z M5.5 11a6.5 6.5 0 0 0 13 0 M12 17.5V21",
        chevron: "M6 9l6 6 6-6",
        list: "M4 6h16 M4 12h16 M4 18h10",
        back: "M4 12a8 8 0 1 0 2.4-5.7 M4 4v4.5h4.5",
        play: "M8 4.5v15l12.5-7.5z",
        pause: "M7 4.5h2a1 1 0 0 1 1 1v13a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1v-13a1 1 0 0 1 1-1z M15 4.5h2a1 1 0 0 1 1 1v13a1 1 0 0 1-1 1h-2a1 1 0 0 1-1-1v-13a1 1 0 0 1 1-1z",
        stop: "M7.5 5.5h9a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2h-9a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2z"
    })

    // 24×24 SVG paths, stroked like line icons unless filled.
    component Icon: Item {
        id: icon
        property string path
        property color color: win.textColor
        property bool filled: false
        implicitWidth: 16; implicitHeight: 16
        Shape {
            preferredRendererType: Shape.CurveRenderer
            scale: icon.width / 24; transformOrigin: Item.TopLeft
            ShapePath {
                strokeColor: icon.filled ? "transparent" : icon.color; strokeWidth: icon.filled ? 0 : 1.8
                fillColor: icon.filled ? icon.color : "transparent"
                capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                PathSvg { path: icon.path }
            }
        }
    }
    component ActionButton: Button {
        id: button
        property bool primary: false
        property string iconPath: ""
        property bool iconFilled: false
        padding: 14; topPadding: 10; bottomPadding: 10
        font.pixelSize: 13; font.weight: Font.DemiBold
        focusPolicy: Qt.TabFocus
        implicitHeight: 42
        readonly property color foreground: button.primary ? win.accentForeground : button.flat && !button.hovered ? win.dimColor : win.textColor
        contentItem: Row {
            spacing: 8; opacity: button.enabled ? 1 : .4
            Icon { visible: button.iconPath !== ""; path: button.iconPath; filled: button.iconFilled; color: button.foreground; anchors.verticalCenter: parent.verticalCenter }
            Text { visible: button.text !== ""; text: button.text; font: button.font; color: button.foreground; anchors.verticalCenter: parent.verticalCenter }
        }
        background: Rectangle {
            radius: win.cornerRadius; color: button.primary ? win.accent : button.flat ? (button.hovered ? "#1f1f23" : "transparent") : button.hovered ? "#3a3a3e" : "#2c2c2f"
            opacity: button.enabled ? 1 : .4
            border.width: button.activeFocus ? 2 : 0; border.color: button.primary ? win.accentForeground : win.accent
        }
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }
    component ThemedDialog: Dialog {
        id: dialog
        focus: true
        Material.roundedScale: win.cornerRadius
        Material.elevation: 0
        background: Rectangle { color: "#202023"; radius: win.cornerRadius; border.color: "#39393e" }
        Overlay.modal: Rectangle { color: "#99000000" }
        header: Label {
            text: dialog.title; color: "#eeeef0"; font.pixelSize: 22
            padding: 24; bottomPadding: 0; elide: Text.ElideRight
        }
        footer: DialogButtonBox {
            Material.roundedScale: win.cornerRadius
            background: Item { }
            visible: count > 0
            delegate: ActionButton { }
        }
        onClosed: liveVideo.forceActiveFocus()
    }
    // A source reads as quiet text with an icon; the dropdown appears on click.
    component SourceChoice: ComboBox {
        id: choice
        property string iconPath
        property string detail: ""
        property real maximumTextWidth: 260
        textRole: "label"
        focusPolicy: Qt.TabFocus
        implicitHeight: 26
        leftPadding: 0; rightPadding: 0; topPadding: 0; bottomPadding: 0
        font.pixelSize: 13
        Material.accent: win.accent
        indicator: Item { }
        background: Item { }
        contentItem: RowLayout {
            spacing: 8
            Icon { path: choice.iconPath; color: win.dimColor }
            Label {
                Layout.maximumWidth: choice.maximumTextWidth; elide: Text.ElideRight; font: choice.font
                text: choice.displayText
                color: choice.activeFocus ? win.accent : choice.enabled ? (choice.hovered ? "#ffffff" : win.textColor) : win.dimColor
            }
            Icon { visible: choice.enabled; path: win.icons.chevron; implicitWidth: 12; implicitHeight: 12; color: win.dimColor }
        }
        popup.width: Math.max(280, choice.width)
        popup.background: Rectangle { color: "#202023"; radius: win.cornerRadius; border.color: "#39393e" }
        ToolTip {
            visible: choice.hovered && choice.currentText !== ""
            text: choice.detail !== "" ? choice.currentText + "\n" + choice.detail : choice.currentText
            background: Rectangle { color: "#303037"; radius: win.cornerRadius }
        }
        HoverHandler { cursorShape: choice.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
    }
    component ShortcutList: GridLayout {
        id: list
        property string heading
        property var rows: []
        columns: 2; columnSpacing: 16; rowSpacing: 6
        Label { Layout.columnSpan: 2; Layout.bottomMargin: 4; text: list.heading; font.pixelSize: 14; font.weight: Font.DemiBold }
        Repeater {
            model: list.rows.length * 2
            Label {
                required property int index
                text: list.rows[Math.floor(index / 2)][index % 2]; font.pixelSize: 13
                font.family: index % 2 ? Qt.application.font.family : "monospace"
                color: index % 2 ? win.textColor : win.accent
            }
        }
    }
    // Round shutter: record, stop a take, play/pause a finished clip.
    component Shutter: AbstractButton {
        id: shutter
        focusPolicy: Qt.TabFocus
        implicitWidth: 64; implicitHeight: 64
        readonly property bool recording: backend.takeActive
        readonly property bool playing: player.playbackState === MediaPlayer.PlayingState
        opacity: enabled ? 1 : .4
        Rectangle {
            anchors.fill: parent; radius: width / 2
            color: win.finished ? (shutter.hovered ? "#34343a" : "#26262a") : "transparent"
            border.width: win.finished ? (shutter.activeFocus ? 2 : 0) : 3
            border.color: shutter.activeFocus ? win.accent : shutter.recording ? win.recordColor : win.textColor
        }
        Rectangle {
            visible: !win.finished
            anchors.centerIn: parent
            width: shutter.recording ? 22 : 50; height: width
            radius: shutter.recording ? Math.min(5, win.cornerRadius + 2) : width / 2
            color: shutter.recording ? win.recordColor : win.accent
            scale: shutter.pressed ? .9 : shutter.hovered ? .96 : 1
            Behavior on width { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
            Behavior on scale { NumberAnimation { duration: 80 } }
        }
        Icon {
            visible: win.finished; filled: true
            anchors.centerIn: parent; anchors.horizontalCenterOffset: shutter.playing ? 0 : 2
            implicitWidth: 24; implicitHeight: 24
            path: shutter.playing ? win.icons.pause : win.icons.play
        }
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }

    ColumnLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 120
            color: "black"; clip: true
            VideoOutput { id: liveVideo; anchors.fill: parent; visible: !win.finished; fillMode: VideoOutput.PreserveAspectFit; focus: true }
            VideoOutput { id: clipVideo; objectName: "clipVideo"; anchors.fill: parent; visible: win.finished; fillMode: VideoOutput.PreserveAspectFit }
            MouseArea { anchors.fill: parent; onClicked: { liveVideo.forceActiveFocus(); if (win.finished) win.togglePlayback() } }
            Column {
                anchors.centerIn: parent; width: Math.min(400, parent.width-40); spacing: 16
                visible: backend.state === "unavailable" || backend.state === "starting" || win.busy
                Label {
                    width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; font.pixelSize: 15
                    text: win.busy ? (backend.state === "saving" ? "Saving your clip…" + (backend.saveProgress > 0 ? " " + Math.floor(backend.saveProgress * 100) + "%" : "") : "Finishing your clip…") : backend.state === "starting" ? "Connecting your camera and microphone…" : backend.message
                }
                ActionButton { anchors.horizontalCenter: parent.horizontalCenter; visible: backend.state === "unavailable"; text: "Retry"; onClicked: backend.retry() }
            }
            Rectangle {
                visible: backend.message !== "" && backend.state !== "unavailable" && !win.busy
                anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottomMargin: 16
                width: Math.min(messageText.implicitWidth + 28, parent.width - 32); height: messageText.implicitHeight + 16
                radius: win.cornerRadius; color: "#dd141416"
                Label { id: messageText; anchors.centerIn: parent; width: parent.width - 28; text: backend.message; color: win.accent; font.pixelSize: 12; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter }
            }
        }
        // Hairline under the frame: the mic level while live, the clip position once finished.
        Item {
            Layout.fillWidth: true; implicitHeight: 3; z: 1
            Rectangle {
                id: meter; visible: !win.finished; anchors.fill: parent; color: "#1a1a1d"
                readonly property real fraction: backend.audioEnabled ? Math.max(0, Math.min(1, (backend.level + 60) / 60)) : 0
                readonly property real peakFraction: backend.audioEnabled ? Math.max(0, Math.min(1, (backend.peakLevel + 60) / 60)) : 0
                Rectangle {
                    width: meter.fraction * parent.width; height: parent.height
                    color: backend.clipping || backend.level >= -3 ? win.recordColor : backend.level >= -12 ? "#e2c77c" : "#8fc38a"
                }
                Rectangle { visible: meter.peakFraction > 0; x: meter.peakFraction * (parent.width - width); width: 2; height: parent.height; color: backend.clipping ? win.recordColor : "#c9c9d0" }
                Accessible.role: Accessible.Indicator
                Accessible.name: "Microphone level: " + backend.meterText
            }
        }
        Item {
            visible: win.finished; z: 1
            Layout.fillWidth: true; Layout.topMargin: 16; Layout.leftMargin: 16; Layout.rightMargin: 16
            implicitHeight: editBar.implicitHeight
            EditBar {
                id: editBar; objectName: "editBar"
                anchors.fill: parent
                enabled: !win.busy
                accent: win.accent; accentForeground: win.accentForeground
                cornerRadius: win.cornerRadius
                onScrub: seconds => { player.pause(); player.position = Math.round(seconds * 1000) }
            }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 104; Layout.maximumHeight: 104
            Layout.leftMargin: 22; Layout.rightMargin: 16; spacing: 16
            Item {
                Layout.fillWidth: true; Layout.preferredWidth: 1; implicitHeight: 64
                ColumnLayout {
                    id: sources; visible: !win.finished; spacing: 2
                    anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    SourceChoice {
                        id: cameraChoice; iconPath: win.icons.camera; detail: backend.formatLabel
                        maximumTextWidth: Math.min(260, sources.width - 44)
                        model: backend.cameras; currentIndex: backend.cameraIndex
                        enabled: !backend.takeActive && !win.busy
                        onActivated: index => { backend.selectCamera(index); liveVideo.forceActiveFocus() }
                        Accessible.name: "Camera"
                    }
                    SourceChoice {
                        id: microphoneChoice; iconPath: win.icons.microphone
                        maximumTextWidth: Math.min(260, sources.width - 44)
                        model: backend.microphones; currentIndex: backend.microphoneIndex
                        enabled: !backend.takeActive && !win.busy
                        onActivated: index => { backend.selectMicrophone(index); liveVideo.forceActiveFocus() }
                        Accessible.name: "Microphone"
                    }
                }
                ActionButton {
                    visible: win.finished; flat: true; iconPath: win.icons.back; text: "New recording"
                    anchors.left: parent.left; anchors.leftMargin: -14; anchors.verticalCenter: parent.verticalCenter
                    enabled: !win.busy && !backend.dialogOpen
                    onClicked: { player.stop(); backend.newRecording(); liveVideo.forceActiveFocus() }
                }
            }
            RowLayout {
                spacing: 18
                Label {
                    Layout.preferredWidth: 90; horizontalAlignment: Text.AlignRight
                    text: win.finished ? win.preciseTime(editBar.playheadSec) : win.time(backend.duration)
                    font.family: "monospace"; font.pixelSize: win.finished ? 13 : 18
                    color: win.finished ? win.dimColor : backend.state === "recording" ? win.textColor : backend.takeActive ? win.dimColor : win.faintColor
                }
                Shutter {
                    id: recordButton
                    enabled: !win.busy && !backend.dialogOpen && (win.finished || backend.ready || backend.takeActive)
                    Accessible.name: win.finished ? "Play or pause clip" : backend.takeActive ? "Stop" : "Record"
                    onClicked: {
                        if (win.finished) win.togglePlayback()
                        else if (backend.takeActive) backend.finish()
                        else backend.toggleRecording()
                        liveVideo.forceActiveFocus()
                    }
                }
                Item {
                    Layout.preferredWidth: 90; implicitHeight: 36
                    RoundButton {
                        id: pauseButton
                        visible: backend.takeActive; enabled: !win.busy
                        anchors.verticalCenter: parent.verticalCenter
                        width: 40; height: 40; padding: 0; flat: true
                        focusPolicy: Qt.TabFocus
                        readonly property bool paused: backend.state === "paused"
                        Accessible.name: paused ? "Resume" : "Pause"
                        ToolTip.visible: hovered; ToolTip.text: (paused ? "Resume" : "Pause") + " (Space)"
                        background: Rectangle {
                            radius: width / 2; color: pauseButton.hovered ? "#34343a" : "#26262a"
                            border.width: pauseButton.activeFocus ? 2 : 0; border.color: win.accent
                        }
                        contentItem: Item {
                            Icon { visible: !pauseButton.paused; anchors.centerIn: parent; filled: true; path: win.icons.pause }
                            Rectangle { visible: pauseButton.paused; anchors.centerIn: parent; width: 14; height: 14; radius: 7; color: win.recordColor }
                        }
                        onClicked: { backend.toggleRecording(); liveVideo.forceActiveFocus() }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                    }
                    Label {
                        visible: win.finished; anchors.verticalCenter: parent.verticalCenter
                        text: win.preciseTime(backend.keptDuration); font.family: "monospace"; font.pixelSize: 13; color: win.dimColor
                        ToolTip.visible: keptHover.hovered; ToolTip.text: "Length of all clips"
                        HoverHandler { id: keptHover }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true; Layout.preferredWidth: 1; spacing: 8
                Item { Layout.fillWidth: true }
                ToolButton {
                    id: recordingsButton; Accessible.name: "Recordings"
                    enabled: !backend.takeActive && !win.busy && !backend.dialogOpen
                    opacity: enabled ? 1 : .4
                    implicitWidth: 40; implicitHeight: 40
                    contentItem: Item { Icon { anchors.centerIn: parent; path: win.icons.list; color: recordingsButton.hovered ? win.textColor : win.dimColor } }
                    onClicked: { player.pause(); backend.refreshRecordings(); recordingsDialog.open() }
                    ToolTip.visible: hovered; ToolTip.text: "Recordings (" + backend.recordings.length + ")"
                }
                ActionButton { visible: win.finished; text: "Save"; primary: true; enabled: !win.busy && !backend.dialogOpen; onClicked: { player.pause(); backend.save() } }
            }
        }
    }
    MediaPlayer {
        id: player; source: win.finished ? backend.clip : ""
        videoOutput: clipVideo
        audioOutput: win.playbackOutput
        // Play only the clips: hop over gaps and stop after the last clip.
        onPositionChanged: {
            if (playbackState === MediaPlayer.PlayingState) {
                var t = position / 1000, next = win.playableFrom(t)
                if (next < 0) { pause(); win.seekTo(backend.clips[backend.clips.length - 1].end); return }
                if (next - t > 0.05) { position = Math.round(next * 1000); return }
            }
            if (!editBar.interacting) editBar.playheadSec = position / 1000
        }
        // Show the first kept frame rather than black until Play, once per clip.
        property bool primed: false
        onSourceChanged: { editBar.playheadSec = 0; primed = false }
        onMediaStatusChanged: {
            if (!primed && mediaStatus === MediaPlayer.LoadedMedia && backend.clips.length > 0) {
                primed = true
                pause(); win.seekTo(backend.clips[0].start)
            }
        }
    }
    Component { id: playbackAudio; AudioOutput {} }
    ThemedDialog {
        id: closeDialog; anchors.centerIn: parent; modal: true; title: "Stop this recording?"
        closePolicy: Popup.CloseOnEscape
        ColumnLayout {
            spacing: 16
            Label { text: "Stop and keep the clip in Recordings before closing, or keep recording."; wrapMode: Text.WordWrap; Layout.maximumWidth: 420 }
            RowLayout {
                ActionButton { text: "Keep recording"; onClicked: closeDialog.close() }
                ActionButton { text: "Discard"; onClicked: { closeDialog.close(); backend.discardAndClose() } }
                ActionButton { text: "Stop and keep"; primary: true; onClicked: { closeDialog.close(); backend.finishAndClose() } }
            }
        }
    }
    ThemedDialog {
        id: recordingsDialog; anchors.centerIn: parent; modal: true
        title: "Recordings"; width: Math.min(win.width-40,700); height: Math.min(win.height-60,490)
        standardButtons: Dialog.Close
        ColumnLayout {
            anchors.fill: parent
            Label { text: "Originals stay here until you discard them."; color: "#aaaab1"; font.pixelSize: 12 }
            Label { visible: backend.recordings.length === 0; text: "Your finished and interrupted takes will appear here."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ListView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 8
                model: backend.recordings
                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width; height: 92; color: "#202023"; radius: 8
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 10; spacing: 3
                        Label { text: modelData.name; Layout.fillWidth: true; elide: Text.ElideMiddle; font.pixelSize: 12 }
                        RowLayout {
                            Label { text: modelData.status + " · " + modelData.size; color: "#aaaab1"; font.pixelSize: 11; Layout.fillWidth: true }
                            ActionButton { text: "Open"; onClicked: { recordingsDialog.close(); backend.openRecording(modelData.id) } }
                            ActionButton { text: "Files"; onClicked: backend.showFiles(modelData.id) }
                            ActionButton { text: "Discard"; onClicked: { win.discardId=modelData.id; discardDialog.open() } }
                        }
                    }
                }
                ScrollBar.vertical: ScrollBar {}
            }
        }
    }
    ThemedDialog {
        id: discardDialog; anchors.centerIn: parent; modal: true; title: "Discard this recording?"
        width: Math.min(win.width-40,460)
        standardButtons: Dialog.Cancel | Dialog.Discard
        Label { width: parent.width; text: "This deletes the original from Recordings. Saved copies are kept."; wrapMode: Text.WordWrap }
        onDiscarded: { player.stop(); backend.discardRecording(win.discardId) }
    }
    ThemedDialog {
        id: restartDialog; objectName: "restartDialog"
        anchors.centerIn: parent; modal: true; title: "Discard this clip and start over?"
        width: Math.min(win.width-40,460)
        closePolicy: Popup.CloseOnEscape
        enter: Transition { }
        exit: Transition { }
        onOpened: restartConfirm.forceActiveFocus()
        footer: DialogButtonBox {
            Material.roundedScale: win.cornerRadius
            background: Item { }
            ActionButton {
                id: restartCancel; objectName: "restartCancel"; text: "Cancel"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                KeyNavigation.tab: restartConfirm; KeyNavigation.backtab: restartConfirm
                KeyNavigation.left: restartConfirm; KeyNavigation.right: restartConfirm
            }
            ActionButton {
                id: restartConfirm; objectName: "restartConfirm"; text: "Confirm"; primary: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                KeyNavigation.tab: restartCancel; KeyNavigation.backtab: restartCancel
                KeyNavigation.left: restartCancel; KeyNavigation.right: restartCancel
            }
        }
        Label { width: parent.width; text: "This deletes the current clip. Saved copies are kept."; wrapMode: Text.WordWrap }
        onAccepted: { player.stop(); backend.discardCurrent() }
    }
    ThemedDialog {
        id: overwriteDialog; anchors.centerIn: parent; modal: true; title: "Replace the existing MP4?"
        property string targetPath: ""
        width: Math.min(win.width-40,460)
        standardButtons: Dialog.Yes | Dialog.No
        Label { width: parent.width; text: overwriteDialog.targetPath + " already exists."; wrapMode: Text.WrapAnywhere }
        onAccepted: backend.confirmOverwrite(true)
        onRejected: backend.confirmOverwrite(false)
    }
    ThemedDialog {
        id: helpDialog; anchors.centerIn: parent; modal: true; title: "Keyboard shortcuts"; standardButtons: Dialog.Close
        RowLayout {
            spacing: 40
            ShortcutList {
                Layout.alignment: Qt.AlignTop; heading: "Recording"
                rows: [["Space", "Record / pause / resume"], ["Enter", "Stop and edit"], ["Esc", "Discard and start over"], ["Q", "Quit"], ["?", "Show shortcuts"]]
            }
            ShortcutList {
                Layout.alignment: Qt.AlignTop; heading: "Editing"
                rows: [["Space", "Play / pause"], ["← →", "Move 1 s (Shift 5 s, Alt 0.2 s)"], ["[ ]", "Previous / next pause or clip edge"],
                       ["S", "Split the clip at the playhead"], ["X", "Remove the clip, or restore the gap"],
                       ["Ctrl+Space", "Clip start to playhead"], ["Alt+Space", "Clip end to playhead"],
                       ["Ctrl+Z", "Undo (Ctrl+Shift+Z redo)"], ["Z", "Zoom to the clip"], ["Ctrl+S", "Save"], ["Esc", "Discard and start over"]]
            }
        }
    }
}
