function CreateTimer(length, on_finish)
    return {
        length = length,
        time_left = length,
        on_finish = on_finish,
    }
end

function TickTimer(timer)
    timer.time_left = timer.time_left - Time.delta()
    if timer.time_left <= 0 then
        if timer.on_finish != nil then
            timer.on_finish(timer)
        end
        return true
    else
        return false
    end
end

function RestartTimer(timer)
    timer.time_left = timer.length
end
