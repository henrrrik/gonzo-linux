import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Rectangle {
    id: bar
    property bool replaceVisible: false

    signal queryChanged(string query)
    signal findNextRequested()
    signal findPreviousRequested()
    signal replaceRequested(string replacement)
    signal replaceAllRequested(string replacement)
    signal closeRequested()

    Material.theme: theme.darkMode ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    width: 360
    implicitHeight: layout.implicitHeight + 16
    color: theme.panelBackground
    border.color: theme.borderColor
    border.width: 1
    radius: theme.radius

    function focusFind() {
        findField.forceActiveFocus()
        findField.selectAll()
    }

    function setQueryText(q) {
        queryDebounce.stop()
        findField.text = q
    }

    Timer {
        id: queryDebounce
        interval: 150
        onTriggered: bar.queryChanged(findField.text)
    }

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            TextField {
                id: findField
                objectName: "findField"
                Layout.fillWidth: true
                placeholderText: qsTr("Find")
                font.family: backend.fontFamily
                font.pixelSize: 12
                color: theme.foreground
                onTextEdited: queryDebounce.restart()
                Keys.onReturnPressed: function(event) {
                    if (queryDebounce.running) {
                        queryDebounce.stop()
                        bar.queryChanged(text)
                    }
                    if (event.modifiers & Qt.ShiftModifier)
                        bar.findPreviousRequested()
                    else
                        bar.findNextRequested()
                }
                Keys.onEscapePressed: bar.closeRequested()
            }

            Label {
                text: {
                    var _lang = translator.language
                    if (findField.text.length === 0)
                        return ""
                    if (backend.searchMatchCount === 0)
                        return qsTr("No matches")
                    return qsTr("%1 of %2")
                        .arg(backend.currentSearchIndex + 1)
                        .arg(backend.searchMatchCount)
                }
                color: theme.muted
                font.family: backend.fontFamily
                font.pixelSize: 11
            }

            ToolButton {
                text: "↑"
                font.pixelSize: 12
                implicitWidth: 28
                implicitHeight: 28
                onClicked: bar.findPreviousRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Previous match")
            }

            ToolButton {
                text: "↓"
                font.pixelSize: 12
                implicitWidth: 28
                implicitHeight: 28
                onClicked: bar.findNextRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Next match")
            }

            ToolButton {
                text: "×"
                font.pixelSize: 14
                implicitWidth: 28
                implicitHeight: 28
                onClicked: bar.closeRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Close")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: bar.replaceVisible
            spacing: 6

            TextField {
                id: replaceField
                objectName: "replaceField"
                Layout.fillWidth: true
                placeholderText: qsTr("Replace")
                font.family: backend.fontFamily
                font.pixelSize: 12
                color: theme.foreground
                Keys.onReturnPressed: bar.replaceRequested(text)
                Keys.onEscapePressed: bar.closeRequested()
            }

            Button {
                text: qsTr("Replace")
                font.pixelSize: 11
                flat: true
                onClicked: bar.replaceRequested(replaceField.text)
            }

            Button {
                text: qsTr("Replace All")
                font.pixelSize: 11
                flat: true
                onClicked: bar.replaceAllRequested(replaceField.text)
            }
        }
    }
}
