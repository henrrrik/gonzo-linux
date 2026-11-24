import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Rectangle {
    id: panel
    signal closeRequested()

    Material.theme: theme.darkMode ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    width: Math.min(540, parent ? parent.width - 48 : 540)
    implicitHeight: contentCol.implicitHeight + 32
    color: theme.panelBackground
    border.color: theme.borderColor
    border.width: 1
    radius: theme.radius

    readonly property var rows: [
        { keys: "Ctrl+N", label: qsTr("New window") },
        { keys: "Ctrl+O", label: qsTr("Open document") },
        { keys: "Ctrl+Shift+O", label: qsTr("Recent documents") },
        { keys: "Ctrl+S / Ctrl+Shift+S", label: qsTr("Save document") + " / " + qsTr("Save as") },
        { keys: "Ctrl+P", label: qsTr("Print Markdown source") },
        { keys: "Ctrl+F / Ctrl+H", label: qsTr("Find in document") + " / " + qsTr("Find and replace") },
        { keys: "Ctrl+G / Ctrl+Shift+G", label: qsTr("Find next / previous") },
        { keys: "Ctrl+E", label: qsTr("Use selection for find") },
        { keys: "Ctrl+B / Ctrl+I / Ctrl+K", label: qsTr("Bold / Italic / Link") },
        { keys: "Shift+Return", label: qsTr("Soft line break") },
        { keys: "Ctrl+= / Ctrl+- / Ctrl+0", label: qsTr("Increase / decrease / reset text size") },
        { keys: "Ctrl+Shift+;", label: qsTr("Toggle spell check") },
        { keys: "F11 / Super+F", label: qsTr("Toggle fullscreen") },
        { keys: "Ctrl+? / F1", label: qsTr("Show shortcuts") },
        { keys: "Ctrl+Q", label: qsTr("Quit") }
    ]

    ColumnLayout {
        id: contentCol
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("Keyboard Shortcuts")
                font.family: backend.fontFamily
                font.pixelSize: 14
                font.bold: true
                color: theme.foreground
                Layout.fillWidth: true
            }

            ToolButton {
                text: "×"
                font.pixelSize: 15
                onClicked: panel.closeRequested()
            }
        }

        Repeater {
            model: panel.rows
            delegate: RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: modelData.label
                    color: theme.foreground
                    font.family: backend.fontFamily
                    font.pixelSize: 12
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }

                Rectangle {
                    color: theme.codeBackground
                    radius: Math.min(4, theme.radius)
                    implicitWidth: keyLabel.implicitWidth + 12
                    implicitHeight: keyLabel.implicitHeight + 4

                    Label {
                        id: keyLabel
                        anchors.centerIn: parent
                        text: modelData.keys
                        color: theme.muted
                        font.family: backend.fontFamily
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}
