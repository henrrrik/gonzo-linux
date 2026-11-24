import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1060
    height: 780
    minimumWidth: 480
    minimumHeight: 360
    visible: true
    title: backend.displayTitle
    color: theme.background

    Material.theme: theme.darkMode ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool closeConfirmed: false
    property bool findOpen: false
    property bool replaceOpen: false
    property bool shortcutsOpen: false
    property bool recentsOpen: false
    property int lastWindowedVisibility: Window.Windowed

    readonly property int footerHeight: 36
    readonly property int sideMargin: Math.max(24, Math.floor((editorFlick.width - backend.columnWidth) / 2))

    onVisibilityChanged: function(vis) {
        if (vis !== Window.Hidden && vis !== Window.Minimized && vis !== Window.FullScreen)
            lastWindowedVisibility = vis
    }

    onClosing: function(close) {
        if (closeConfirmed || !backend.modified)
            return
        close.accepted = false
        backend.requestClose()
    }

    function applySelection(result) {
        if (result && result.handled && result.selectionStart !== undefined) {
            editor.select(result.selectionStart, result.selectionEnd)
            ensureCursorVisible()
        }
    }

    function ensureCursorVisible() {
        scroller.cancel()
        var r = editor.cursorRectangle
        var top = r.y + editor.y - 32
        var bottom = r.y + editor.y + r.height + 32
        var maxY = Math.max(0, editorFlick.contentHeight - editorFlick.height)
        if (top < editorFlick.contentY)
            editorFlick.contentY = Math.max(0, top)
        else if (bottom > editorFlick.contentY + editorFlick.height)
            editorFlick.contentY = Math.min(maxY, bottom - editorFlick.height)
    }

    function revealMatch(match) {
        if (!match || !match.found)
            return
        scroller.cancel()
        editor.select(match.start, match.start + match.length)
        ensureCursorVisible()
    }

    function openFind(withReplace) {
        findOpen = true
        replaceOpen = withReplace
        var selected = backend.selectionForFind(editor.selectionStart, editor.selectionEnd)
        if (selected.length > 0 && selected.indexOf("\n") === -1)
            findBar.setQueryText(selected)
        findBar.focusFind()
    }

    function closeFind() {
        findOpen = false
        replaceOpen = false
        backend.clearSearch()
        editor.forceActiveFocus()
    }

    function toggleFullscreen() {
        if (visibility === Window.FullScreen)
            visibility = lastWindowedVisibility
        else
            visibility = Window.FullScreen
    }

    Connections {
        target: backend
        function onCloseApproved() {
            win.closeConfirmed = true
            Qt.callLater(function() {
                win.close()
                Qt.quit()
            })
        }
        function onUnsavedChangesRequested() {
            unsavedDialog.open()
        }
        function onExternalChangeDetected(deleted, locallyModified) {
            externalDialog.fileDeleted = deleted
            externalDialog.open()
        }
        function onSelectionRequested(start, end) {
            editor.select(start, end)
            if (start === 0 && end === 0) {
                scroller.cancel()
                editorFlick.contentY = 0
            } else {
                win.ensureCursorVisible()
            }
        }
    }

    Connections {
        target: scroller
        function onScrolled() {
            scrollLinger.restart()
        }
        function onZoomRequested(direction) {
            if (direction > 0)
                backend.increaseTextSize()
            else
                backend.decreaseTextSize()
        }
    }

    // Application-wide keyboard shortcuts.
    Shortcut { sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; onActivated: backend.newWindow() }
    Shortcut { sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; onActivated: backend.openDialog() }
    Shortcut { sequence: "Ctrl+Shift+O"; context: Qt.ApplicationShortcut; onActivated: win.recentsOpen = !win.recentsOpen }
    Shortcut { sequence: "Ctrl+S"; context: Qt.ApplicationShortcut; onActivated: backend.save() }
    Shortcut { sequence: "Ctrl+Shift+S"; context: Qt.ApplicationShortcut; onActivated: backend.saveAsDialog() }
    Shortcut { sequence: "Ctrl+P"; context: Qt.ApplicationShortcut; onActivated: backend.printDocument() }
    Shortcut { sequence: "Ctrl+W"; context: Qt.ApplicationShortcut; onActivated: backend.requestClose() }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: backend.requestClose() }

    Shortcut { sequence: "Ctrl+F"; context: Qt.ApplicationShortcut; onActivated: win.openFind(false) }
    Shortcut { sequences: ["Ctrl+H", "Ctrl+Alt+F"]; context: Qt.ApplicationShortcut; onActivated: win.openFind(true) }
    Shortcut { sequence: "Ctrl+G"; context: Qt.ApplicationShortcut; onActivated: win.revealMatch(backend.findNext(editor.cursorPosition)) }
    Shortcut { sequence: "Ctrl+Shift+G"; context: Qt.ApplicationShortcut; onActivated: win.revealMatch(backend.findPrevious(editor.cursorPosition)) }
    Shortcut {
        sequence: "Ctrl+E"
        context: Qt.ApplicationShortcut
        onActivated: {
            var s = backend.selectionForFind(editor.selectionStart, editor.selectionEnd)
            if (s.length > 0) {
                win.findOpen = true
                findBar.setQueryText(s)
                win.revealMatch(backend.setSearchQuery(s, editor.selectionStart))
            }
        }
    }

    Shortcut { sequence: "Ctrl+B"; context: Qt.ApplicationShortcut; onActivated: win.applySelection(backend.applyBold(editor.selectionStart, editor.selectionEnd)) }
    Shortcut { sequence: "Ctrl+I"; context: Qt.ApplicationShortcut; onActivated: win.applySelection(backend.applyItalic(editor.selectionStart, editor.selectionEnd)) }
    Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; onActivated: win.applySelection(backend.applyLink(editor.selectionStart, editor.selectionEnd)) }

    Shortcut { sequences: ["Ctrl+=", "Ctrl++"]; context: Qt.ApplicationShortcut; onActivated: backend.increaseTextSize() }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; onActivated: backend.decreaseTextSize() }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: backend.resetTextSize() }

    Shortcut { sequence: "Ctrl+Shift+;"; context: Qt.ApplicationShortcut; onActivated: backend.toggleSpellCheck() }
    Shortcut { sequences: ["F11", "Meta+F"]; context: Qt.ApplicationShortcut; onActivated: win.toggleFullscreen() }
    Shortcut { sequences: ["Ctrl+?", "Ctrl+/", "F1"]; context: Qt.ApplicationShortcut; onActivated: win.shortcutsOpen = !win.shortcutsOpen }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.shortcutsOpen)
                win.shortcutsOpen = false
            else if (win.recentsOpen)
                win.recentsOpen = false
            else if (win.findOpen)
                win.closeFind()
        }
    }

    // Right-to-left languages mirror the window; Translator sets layoutDirection.
    Item {
        id: root
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Flickable {
            id: editorFlick
            objectName: "editorScroll"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: footer.top
            clip: true
            interactive: false
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: width
            contentHeight: Math.max(height, editor.implicitHeight + 96 + 160)

            Component.onCompleted: scroller.attachFlickable(editorFlick, 120)

            onWidthChanged: scroller.cancel()
            onHeightChanged: scroller.cancel()

            FrameAnimation {
                id: kineticFrame
                running: scroller.active
                onTriggered: {
                    var maxY = Math.max(0, editorFlick.contentHeight - editorFlick.height)
                    editorFlick.contentY = scroller.advance(
                        frameTime, editorFlick.contentY, maxY, Screen.devicePixelRatio)
                }
            }

            Timer {
                id: scrollLinger
                interval: 600
            }

            ScrollBar.vertical: ScrollBar {
                id: vbar
                policy: ScrollBar.AsNeeded
                active: hovered || pressed || scroller.active || scrollLinger.running
                onPressedChanged: {
                    if (pressed)
                        scroller.cancel()
                }
            }

            TextArea {
                id: editor
                objectName: "editor"
                x: win.sideMargin
                y: 48
                width: Math.max(120, editorFlick.width - 2 * win.sideMargin)
                height: Math.max(editorFlick.height - 96, implicitHeight)
                padding: 0
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                background: null

                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                color: theme.foreground
                selectionColor: theme.selection
                selectedTextColor: theme.selectionForeground
                font.family: backend.fontFamily
                font.pointSize: backend.fontSize

                Component.onCompleted: {
                    backend.attachDocument(editor.textDocument)
                    editor.forceActiveFocus()
                }

                onCursorRectangleChanged: {
                    if (activeFocus && !scroller.active)
                        win.ensureCursorVisible()
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.RightButton
                    cursorShape: Qt.IBeamCursor
                    onPressed: function(mouse) {
                        scroller.cancel()
                        var pos = editor.positionAt(mouse.x, mouse.y)
                        var info = backend.spellingContextAt(pos)
                        spellMenu.openAt(mouse.x, mouse.y, info)
                    }
                }

                Keys.onPressed: function(event) {
                    scroller.cancel()
                    if (editor.inputMethodComposing)
                        return

                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        var shift = (event.modifiers & Qt.ShiftModifier) !== 0
                        var res = backend.handleReturn(editor.selectionStart, editor.selectionEnd, shift)
                        if (res.handled) {
                            win.applySelection(res)
                            event.accepted = true
                        }
                    } else if (event.key === Qt.Key_Backspace && event.modifiers === Qt.NoModifier) {
                        var del = backend.handleBackspace(editor.selectionStart, editor.selectionEnd)
                        if (del.handled) {
                            win.applySelection(del)
                            event.accepted = true
                        }
                    } else if (event.matches(StandardKey.Paste)) {
                        var pasted = backend.handlePaste(editor.selectionStart, editor.selectionEnd)
                        if (pasted.handled) {
                            win.applySelection(pasted)
                            event.accepted = true
                        }
                    }
                }
            }
        }

        FindBar {
            id: findBar
            visible: win.findOpen
            replaceVisible: win.replaceOpen
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 12
            anchors.rightMargin: 18
            z: 10
            onQueryChanged: function(query) {
                win.revealMatch(backend.setSearchQuery(query, editor.cursorPosition))
            }
            onFindNextRequested: win.revealMatch(backend.findNext(editor.cursorPosition))
            onFindPreviousRequested: win.revealMatch(backend.findPrevious(editor.cursorPosition))
            onReplaceRequested: function(replacement) {
                win.revealMatch(backend.replaceCurrent(replacement))
            }
            onReplaceAllRequested: function(replacement) {
                backend.replaceAll(replacement)
            }
            onCloseRequested: win.closeFind()
        }

        // Quiet 36px footer matching gonzo-mac: Open, Recent, Save, Status | Word count.
        Rectangle {
            id: footer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: win.footerHeight
            color: theme.background

            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: theme.borderColor
                opacity: 0.65
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 18
                spacing: 12

                ToolButton {
                    id: openBtn
                    text: qsTr("Open")
                    font.family: backend.fontFamily
                    font.pixelSize: 11
                    implicitHeight: 26
                    leftPadding: 8
                    rightPadding: 8
                    palette.buttonText: hovered ? theme.foreground : theme.muted
                    onClicked: backend.openDialog()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Open document") + " (Ctrl+O)"
                }

                ToolButton {
                    id: recentBtn
                    text: qsTr("Recent")
                    font.family: backend.fontFamily
                    font.pixelSize: 11
                    implicitHeight: 26
                    leftPadding: 8
                    rightPadding: 8
                    palette.buttonText: hovered ? theme.foreground : theme.muted
                    onClicked: win.recentsOpen = !win.recentsOpen
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Recent documents") + " (Ctrl+Shift+O)"
                }

                ToolButton {
                    id: saveBtn
                    text: qsTr("Save")
                    font.family: backend.fontFamily
                    font.pixelSize: 11
                    implicitHeight: 26
                    leftPadding: 8
                    rightPadding: 8
                    palette.buttonText: backend.modified ? theme.accent : (hovered ? theme.foreground : theme.muted)
                    onClicked: backend.save()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Save document") + " (Ctrl+S)"
                }

                Label {
                    id: statusLabel
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: backend.statusText
                    color: backend.statusIsError ? theme.accent : theme.muted
                    font.family: backend.fontFamily
                    font.pixelSize: 11
                }

                Label {
                    id: wordCountLabel
                    objectName: "wordCountLabel"
                    text: {
                        // Read translator.language so numbers re-format on language switch.
                        var _lang = translator.language
                        var formatted = backend.wordCount.toLocaleString(Qt.locale(), "f", 0)
                        return qsTr("%n words", "", backend.wordCount).replace(String(backend.wordCount), formatted)
                    }
                    color: theme.muted
                    font.family: backend.fontFamily
                    font.pixelSize: 11
                }
            }
        }

        RecentsPopup {
            id: recentsPopup
            visible: win.recentsOpen
            anchors.left: parent.left
            anchors.bottom: footer.top
            anchors.leftMargin: 16
            anchors.bottomMargin: 8
            z: 15
            onFilePicked: function(path) {
                win.recentsOpen = false
                backend.openFile(path)
                editor.forceActiveFocus()
            }
            onCloseRequested: win.recentsOpen = false
        }

        SpellMenu {
            id: spellMenu
            parent: editor
            onSuggestionPicked: function(start, length, word) {
                backend.replaceWordRange(start, length, word)
                editor.forceActiveFocus()
            }
            onIgnorePicked: function(word) {
                backend.ignoreWord(word)
                editor.forceActiveFocus()
            }
            onToggleSpellPicked: backend.toggleSpellCheck()
        }

        ShortcutsDialog {
            id: shortcutsDialog
            visible: win.shortcutsOpen
            anchors.centerIn: parent
            z: 20
            onCloseRequested: {
                win.shortcutsOpen = false
                editor.forceActiveFocus()
            }
        }

        ConfirmDialog {
            id: unsavedDialog
            titleText: qsTr("Unsaved Changes")
            bodyText: qsTr("Save changes to %1 before closing?").arg(backend.fileName)
            primaryText: qsTr("Save")
            secondaryText: qsTr("Discard")
            cancelText: qsTr("Cancel")
            onPrimaryClicked: backend.confirmSaveAndClose()
            onSecondaryClicked: backend.confirmDiscardAndClose()
            onCancelled: backend.cancelPendingClose()
        }

        ConfirmDialog {
            id: externalDialog
            property bool fileDeleted: false
            titleText: qsTr("External Change")
            bodyText: (fileDeleted
                ? qsTr("%1 was moved or deleted outside Gonzo.").arg(backend.fileName)
                : qsTr("%1 was changed outside Gonzo.").arg(backend.fileName))
                + " "
                + qsTr("Your edits have not overwritten it. Use Save As to keep your changes in another file, or Revert to Saved to load the external changes.")
            primaryText: qsTr("Save As")
            secondaryText: fileDeleted ? qsTr("Keep Local") : qsTr("Revert to Saved")
            cancelText: qsTr("Cancel")
            onPrimaryClicked: backend.saveAsDialog()
            onSecondaryClicked: {
                if (fileDeleted)
                    backend.keepLocalVersion()
                else
                    backend.revertToSaved()
            }
            onCancelled: backend.keepLocalVersion()
        }
    }
}
