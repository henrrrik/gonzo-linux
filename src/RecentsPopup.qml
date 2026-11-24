import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Rectangle {
    id: popup
    signal filePicked(string path)
    signal closeRequested()

    Material.theme: theme.darkMode ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    width: 340
    implicitHeight: col.implicitHeight + 16
    color: theme.panelBackground
    border.color: theme.borderColor
    border.width: 1
    radius: theme.radius

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        Label {
            visible: backend.recentFiles.length === 0
            text: qsTr("No recent documents")
            color: theme.muted
            font.family: backend.fontFamily
            font.pixelSize: 12
            padding: 8
        }

        Repeater {
            model: backend.recentFiles
            delegate: ItemDelegate {
                Layout.fillWidth: true
                implicitHeight: 32
                text: modelData.split("/").pop()
                font.family: backend.fontFamily
                font.pixelSize: 12
                ToolTip.visible: hovered
                ToolTip.text: modelData
                onClicked: popup.filePicked(modelData)
            }
        }

        Rectangle {
            visible: backend.recentFiles.length > 0
            Layout.fillWidth: true
            height: 1
            color: theme.borderColor
        }

        ItemDelegate {
            visible: backend.recentFiles.length > 0
            Layout.fillWidth: true
            implicitHeight: 30
            text: qsTr("Clear Recent")
            font.family: backend.fontFamily
            font.pixelSize: 11
            onClicked: {
                backend.clearRecentFiles()
                popup.closeRequested()
            }
        }
    }
}
