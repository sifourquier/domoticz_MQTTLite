return {

	baseType = 'device',

	name = 'Air quality device',

	matches = function (device, adapterManager)
		local res = (device.deviceType == 'Air Quality')

		if (not res) then
			adapterManager.addDummyMethod(device, 'updateAirQuality')
		end

		return res
	end,

	process = function (device, data, domoticz, utils, adapterManager)

		-- from data: quality

		device['co2'] = tonumber(data.data._nValue) -- co2 (ppm)

		-- quality is automatically added (named)

		device['updateAirQuality'] = function(quality) --ppm
			local url = domoticz.settings['Domoticz url'] .. '/json.htm?type=command&param=udevice&idx=' .. device.id .. '&nvalue=' .. tonumber(quality)
			domoticz.openURL(url)
		end

	end

}