import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Dialog {
    id: dlg
    property string titleText: ""
    property string bodyText: ""
    property string primaryText: ""
    property string secondaryText: ""
    property string cancelText: qsTr("Cancel")

    signal primaryClicked()
    signal secondaryClicked()
    signal cancelled()

    modal: true
    anchors.centerIn: parent
    width: Math.min(440, parent ? parent.width - 48 : 440)
    padding: 20

    Material.theme: theme.darkMode ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    background: Rectangle {
        color: theme.panelBackground
        border.color: theme.borderColor
        border.width: 1
        radius: theme.radius
    }

    contentItem: ColumnLayout {
        spacing: 14

        Label {
            text: dlg.titleText
            font.family: backend.fontFamily
            font.pixelSize: 15
            font.bold: true
            color: theme.foreground
            Layout.fillWidth: true
        }

        Label {
            text: dlg.bodyText
            font.family: backend.fontFamily
            font.pixelSize: 12
            color: theme.muted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                text: dlg.cancelText
                flat: true
                font.family: backend.fontFamily
                font.pixelSize: 12
                onClicked: {
                    dlg.close()
                    dlg.cancelled()
                }
            }

            Button {
                visible: dlg.secondaryText.length > 0
                text: dlg.secondaryText
                flat: true
                font.family: backend.fontFamily
                font.pixelSize: 12
                onClicked: {
                    dlg.close()
                    dlg.secondaryClicked()
                }
            }

            Button {
                text: dlg.primaryText
                highlighted: true
                font.family: backend.fontFamily
                font.pixelSize: 12
                Material.background: theme.accent
                Material.foreground: theme.accentForeground
                onClicked: {
                    dlg.close()
                    dlg.primaryClicked()
                }
            }
        }
    }
}
