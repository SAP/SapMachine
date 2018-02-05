(function (window, document, $, undefined) {

    (function SapMachine() {
        this._imageTypeSelector = $('#sapmachine_imagetype_select')
        this._osSelector = $('#sapmachine_os_select')
        this._downloadButton = $('#sapmachine_download_button')
        this._selectedImageType = null
        this._selectedOS = null
        this._assets = null

        this._updateImageTypeAndOS = () => {
            if (this._imageTypeSelector.index() !== -1)
                this._selectedImageType = this._imageTypeSelector.val()

            if (this._osSelector.index() !== -1)
                this._selectedOS = this._osSelector.val()

            var imageType = 'JDK'

            if (this._selectedImageType.includes('jre')) {
                imageType = 'JRE'
            }

            const asset_available = this._assets[this._selectedImageType]['releases'][0].hasOwnProperty(this._selectedOS)

            this._downloadButton.text(`Download "${this._assets[this._selectedImageType]['releases'][0]['tag']} ${imageType}"`)
            this._downloadButton.prop('disabled', !asset_available)

            if (!asset_available) {
                this._downloadButton.addClass('download_button_disabled')
            } else {
                this._downloadButton.removeClass('download_button_disabled')
            }
        }

        this._imageTypeSelector.change(() => {
            this._updateImageTypeAndOS()
        })

        this._osSelector.change(() => {
            this._updateImageTypeAndOS()
        })

        this._downloadButton.click(() => {
            window.location.href = this._assets[this._selectedImageType]['releases'][0][this._selectedOS]
        })

        $.getJSON('assets/data/sapmachine_releases.json', (data) => {
            for (var imageType of data.imageTypes) {
                var optionElement = $('<option></option>')
                optionElement.text(imageType.value)
                optionElement.attr({'value': imageType.key })
                optionElement.addClass('download_select_option')
                this._imageTypeSelector.append(optionElement)
            }

            for (var os of data.os) {
                var optionElement = $('<option></option>')
                optionElement.text(os.value)
                optionElement.attr({'value': os.key })
                optionElement.addClass('download_select_option')
                this._osSelector.append(optionElement)
            }

            this._assets = data.assets
            this._updateImageTypeAndOS()
        })
    })()

})(window, document, jQuery)
