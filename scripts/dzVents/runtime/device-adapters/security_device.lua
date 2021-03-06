local TimedCommand = require('TimedCommand')

return {

	baseType = 'device',

	name = 'Switch device adapter',

	matches = function (device, adapterManager)
		local res = (device.deviceType == 'Security' and device.deviceSubType == 'Security Panel')
		if (not res) then
			adapterManager.addDummyMethod(device, 'disarm')
			adapterManager.addDummyMethod(device, 'armAway')
			adapterManager.addDummyMethod(device, 'armHome')
		end
		return res
	end,

	process = function (device, data, domoticz, utils, adapterManager)

		-- from data: maxDimLevel???

		device.state = domoticz.security -- normalize to domoticz.security states

		function device.disarm()
			return TimedCommand(domoticz, device.name, 'Disarm')
		end

		function device.armAway()
			return TimedCommand(domoticz, device.name, 'Arm Away')
		end

		function device.armHome()
			return TimedCommand(domoticz, device.name, 'Arm Home')
		end

	end

}