import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

Menu {
    id: menu

    Material.theme: theme.darkMode ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property string currentWord: ""
    property int wordStart: 0
    property int wordLength: 0
    property var suggestions: []
    property bool hasMisspelling: false

    signal suggestionPicked(int start, int length, string word)
    signal ignorePicked(string word)
    signal toggleSpellPicked()

    function openAt(px, py, info) {
        hasMisspelling = Boolean(info && info.misspelled)
        currentWord = hasMisspelling ? info.word : ""
        wordStart = hasMisspelling ? info.start : 0
        wordLength = hasMisspelling ? info.length : 0
        suggestions = hasMisspelling && info.suggestions ? info.suggestions : []
        popup(px, py)
    }

    Instantiator {
        model: menu.hasMisspelling ? menu.suggestions : []
        delegate: MenuItem {
            text: modelData
            onTriggered: menu.suggestionPicked(menu.wordStart, menu.wordLength, modelData)
        }
        onObjectAdded: function(index, obj) { menu.insertItem(index, obj) }
        onObjectRemoved: function(index, obj) { menu.removeItem(obj) }
    }

    MenuItem {
        visible: menu.hasMisspelling && menu.suggestions.length === 0
        height: visible ? implicitHeight : 0
        enabled: false
        text: qsTr("No suggestions")
    }

    MenuItem {
        visible: menu.hasMisspelling
        height: visible ? implicitHeight : 0
        text: qsTr("Ignore word")
        onTriggered: menu.ignorePicked(menu.currentWord)
    }

    MenuSeparator {
        visible: menu.hasMisspelling
        height: visible ? implicitHeight : 0
    }

    MenuItem {
        text: backend.spellCheckEnabled ? qsTr("Spell check off") : qsTr("Spell check on")
        enabled: backend.spellCheckAvailable
        onTriggered: menu.toggleSpellPicked()
    }
}
