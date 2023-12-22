/*
Copyright (c) 2001-2018 by SAP SE, Walldorf, Germany.
All rights reserved. Confidential and proprietary.
*/

"use strict";

(function (window, document, $, undefined) {

	var _jsondata = [];
	
	// check for get parameter "tracing=true" to enable console output
	var isTracingActive = true;
	var url = window.location.href;
	var traceParam = "tracing";
    traceParam = traceParam.replace(/[\[]/,"\\\[").replace(/[\]]/,"\\\]");
    var regexS = "[\\?&]"+traceParam+"=([^&#]*)";
    var regex = new RegExp( regexS );
    var results = regex.exec( url );
    var result = results == null ? null : results[1];
	if (result == "true") isTracingActive = true;
	
	
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

    function imageTypeComparator(a, b) {
        return osComparator(a, b)
    }

    function tagComparator(a, b) {
        var re = /((\S)+)-(((([0-9]+)((\.([0-9]+))*)?)(\+([0-9]+))?)(-([0-9]+))?)(\-((\S)+))?/
        
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

    function majorsComparator(a, b) {
        if (a.ea && !b.ea)
            return 1

        if (!a.ea && b.ea)
            return -1

		// sort by label descending
        if (a.label < b.label) {
            return 1
        }

        if (a.label > b.label) {
            return -1
        }

        return 0
    }

    function getOsSelectValue(ignoreInstaller = false) {
        try {
            var isMac = navigator.platform.toUpperCase().indexOf('MAC') !== -1
            var isWindows = navigator.platform.toUpperCase().indexOf('WIN') !== -1
            var isLinux = navigator.platform.toUpperCase().indexOf('LINUX') !== -1

            if (isMac) {
                return (ignoreInstaller ? 'macos-x64' : 'macos-x64-installer');
            }

            if (isWindows) {
                return (ignoreInstaller ? 'windows-x64' : 'windows-x64-installer');
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

	function extendArray (target, source) {
	  target = target || new Array;
	  for (var key in source) {
		if (typeof source[key] === 'object') {
		  if (target[key] && !isNaN(target[key])) {
			  // check for duplicate OS entries and skip already existing OSes. 
			  // actually it's not known if this is OS section due to recursive call
			  // so check for key/value attributes which are used for OS and imageType.
			  var entryAlreadyExists = false;
			  Object.entries(target).forEach(([k, v]) => {
			    if (v['key'] !== undefined && v['key'] === source[key]['key']) {
			      entryAlreadyExists = true;
			    }
			  });
			  if (!entryAlreadyExists) target[target.length] = extendArray(target[target.length], source[key]);
		  } else {
			target[key] = extendArray(target[key], source[key]);
		  }
		} else {
 		  target[key] = source[key];
		}
	  }
	  return target;
	}

    function SapMachine(jsonData) {
        this._majorSelector = $('#sapmachine_major_select')
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
		this._data = new Array()

		for (let i = 0; i < _jsondata.length; i++) {
			this._data = extendArray(this._data, _jsondata[i]);
		}

		if (isTracingActive) {
			console.log('found major releases: ');
			Object.entries(this._data.majors).forEach(([key, value]) => {
				console.log(value['id'] + '  [' + value['label'] + ']');				
			});
		}

		this._getPlatformsFromAsset = function _getPlatformFromAsset(major, imageType) {
			var data = this._data;		
			var majorData;
			var platforms = new Array()
			
			if (data !== undefined) {
				for (var i in data.majors) {
						if (data.majors[i].id == major) {
							majorData = data.majors[i];
							break;
						}
				}
			}
			
			
			if (majorData !== undefined) {
				var imageTypeSelector = true;
				var platformSelector = true;
				for (let j = 1; j < _jsondata.length; j++) {
					for (let k = 0; k < _jsondata[j].majors.length; k++) {
						if (_jsondata[j].majors[k].id === majorData.id) {
							imageTypeSelector = _jsondata[0].buildfiles[j-1].imageTypeSelector;
							platformSelector = _jsondata[0].buildfiles[j-1].platformSelector;
						}
					}
				}

				var urls = [];
				var counter = 0;
				if (imageTypeSelector) {
					if (imageType) {
						for (let i = 0; i < this._assets[majorData.id]['releases'].length; i++) {
							if (this._assets[majorData.id]['releases'][i][imageType] !== undefined) {
								for (let [key, value] of Object.entries(this._assets[majorData.id]['releases'][i][imageType])) {
									platforms[counter++] = key;
								}
							}
						}
					}
				} else if (platformSelector) {
					for (let i = 0; i < this._assets[majorData.id]['releases'].length; i++) {
						if (this._assets[majorData.id]['releases'][i]['urls'] !== undefined) {
							for (let [key, value] of Object.entries(this._assets[majorData.id]['releases'][i]['urls'])) {
								platforms[counter++] = key;
							}
						}
					}
				}

			}
			return platforms;
		}

        this._updateDownloadLabel = function updateDownloadLabel() {
            if (this._versionSelector.index() !== -1) {
                this._downloadLabel .text(this._versionSelector.val());
            }
        }.bind(this)

        this._onUpdateImageTypeAndOS = function onUpdateImageTypeAndOS() {
            var data = this._data
		
            var selectedMajor;
            var selectedImageType;
            var selectedOS;
            var majorData;
            var hasImageType;
            var isPlatformDependent;

            if (this._majorSelector.index() !== -1)
                selectedMajor = this._majorSelector.val()

            for (var i in data.majors) {
                if (data.majors[i].id == selectedMajor) {
					majorData = data.majors[i];
					break;
				}
			}

			var imageTypeSelector = true;
			var platformSelector = true;
			for (let j = 1; j < _jsondata.length; j++) {
				for (let k = 0; k < _jsondata[j].majors.length; k++) {
					if (_jsondata[j].majors[k].id === majorData.id) {
						imageTypeSelector = _jsondata[0].buildfiles[j-1].imageTypeSelector;
						platformSelector = _jsondata[0].buildfiles[j-1].platformSelector;
					}
				}
			}			
			
			if (!imageTypeSelector) {
				this._imageTypeSelector.prop('disabled', true);
				this._imageTypeSelector.hide();
				hasImageType = false;
			} else {
				this._imageTypeSelector.prop('disabled', false);
				this._imageTypeSelector.show();
				hasImageType = true;
			}
				
			if (!platformSelector) {
				this._osSelector.prop('disabled', true);
				this._osSelector.hide();
				isPlatformDependent = false;
			} else {
				this._osSelector.prop('disabled', false);
				this._osSelector.show();
				isPlatformDependent = true;
			}				

			if (isTracingActive) {
				console.log("selected major..........: " + majorData.id);
				console.log("imageType supported.....: " + imageTypeSelector);
				console.log("is platform dependent...: " + platformSelector);
			}

            if (this._imageTypeSelector.index() !== -1)
                selectedImageType = this._imageTypeSelector.val()

            if (this._osSelector.index() !== -1)
                selectedOS = this._osSelector.val()

            this._versionSelector.empty()
			

			this._osSelector.children('option').each(function(){ 
				$(this).hide();
			});
					
			var osSelector = this._osSelector;
			var selectedOSavailable = false;
			var downloadablePlatforms = this._getPlatformsFromAsset(selectedMajor, selectedImageType);
			downloadablePlatforms.forEach(function(value, i) {
				if (selectedOS === value) {
					selectedOSavailable = true;
				}
				
				if (osSelector !== undefined) {
					osSelector.children('option').each(function(){ 
						if (value === $(this).val()) {
							$(this).show();			
						}
					});
				}
			});					

			
			if (!downloadablePlatforms.includes(selectedOS)) {
				if (isTracingActive) console.log('OS selected.............: ' + selectedOS);
				var osDefault = getOsSelectValue();
				if (isTracingActive) console.log('OS default..............: ' + osDefault);
                if (downloadablePlatforms.includes(osDefault)) {
					this._osSelector.val(osDefault)
				} else {
					var osDefaultAlternative = getOsSelectValue(true);
					if (isTracingActive) console.log('OS default alternative..: ' + osDefaultAlternative);

					if (downloadablePlatforms.includes(osDefaultAlternative)) {
						this._osSelector.val(osDefaultAlternative);

					} else {
						this._osSelector.val(downloadablePlatforms[0]);
					}
				}
				
				selectedOS = this._osSelector.val();
			}

			if (isTracingActive) {
				if (hasImageType) console.log('selectedImageType.......: ' + selectedImageType);
				if (isPlatformDependent) console.log('selectedOS..............: ' + selectedOS);
			}

            for (var i in this._assets[selectedMajor]['releases'].sort(tagComparator)) {
                var release = this._assets[selectedMajor]['releases'][i]
				var hasDownload = false;
				var downloadUrl = null;
				if (isPlatformDependent) {
					var asset = null;
					if (hasImageType) {
						if (release.hasOwnProperty(selectedImageType)) asset = release[selectedImageType];
					} else {
						if (release.hasOwnProperty('urls')) {
							asset = release['urls'];
							
						}
					}
					
					if (asset) {
						if (asset.hasOwnProperty(selectedOS)) {
							downloadUrl = asset[selectedOS];
							hasDownload = true;
						}
					}


				} else {
					if (release.hasOwnProperty('url')) {
						downloadUrl = release['url'];
						hasDownload = true;
					}
				}
							
				if (hasDownload) {
						var optionElement = $('<option></option>')
						optionElement.text(release.tag)
						optionElement.attr({'value': downloadUrl})
						if (isTracingActive) { 
							console.log('release.tag.............: ' + release.tag);
							console.log('download URL............: ' + optionElement.val()); 

						}
						
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
            this._majorSelector.empty()
            this._imageTypeSelector.empty()
            this._osSelector.empty()
            this._versionSelector.empty()

			if (data.majors !== undefined) {
				for (var i in data.majors.sort(majorsComparator)) {
					if (data.majors[i].lts && !this._ltsCheckbox.is(':checked'))
						continue

					if (data.majors[i].ea && !this._eaCheckbox.is(':checked'))
						continue

					if ((!data.majors[i].lts && !data.majors[i].ea) && !this._nonLtsCheckbox.is(':checked'))
						continue

					var labelElement = $('<div>' + data.majors[i].label + '</div>')
					var optionElement = $('<option></option>')
					optionElement.attr({'value': data.majors[i].id })
					optionElement.addClass('download_select_option')
					optionElement.append(labelElement)

					if (data.majors[i].ea)
						optionElement.append($('<div> (Pre-Release)</div>'))
					else if (data.majors[i].lts)
						optionElement.append($('<div> (Long Term Support)</div>'))

					this._majorSelector.append(optionElement)
				}
			}

            var majorSelectorEmpty = (this._majorSelector.has('option').length <= 0)

            if (majorSelectorEmpty) {
                this._downloadButton.addClass('download_button_disabled')
                this._copyURLButton.addClass('download_button_disabled')
                this._imageTypeSelector.addClass('download_select_disabled')
                this._versionSelector.addClass('download_select_disabled')
                this._osSelector.addClass('download_select_disabled')
                this._majorSelector.addClass('download_select_disabled')
            } else {
                this._downloadButton.removeClass('download_button_disabled')
                this._copyURLButton.removeClass('download_button_disabled')
                this._imageTypeSelector.removeClass('download_select_disabled')
                this._versionSelector.removeClass('download_select_disabled')
                this._osSelector.removeClass('download_select_disabled')
                this._majorSelector.removeClass('download_select_disabled')

				if (data.imageTypes !== undefined) {
					for (var i in data.imageTypes.sort(imageTypeComparator)) {
						var optionElement = $('<option></option>')
						optionElement.text(data.imageTypes[i].value)
						optionElement.attr({'value': data.imageTypes[i].key })
						optionElement.addClass('download_select_option')
						this._imageTypeSelector.append(optionElement)
					}
				}
				
				if (data.os !== undefined) {
					for (var i in data.os.sort(osComparator)) {
						var optionElement = $('<option></option>')
						optionElement.text(data.os[i].value)
						optionElement.attr({'value': data.os[i].key })
						optionElement.addClass('download_select_option')
						this._osSelector.append(optionElement)
					}
                }
	
                this._osSelector.val(getOsSelectValue())

                this._assets = data.assets
                this._onUpdateImageTypeAndOS()
            }
        }.bind(this)

        this._majorSelector.change(function majorSelectorOnChange() {
            this._onUpdateImageTypeAndOS()
        }.bind(this))

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
		
		this._onUpdateData();

    }

    $(document).ready(function () {

		$.ajaxSetup({
			async: false
		});
		
		$.getJSON('assets/data/env.json', function onJSONDataReceived(data) {
			_jsondata[0] = data;
        });
		
		var i = 0;
		Object.entries(_jsondata[0]['buildfiles']).forEach(([key, value]) => {
			if (isTracingActive) console.log('loading config file: ' + key + ' -> id: ' + value['id'] + '  [' + value['file'] + ']');
			
			$.getJSON(value['file'], function onJSONDataReceived(data) {
				_jsondata[++i] = data;
			});
		});
		
		$.ajaxSetup({
			async: true
		});

		const sapMachine = new SapMachine(_jsondata)
    });

})(window, document, jQuery)
