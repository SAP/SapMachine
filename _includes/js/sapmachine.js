/*
Copyright (c) 2001-2018 by SAP SE, Walldorf, Germany.
All rights reserved. Confidential and proprietary.
*/

"use strict";

(function (window, document, $, ga, undefined) {

    // polyfill for String.includes
    if (!String.prototype.includes) {
        String.prototype.includes = function(search, start) {
            if (typeof start !== 'number') {
                start = 0
            }

            if (start + search.length > this.length) {
                return false
            } else {
                return this.indexOf(search, start) !== -1
            }
        };
    }

    function osComparator(a, b) {
        return a['ordinal'] - b['ordinal']
    }

    function tagComparator(a, b) {
        var re = /(sapmachine)-(((([0-9]+)((\.([0-9]+))*)?)\+([0-9]+))(-([0-9]+))?)(\-((\S)+))?/

        var aMatch = a.tag.match(re)
        var bMatch = b.tag.match(re)

        var aVersionParts   = aMatch[4].split('.')
        var aBuildNumber    = parseInt(aMatch[9])
        var aSapBuildNumber = parseInt(aMatch[11])

        var bVersionParts   = bMatch[4].split('.')
        var bBuildNumber    = parseInt(bMatch[9])
        var bSapBuildNumber = parseInt(bMatch[11])

        if (aVersionParts.length > bVersionParts.length) {
            return -1
        }

        if (bVersionParts.length > aVersionParts.length) {
            return 1
        }

        for (var i in aVersionParts) {
            var aVersionPart = parseInt(aVersionParts[i])
            var bVersionPart = parseInt(bVersionParts[i])

            if (aVersionPart > bVersionPart) {
                return -1
            }

            if (bVersionPart > aVersionPart) {
                return 1
            }
        }

        if (aBuildNumber > bBuildNumber) {
            return -1
        }

        if (bBuildNumber > aBuildNumber) {
            return 1
        }

        if (aSapBuildNumber > bSapBuildNumber) {
            return -1
        }

        if (bSapBuildNumber > aSapBuildNumber) {
            return 1
        }

        return 0
    }

    function imageTypeComparator(a, b) {
        var aIsPreRelease = a.key.includes('-ea')
        var bIsPreRelease = b.key.includes('-ea')
        var re = /([0-9]+).*/;

        var aMajor = a.key.match(re)[1]
        var bMajor = b.key.match(re)[1]

        if ((aIsPreRelease && bIsPreRelease) ||
            (!aIsPreRelease && !bIsPreRelease)) {
            if (aMajor < bMajor) {
                return 1
            }

            if (aMajor > bMajor) {
                return -1
            }

            return 0
        }

        if (!aIsPreRelease && bIsPreRelease) {
            return -1
        }

        if (aIsPreRelease && !bIsPreRelease) {
            return 1
        }

        return 0
    }

    function SapMachine() {
        this._imageTypeSelector = $('#sapmachine_imagetype_select')
        this._osSelector = $('#sapmachine_os_select')
        this._versionSelector = $('#sapmachine_version_select')
        this._downloadButton = $('#sapmachine_download_button')
        this._assets = null

        this._updateImageTypeAndOS = function updateImageTypeAndOS() {
            var selectedImageType;
            var selectedOS;

            if (this._imageTypeSelector.index() !== -1)
                selectedImageType = this._imageTypeSelector.val()

            if (this._osSelector.index() !== -1)
                selectedOS = this._osSelector.val()

            this._versionSelector.empty()

            for (var i in this._assets[selectedImageType]['releases'].sort(tagComparator)) {
                var release = this._assets[selectedImageType]['releases'][i]

                if (release.hasOwnProperty(selectedOS)) {
                    var optionElement = $('<option></option>')
                    optionElement.text(release.tag)
                    optionElement.attr({'value': release[selectedOS]})
                    optionElement.addClass('download_select_option')
                    this._versionSelector.append(optionElement)
                }
            }

            var versionSelectorEmpty = (this._versionSelector.has('option').length <= 0)


            if (versionSelectorEmpty) {
                this._downloadButton.addClass('download_button_disabled')
                this._versionSelector.addClass('download_select_disabled')
            } else {
                this._downloadButton.removeClass('download_button_disabled')
                this._versionSelector.removeClass('download_select_disabled')
            }

            this._downloadButton.prop('disabled', versionSelectorEmpty)
            this._versionSelector.prop('disabled', versionSelectorEmpty)
        }.bind(this)

        this._imageTypeSelector.change(function imageTypeSelectorOnChange() {
            this._updateImageTypeAndOS()
        }.bind(this))

        this._osSelector.change(function osSelectorOnChange() {
            this._updateImageTypeAndOS()
        }.bind(this))

        this._versionSelector.change(function versionSelectorOnChange() {
        }.bind(this))

        this._downloadButton.click(function downloadButtonOnClick() {
            if (this._versionSelector.index() !== -1)
                sendDownloadEvent(this._versionSelector.val())
        }.bind(this))

        $.getJSON('assets/data/sapmachine_releases.json', function onJSONDataReceived(data) {
            for (var i in data.imageTypes.sort(imageTypeComparator)) {
                var optionElement = $('<option></option>')
                optionElement.text(data.imageTypes[i].value)
                optionElement.attr({'value': data.imageTypes[i].key })
                optionElement.addClass('download_select_option')
                this._imageTypeSelector.append(optionElement)
            }

            for (var i in data.os.sort(osComparator)) {
                var optionElement = $('<option></option>')
                optionElement.text(data.os[i].value)
                optionElement.attr({'value': data.os[i].key })
                optionElement.addClass('download_select_option')
                this._osSelector.append(optionElement)
            }

            this._assets = data.assets
            this._updateImageTypeAndOS()
        }.bind(this))
    }

    $(document).ready(function () {
        const sapMachine = new SapMachine()
    });

})(window, document, jQuery, ga)
