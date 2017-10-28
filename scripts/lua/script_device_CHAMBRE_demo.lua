package.path = "/home/simon/domoticz-master_15_09_2017/lua/?.lua"
require "modules"

commandArray = {}

if(devicechanged['Schambre_1'] or devicechanged['Schambre_2'] or devicechanged['Schambre_3'] or devicechanged['Schambre_4']) then
    if(otherdevices['Schambre_1']~='Off' or otherdevices['Schambre_2']~='Off' or otherdevices['Schambre_3']~='Off' or otherdevices['Schambre_4']~='Off') then
        if(otherdevices['chambre']~='Off') then
            switchOff('chambre')
        else
            switchOn('chambre',38)
        end
    end
end

return commandArray
