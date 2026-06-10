math.randomseed((os.time() % 100000) + math.floor((os.clock() or 0) * 1000))

local value = math.random(180, 320) / 10

return string.format('[{"object":3303,"instance":0,"resource":5700,"value":%.1f}]', value)