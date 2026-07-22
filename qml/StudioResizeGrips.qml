import QtQuick

// Invisible edge + corner drag handles that give a frameless Window native
// resizing. A frameless window has no system border, so without these the
// window's declared minimumWidth/minimumHeight can never be reached by dragging —
// startSystemResize() hands the drag to the compositor (Wayland/X11), which is
// also what enforces the minimum size. Instantiate as a direct child of the
// Window's root with `window` set to the host: this Item fills the window but
// carries no MouseArea of its own, so only the thin edge/corner strips capture
// input — content underneath is untouched except within `thickness` px of the
// very border.
Item {
    id: root

    // The frameless Window to resize.
    required property var window
    // Grip thickness in px (edge strips and corner squares).
    property int thickness: 6

    anchors.fill: parent
    // Above the chrome and content so the border strips win the edge pixels.
    z: 1000

    function grab(edges) { if (root.window) root.window.startSystemResize(edges) }

    // ---- Edges ----
    MouseArea { // top
        height: root.thickness
        anchors { top: parent.top; left: parent.left; right: parent.right
                  leftMargin: root.thickness; rightMargin: root.thickness }
        cursorShape: Qt.SizeVerCursor
        onPressed: root.grab(Qt.TopEdge)
    }
    MouseArea { // bottom
        height: root.thickness
        anchors { bottom: parent.bottom; left: parent.left; right: parent.right
                  leftMargin: root.thickness; rightMargin: root.thickness }
        cursorShape: Qt.SizeVerCursor
        onPressed: root.grab(Qt.BottomEdge)
    }
    MouseArea { // left
        width: root.thickness
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom
                  topMargin: root.thickness; bottomMargin: root.thickness }
        cursorShape: Qt.SizeHorCursor
        onPressed: root.grab(Qt.LeftEdge)
    }
    MouseArea { // right
        width: root.thickness
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom
                  topMargin: root.thickness; bottomMargin: root.thickness }
        cursorShape: Qt.SizeHorCursor
        onPressed: root.grab(Qt.RightEdge)
    }

    // ---- Corners (square, sitting over the ends of the edge strips) ----
    MouseArea { // top-left
        width: root.thickness; height: root.thickness
        anchors { top: parent.top; left: parent.left }
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.grab(Qt.TopEdge | Qt.LeftEdge)
    }
    MouseArea { // top-right
        width: root.thickness; height: root.thickness
        anchors { top: parent.top; right: parent.right }
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.grab(Qt.TopEdge | Qt.RightEdge)
    }
    MouseArea { // bottom-left
        width: root.thickness; height: root.thickness
        anchors { bottom: parent.bottom; left: parent.left }
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.grab(Qt.BottomEdge | Qt.LeftEdge)
    }
    MouseArea { // bottom-right
        width: root.thickness; height: root.thickness
        anchors { bottom: parent.bottom; right: parent.right }
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.grab(Qt.BottomEdge | Qt.RightEdge)
    }
}
