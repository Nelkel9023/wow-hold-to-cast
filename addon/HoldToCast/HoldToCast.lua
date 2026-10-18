-- Hold To Cast - notifier only. All behaviour lives in dinput8.dll, which
-- publishes the global HOLDTOCAST_LOADED(). This addon just reports whether
-- the native module is loaded.

local frame = CreateFrame("Frame")
frame:RegisterEvent("PLAYER_LOGIN")
frame:RegisterEvent("ADDON_LOADED")
frame:SetScript("OnEvent", function(self, event, name)
    if event == "ADDON_LOADED" and name ~= "HoldToCast" then return end
    local loaded = (type(HOLDTOCAST_LOADED) == "function") and HOLDTOCAST_LOADED()
    if loaded then
        DEFAULT_CHAT_FRAME:AddMessage("|cff33ff99HoldToCast|r: DLL loaded, holding action keys will repeat casts.")
    else
        DEFAULT_CHAT_FRAME:AddMessage("|cffff5555HoldToCast|r: DLL NOT loaded (dinput8.dll missing).")
    end
end)
