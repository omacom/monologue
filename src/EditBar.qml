import QtQuick

// The edited recording as a row of clips over a filmstrip. Each clip has a
// handle at both ends, like Omacut's trim; whatever falls outside every clip
// is cut. Double-click a clip to split it, double-click a gap or a split to
// join the clips around it, and hover a clip to remove it. Times are seconds.
Item {
    id: root
    implicitHeight: 64
    // Time runs left to right in every language, and the hit testing below assumes it.
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    // What the pointer is doing: nothing, dragging a handle, scrubbing, or pressing a clip's ×.
    enum Mode { Idle, Trim, Scrub, Remove }

    readonly property real durationSec: backend.duration
    readonly property var clips: backend.clips
    readonly property var pauses: backend.pauses
    property real playheadSec: 0
    property color accent: "#FFD60A"
    property color accentForeground: "black"
    property int cornerRadius: 6
    property bool zoomed: false
    property real viewStartSec: 0
    property real viewEndSec: 0
    // Something is being dragged, so playback must not move the playhead.
    readonly property bool interacting: area.mode !== EditBar.Idle

    readonly property real handleW: 12
    // The clip frames' border; the filmstrip sits exactly inside it.
    readonly property int frameW: 3
    readonly property real frameRadius: cornerRadius > 0 ? Math.min(8, cornerRadius + frameW) : 0
    readonly property real windowStart: zoomed ? viewStartSec : 0
    readonly property real windowEnd: zoomed ? viewEndSec : durationSec
    readonly property real windowLen: Math.max(windowEnd - windowStart, 0.001)
    readonly property color trackColor: "#1c1c1e"
    // The remove ×, shared by the button and its hit test.
    readonly property int removeSize: 20
    readonly property int removeInset: 7
    readonly property int removeGap: 4
    readonly property int removeMinWidth: 60

    signal scrub(real seconds)

    function xForTime(t) { return durationSec <= 0 ? 0 : ((t - windowStart) / windowLen) * width }
    function timeForX(x) {
        if (width <= 0 || durationSec <= 0) return 0
        return windowStart + Math.max(0, Math.min(1, x / width)) * windowLen
    }
    function nearClipEdge(t) {
        for (var i = 0; i < clips.length; ++i)
            if (Math.abs(xForTime(clips[i].start) - xForTime(t)) < 8 || Math.abs(xForTime(clips[i].end) - xForTime(t)) < 8) return true
        return false
    }
    function clipAt(t) {
        for (var i = 0; i < clips.length; ++i)
            if (t >= clips[i].start && t < clips[i].end) return i
        return -1
    }
    // Z frames the clip under the playhead with some slack; again zooms out.
    function toggleZoom() {
        if (durationSec <= 0) return
        var i = clipAt(playheadSec)
        var from = i >= 0 ? clips[i].start : 0, to = i >= 0 ? clips[i].end : durationSec
        var slack = (to - from) / 8
        var newStart = Math.max(0, from - slack), newEnd = Math.min(durationSec, to + slack)
        if (zoomed && newStart === viewStartSec && newEnd === viewEndSec) zoomed = false
        else { viewStartSec = newStart; viewEndSec = newEnd; zoomed = true }
        backend.thumbnails.request(windowStart, windowEnd)
    }
    onDurationSecChanged: zoomed = false

    Rectangle {
        id: track
        anchors.fill: parent; anchors.topMargin: root.frameW; anchors.bottomMargin: root.frameW
        radius: Math.max(0, root.frameRadius - root.frameW); color: root.trackColor; clip: true
        Item {
            anchors.fill: parent
            Repeater {
                model: backend.thumbnails.count
                Image {
                    // Whole-pixel edges, so neighbouring frames never leave a seam between them.
                    readonly property real slot: track.width / Math.max(backend.thumbnails.count, 1)
                    x: Math.round(index * slot); width: Math.round((index + 1) * slot) - x; height: track.height
                    sourceSize.height: track.height
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true; cache: false
                    source: index < backend.thumbnails.ready ? "image://thumbs/" + backend.thumbnails.revision + "/" + index : ""
                }
            }
        }
        // Dim the whole strip; clips punch through below by drawing nothing.
        Repeater {
            model: root.clips.length + 1
            Rectangle {
                required property int index
                readonly property real from: index === 0 ? 0 : root.clips[index - 1].end
                readonly property real to: index === root.clips.length ? root.durationSec : root.clips[index].start
                visible: to > from
                // Rounded like the clip frames, so no undimmed sliver shows between a gap and a handle.
                x: Math.round(root.xForTime(from)); width: Math.max(0, Math.round(root.xForTime(to)) - x); height: track.height
                color: area.gapHovered === index ? "#90000000" : "#c8000000"
            }
        }
        Repeater {
            model: root.pauses
            Item {
                required property var modelData
                // A split made at this pause already shows it; a half-hidden mark would peek out beside the handle.
                visible: modelData >= root.windowStart && modelData <= root.windowEnd && !root.nearClipEdge(modelData)
                x: root.xForTime(modelData) - 4; width: 8; height: track.height
                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 1; height: parent.height; color: "#70ffffff" }
                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 6; height: 6; radius: 3; y: 3; color: "white" }
            }
        }
    }

    // Beneath the clip frames, so it never draws over a handle.
    Rectangle {
        visible: root.durationSec > 0 && root.playheadSec >= root.windowStart && root.playheadSec <= root.windowEnd
        // Within a clip, keep the line between its handles, so a playhead at the clip's
        // first or last frame shows on that frame instead of hiding under the handle.
        x: {
            var px = root.xForTime(root.playheadSec) - 1
            for (var i = 0; i < root.clips.length; ++i) {
                var c = root.clips[i]
                if (root.playheadSec < c.start || root.playheadSec > c.end) continue
                var from = Math.round(root.xForTime(c.start)) + root.handleW
                var to = Math.round(root.xForTime(c.end)) - root.handleW - width
                if (to >= from) px = Math.max(from, Math.min(px, to))
                break
            }
            return Math.max(0, Math.min(root.width - width, px))
        }
        y: track.y
        width: 2; height: track.height; color: "white"
    }

    // One accent frame per clip, with its handles inside the frame.
    Repeater {
        model: root.clips
        Item {
            id: clipItem
            required property var modelData
            required property int index
            readonly property bool hovered: area.clipHovered === index
            // Whole pixels, so handles meet the filmstrip without an antialiased seam.
            x: Math.round(root.xForTime(modelData.start)); width: Math.max(4, Math.round(root.xForTime(modelData.end)) - x)
            height: root.height
            Rectangle {
                anchors.fill: parent
                radius: root.frameRadius; color: "transparent"
                border.color: root.accent; border.width: root.frameW
            }
            Rectangle {
                width: Math.min(root.handleW, parent.width / 2); height: parent.height
                topLeftRadius: root.frameRadius; bottomLeftRadius: root.frameRadius; color: root.accent
                Rectangle { anchors.centerIn: parent; width: 2; height: 16; radius: 1; color: root.trackColor }
            }
            Rectangle {
                x: parent.width - width
                width: Math.min(root.handleW, parent.width / 2); height: parent.height
                topRightRadius: root.frameRadius; bottomRightRadius: root.frameRadius; color: root.accent
                Rectangle { anchors.centerIn: parent; width: 2; height: 16; radius: 1; color: root.trackColor }
            }
            // Remove this clip. Only offered when another clip would remain.
            Rectangle {
                id: remove
                visible: clipItem.hovered && root.clips.length > 1 && clipItem.width > root.removeMinWidth && area.mode === EditBar.Idle
                anchors.top: parent.top; anchors.right: parent.right
                anchors.topMargin: root.removeInset; anchors.rightMargin: root.handleW + root.removeGap
                width: root.removeSize; height: root.removeSize; radius: root.removeSize / 2
                readonly property bool hot: area.removeHovered
                color: hot ? root.accent : "#e0202023"
                Text {
                    anchors.centerIn: parent; anchors.verticalCenterOffset: -1
                    text: "×"; font.pixelSize: 16; font.weight: Font.DemiBold
                    color: remove.hot ? root.accentForeground : "white"
                }
            }
        }
    }


    // Hint above the timeline: the time under a dragged handle, or what a double-click will do.
    Rectangle {
        readonly property bool dragging: area.mode === EditBar.Trim
        readonly property string hint: area.gapHovered >= 0 ? qsTr("Double-click to restore")
                                      : area.splitHovered >= 0 ? qsTr("Double-click to join")
                                      : area.clipHovered >= 0 && !area.onHandle ? qsTr("Double-click to split") : ""
        visible: root.enabled && (dragging || (area.mode === EditBar.Idle && area.containsMouse && !area.removeHovered && hint !== ""))
        width: label.implicitWidth + 20; height: 26
        radius: Math.min(7, root.cornerRadius + 1); color: "#2c2c2f"
        x: Math.max(0, Math.min(root.width - width, (dragging ? root.xForTime(area.activeTime) : area.mouseX) - width / 2)); y: -height - 8
        Text {
            id: label; anchors.centerIn: parent; color: "white"
            text: parent.dragging ? win.preciseTime(area.activeTime) : parent.hint
            font.pixelSize: parent.dragging ? 13 : 12
            font.family: parent.dragging ? "monospace" : Qt.application.font.family
            font.weight: parent.dragging ? Font.DemiBold : Font.Normal
        }
    }

    MouseArea {
        id: area
        objectName: "timelineArea"
        anchors.fill: parent
        hoverEnabled: true
        property int mode: EditBar.Idle
        property int clipIndex: -1
        property bool leftEdge: true
        property real activeTime: 0
        property int clipHovered: -1
        property int gapHovered: -1
        property int splitHovered: -1
        property bool onHandle: false
        property bool removeHovered: false

        // The hovered clip's × sits in its top-right corner, inside the handle.
        function overRemoveButton(x, y) {
            if (clipHovered < 0 || root.clips.length < 2) return false
            var right = root.xForTime(root.clips[clipHovered].end) - root.handleW - root.removeGap
            var width = root.xForTime(root.clips[clipHovered].end) - root.xForTime(root.clips[clipHovered].start)
            return width > root.removeMinWidth && x >= right - root.removeSize && x <= right
                && y >= root.removeInset && y <= root.removeInset + root.removeSize
        }

        // Where x lands: a handle of some clip, a split between touching clips, inside a clip, or a gap.
        function hitTest(x) {
            var best = null
            for (var i = 0; i < root.clips.length; ++i) {
                var a = root.xForTime(root.clips[i].start), b = root.xForTime(root.clips[i].end)
                var w = Math.min(root.handleW, (b - a) / 2) + 2
                if (x >= a - 3 && x <= a + w) best = { handle: true, clip: i, left: true }
                else if (x >= b - w && x <= b + 3 && !best) best = { handle: true, clip: i, left: false }
                if (best) {
                    // Between touching clips, the side of the line decides which clip is grabbed.
                    if (!best.left && i + 1 < root.clips.length && root.clips[i + 1].start === root.clips[i].end && x > b)
                        best = { handle: true, clip: i + 1, left: true }
                    var edge = best.left ? root.clips[best.clip].start : root.clips[best.clip].end
                    var neighbour = best.left ? best.clip - 1 : best.clip + 1
                    best.split = neighbour >= 0 && neighbour < root.clips.length
                        && (best.left ? root.clips[neighbour].end === edge : root.clips[neighbour].start === edge)
                        ? Math.min(best.clip, neighbour) : -1
                    return best
                }
                if (x > a && x < b) return { handle: false, clip: i, split: -1 }
            }
            var t = root.timeForX(x), gap = 0
            for (var j = 0; j < root.clips.length && root.clips[j].end <= t; ++j) gap = j + 1
            return { handle: false, clip: -1, gap: gap, split: -1 }
        }
        // Handles catch on pause marks, neighbouring clip edges, and the playhead.
        function snap(t) {
            var best = t, bestDistance = 8
            function consider(s) {
                var d = Math.abs(root.xForTime(s) - root.xForTime(t))
                if (d < bestDistance) { best = s; bestDistance = d }
            }
            for (var i = 0; i < root.pauses.length; ++i) consider(root.pauses[i])
            for (var j = 0; j < root.clips.length; ++j)
                if (j !== clipIndex) { consider(root.clips[j].start); consider(root.clips[j].end) }
            consider(root.playheadSec)
            return best
        }
        // A split catches only on pause marks: the double-click's first click already moved the playhead here.
        function snapToPause(t) {
            var best = t, bestDistance = 8
            for (var i = 0; i < root.pauses.length; ++i) {
                var d = Math.abs(root.xForTime(root.pauses[i]) - root.xForTime(t))
                if (d < bestDistance) { best = root.pauses[i]; bestDistance = d }
            }
            return best
        }
        function seek(t) { root.playheadSec = t; root.scrub(t) }
        function hover(x, y) {
            var hit = hitTest(x)
            onHandle = hit.handle
            clipHovered = hit.clip
            splitHovered = hit.split
            gapHovered = hit.clip < 0 ? hit.gap : -1
            removeHovered = overRemoveButton(x, y)
            cursorShape = removeHovered ? Qt.PointingHandCursor : hit.handle ? Qt.SizeHorCursor : Qt.ArrowCursor
        }

        onPositionChanged: mouse => {
            if (root.durationSec <= 0) return
            var t = root.timeForX(mouse.x)
            if (mode === EditBar.Idle) { hover(mouse.x, mouse.y); return }
            if (mode === EditBar.Scrub) { seek(t); return }
            if (mode !== EditBar.Trim) return
            var clip = root.clips[clipIndex]
            activeTime = snap(t)
            if (leftEdge) backend.setClip(clipIndex, Math.min(activeTime, clip.end - 0.1), clip.end)
            else backend.setClip(clipIndex, clip.start, Math.max(activeTime, clip.start + 0.1))
            activeTime = leftEdge ? root.clips[clipIndex].start : root.clips[clipIndex].end
            seek(activeTime)
        }
        onPressed: mouse => {
            if (root.durationSec <= 0) return
            if (removeHovered) { mode = EditBar.Remove; return }
            var hit = hitTest(mouse.x)
            if (hit.handle) {
                mode = EditBar.Trim; clipIndex = hit.clip; leftEdge = hit.left
                activeTime = leftEdge ? root.clips[clipIndex].start : root.clips[clipIndex].end
                backend.beginGesture()
            } else {
                mode = EditBar.Scrub; seek(root.timeForX(mouse.x))
            }
        }
        onReleased: {
            if (mode === EditBar.Trim) backend.endGesture()
            if (mode === EditBar.Remove && overRemoveButton(mouseX, mouseY)) backend.removeClip(clipHovered)
            mode = EditBar.Idle; clipIndex = -1
            // A drag can end off the bar, and then no exit follows to clear it.
            if (containsMouse) hover(mouseX, mouseY)
            else unhover()
        }
        onDoubleClicked: mouse => {
            if (overRemoveButton(mouse.x, mouse.y)) return
            var hit = hitTest(mouse.x)
            if (hit.split >= 0) backend.joinClips(hit.split)
            else if (hit.clip >= 0 && !hit.handle) backend.split(snapToPause(root.timeForX(mouse.x)))
            else if (hit.clip < 0) win.restoreGap(hit.gap)
            hover(mouse.x, mouse.y)
        }
        // Another item took the press: end the gesture, or the backend keeps refusing undo and redo.
        onCanceled: {
            if (mode === EditBar.Trim) backend.endGesture()
            mode = EditBar.Idle; clipIndex = -1
        }
        function unhover() { clipHovered = -1; gapHovered = -1; splitHovered = -1; removeHovered = false; onHandle = false }
        onExited: unhover()
    }
}
