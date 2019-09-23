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

    function copyToClipboard(text) {
        var tempInput = $("<input>");
        $("body").append(tempInput);
        tempInput.val(text).select();
        document.execCommand("copy");
        tempInput.remove();
    }

    function osComparator(a, b) {
        return a['ordinal'] - b['ordinal']
    }

    function tagComparator(a, b) {
        var re = /(sapmachine)-(((([0-9]+)((\.([0-9]+))*)?)(\+([0-9]+))?)(-([0-9]+))?)(\-((\S)+))?/

        var aMatch = a.tag.match(re)
        var bMatch = b.tag.match(re)

        var aVersionParts   = aMatch[4].split('.')
        var aBuildNumber = 99999
        var aSapBuildNumber = 0

        if (aMatch.length >= 10 && aMatch[9]) {
            aBuildNumber = parseInt(aMatch[9])
        }

        if (aMatch.length >= 13 && aMatch[12]) {
            aSapBuildNumber = parseInt(aMatch[12])
        }

        var bVersionParts   = bMatch[4].split('.')
        var bBuildNumber = 99999
        var bSapBuildNumber = 0

        if (bMatch.length >= 10 && bMatch[9]) {
            bBuildNumber = parseInt(bMatch[9])
        }

        if (bMatch.length >= 13 && bMatch[12]) {
            bSapBuildNumber = parseInt(bMatch[12])
        }

        if (aVersionParts.length < 5) {
            aVersionParts.fill('0', aVersionParts.length, 4)
        }

        if (bVersionParts.length < 5) {
            bVersionParts.fill('0', bVersionParts.length, 4)
        }

        for (var i = 0; i < aVersionParts.length && i < bVersionParts.length; ++i) {
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
        var aMajor = a.id
        var bMajor = b.id

        if ((a.lts && b.lts) ||
            (!a.lts && !b.lts)) {
            if (aMajor < bMajor) {
                return 1
            }

            if (aMajor > bMajor) {
                return -1
            }

            return 0
        }

        if (!a.lts && b.lts) {
            return 1
        }

        if (a.lts && !b.lts) {
            return -1
        }

        return 0
    }

    function getOsSelectValue() {
        try {
            var isMac = navigator.platform.toUpperCase().indexOf('MAC') !== -1
            var isWindows = navigator.platform.toUpperCase().indexOf('WIN') !== -1
            var isLinux = navigator.platform.toUpperCase().indexOf('LINUX') !== -1

            if (isMac) {
                return 'osx-x64'
            }

            if (isWindows) {
                return 'windows-x64'
            }

            if (isLinux) {
                if (navigator.platform.toUpperCase().indexOf('PPC64LE') !== -1) {
                    return 'linux-ppc64le'
                }
                if (navigator.platform.toUpperCase().indexOf('PPC64') !== -1) {
                    return 'linux-ppc64'
                }
            }
        } catch (e) {
            // ignore
        }

        return 'linux-x64'
    }

    function SapMachine() {
        this._imageTypeSelector = $('#sapmachine_imagetype_select')
        this._osSelector = $('#sapmachine_os_select')
        this._versionSelector = $('#sapmachine_version_select')
        this._downloadButton = $('#sapmachine_download_button')
        this._copyURLButton = $('#sapmachine_copy_button')
        this._ltsCheckbox = $('#sapmachine_lts_checkbox')
        this._nonLtsCheckbox = $('#sapmachine_nonlts_checkbox')
        this._eaCheckbox = $('#sapmachine_ea_checkbox')
        this._downloadLabel = $('#download_label')
        this._assets = null
        this._data = null

        this._updateDownloadLabel = function updateDownloadLabel() {
            if (this._versionSelector.index() !== -1) {
                this._downloadLabel .text(this._versionSelector.val());
            }
        }.bind(this)

        this._onUpdateImageTypeAndOS = function onUpdateImageTypeAndOS() {
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
                this._copyURLButton.addClass('download_button_disabled')
                this._versionSelector.addClass('download_select_disabled')
            } else {
                this._downloadButton.removeClass('download_button_disabled')
                this._copyURLButton.removeClass('download_button_disabled')
                this._versionSelector.removeClass('download_select_disabled')
            }

            this._updateDownloadLabel()
        }.bind(this)

        this._onUpdateData = function onUpdateData() {
            var data = this._data

            this._imageTypeSelector.empty()
            this._osSelector.empty()
            this._versionSelector.empty()

            for (var i in data.imageTypes.sort(imageTypeComparator)) {
                if (data.imageTypes[i].lts && !this._ltsCheckbox.is(':checked'))
                    continue

                if (data.imageTypes[i].ea && !this._eaCheckbox.is(':checked'))
                    continue

                if ((!data.imageTypes[i].lts && !data.imageTypes[i].ea) && !this._nonLtsCheckbox.is(':checked'))
                    continue

                var labelElement = $('<div>' + data.imageTypes[i].label + '</div>')
                var optionElement = $('<option></option>')
                optionElement.attr({'value': data.imageTypes[i].id })
                optionElement.addClass('download_select_option')
                optionElement.append(labelElement)

                if (data.imageTypes[i].lts)
                    optionElement.append($('<div> (Long Term Support)</div>'))

                if (data.imageTypes[i].ea)
                    optionElement.append($('<div> (Pre-Release)</div>'))

                this._imageTypeSelector.append(optionElement)
            }

            var imageTypeSelectorEmpty = (this._imageTypeSelector.has('option').length <= 0)

            if (imageTypeSelectorEmpty) {
                this._downloadButton.addClass('download_button_disabled')
                this._copyURLButton.addClass('download_button_disabled')
                this._versionSelector.addClass('download_select_disabled')
                this._osSelector.addClass('download_select_disabled')
                this._imageTypeSelector.addClass('download_select_disabled')
            } else {
                this._downloadButton.removeClass('download_button_disabled')
                this._copyURLButton.removeClass('download_button_disabled')
                this._versionSelector.removeClass('download_select_disabled')
                this._osSelector.removeClass('download_select_disabled')
                this._imageTypeSelector.removeClass('download_select_disabled')

                for (var i in data.os.sort(osComparator)) {
                    var optionElement = $('<option></option>')
                    optionElement.text(data.os[i].value)
                    optionElement.attr({'value': data.os[i].key })
                    optionElement.addClass('download_select_option')
                    this._osSelector.append(optionElement)
                }

                this._osSelector.val(getOsSelectValue())

                this._assets = data.assets
                this._onUpdateImageTypeAndOS()
            }
        }.bind(this)

        this._imageTypeSelector.change(function imageTypeSelectorOnChange() {
            this._onUpdateImageTypeAndOS()
        }.bind(this))

        this._osSelector.change(function osSelectorOnChange() {
            this._onUpdateImageTypeAndOS()
        }.bind(this))

        this._versionSelector.change(function versionSelectorOnChange() {
            this._updateDownloadLabel()
        }.bind(this))

        this._downloadButton.click(function downloadButtonOnClick() {
            if (this._versionSelector.index() !== -1)
                sendDownloadEvent(this._versionSelector.val())
        }.bind(this))

        this._ltsCheckbox.click(function ltsCheckboxOnClick() {
            this._onUpdateData()
        }.bind(this))

        this._nonLtsCheckbox.click(function nonltsCheckboxOnClick() {
            this._onUpdateData()
        }.bind(this))

        this._eaCheckbox.click(function eaCheckboxOnClick() {
            this._onUpdateData()
        }.bind(this))

        this._copyURLButton.click(function onCopyButtonClick() {
            copyToClipboard(this._downloadLabel.text())
        }.bind(this))

        $.getJSON('assets/data/sapmachine_releases.json', function onJSONDataReceived(data) {
            this._data = data
            this._onUpdateData()
        }.bind(this))
    }

    $(document).ready(function () {
        const sapMachine = new SapMachine()
    });

})(window, document, jQuery, ga)
